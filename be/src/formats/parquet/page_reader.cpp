// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "formats/parquet/page_reader.h"

#include <glog/logging.h>

#include <algorithm>
#include <ostream>
#include <vector>

#include "common/compiler_util.h"
#include "exec/hdfs_scanner.h"
#include "gutil/strings/substitute.h"
#include "util/thrift_util.h"

namespace starrocks::parquet {

// Reference for:
// https://github.com/apache/arrow/blob/7ebc88c8fae62ed97bc30865c845c8061132af7e/cpp/src/parquet/column_reader.h#L54-L57
static constexpr size_t kDefaultPageHeaderSize = 16 * 1024;
// 16MB is borrowed from Arrow
static constexpr size_t kMaxPageHeaderSize = 16 * 1024 * 1024;

PageReader::PageReader(io::SeekableInputStream* stream, uint64_t start_offset, uint64_t length, uint64_t num_values,
                       HdfsScanStats* stats)
        : _stream(stream), _finish_offset(start_offset + length), _num_values_total(num_values), _stats(stats) {}

Status PageReader::next_header() {
    if (_offset != _next_header_pos) {
        return Status::InternalError(
                strings::Substitute("Try to parse parquet column header in wrong position, offset=$0 vs expect=$1",
                                    _offset, _next_header_pos));
    }

    DCHECK(_num_values_read <= _num_values_total);
    if (_num_values_read >= _num_values_total || _next_read_page_idx >= _page_num) {
        LOG_IF(WARNING, _num_values_read > _num_values_total)
                << "Read more values than expected, read=" << _num_values_read << ", expect=" << _num_values_total;
        return Status::EndOfFile("");
    }

    size_t allowed_page_size = kDefaultPageHeaderSize;
    size_t remaining = _finish_offset - _offset;
    uint32_t header_length = 0;

    RETURN_IF_ERROR(_stream->seek(_offset));

    do {
        allowed_page_size = std::min(std::min(allowed_page_size, remaining), kMaxPageHeaderSize);

        std::vector<uint8_t> page_buffer;
        const uint8_t* page_buf = nullptr;

        // prefer peek data instead to read data.
        bool peek_mode = false;
        {
            auto st = _stream->peek(allowed_page_size);
            if (st.ok() && st.value().size() == allowed_page_size) {
                page_buf = (const uint8_t*)st.value().data();
                peek_mode = true;
            } else {
                page_buffer.reserve(allowed_page_size);
                RETURN_IF_ERROR(_stream->read_at_fully(_offset, page_buffer.data(), allowed_page_size));
                page_buf = page_buffer.data();
                auto st = _stream->peek(allowed_page_size);
                if (st.ok()) {
                    _stats->bytes_read -= allowed_page_size;
                    peek_mode = true;
                }
            }
        }

        header_length = allowed_page_size;
        auto st = deserialize_thrift_msg(page_buf, &header_length, TProtocolType::COMPACT, &_cur_header);

        if (st.ok()) {
            if (peek_mode) {
                _stats->bytes_read += header_length;
            }
            break;
        }

        if (UNLIKELY((allowed_page_size >= kMaxPageHeaderSize) || (_offset + allowed_page_size) >= _finish_offset)) {
            // Notice, here (_offset + allowed_page_size) >= _finish_offset
            // is using '>=' just to prevent loop infinitely.
            return Status::Corruption(
                    strings::Substitute("Failed to decode parquet page header, page header's size is out of range.  "
                                        "allowed_page_size=$0, max_page_size=$1, offset=$2, finish_offset=$3",
                                        allowed_page_size, kMaxPageHeaderSize, _offset, _finish_offset));
        }

        allowed_page_size *= 2;
    } while (true);
    DCHECK(header_length > 0);
    _offset += header_length;
    _next_header_pos = _offset + _cur_header.compressed_page_size;
    if (_cur_header.type == tparquet::PageType::DATA_PAGE || _cur_header.type == tparquet::PageType::DATA_PAGE_V2) {
        _num_values_read += _cur_header.data_page_header.num_values;
        _next_read_page_idx++;
    }
    return Status::OK();
}

Status PageReader::read_bytes(void* buffer, size_t size) {
    if (_offset + size > _next_header_pos) {
        return Status::InternalError("Size to read exceed page size");
    }
    RETURN_IF_ERROR(_stream->read_at_fully(_offset, buffer, size));
    _offset += size;
    return Status::OK();
}

Status PageReader::skip_bytes(size_t size) {
    if (UNLIKELY(_offset + size > _next_header_pos)) {
        return Status::InternalError("Size to skip exceed page size");
    }
    _offset += size;
    RETURN_IF_ERROR(_stream->skip(size));
    return Status::OK();
}

StatusOr<std::string_view> PageReader::peek(size_t size) {
    if (_offset + size > _next_header_pos) {
        return Status::InternalError("Size to read exceed page size");
    }
    RETURN_IF_ERROR(_stream->seek(_offset));
    ASSIGN_OR_RETURN(auto ret, _stream->peek(size));
    return ret;
}

} // namespace starrocks::parquet
