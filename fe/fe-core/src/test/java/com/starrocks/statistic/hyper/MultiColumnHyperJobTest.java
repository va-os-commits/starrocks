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

package com.starrocks.statistic.hyper;

import com.google.common.collect.Maps;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.OlapTable;
import com.starrocks.catalog.Partition;
import com.starrocks.catalog.Table;
import com.starrocks.catalog.Type;
import com.starrocks.common.FeConstants;
import com.starrocks.qe.ConnectContext;
import com.starrocks.qe.StmtExecutor;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.sql.plan.DistributedEnvPlanTestBase;
import com.starrocks.sql.plan.PlanTestBase;
import com.starrocks.statistic.AnalyzeStatus;
import com.starrocks.statistic.HyperStatisticsCollectJob;
import com.starrocks.statistic.MultiColumnHyperStatisticsCollectJob;
import com.starrocks.statistic.NativeAnalyzeStatus;
import com.starrocks.statistic.StatisticUtils;
import com.starrocks.statistic.StatsConstants;
import com.starrocks.statistic.base.DefaultColumnStats;
import com.starrocks.statistic.base.MultiColumnStats;
import com.starrocks.utframe.StarRocksAssert;
import mockit.Mock;
import mockit.MockUp;
import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

import java.time.LocalDateTime;
import java.util.HashMap;
import java.util.List;

public class MultiColumnHyperJobTest extends DistributedEnvPlanTestBase {
    private static Database db;

    private static Table table;

    @BeforeAll
    public static void beforeClass() throws Exception {
        PlanTestBase.beforeClass();
        StarRocksAssert starRocksAssert = new StarRocksAssert(connectContext);
        FeConstants.runningUnitTest = true;
        starRocksAssert.withTable("create table t_struct(c0 INT, " +
                "c1 date," +
                "c2 varchar(255)," +
                "c3 decimal(10, 2)," +
                "c4 struct<a int, b array<struct<a int, b int>>>," +
                "c5 struct<a int, b int>," +
                "c6 struct<a int, b int, c struct<a int, b int>, d array<int>>) " +
                "duplicate key(c0) distributed by hash(c0) buckets 1 " +
                "properties('replication_num'='1');");
        db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
        table = GlobalStateMgr.getCurrentState().getLocalMetastore().getTable("test", "t_struct");

        for (Partition partition : ((OlapTable) table).getAllPartitions()) {
            partition.getDefaultPhysicalPartition().getBaseIndex().setRowCount(10000);
        }
    }

    @AfterAll
    public static void afterClass() {
        FeConstants.runningUnitTest = false;
    }

    @Test
    public void testFullMultiColumnHyperJob() {
        List<String> columnNames = List.of("c1", "c2", "c3");

        List<HyperQueryJob> jobs = HyperQueryJob.createMultiColumnQueryJobs(connectContext, db, table, List.of(columnNames),
                StatsConstants.AnalyzeType.FULL, List.of(StatsConstants.StatisticsType.MCDISTINCT), null);

        Assertions.assertEquals(1, jobs.size());

        String sql = ((MultiColumnQueryJob) jobs.get(0)).buildStatisticsQuery();
        String expectedSql = "SELECT cast(12 as INT), '1#2#3', cast(ndv(murmur_hash3_32(coalesce(`c1`, '')," +
                " coalesce(`c2`, ''), coalesce(`c3`, ''))) as BIGINT) from `test`.`t_struct`";
        Assertions.assertEquals(expectedSql, sql);
    }

    @Test
    public void testSampleMultiColumnHyperJob() {
        List<String> columnNames = List.of("c1", "c2", "c3");

        List<HyperQueryJob> jobs = HyperQueryJob.createMultiColumnQueryJobs(connectContext, db, table, List.of(columnNames),
                StatsConstants.AnalyzeType.SAMPLE, List.of(StatsConstants.StatisticsType.MCDISTINCT), new HashMap<>());
        Assertions.assertEquals(1, jobs.size());
        String sql = ((MultiColumnQueryJob) jobs.get(0)).buildStatisticsQuery();
        String expectedSql = "WITH base_cte_table as (SELECT murmur_hash3_32(coalesce(`c1`, ''), " +
                "coalesce(`c2`, ''), coalesce(`c3`, '')) as combined_column_key FROM `test`.`t_struct` LIMIT 200000" +
                ")  SELECT\n" +
                "    cast(12 as INT),\n" +
                "    '1#2#3',\n" +
                "    cast(IFNULL(COUNT(1) + (sqrt(1 / SUM(t1.count)) - 1) * SUM(IF(t1.count = 1, 1, 0)), COUNT(1)) as BIGINT)\n" +
                "FROM (\n" +
                "    SELECT\n" +
                "        t0.`column_key`,\n" +
                "        COUNT(1) as count\n" +
                "    FROM (\n" +
                "        SELECT\n" +
                "            combined_column_key AS column_key\n" +
                "        FROM\n" +
                "            `base_cte_table`\n" +
                "    ) as t0\n" +
                "    GROUP BY t0.column_key \n" +
                ") AS t1;";
        Assertions.assertEquals(expectedSql, sql);
    }

    @Test
    public void testMultiColumnHyperQueryStatisticsJobs() {
        new MockUp<StmtExecutor>() {
            @Mock
            public void execute() throws Exception {
            }
        };
        List<String> columnNames = List.of("c4", "c5", "c6");

        HyperStatisticsCollectJob job = new MultiColumnHyperStatisticsCollectJob(db, table, null, columnNames, null,
                StatsConstants.AnalyzeType.FULL, StatsConstants.ScheduleType.ONCE, Maps.newHashMap(),
                List.of(StatsConstants.StatisticsType.MCDISTINCT), List.of(columnNames));

        ConnectContext context = StatisticUtils.buildConnectContext();
        AnalyzeStatus status = new NativeAnalyzeStatus(1, 1, 1, columnNames, StatsConstants.AnalyzeType.FULL,
                StatsConstants.ScheduleType.ONCE, Maps.newHashMap(), LocalDateTime.now());
        try {
            job.collect(context, status);
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    @Test
    public void testColumnStats() {
        DefaultColumnStats defaultColumnStats = new DefaultColumnStats("c1", Type.DATE, 1);
        Assertions.assertEquals(1, defaultColumnStats.getColumnId());
        Assertions.assertEquals("", defaultColumnStats.getMax());
        Assertions.assertEquals("", defaultColumnStats.getMin());
        Assertions.assertEquals("", defaultColumnStats.getFullDataSize());
        Assertions.assertEquals("", defaultColumnStats.getNDV());
        Assertions.assertEquals("", defaultColumnStats.getSampleDateSize(null));
        Assertions.assertEquals("", defaultColumnStats.getSampleNullCount(null));

        MultiColumnStats multiColumnStats = new MultiColumnStats(List.of(), List.of());
        Assertions.assertEquals(0, multiColumnStats.getTypeSize());
        Assertions.assertEquals("", multiColumnStats.getQuotedColumnName());
        Assertions.assertEquals("", multiColumnStats.getMax());
        Assertions.assertEquals("", multiColumnStats.getMin());
        Assertions.assertEquals("", multiColumnStats.getCollectionSize());
        Assertions.assertEquals("", multiColumnStats.getFullDataSize());
        Assertions.assertEquals("", multiColumnStats.getFullNullCount());
        Assertions.assertEquals("", multiColumnStats.getSampleDateSize(null));
        Assertions.assertEquals("", multiColumnStats.getSampleNullCount(null));
    }
}
