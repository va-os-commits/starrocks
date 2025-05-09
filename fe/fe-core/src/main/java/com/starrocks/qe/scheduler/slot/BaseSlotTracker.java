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

package com.starrocks.qe.scheduler.slot;

import com.starrocks.metric.MetricRepo;
import com.starrocks.qe.GlobalVariable;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.server.WarehouseManager;
import com.starrocks.thrift.TUniqueId;
import com.starrocks.warehouse.Warehouse;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.ArrayList;
import java.util.Collection;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;

/**
 * SlotTracker is used to track the status of slots.
 * It is responsible for managing the life cycle of slots, including requiring, allocating, and releasing slots.
 *
 * <p> All the methods except {@link #getSlots()} are not thread-safe.
 */
public abstract class BaseSlotTracker {
    private static final Logger LOG = LogManager.getLogger(BaseSlotTracker.class);

    protected final ConcurrentMap<TUniqueId, LogicalSlot> slots = new ConcurrentHashMap<>();
    protected final Set<LogicalSlot> slotsOrderByExpiredTime = new TreeSet<>(
            Comparator.comparingLong(LogicalSlot::getExpiredPendingTimeMs)
                    .thenComparing(LogicalSlot::getSlotId));

    protected final Map<TUniqueId, LogicalSlot> pendingSlots = new HashMap<>();
    protected final Map<TUniqueId, LogicalSlot> allocatedSlots = new HashMap<>();
    protected final ResourceUsageMonitor resourceUsageMonitor;
    protected final long warehouseId;

    protected int numAllocatedSlots = 0;
    protected Optional<String> warehouseName = Optional.empty();

    protected List<BaseSlotTracker.Listener> listeners;
    protected SlotSelectionStrategy slotSelectionStrategy;

    public BaseSlotTracker(ResourceUsageMonitor resourceUsageMonitor, long warehouseId) {
        this.resourceUsageMonitor = resourceUsageMonitor;
        this.warehouseId = warehouseId;
    }

    public String getWarehouseName() {
        try {
            if (warehouseName.isEmpty()) {
                WarehouseManager warehouseManager = GlobalStateMgr.getCurrentState().getWarehouseMgr();
                Warehouse warehouse = warehouseManager.getWarehouse(this.warehouseId);
                this.warehouseName = Optional.of(warehouse.getName());
            }
            return warehouseName.orElse("");
        } catch (Exception e) {
            LOG.warn("Failed to get warehouse name for warehouseId: {}", warehouseId, e);
            return "";
        }
    }

    public long getQueuePendingLength() {
        return pendingSlots.size();
    }

    public long getAllocatedLength() {
        return allocatedSlots.size();
    }

    public Optional<Integer> getMaxRequiredSlots() {
        return pendingSlots.values().stream()
                .map(LogicalSlot::getNumPhysicalSlots)
                .max(Integer::compareTo);
    }

    public Optional<Integer> getSumRequiredSlots() {
        return pendingSlots.values().stream()
                .map(LogicalSlot::getNumPhysicalSlots)
                .reduce(Integer::sum);
    }


    public Optional<Integer> getRemainSlots() {
        return getMaxSlots().map(s -> Math.max(0, s - numAllocatedSlots));
    }

    public long getMaxQueueQueueLength() {
        final BaseSlotManager slotManager = GlobalStateMgr.getCurrentState().getSlotManager();
        return slotManager.getQueryQueueMaxQueuedQueries(warehouseId);
    }

    public long getMaxQueuePendingTimeSecond() {
        final BaseSlotManager slotManager = GlobalStateMgr.getCurrentState().getSlotManager();
        return slotManager.getQueryQueuePendingTimeoutSecond(warehouseId);
    }

    /**
     * Return the max slots of the slot tracker if query queuen is supported.
     */
    public abstract Optional<Integer> getMaxSlots();

    /**
     * Add a slot requirement.
     * @param slot The required slot.
     * @return True if the slot is required successfully or already required , false if the query queue is full.
     */
    public boolean requireSlot(LogicalSlot slot) {
        final BaseSlotManager slotManager = GlobalStateMgr.getCurrentState().getSlotManager();
        if (GlobalVariable.isQueryQueueMaxQueuedQueriesEffective() &&
                pendingSlots.size() >= slotManager.getQueryQueueMaxQueuedQueries(slot.getWarehouseId())) {
            return false;
        }

        if (slots.containsKey(slot.getSlotId())) {
            return true;
        }

        slots.put(slot.getSlotId(), slot);
        slotsOrderByExpiredTime.add(slot);
        pendingSlots.put(slot.getSlotId(), slot);

        MetricRepo.COUNTER_QUERY_QUEUE_SLOT_PENDING.increase((long) slot.getNumPhysicalSlots());

        listeners.forEach(listener -> listener.onRequireSlot(slot));
        slot.onRequire();

        return true;
    }

