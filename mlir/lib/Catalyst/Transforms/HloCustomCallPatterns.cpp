

// Copyright 2023 Xanadu Quantum Technologies Inc.

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define DEBUG_TYPE "scatter"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/PatternMatch.h"

#include "mhlo/IR/hlo_ops.h"

using namespace mlir;

namespace catalyst {

struct HloCustomCallOpRewritePattern : public mlir::OpRewritePattern<mhlo::CustomCallOp> {
    using mlir::OpRewritePattern<mhlo::CustomCallOp>::OpRewritePattern;

    mlir::LogicalResult matchAndRewrite(mhlo::CustomCallOp op,
                                        mlir::PatternRewriter &rewriter) const override
    {
        auto *ctx = rewriter.getContext();
        auto fnType = FunctionType::get(
            ctx, /*inputs=*/
            op.getOperandTypes(),
            /*outputs=*/op.getResultTypes());
        auto calleeName = op.getCallTargetName();
        assert(calleeName=="lapack_dgesdd" && "This custom call is currently not supported.");
        ModuleOp moduleOp = op->getParentOfType<ModuleOp>();
        auto savedPoint = rewriter.saveInsertionPoint();
        rewriter.setInsertionPointToStart(moduleOp.getBody());
        auto declaration = rewriter.create<func::FuncOp>(op.getLoc(), calleeName, fnType);
        declaration.setPrivate();
        rewriter.restoreInsertionPoint(savedPoint);
        auto operands = op.getOperands();
        TypeRange resultsType = op.getResultTypes();
        rewriter.replaceOpWithNewOp<func::CallOp>(op, declaration.getName(), resultsType, operands);
        return success();
    }
};

void populateHloCustomCallPatterns(RewritePatternSet &patterns)
{
    patterns.add<catalyst::HloCustomCallOpRewritePattern>(patterns.getContext(), 1);
}

} // namespace catalyst
