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
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/test/java/org/apache/doris/common/util/SmallFileMgrTest.java

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

package com.starrocks.common.util;

import com.starrocks.catalog.Database;
import com.starrocks.common.Config;
import com.starrocks.common.DdlException;
import com.starrocks.common.jmockit.Deencapsulation;
import com.starrocks.common.util.SmallFileMgr.SmallFile;
import com.starrocks.persist.EditLog;
import com.starrocks.persist.metablock.SRMetaBlockReader;
import com.starrocks.persist.metablock.SRMetaBlockReaderV2;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.sql.ast.CreateFileStmt;
import com.starrocks.utframe.UtFrameUtils;
import mockit.Expectations;
import mockit.Injectable;
import mockit.Mocked;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Disabled;
import org.junit.jupiter.api.Test;

public class SmallFileMgrTest {

    @Mocked
    GlobalStateMgr globalStateMgr;
    @Mocked
    EditLog editLog;
    @Mocked
    Database db;

    @BeforeEach
    public void setUp() {
        UtFrameUtils.setUpForPersistTest();
    }

    @AfterEach
    public void teardown() {
        UtFrameUtils.tearDownForPersisTest();
    }

    @Disabled // Could not find a way to mock a private method
    @Test
    public void test(@Injectable CreateFileStmt stmt1, @Injectable CreateFileStmt stmt2) throws DdlException {
        new Expectations() {
            {
                db.getId();
                minTimes = 0;
                result = 1L;
                globalStateMgr.getLocalMetastore().getDb(anyString);
                minTimes = 0;
                result = db;
                stmt1.getDbName();
                minTimes = 0;
                result = "db1";
                stmt1.getFileName();
                minTimes = 0;
                result = "file1";
                stmt1.getCatalogName();
                minTimes = 0;
                result = "kafka";
                stmt1.getDownloadUrl();
                minTimes = 0;
                result = "http://127.0.0.1:8001/file1";

                stmt2.getDbName();
                minTimes = 0;
                result = "db1";
                stmt2.getFileName();
                minTimes = 0;
                result = "file2";
                stmt2.getCatalogName();
                minTimes = 0;
                result = "kafka";
                stmt2.getDownloadUrl();
                minTimes = 0;
                result = "http://127.0.0.1:8001/file2";
            }
        };

        SmallFile smallFile = new SmallFile(1L, "kafka", "file1", 10001L, "ABCD", 12, "12345", true);
        final SmallFileMgr smallFileMgr = new SmallFileMgr();
        new Expectations(smallFileMgr) {
            {
                Deencapsulation.invoke(smallFileMgr, "downloadAndCheck", anyLong, anyString, anyString, anyString,
                        anyString, anyBoolean);
                result = smallFile;
            }
        };

        // 1. test create
        try {
            smallFileMgr.createFile(stmt1);
        } catch (DdlException e) {
            e.printStackTrace();
            Assertions.fail(e.getMessage());
        }

        Assertions.assertTrue(smallFileMgr.containsFile(1L, "kafka", "file1"));
        SmallFile gotFile = smallFileMgr.getSmallFile(1L, "kafka", "file1", true);
        Assertions.assertEquals(10001L, gotFile.id);
        gotFile = smallFileMgr.getSmallFile(10001L);
        Assertions.assertEquals(10001L, gotFile.id);

        // 2. test file num limit
        Config.max_small_file_number = 1;
        boolean fail = false;
        try {
            smallFileMgr.createFile(stmt2);
        } catch (DdlException e) {
            fail = true;
            Assertions.assertTrue(e.getMessage().contains("File number exceeds limit"));
        }
        Assertions.assertTrue(fail);

        // 3. test remove
        try {
            smallFileMgr.removeFile(2L, "kafka", "file1", true);
        } catch (DdlException e) {
            // this is expected
        }
        gotFile = smallFileMgr.getSmallFile(10001L);
        Assertions.assertEquals(10001L, gotFile.id);
        smallFileMgr.removeFile(1L, "kafka", "file1", true);
        gotFile = smallFileMgr.getSmallFile(10001L);
        Assertions.assertNull(gotFile);

        // 4. test file limit again
        try {
            smallFileMgr.createFile(stmt1);
        } catch (DdlException e) {
            e.printStackTrace();
            Assertions.fail(e.getMessage());
        }
        gotFile = smallFileMgr.getSmallFile(10001L);
        Assertions.assertEquals(10001L, gotFile.id);
    }

    @Test
    public void testSaveLoadJsonFormatImage() throws Exception {
        SmallFileMgr smallFileMgr = new SmallFileMgr();
        SmallFile smallFile = new SmallFile(1L, "c1", "f1", 2L, "xxx", 3, "xxx", true);
        smallFileMgr.replayCreateFile(smallFile);

        UtFrameUtils.PseudoImage image = new UtFrameUtils.PseudoImage();
        smallFileMgr.saveSmallFilesV2(image.getImageWriter());

        SmallFileMgr followerMgr = new SmallFileMgr();
        SRMetaBlockReader reader = new SRMetaBlockReaderV2(image.getJsonReader());
        followerMgr.loadSmallFilesV2(reader);
        reader.close();

        Assertions.assertNotNull(followerMgr.getSmallFile(2L));
    }
}