    /**
     * Allocate a slot which has already been required.
     * If the slot has not been required, this method has no effect.
     * @param slot The slot to be allocated.
     */
    public void allocateSlot(LogicalSlot slot) {
        TUniqueId slotId = slot.getSlotId();
        if (!slots.containsKey(slotId)) {
            return;
        }

        if (pendingSlots.remove(slotId) == null) {
            return;
        }
        MetricRepo.COUNTER_QUERY_QUEUE_SLOT_PENDING.increase((long) -slot.getNumPhysicalSlots());

        if (allocatedSlots.put(slotId, slot) != null) {
            return;
        }

        MetricRepo.COUNTER_QUERY_QUEUE_SLOT_RUNNING.increase((long) slot.getNumPhysicalSlots());
        numAllocatedSlots += slot.getNumPhysicalSlots();

        listeners.forEach(listener -> listener.onAllocateSlot(slot));
        slot.onAllocate();
    }

    /**
     * Release a slot which has already been allocated or required.
     * @param slotId The slot id to be released.
     * @return The released slot, or null if the slot has not been required or allocated.
     */
    public LogicalSlot releaseSlot(TUniqueId slotId) {
        LogicalSlot slot = slots.remove(slotId);
        if (slot == null) {
            return null;
        }

        slotsOrderByExpiredTime.remove(slot);

        if (allocatedSlots.remove(slotId) != null) {
            numAllocatedSlots -= slot.getNumPhysicalSlots();
            MetricRepo.COUNTER_QUERY_QUEUE_SLOT_RUNNING.increase((long) -slot.getNumPhysicalSlots());
        } else {
            if (pendingSlots.remove(slotId) != null) {
                MetricRepo.COUNTER_QUERY_QUEUE_SLOT_PENDING.increase((long) -slot.getNumPhysicalSlots());
            }
        }

        listeners.forEach(listener -> listener.onReleaseSlot(slot));
        slot.onRelease();

        return slot;
    }

    /**
     * Peak the slots to be allocated from slot manager.
     */
    public Collection<LogicalSlot> peakSlotsToAllocate() {
        return slotSelectionStrategy.peakSlotsToAllocate(this);
    }

    /**
     * Peak all the expired slots.
     *
     * <p> Note that this method does not remove the expired slots from the tracker,
     * and {@link #releaseSlot} should be called to release these slots after peaking.
     * @return The expired slots.
     */
    public List<LogicalSlot> peakExpiredSlots() {
        final long nowMs = System.currentTimeMillis();
        List<LogicalSlot> expiredSlots = new ArrayList<>();
        for (LogicalSlot slot : slotsOrderByExpiredTime) {
            if (!slot.isAllocatedExpired(nowMs)) {
                break;
            }
            expiredSlots.add(slot);
        }
        return expiredSlots;
    }

    public long getMinExpiredTimeMs() {
        if (slotsOrderByExpiredTime.isEmpty()) {
            return 0;
        }
        return slotsOrderByExpiredTime.iterator().next().getExpiredPendingTimeMs();
    }

    public double getEarliestQueryWaitTimeSecond() {
        return slots.values().stream().map(LogicalSlot::getStartTimeMs).min(Long::compareTo)
                .map(t -> (System.currentTimeMillis() - t) / 1000.0).orElse(0.0);
    }

    public Collection<LogicalSlot> getSlots() {
        return slots.values();
    }

    public LogicalSlot getSlot(TUniqueId slotId) {
        return slots.get(slotId);
    }

    public int getNumAllocatedSlots() {
        return numAllocatedSlots;
    }

    public long getWarehouseId() {
        return warehouseId;
    }

    public interface Listener {
        void onRequireSlot(LogicalSlot slot);

        void onAllocateSlot(LogicalSlot slot);

        void onReleaseSlot(LogicalSlot slot);
    }

    public static class SlotListenerForPipelineDriverAllocator implements BaseSlotTracker.Listener {
        private final PipelineDriverAllocator pipelineDriverAllocator = new PipelineDriverAllocator();

        @Override
        public void onRequireSlot(LogicalSlot slot) {
            // Do nothing.
        }

        @Override
        public void onAllocateSlot(LogicalSlot slot) {
            pipelineDriverAllocator.allocate(slot);
        }

        @Override
        public void onReleaseSlot(LogicalSlot slot) {
            pipelineDriverAllocator.release(slot);
        }
    }
}
