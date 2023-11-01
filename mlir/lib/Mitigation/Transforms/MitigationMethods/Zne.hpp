// Copyright 2022-2023 Xanadu Quantum Technologies Inc.

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "Mitigation/IR/MitigationOps.h"
#include "mlir/IR/PatternMatch.h"

using namespace mlir;

namespace catalyst {
namespace mitigation {

struct ZneLowering : public OpRewritePattern<mitigation::ZneOp> {
    using OpRewritePattern<mitigation::ZneOp>::OpRewritePattern;

    LogicalResult match(mitigation::ZneOp op) const override;
    void rewrite(mitigation::ZneOp op, PatternRewriter &rewriter) const override;

  private:
    static FlatSymbolRefAttr getOrInsertFoldedCircuit(Location loc, PatternRewriter &builder, mitigation::ZneOp op, Type scalarType);
    static void exploreTreeAndStoreLeafValues(Operation* op, std::vector<ValueRange>& leafValues);
    static void removeQuantumMeasurements(Block &block);
};

} // namespace mitigation
} // namespace catalyst
