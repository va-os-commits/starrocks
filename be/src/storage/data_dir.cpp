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

// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/data_dir.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "storage/data_dir.h"

#include <filesystem>
#include <set>
#include <sstream>
#include <utility>

#include "common/config.h"
#include "fs/fs.h"
#include "fs/fs_util.h"
#include "gutil/strings/substitute.h"
#include "runtime/exec_env.h"
#include "service/backend_options.h"
#include "storage/olap_define.h"
#include "storage/rowset/rowset_factory.h"
#include "storage/rowset/rowset_meta.h"
#include "storage/rowset/rowset_meta_manager.h"
#include "storage/storage_engine.h"
#include "storage/tablet_manager.h"
#include "storage/tablet_meta_manager.h"
#include "storage/tablet_updates.h"
#include "storage/txn_manager.h"
#include "storage/utils.h" // for check_dir_existed
#include "util/defer_op.h"
#include "util/errno.h"
#include "util/monotime.h"
#include "util/string_util.h"

using strings::Substitute;

namespace starrocks {

static const char* const kTestFilePath = "/.testfile";

DataDir::DataDir(const std::string& path, TStorageMedium::type storage_medium, TabletManager* tablet_manager,
                 TxnManager* txn_manager)
        : _path(path),
          _available_bytes(0),
          _disk_capacity_bytes(0),
          _storage_medium(storage_medium),
          _tablet_manager(tablet_manager),
          _txn_manager(txn_manager),
          _cluster_id_mgr(std::make_shared<ClusterIdMgr>(path)),
          _current_shard(0) {}

DataDir::~DataDir() {
    delete _id_generator;
    delete _kv_store;
}

Status DataDir::init(bool read_only) {
    ASSIGN_OR_RETURN(_fs, FileSystem::CreateSharedFromString(_path));
    RETURN_IF_ERROR(_fs->path_exists(_path));
    std::string align_tag_path = _path + ALIGN_TAG_PREFIX;
    if (access(align_tag_path.c_str(), F_OK) == 0) {
        RETURN_IF_ERROR_WITH_WARN(Status::NotFound(Substitute("align tag $0 was found", align_tag_path)),
                                  "access file failed");
    }

    RETURN_IF_ERROR_WITH_WARN(update_capacity(), "update_capacity failed");
    RETURN_IF_ERROR_WITH_WARN(_cluster_id_mgr->init(), "_cluster_id_mgr init failed");
    RETURN_IF_ERROR_WITH_WARN(_init_data_dir(), "_init_data_dir failed");
    RETURN_IF_ERROR_WITH_WARN(_init_tmp_dir(), "_init_tmp_dir failed");
    RETURN_IF_ERROR_WITH_WARN(_init_meta(read_only), "_init_meta failed");
    RETURN_IF_ERROR_WITH_WARN(init_persistent_index_dir(), "_init_persistent_index_dir failed");

    _state = DiskState::ONLINE;
    return Status::OK();
}

void DataDir::stop_bg_worker() {
    _stop_bg_worker = true;
}

Status DataDir::_init_data_dir() {
    std::string data_path = _path + DATA_PREFIX;
    auto st = _fs->create_dir_recursive(data_path);
    LOG_IF(ERROR, !st.ok()) << "failed to create data directory " << data_path;
    return st;
}

Status DataDir::init_persistent_index_dir() {
    std::string persistent_index_path = get_persistent_index_path();
    auto st = _fs->create_dir_recursive(persistent_index_path);
    LOG_IF(ERROR, !st.ok()) << "failed to create persistent directory " << persistent_index_path;
    return st;
}

Status DataDir::_init_tmp_dir() {
    std::string tmp_path = get_tmp_path();
    auto st = _fs->create_dir_recursive(tmp_path);
    LOG_IF(ERROR, !st.ok()) << "failed to create temp directory " << tmp_path;
    return st;
}

Status DataDir::_init_meta(bool read_only) {
    // init path hash
    _path_hash = hash_of_path(BackendOptions::get_localhost(), _path);
    LOG(INFO) << "path: " << _path << ", hash: " << _path_hash;

    // init meta
    _kv_store = new (std::nothrow) KVStore(_path);
    if (_kv_store == nullptr) {
        RETURN_IF_ERROR_WITH_WARN(Status::MemoryAllocFailed("allocate memory for KVStore failed"),
                                  "new KVStore failed");
    }
    Status res = _kv_store->init(read_only);
    LOG_IF(WARNING, !res.ok()) << "Fail to init meta store: " << res;
    return res;
}

Status DataDir::set_cluster_id(int32_t cluster_id) {
    return _cluster_id_mgr->set_cluster_id(cluster_id);
}

void DataDir::health_check() {
    const int retry_times = 10;
    // check disk
    if (_state != DiskState::DECOMMISSIONED && _state != DiskState::DISABLED) {
        bool all_failed = true;
        for (int i = 0; i < retry_times; i++) {
            Status res = _read_and_write_test_file();
            if (res.ok() || !is_io_error(res)) {
                all_failed = false;
                break;
            } else {
                LOG(WARNING) << "store read/write test file occur IO Error. path=" << _path
                             << ", res=" << res.to_string();
            }
        }
        if (all_failed) {
            LOG(WARNING) << "store test failed " << retry_times << " times, set _state to OFFLINE. path=" << _path;
            _state = DiskState::OFFLINE;
        } else {
            _state = DiskState::ONLINE;
        }
    }
}

Status DataDir::_read_and_write_test_file() {
    std::string test_file = _path + kTestFilePath;
    return read_write_test_file(test_file);
}

Status DataDir::get_shard(uint64_t* shard) {
    std::stringstream shard_path_stream;
    uint32_t next_shard = 0;
    {
        std::lock_guard<std::mutex> l(_mutex);
        next_shard = _current_shard;
        _current_shard = (_current_shard + 1) % MAX_SHARD_NUM;
    }
    shard_path_stream << _path << DATA_PREFIX << "/" << next_shard;
    std::string shard_path = shard_path_stream.str();
    // First check whether the shard path exists. If it does not exist, sync the data directory.
    bool sync_data_path = false;
    if (!fs::path_exist(shard_path)) {
        sync_data_path = true;
    }
    RETURN_IF_ERROR(_fs->create_dir_recursive(shard_path));
    if (sync_data_path) {
        std::string data_path = _path + DATA_PREFIX;
        if (config::sync_tablet_meta) {
            Status st = fs::sync_dir(data_path);
            if (!st.ok()) {
                LOG(WARNING) << "Fail to sync " << data_path << ": " << st.to_string();
                return st;
            }
        }
    }
    *shard = next_shard;
    return Status::OK();
}

void DataDir::register_tablet(Tablet* tablet) {
    TabletInfo tablet_info(tablet->tablet_id(), tablet->schema_hash(), tablet->tablet_uid());

    std::lock_guard<std::mutex> l(_mutex);
    _tablet_set.emplace(tablet_info);
}

void DataDir::deregister_tablet(Tablet* tablet) {
    TabletInfo tablet_info(tablet->tablet_id(), tablet->schema_hash(), tablet->tablet_uid());

    std::lock_guard<std::mutex> l(_mutex);
    _tablet_set.erase(tablet_info);
}

void DataDir::clear_tablets(std::vector<TabletInfo>* tablet_infos) {
    std::lock_guard<std::mutex> l(_mutex);

    tablet_infos->insert(tablet_infos->end(), _tablet_set.begin(), _tablet_set.end());
    _tablet_set.clear();
}

std::string DataDir::get_absolute_shard_path(int64_t shard_id) {
    return strings::Substitute("$0$1/$2", _path, DATA_PREFIX, shard_id);
}

std::string DataDir::get_absolute_tablet_path(int64_t shard_id, int64_t tablet_id, int32_t schema_hash) {
    return strings::Substitute("$0/$1/$2", get_absolute_shard_path(shard_id), tablet_id, schema_hash);
}

Status DataDir::create_dir_if_path_not_exists(const std::string& path) {
    auto st = _fs->create_dir_recursive(path);
    LOG_IF(ERROR, !st.ok()) << "failed to create directory " << path;
    return st;
}

void DataDir::find_tablet_in_trash(int64_t tablet_id, std::vector<std::string>* paths) {
    // path: /root_path/trash/time_label/tablet_id/schema_hash
    std::string trash_path = _path + TRASH_PREFIX;
    std::vector<std::string> sub_dirs;
    (void)_fs->get_children(trash_path, &sub_dirs);
    for (auto& sub_dir : sub_dirs) {
        // sub dir is time_label
        std::string sub_path = trash_path + "/" + sub_dir;
        auto is_dir = _fs->is_directory(sub_path);
        if (!is_dir.ok() || !is_dir.value()) {
            continue;
        }
        std::string tablet_path = sub_path + "/" + std::to_string(tablet_id);
        if (_fs->path_exists(tablet_path).ok()) {
            paths->emplace_back(std::move(tablet_path));
        }
    }
}

std::string DataDir::get_root_path_from_schema_hash_path_in_trash(const std::string& schema_hash_dir_in_trash) {
    return std::filesystem::path(schema_hash_dir_in_trash)
            .parent_path()
            .parent_path()
            .parent_path()
            .parent_path()
            .string();
}

// [NOTICE] we must ensure that all tablets are either properly loaded or handled within load().
void DataDir::load() {
    // load tablet
    // create tablet from tablet meta and add it to tablet mgr
    int64_t load_tablet_start = MonotonicMillis();
    LOG(INFO) << "begin loading tablet from meta " << _path;
    std::set<int64_t> tablet_ids;
    std::set<int64_t> failed_tablet_ids;
    auto load_tablet_func = [this, &tablet_ids, &failed_tablet_ids](int64_t tablet_id, int32_t schema_hash,
                                                                    std::string_view value) -> bool {
        Status st =
                _tablet_manager->load_tablet_from_meta(this, tablet_id, schema_hash, value, false, false, false, false);
        if (!st.ok() && !st.is_not_found() && !st.is_already_exist()) {
            // load_tablet_from_meta() may return NotFound which means the tablet status is DELETED
            // This may happen when the tablet was just deleted before the BE restarted,
            // but it has not been cleared from rocksdb. At this time, restarting the BE
            // will read the tablet in the DELETE state from rocksdb. These tablets have been
            // added to the garbage collection queue and will be automatically deleted afterwards.
            // Therefore, we believe that this situation is not a failure.
            LOG(WARNING) << "load tablet from header failed. status:" << st.to_string() << ", tablet=" << tablet_id
                         << "." << schema_hash;
            failed_tablet_ids.insert(tablet_id);
        } else {
            tablet_ids.insert(tablet_id);
        }
        return true;
    };
    Status load_tablet_status =
            TabletMetaManager::walk_until_timeout(_kv_store, load_tablet_func, config::load_tablet_timeout_seconds);
    if (load_tablet_status.is_time_out()) {
        LOG(WARNING) << "load tablets from rocksdb timeout, try to compact meta and retry. path: " << _path;
        Status s = _kv_store->compact();
        if (!s.ok()) {
            // We don't need to make sure compact MUST success. Just ignore the error.
            LOG(ERROR) << "data dir " << _path << " compact meta before load failed";
        } else {
            LOG(WARNING) << "compact meta finished, retry load tablets from rocksdb. path: " << _path;
        }
        for (auto tablet_id : tablet_ids) {
            Status s = _tablet_manager->drop_tablet(tablet_id, kKeepMetaAndFiles);
            if (!s.ok()) {
                // Only print log, do not return error. Later load tablet from rocksdb can handle this.
                LOG(ERROR) << "data dir " << _path << " drop_tablet failed: " << s.message();
            }
        }
        tablet_ids.clear();
        failed_tablet_ids.clear();
        load_tablet_status = TabletMetaManager::walk(_kv_store, load_tablet_func);
    }

    if (failed_tablet_ids.size() != 0) {
        LOG(ERROR) << "load tablets from header failed"
                   << ", loaded tablet: " << tablet_ids.size() << ", error tablet: " << failed_tablet_ids.size()
                   << ", path: " << _path;
        if (!config::ignore_load_tablet_failure) {
            LOG(FATAL) << "load tablets encounter failure. stop BE process. path: " << _path;
        }
    }
    if (!load_tablet_status.ok()) {
        LOG(FATAL) << "there is failure when scan rockdb tablet metas, quit process"
                   << ". loaded tablet: " << tablet_ids.size() << " error tablet: " << failed_tablet_ids.size()
                   << ", path: " << _path << " error: " << load_tablet_status.message()
                   << " duration: " << (MonotonicMillis() - load_tablet_start) << "ms";
    } else {
        LOG(INFO) << "load tablet from meta finished"
                  << ", loaded tablet: " << tablet_ids.size() << ", error tablet: " << failed_tablet_ids.size()
                  << ", path: " << _path << " duration: " << (MonotonicMillis() - load_tablet_start) << "ms";
    }

    for (int64_t tablet_id : tablet_ids) {
        TabletSharedPtr tablet = _tablet_manager->get_tablet(tablet_id);
        /*
         * check path here, in migration case, it is possible that
         * there are two different tablets with the same tablet id
         * in two different paths. And one of them is shutdown.
         * For the path with shutdown tablet, should skip the
         * tablet meta save here. (tablet get from manager is not the shutdown one)
        */
        if (tablet && tablet->data_dir()->path_hash() == this->path_hash() &&
            tablet->set_tablet_schema_into_rowset_meta()) {
            TabletMetaPB tablet_meta_pb;
            tablet->tablet_meta()->to_meta_pb(&tablet_meta_pb);
            Status s = TabletMetaManager::save(this, tablet_meta_pb);
            if (!s.ok()) {
                // Only print log, do not return error. We can handle it later.
                LOG(ERROR) << "data dir " << _path << " save tablet meta failed: " << s.message();
            }
        }
    }

    // load rowset meta from meta env and create rowset
    // COMMITTED: add to txn manager
    // VISIBLE: add to tablet
    // if one rowset load failed, then the total data dir will not be loaded
    int64_t load_rowset_start = MonotonicMillis();
    size_t error_rowset_count = 0;
    size_t total_rowset_count = 0;
    LOG(INFO) << "begin loading rowset from meta " << _path;
    auto load_rowset_func = [&](const TabletUid& tablet_uid, RowsetId rowset_id, std::string_view meta_str) -> bool {
        total_rowset_count++;
        bool parsed = false;
        auto rowset_meta = std::make_shared<RowsetMeta>(meta_str, &parsed);
        if (!parsed) {
            LOG(WARNING) << "parse rowset meta string failed for rowset_id:" << rowset_id;
            // return false will break meta iterator, return true to skip this error
            error_rowset_count++;
            return true;
        }
        TabletSharedPtr tablet = _tablet_manager->get_tablet(rowset_meta->tablet_id(), false);
        // tablet maybe dropped, but not drop related rowset meta
        if (tablet == nullptr) {
            // maybe too many due to historical bug, limit logging
            LOG_EVERY_SECOND(WARNING) << "could not find tablet id: " << rowset_meta->tablet_id()
                                      << " for rowset: " << rowset_meta->rowset_id() << ", skip loading this rowset";
            error_rowset_count++;
            return true;
        }
        RowsetSharedPtr rowset;
        Status create_status =
                RowsetFactory::create_rowset(tablet->tablet_schema(), tablet->schema_hash_path(), rowset_meta, &rowset);
        if (!create_status.ok()) {
            LOG(WARNING) << "Fail to create rowset from rowsetmeta,"
                         << " rowset=" << rowset_meta->rowset_id() << " state=" << rowset_meta->rowset_state();
            error_rowset_count++;
            return true;
        }
        if (rowset_meta->rowset_state() == RowsetStatePB::COMMITTED &&
            rowset_meta->tablet_uid() == tablet->tablet_uid()) {
            if (!rowset_meta->tablet_schema()) {
                auto tablet_schema_ptr = tablet->tablet_schema();
                rowset_meta->set_tablet_schema(tablet_schema_ptr);
                rowset_meta->set_skip_tablet_schema(true);
            }
            Status commit_txn_status = _txn_manager->commit_txn(
                    _kv_store, rowset_meta->partition_id(), rowset_meta->txn_id(), rowset_meta->tablet_id(),
                    rowset_meta->tablet_schema_hash(), rowset_meta->tablet_uid(), rowset_meta->load_id(), rowset, true);
            if (!commit_txn_status.ok() && !commit_txn_status.is_already_exist()) {
                LOG(WARNING) << "Fail to add committed rowset=" << rowset_meta->rowset_id()
                             << " tablet=" << rowset_meta->tablet_id() << " txn_id: " << rowset_meta->txn_id();
                error_rowset_count++;
            } else {
                LOG(INFO) << "Added committed rowset=" << rowset_meta->rowset_id()
                          << " tablet=" << rowset_meta->tablet_id() << " txn_id: " << rowset_meta->txn_id();
            }

        } else if (rowset_meta->rowset_state() == RowsetStatePB::VISIBLE &&
                   rowset_meta->tablet_uid() == tablet->tablet_uid()) {
            if (tablet->keys_type() == KeysType::PRIMARY_KEYS) {
                VLOG(1) << "skip a visible rowset meta, tablet: " << tablet->tablet_id()
                        << ", rowset: " << rowset_meta->rowset_id();
            } else {
                Status publish_status = tablet->load_rowset(rowset);
                if (!rowset_meta->tablet_schema()) {
                    rowset_meta->set_tablet_schema(tablet->tablet_schema());
                    rowset_meta->set_skip_tablet_schema(true);
                }
                if (!publish_status.ok() && !publish_status.is_already_exist()) {
                    LOG(WARNING) << "Fail to add visible rowset=" << rowset->rowset_id()
                                 << " to tablet=" << rowset_meta->tablet_id() << " txn id=" << rowset_meta->txn_id()
                                 << " start version=" << rowset_meta->version().first
                                 << " end version=" << rowset_meta->version().second;
                    error_rowset_count++;
                }
            }
        } else {
            LOG(WARNING) << "Found invalid rowset=" << rowset_meta->rowset_id()
                         << " tablet id=" << rowset_meta->tablet_id() << " tablet uid=" << rowset_meta->tablet_uid()
                         << " txn_id: " << rowset_meta->txn_id()
                         << " current valid tablet uid=" << tablet->tablet_uid();
            error_rowset_count++;
        }
        return true;
    };
    Status load_rowset_status = RowsetMetaManager::traverse_rowset_metas(_kv_store, load_rowset_func);

    if (!load_rowset_status.ok()) {
        LOG(WARNING) << "load rowset from meta finished, data dir: " << _path << " error/total: " << error_rowset_count
                     << "/" << total_rowset_count << " error: " << load_rowset_status.message()
                     << " duration: " << (MonotonicMillis() - load_rowset_start) << "ms";
    } else {
        LOG(INFO) << "load rowset from meta finished, data dir: " << _path << " error/total: " << error_rowset_count
                  << "/" << total_rowset_count << " duration: " << (MonotonicMillis() - load_rowset_start) << "ms";
    }

    for (int64_t tablet_id : tablet_ids) {
        TabletSharedPtr tablet = _tablet_manager->get_tablet(tablet_id, false);
        if (tablet == nullptr) {
            continue;
        }
        // ignore the failure, and this behaviour is the same as that when failed to load rowset above.
        // For full data, FE will repair it by cloning data from other replicas. For binlog, there may
        // be data loss, because there is no clone mechanism for binlog currently, and the application
        // should deal with the case. For example, realtime MV can initialize with the newest full data
        // to skip the lost binlog, and process the new binlog after that. The situation is similar with
        // that the binlog is expired and deleted before the application processes it.
        Status st = tablet->finish_load_rowsets();
        if (!st.ok()) {
            LOG(WARNING) << "Fail to finish loading rowsets, tablet id=" << tablet_id << ", status: " << st.to_string();
        }
    }
}

// gc unused tablet schemahash dir
const size_t LOG_GC_BATCH_SIZE = 50;
void DataDir::perform_path_gc_by_tablet() {
    std::unique_lock<std::mutex> lck(_check_path_mutex);
    if (_stop_bg_worker || _all_tablet_schemahash_paths.empty()) {
        return;
    }
    LOG(INFO) << "start to path gc by tablet schema hash.";
    int counter = 0;
    std::vector<string> delete_success_tablet_paths;
    auto log_tablet_path = [](const auto& paths) {
        if (paths.empty()) return;
        std::stringstream ss;
        for (const auto& path : paths) {
            ss << path << ",";
        }
        LOG(INFO) << "Move tablet_id_path to trash: [" << ss.str() << "]";
    };
    for (auto& path : _all_tablet_schemahash_paths) {
        ++counter;
        if (config::path_gc_check_step > 0 && counter % config::path_gc_check_step == 0) {
            SleepFor(MonoDelta::FromMilliseconds(config::path_gc_check_step_interval_ms));
        }
        TTabletId tablet_id = -1;
        TSchemaHash schema_hash = -1;
        bool is_valid = _tablet_manager->get_tablet_id_and_schema_hash_from_path(path, &tablet_id, &schema_hash);
        if (!is_valid) {
            LOG(WARNING) << "unknown path:" << path;
            continue;
        }
        // should not happen, because already check it is a valid tablet schema hash path in previous step
        // so that log fatal here
        if (tablet_id < 1 || schema_hash < 1) {
            LOG(WARNING) << "invalid tablet id " << tablet_id << " or schema hash " << schema_hash << ", path=" << path;
            continue;
        }
        TabletSharedPtr tablet = _tablet_manager->get_tablet(tablet_id, true);
        if (tablet != nullptr) {
            // could find the tablet, then skip check it
            continue;
        }
        std::filesystem::path schema_hash_path(path);
        std::filesystem::path tablet_id_path = schema_hash_path.parent_path();
        std::filesystem::path data_dir_path = tablet_id_path.parent_path().parent_path().parent_path();
        std::string data_dir_string = data_dir_path.string();
        DataDir* data_dir = StorageEngine::instance()->get_store(data_dir_string);
        if (data_dir == nullptr) {
            LOG(WARNING) << "could not find data dir for tablet path " << path;
            continue;
        }
        auto st = _tablet_manager->try_delete_unused_tablet_path(data_dir, tablet_id, schema_hash,
                                                                 tablet_id_path.string());
        if (!st.ok()) {
            LOG(INFO) << "remove " << tablet_id_path << "failed, status: " << st;
        } else {
            if (delete_success_tablet_paths.size() > LOG_GC_BATCH_SIZE) {
                log_tablet_path(delete_success_tablet_paths);
                delete_success_tablet_paths.clear();
            }
            delete_success_tablet_paths.emplace_back(tablet_id_path.string());
        }
    }
    if (delete_success_tablet_paths.size() > 0) {
        log_tablet_path(delete_success_tablet_paths);
        delete_success_tablet_paths.clear();
    }

    _all_tablet_schemahash_paths.clear();
    LOG(INFO) << "finished one time path gc by tablet.";
}

bool DataDir::_need_gc_delta_column_files(
        const std::string& path, int64_t tablet_id,
        std::unordered_map<int64_t, std::unordered_set<std::string>>& delta_column_files) {
    TabletSharedPtr tablet = _tablet_manager->get_tablet(tablet_id, false);
    if (tablet == nullptr || tablet->keys_type() != KeysType::PRIMARY_KEYS ||
        tablet->tablet_state() != TABLET_RUNNING || _tablet_manager->check_clone_tablet(tablet_id) ||
        tablet->is_migrating()) {
        // skip gc when tablet is doing schema change, clone or migration.
        return false;
    }
    if (delta_column_files.count(tablet_id) == 0) {
        // load dcg file list
        DeltaColumnGroupList dcgs;
        if (tablet->updates()->need_apply()) {
            // if this tablet has apply task to handle, skip it, because it can't get latest dcgs
            return false;
        }
        auto st = TabletMetaManager::scan_tablet_delta_column_group(_kv_store, tablet_id, &dcgs);
        if (!st.ok()) {
            LOG(WARNING) << "scan tablet delta column group failed, tablet_id: " << tablet_id << ", st: " << st;
            return false;
        }
        auto& files = delta_column_files[tablet_id];
        for (const auto& dcg : dcgs) {
            const auto& column_files = dcg->relative_column_files();
            files.insert(column_files.begin(), column_files.end());
        }
    }
    std::string filename = std::filesystem::path(path).filename().string();
    return delta_column_files[tablet_id].count(filename) == 0;
}

static bool is_delta_column_file(const std::string& path) {
    StringPiece sp(path);
    if (sp.ends_with(".cols")) {
        return true;
    }
    return false;
}

void DataDir::perform_delta_column_files_gc() {
    std::unique_lock<std::mutex> lck(_check_path_mutex);
    if (_stop_bg_worker || _all_check_dcg_files.empty()) {
        return;
    }
    LOG(INFO) << "start to do delta column files gc.";
    std::unordered_map<int64_t, std::unordered_set<std::string>> delta_column_files;
    int counter = 0;
    for (auto& path : _all_check_dcg_files) {
        ++counter;
        if (config::path_gc_check_step > 0 && counter % config::path_gc_check_step == 0) {
            SleepFor(MonoDelta::FromMilliseconds(config::path_gc_check_step_interval_ms));
        }
        TTabletId tablet_id = -1;
        TSchemaHash schema_hash = -1;
        bool is_valid = _tablet_manager->get_tablet_id_and_schema_hash_from_path(path, &tablet_id, &schema_hash);
        if (!is_valid) {
            LOG(WARNING) << "unknown path:" << path;
            continue;
        }
        if (tablet_id > 0 && schema_hash > 0) {
            if (_need_gc_delta_column_files(path, tablet_id, delta_column_files)) {
                _process_garbage_path(path);
            }
        }
    }
    _all_check_dcg_files.clear();
    LOG(INFO) << "finished one time delta column files gc.";
}

void DataDir::perform_path_gc_by_rowsetid() {
    // init the set of valid path
    // validate the path in data dir
    std::unique_lock<std::mutex> lck(_check_path_mutex);
    if (_stop_bg_worker || _all_check_paths.empty()) {
        return;
    }
    LOG(INFO) << "start to path gc by rowsetid.";
    int counter = 0;
    for (auto& path : _all_check_paths) {
        ++counter;
        if (config::path_gc_check_step > 0 && counter % config::path_gc_check_step == 0) {
            SleepFor(MonoDelta::FromMilliseconds(config::path_gc_check_step_interval_ms));
        }
        TTabletId tablet_id = -1;
        TSchemaHash schema_hash = -1;
        bool is_valid = _tablet_manager->get_tablet_id_and_schema_hash_from_path(path, &tablet_id, &schema_hash);
        if (!is_valid) {
            LOG(WARNING) << "unknown path:" << path;
            continue;
        }
        if (tablet_id > 0 && schema_hash > 0) {
            // tablet schema hash path or rowset file path
            // gc thread should get tablet include deleted tablet
            // or it will delete rowset file before tablet is garbage collected
            RowsetId rowset_id;
            bool is_rowset_file = TabletManager::get_rowset_id_from_path(path, &rowset_id);
            if (is_rowset_file) {
                TabletSharedPtr tablet = _tablet_manager->get_tablet(tablet_id, false);
                if (tablet != nullptr) {
                    if (!tablet->check_rowset_id(rowset_id) &&
                        !StorageEngine::instance()->check_rowset_id_in_unused_rowsets(rowset_id)) {
                        _process_garbage_path(path);
                    }
                }
            }
        }
    }
    _all_check_paths.clear();
    LOG(INFO) << "finished one time path gc by rowsetid.";
}

void DataDir::perform_crm_gc(int32_t unused_crm_file_threshold_sec) {
    // init the set of valid path
    // validate the path in data dir
    std::unique_lock<std::mutex> lck(_check_path_mutex);
    if (_stop_bg_worker || _all_check_crm_files.empty()) {
        return;
    }
    LOG(INFO) << "start to crm file gc.";
    int counter = 0;
    for (auto& path : _all_check_crm_files) {
        ++counter;
        if (config::path_gc_check_step > 0 && counter % config::path_gc_check_step == 0) {
            SleepFor(MonoDelta::FromMilliseconds(config::path_gc_check_step_interval_ms));
        }
        auto now = time(nullptr);
        auto mtime_or = FileSystem::Default()->get_file_modified_time(path);
        if (!mtime_or.ok() || (*mtime_or) <= 0) {
            continue;
        }
        if (now >= unused_crm_file_threshold_sec + (*mtime_or)) {
            _process_garbage_path(path);
        }
    }
    _all_check_crm_files.clear();
    LOG(INFO) << "finished one time crm file gc.";
}

void DataDir::perform_tmp_path_scan() {
    std::unique_lock<std::mutex> lck(_check_path_mutex);
    if (!_all_check_crm_files.empty()) {
        LOG(INFO) << "_all_check_crm_files is not empty when tmp path scan.";
        return;
    }
    LOG(INFO) << "start to scan tmp dir path.";
    std::string tmp_path_str = _path + TMP_PREFIX;
    std::filesystem::path tmp_path(tmp_path_str.c_str());
    try {
        for (const auto& entry : std::filesystem::directory_iterator(tmp_path)) {
            if (entry.is_regular_file()) {
                const auto& filename = entry.path().string();
                if (filename.ends_with(".crm")) {
                    _all_check_crm_files.insert(filename);
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& ex) {
        LOG(ERROR) << "Iterate dir " << tmp_path_str << " Filesystem error: " << ex.what();
        // do nothing
    } catch (const std::exception& ex) {
        LOG(ERROR) << "Iterate dir " << tmp_path_str << " Standard error: " << ex.what();
        // do nothing
    } catch (...) {
        LOG(ERROR) << "Iterate dir " << tmp_path_str << " Unknown exception occurred.";
        // do nothing
    }
}

// path producer
void DataDir::perform_path_scan() {
    {
        std::unique_lock<std::mutex> lck(_check_path_mutex);
        if (!_all_check_paths.empty() || !_all_check_dcg_files.empty()) {
            LOG(INFO) << "_all_check_paths or _all_check_dcg_files is not empty when path scan.";
            return;
        }
        LOG(INFO) << "start to scan data dir path:" << _path;
        std::set<std::string> shards;
        std::string data_path = _path + DATA_PREFIX;

        Status ret = fs::list_dirs_files(_fs.get(), data_path, &shards, nullptr);
        if (!ret.ok()) {
            LOG(WARNING) << "fail to walk dir. path=[" + data_path << "] error[" << ret.to_string() << "]";
            return;
        }

        for (const auto& shard : shards) {
            std::string shard_path = data_path + "/" + shard;
            std::set<std::string> tablet_ids;
            ret = fs::list_dirs_files(_fs.get(), shard_path, &tablet_ids, nullptr);
            if (!ret.ok()) {
                LOG(WARNING) << "fail to walk dir. [path=" << shard_path << "] error[" << ret.to_string() << "]";
                continue;
            }
            for (const auto& tablet_id : tablet_ids) {
                std::string tablet_id_path = shard_path + "/" + tablet_id;
                std::set<std::string> schema_hashes;
                ret = fs::list_dirs_files(_fs.get(), tablet_id_path, &schema_hashes, nullptr);
                if (!ret.ok()) {
                    LOG(WARNING) << "fail to walk dir. [path=" << tablet_id_path << "]"
                                 << " error[" << ret.to_string() << "]";
                    continue;
                }
                for (const auto& schema_hash : schema_hashes) {
                    std::string tablet_schema_hash_path = tablet_id_path + "/" + schema_hash;
                    _all_tablet_schemahash_paths.insert(tablet_schema_hash_path);
                    std::set<std::string> rowset_files;
                    std::set<std::string> inverted_dirs;

                    ret = fs::list_dirs_files(_fs.get(), tablet_schema_hash_path, &inverted_dirs, &rowset_files);
                    if (!ret.ok()) {
                        LOG(WARNING) << "fail to walk dir. [path=" << tablet_schema_hash_path << "] error["
                                     << ret.to_string() << "]";
                        continue;
                    }
                    for (const auto& rowset_file : rowset_files) {
                        std::string rowset_file_path = tablet_schema_hash_path + "/" + rowset_file;
                        if (is_delta_column_file(rowset_file)) {
                            _all_check_dcg_files.insert(rowset_file_path);
                        } else {
                            _all_check_paths.insert(rowset_file_path);
                        }
                    }
                    for (const auto& inverted_dir : inverted_dirs) {
                        std::string inverted_index_path = tablet_schema_hash_path + "/" + inverted_dir;
                        if (inverted_index_path.ends_with(".ivt")) {
                            _all_check_paths.insert(inverted_index_path);
                        }
                    }
                }
            }
        }
        LOG(INFO) << "scan data dir path:" << _path << " finished. path size:" << _all_check_paths.size()
                  << " dcg file size: " << _all_check_dcg_files.size();
    }
}

void DataDir::_process_garbage_path(const std::string& path) {
    if (_fs->path_exists(path).ok()) {
        LOG(INFO) << "collect garbage dir path: " << path;
        auto st = _fs->delete_dir_recursive(path);
        LOG_IF(WARNING, !st.ok()) << "failed to remove garbage dir " << path << ": " << st;
    }
}

Status DataDir::update_capacity() {
    ASSIGN_OR_RETURN(auto space_info, FileSystem::Default()->space(_path));
    _available_bytes = space_info.available;
    _disk_capacity_bytes = space_info.capacity;
    return Status::OK();
}

bool DataDir::capacity_limit_reached(int64_t incoming_data_size) {
    double used_pct = disk_usage(incoming_data_size);
    int64_t left_bytes = _available_bytes - incoming_data_size;

    if (used_pct >= config::storage_flood_stage_usage_percent / 100.0 &&
        left_bytes <= config::storage_flood_stage_left_capacity_bytes) {
        LOG(WARNING) << "reach capacity limit. used pct: " << used_pct << ", left bytes: " << left_bytes
                     << ", path: " << _path;
        return true;
    }
    return false;
}
} // namespace starrocks
