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


package com.starrocks.lake.compaction;

import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class ScoreSorterTest {

    @Test
    public void test() {
        List<PartitionStatisticsSnapshot> statisticsList = new ArrayList<>();
        PartitionStatistics statistics = new PartitionStatistics(new PartitionIdentifier(1, 2, 3));
        statistics.setCompactionScore(Quantiles.compute(Arrays.asList(0.0, 0.0, 0.0)));
        statisticsList.add(new PartitionStatisticsSnapshot(statistics));

        statistics = new PartitionStatistics(new PartitionIdentifier(1, 2, 6));
        statistics.setCompactionScore(Quantiles.compute(Arrays.asList(1.1, 1.1, 1.2)));
        statisticsList.add(new PartitionStatisticsSnapshot(statistics));

        statistics = new PartitionStatistics(new PartitionIdentifier(1, 2, 4));
        statistics.setCompactionScore(Quantiles.compute(Arrays.asList(0.99, 0.99, 0.99)));
        statisticsList.add(new PartitionStatisticsSnapshot(statistics));

        statistics = new PartitionStatistics(new PartitionIdentifier(1, 2, 5));
        statistics.setCompactionScore(Quantiles.compute(Arrays.asList(1.0, 1.0)));
        statisticsList.add(new PartitionStatisticsSnapshot(statistics));

        ScoreSorter sorter = new ScoreSorter();

        List<PartitionStatisticsSnapshot> sortedList = sorter.sort(statisticsList);
        Assertions.assertEquals(4, sortedList.size());
        Assertions.assertEquals(6, sortedList.get(0).getPartition().getPartitionId());
        Assertions.assertEquals(5, sortedList.get(1).getPartition().getPartitionId());
        Assertions.assertEquals(4, sortedList.get(2).getPartition().getPartitionId());
        Assertions.assertEquals(3, sortedList.get(3).getPartition().getPartitionId());
    }

    @Test
    public void testPriority() {
        // no priority
        {
            List<PartitionStatisticsSnapshot> statisticsList = new ArrayList<>();
            PartitionStatistics statistics = new PartitionStatistics(new PartitionIdentifier(1, 2, 3));
            statistics.setCompactionScore(Quantiles.compute(Arrays.asList(0.0, 0.0, 0.0)));
            statisticsList.add(new PartitionStatisticsSnapshot(statistics));

            statistics = new PartitionStatistics(new PartitionIdentifier(1, 2, 4));
            statistics.setCompactionScore(Quantiles.compute(Arrays.asList(1.1, 1.1, 1.2)));
            statisticsList.add(new PartitionStatisticsSnapshot(statistics));

            ScoreSorter sorter = new ScoreSorter();
            List<PartitionStatisticsSnapshot> sortedList = sorter.sort(statisticsList);
            Assertions.assertEquals(4, sortedList.get(0).getPartition().getPartitionId());
            Assertions.assertEquals(3, sortedList.get(1).getPartition().getPartitionId());
        }

        // with priority
        {
            List<PartitionStatisticsSnapshot> statisticsList = new ArrayList<>();
            PartitionStatistics statistics = new PartitionStatistics(new PartitionIdentifier(1, 2, 3));
            statistics.setCompactionScore(Quantiles.compute(Arrays.asList(0.0, 0.0, 0.0)));
            statistics.setPriority(PartitionStatistics.CompactionPriority.MANUAL_COMPACT);
            statisticsList.add(new PartitionStatisticsSnapshot(statistics));

            statistics = new PartitionStatistics(new PartitionIdentifier(1, 2, 4));
            statistics.setCompactionScore(Quantiles.compute(Arrays.asList(1.1, 1.1, 1.2)));
            statisticsList.add(new PartitionStatisticsSnapshot(statistics));

            ScoreSorter sorter = new ScoreSorter();
            List<PartitionStatisticsSnapshot> sortedList = sorter.sort(statisticsList);
            Assertions.assertEquals(3, sortedList.get(0).getPartition().getPartitionId());
            Assertions.assertEquals(4, sortedList.get(1).getPartition().getPartitionId());
        }

        // same priority, should sort by compaction score
        {
            List<PartitionStatisticsSnapshot> statisticsList = new ArrayList<>();
            PartitionStatistics statistics = new PartitionStatistics(new PartitionIdentifier(1, 2, 3));
            statistics.setCompactionScore(Quantiles.compute(Arrays.asList(0.0, 0.0, 0.0)));
            statistics.setPriority(PartitionStatistics.CompactionPriority.MANUAL_COMPACT);
            statisticsList.add(new PartitionStatisticsSnapshot(statistics));

            statistics = new PartitionStatistics(new PartitionIdentifier(1, 2, 4));
            statistics.setCompactionScore(Quantiles.compute(Arrays.asList(1.1, 1.1, 1.2)));
            statistics.setPriority(PartitionStatistics.CompactionPriority.MANUAL_COMPACT);
            statisticsList.add(new PartitionStatisticsSnapshot(statistics));

            ScoreSorter sorter = new ScoreSorter();
            List<PartitionStatisticsSnapshot> sortedList = sorter.sort(statisticsList);
            Assertions.assertEquals(4, sortedList.get(0).getPartition().getPartitionId());
            Assertions.assertEquals(3, sortedList.get(1).getPartition().getPartitionId());
        }
    }
}
