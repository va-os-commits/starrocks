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

package com.starrocks.common.util;

import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.time.Instant;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

public class UUIDUtilTest {
    /**
     * Extract timestamp from a UUIDv7.
     * UUIDv7 has the timestamp in the first 48 bits
     */
    private long extractTimestampFromUuid(UUID uuid) {
        long msb = uuid.getMostSignificantBits();
        return (msb >>> 16) & 0xFFFFFFFFFFFL; // 48 bits
    }

    /**
     * Extract random component from UUIDv7 for comparison
     */
    private long extractRandomFromUuid(UUID uuid) {
        return uuid.getLeastSignificantBits();
    }

    @Test
    public void testUniqueness() {
        final int numUuids = 10000;
        Set<String> uuidStrings = new HashSet<>();

        for (int i = 0; i < numUuids; i++) {
            UUID uuid = UUIDUtil.genUUID();
            String uuidStr = uuid.toString();

            // Make sure we haven't seen this UUID before
            Assertions.assertTrue(uuidStrings.add(uuidStr),
                    "Generated duplicate UUID: " + uuidStr);
        }
    }

    @Test
    public void testMonotonicIncrease() throws InterruptedException {
        final int numUuids = 100;

        UUID prevUuid = UUIDUtil.genUUID();
        long prevTimestamp = extractTimestampFromUuid(prevUuid);

        // Sleep to ensure timestamp changes
        Thread.sleep(5);

        for (int i = 0; i < numUuids; i++) {
            UUID uuid = UUIDUtil.genUUID();
            long timestamp = extractTimestampFromUuid(uuid);

            // Timestamp should be >= previous one
            Assertions.assertTrue(timestamp >= prevTimestamp,
                    "UUID timestamp not monotonically increasing");

            prevTimestamp = timestamp;
        }
    }

    @Test
    public void testTimestampCorrelation() {

        long before = Instant.now().toEpochMilli();
        UUID uuid = UUIDUtil.genUUID();
        long after = Instant.now().toEpochMilli();

        long uuidTimestamp = extractTimestampFromUuid(uuid);

        // UUID timestamp should be between 'before' and 'after'
        Assertions.assertTrue(uuidTimestamp >= before,
                "UUID timestamp should be >= system time before generation");
        Assertions.assertTrue(uuidTimestamp <= after,
                "UUID timestamp should be <= system time after generation");
    }

    @Test
    public void testRandomComponent() {
        final int numUuids = 1000;
        List<Long> randomParts = new ArrayList<>();

        for (int i = 0; i < numUuids; i++) {
            UUID uuid = UUIDUtil.genUUID();
            randomParts.add(extractRandomFromUuid(uuid));
        }

        Set<Long> uniqueRandoms = new HashSet<>(randomParts);

        Assertions.assertEquals(randomParts.size(), uniqueRandoms.size(), "Random component is not unique enough between UUIDs");
    }

    @Test
    public void testParallelGeneration() throws InterruptedException {
        final int numThreads = 8;
        final int uuidsPerThread = 1000;
        final Set<String> allUuids = Collections.newSetFromMap(new ConcurrentHashMap<>());
        final CountDownLatch latch = new CountDownLatch(numThreads);

        ExecutorService executor = Executors.newFixedThreadPool(numThreads);

        for (int t = 0; t < numThreads; t++) {
            executor.submit(() -> {
                try {
                    for (int i = 0; i < uuidsPerThread; i++) {
                        UUID uuid = UUIDUtil.genUUID();
                        String uuidStr = uuid.toString();

                        Assertions.assertTrue(allUuids.add(uuidStr),
                                "Generated duplicate UUID in parallel: " + uuidStr);
                    }
                } finally {
                    latch.countDown();
                }
            });
        }

        latch.await();
        executor.shutdown();
        executor.awaitTermination(10, TimeUnit.SECONDS);

        Assertions.assertEquals(numThreads * uuidsPerThread, allUuids.size(), "Parallel UUID generation produced duplicates");
    }

    @Test
    public void testVersion() {
        UUID uuid = UUIDUtil.genUUID();

        int version = uuid.version();
        Assertions.assertEquals(7, version, "UUID should be version 7");

        int variant = uuid.variant();
        Assertions.assertEquals(2, variant, "UUID should have RFC 4122 variant");
    }
}
