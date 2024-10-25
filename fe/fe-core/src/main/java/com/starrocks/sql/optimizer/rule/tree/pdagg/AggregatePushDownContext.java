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


package com.starrocks.sql.optimizer.rule.tree.pdagg;

import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.starrocks.sql.optimizer.operator.logical.LogicalAggregationOperator;
import com.starrocks.sql.optimizer.operator.scalar.CallOperator;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;

import java.util.List;
import java.util.Map;

public class AggregatePushDownContext {
    public static final AggregatePushDownContext EMPTY = new AggregatePushDownContext();

    public LogicalAggregationOperator origAggregator;
    public final Map<ColumnRefOperator, CallOperator> aggregations;
    public final Map<ColumnRefOperator, ScalarOperator> groupBys;

    // Push-down aggregate function can be split into partial and final stage, partial stage is pushed down to
    // scan operator and final stage is pushed down to the parent operator of scan operator.
    // For equivalent aggregate function, we need to record the final stage aggregate function to replace the original
    // aggregate function.
    public final Map<CallOperator, CallOperator> aggToPartialAggMap = Maps.newHashMap();
    public final Map<CallOperator, CallOperator> aggToFinalAggMap = Maps.newHashMap();
    public final Map<CallOperator, CallOperator> aggToOrigAggMap = Maps.newHashMap();
    // Query's aggregate call operator to push down aggregate call operator mapping,
    // those two operators are not the same so record it to be used later.
    public final Map<ColumnRefOperator, CallOperator> aggColRefToPushDownAggMap = Maps.newHashMap();

    public boolean hasWindow = false;

    // record push down path
    // the index of children which should push down
    public final List<Integer> pushPaths;

    public AggregatePushDownContext() {
        origAggregator = null;
        aggregations = Maps.newHashMap();
        groupBys = Maps.newHashMap();
        pushPaths = Lists.newArrayList();
    }

    public void setAggregator(LogicalAggregationOperator aggregator) {
        this.origAggregator = aggregator;
        this.aggregations.putAll(aggregator.getAggregations());
        aggregator.getGroupingKeys().forEach(c -> groupBys.put(c, c));
        this.pushPaths.clear();
    }

    public boolean isEmpty() {
        return origAggregator == null;
    }

    /**
     * If the push-down agg has been rewritten, record its partial and final stage aggregate function.
     */
    public void registerAggRewriteInfo(CallOperator aggFunc,
                                       CallOperator partialStageAgg,
                                       CallOperator finalStageAgg) {
        aggToPartialAggMap.put(aggFunc, partialStageAgg);
        aggToFinalAggMap.put(aggFunc, finalStageAgg);
    }

    public void registerOrigAggRewriteInfo(CallOperator aggFunc,
                                       CallOperator origAgg) {
        aggToOrigAggMap.put(aggFunc, origAgg);
    }

    /**
     * Combine input ctx into current context.
     */
    public void combine(AggregatePushDownContext ctx) {
        aggToFinalAggMap.putAll(ctx.aggToFinalAggMap);
        aggColRefToPushDownAggMap.putAll(ctx.aggColRefToPushDownAggMap);
    }

    public boolean isRewrittenByEquivalent(CallOperator aggCall) {
        return aggToFinalAggMap.containsKey(aggCall);
    }
}
