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

#include "iostream"
#include "llvm/Support/raw_ostream.h"

#include <deque>
#include <string>
#include <vector>

#include "mlir/Conversion/LLVMCommon/MemRefBuilder.h"
#include "mlir/Conversion/LLVMCommon/Pattern.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Bufferization/Transforms/Bufferize.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Index/IR/IndexOps.h"
#include "mlir/Dialect/LLVMIR/FunctionCallUtils.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/IRMapping.h"

#include "Gradient/IR/GradientOps.h"
#include "Gradient/Transforms/Patterns.h"
#include "Gradient/Utils/CompDiffArgIndices.h"
#include "Gradient/Utils/GradientShape.h"
#include "Quantum/IR/QuantumOps.h"
#include "Quantum/Utils/RemoveQuantumMeasurements.h"

using namespace mlir;
using namespace catalyst::gradient;

using llvm::errs;

namespace {

constexpr int64_t UNKNOWN = ShapedType::kDynamic;

LLVM::LLVMFuncOp ensureFunctionDeclaration(PatternRewriter &rewriter, Operation *op,
                                           StringRef fnSymbol, Type fnType)
{
    Operation *fnDecl = SymbolTable::lookupNearestSymbolFrom(op, rewriter.getStringAttr(fnSymbol));

    if (!fnDecl) {
        PatternRewriter::InsertionGuard insertGuard(rewriter);
        ModuleOp mod = op->getParentOfType<ModuleOp>();
        rewriter.setInsertionPointToStart(mod.getBody());

        fnDecl = rewriter.create<LLVM::LLVMFuncOp>(op->getLoc(), fnSymbol, fnType);
    }
    else {
        assert(isa<LLVM::LLVMFuncOp>(fnDecl) && "QIR function declaration is not a LLVMFuncOp");
    }

    return cast<LLVM::LLVMFuncOp>(fnDecl);
}

struct AdjointOpPattern : public ConvertOpToLLVMPattern<AdjointOp> {
    using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

    LogicalResult matchAndRewrite(AdjointOp op, AdjointOpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override
    {
        Location loc = op.getLoc();
        MLIRContext *ctx = getContext();
        TypeConverter *conv = getTypeConverter();

        Type vectorType = conv->convertType(MemRefType::get({UNKNOWN}, Float64Type::get(ctx)));

        for (Type type : op.getResultTypes()) {
            if (!type.isa<MemRefType>())
                return op.emitOpError("must be bufferized before lowering");

            // Currently only expval gradients are supported by the runtime,
            // leading to tensor<?xf64> return values.
            if (type.dyn_cast<MemRefType>() != MemRefType::get({UNKNOWN}, Float64Type::get(ctx)))
                return op.emitOpError("adjoint can only return MemRef<?xf64> or tuple thereof");
        }

        // The callee of the adjoint op must return as a single result the quantum register.
        func::FuncOp callee =
            SymbolTable::lookupNearestSymbolFrom<func::FuncOp>(op, op.getCalleeAttr());
        assert(callee && callee.getNumResults() == 1 && "invalid qfunc symbol in adjoint op");

        StringRef cacheFnName = "__quantum__rt__toggle_recorder";
        StringRef gradFnName = "__quantum__qis__Gradient";
        Type cacheFnSignature =
            LLVM::LLVMFunctionType::get(LLVM::LLVMVoidType::get(ctx), IntegerType::get(ctx, 1));
        Type gradFnSignature = LLVM::LLVMFunctionType::get(
            LLVM::LLVMVoidType::get(ctx), IntegerType::get(ctx, 64), /*isVarArg=*/true);

        LLVM::LLVMFuncOp cacheFnDecl =
            ensureFunctionDeclaration(rewriter, op, cacheFnName, cacheFnSignature);
        LLVM::LLVMFuncOp gradFnDecl =
            ensureFunctionDeclaration(rewriter, op, gradFnName, gradFnSignature);

        // Run the forward pass and cache the circuit.
        Value c_true = rewriter.create<LLVM::ConstantOp>(
            loc, rewriter.getIntegerAttr(IntegerType::get(ctx, 1), 1));
        Value c_false = rewriter.create<LLVM::ConstantOp>(
            loc, rewriter.getIntegerAttr(IntegerType::get(ctx, 1), 0));
        rewriter.create<LLVM::CallOp>(loc, cacheFnDecl, c_true);
        Value qreg = rewriter.create<func::CallOp>(loc, callee, op.getArgs()).getResult(0);
        if (!qreg.getType().isa<catalyst::quantum::QuregType>())
            return callee.emitOpError("qfunc must return quantum register");
        rewriter.create<LLVM::CallOp>(loc, cacheFnDecl, c_false);

        // We follow the C ABI convention of passing result memrefs as struct pointers in the
        // arguments to the C function, although in this case as a variadic argument list to allow
        // for a varying number of results in a single signature.
        Value c1 = rewriter.create<LLVM::ConstantOp>(loc, rewriter.getI64IntegerAttr(1));
        Value numResults = rewriter.create<LLVM::ConstantOp>(
            loc, rewriter.getI64IntegerAttr(op.getDataIn().size()));
        SmallVector<Value> args = {numResults};
        for (Value memref : adaptor.getDataIn()) {
            Value newArg =
                rewriter.create<LLVM::AllocaOp>(loc, LLVM::LLVMPointerType::get(vectorType), c1);
            rewriter.create<LLVM::StoreOp>(loc, memref, newArg);
            args.push_back(newArg);
        }

        rewriter.create<LLVM::CallOp>(loc, gradFnDecl, args);
        rewriter.create<catalyst::quantum::DeallocOp>(loc, qreg);
        rewriter.eraseOp(op);

        return success();
    }
};

/// Options that configure preprocessing done on MemRefs before being passed to Enzyme.
struct EnzymeMemRefInterfaceOptions {
    /// Fill memref with zero values
    bool zeroOut = false;
    /// Mark memref as dupnoneed, allowing Enzyme to avoid computing its primal value.
    bool dupNoNeed = false;
};

static constexpr const char *enzyme_autodiff_func_name = "__enzyme_autodiff";
static constexpr const char *enzyme_allocation_key = "__enzyme_allocation_like";
static constexpr const char *enzyme_custom_gradient_key = "__enzyme_register_gradient_";
static constexpr const char *enzyme_const_key = "enzyme_const";
static constexpr const char *enzyme_dupnoneed_key = "enzyme_dupnoneed";

/// Convert every MemRef-typed return value in callee to writing to a new argument in
/// destination-passing style.
void convertToDestinationPassingStyle(func::FuncOp callee)
{
    MLIRContext *ctx = callee.getContext();
    if (callee.getNumResults() == 0) {
        // Callee is already in destination-passing style
        return;
    }

    SmallVector<Value> memRefReturns;
    SmallVector<unsigned> outputIndices;
    SmallVector<Type> nonMemRefReturns;
    callee.walk([&](func::ReturnOp returnOp) {
        // This is the first return op we've seen.
        if (memRefReturns.empty()) {
            for (const auto &[idx, operand] : llvm::enumerate(returnOp.getOperands())) {
                if (isa<MemRefType>(operand.getType())) {
                    memRefReturns.push_back(operand);
                    outputIndices.push_back(idx);
                }
                else {
                    nonMemRefReturns.push_back(operand.getType());
                }
            }
            return WalkResult::interrupt();
        }
        return WalkResult::advance();
    });

    // Insert the new output arguments to the function.
    unsigned dpsOutputIdx = callee.getNumArguments();
    SmallVector<unsigned> argIndices(/*size=*/memRefReturns.size(),
                                     /*values=*/dpsOutputIdx);
    SmallVector<Type> memRefTypes{memRefReturns.size()};
    SmallVector<DictionaryAttr> argAttrs{memRefReturns.size()};
    SmallVector<Location> argLocs{memRefReturns.size(), UnknownLoc::get(ctx)};

    llvm::transform(memRefReturns, memRefTypes.begin(),
                    [](Value memRef) { return memRef.getType(); });
    llvm::transform(memRefReturns, argLocs.begin(), [](Value memRef) { return memRef.getLoc(); });

    callee.insertArguments(argIndices, memRefTypes, argAttrs, argLocs);
    callee.setFunctionType(FunctionType::get(ctx, callee.getArgumentTypes(), nonMemRefReturns));

    // Update the old MemRefs to be replaced with the output argument. Many allocations will be
    // able to be trivially canonicalized away.
    callee.walk([&](func::ReturnOp returnOp) {
        SmallVector<Value> nonMemRefReturns;
        size_t idx = 0;
        for (Value operand : returnOp.getOperands()) {
            if (isa<MemRefType>(operand.getType())) {
                operand.replaceAllUsesWith(callee.getArgument(idx + dpsOutputIdx));
                idx++;
            }
            else {
                nonMemRefReturns.push_back(operand);
            }
        }
        returnOp.getOperandsMutable().assign(nonMemRefReturns);
    });
}

LogicalResult traverseCallGraph(func::FuncOp start, SymbolTableCollection &symbolTable,
                                function_ref<LogicalResult(func::FuncOp)> processCallable)
{
    DenseSet<Operation *> visited{start};
    std::deque<Operation *> frontier{start};

    while (!frontier.empty()) {
        auto callable = cast<func::FuncOp>(frontier.front());
        frontier.pop_front();

        if (failed(processCallable(callable))) {
            return failure();
        }

        callable.walk([&](CallOpInterface callOp) {
            if (auto nextFunc = dyn_cast<func::FuncOp>(callOp.resolveCallable(&symbolTable))) {
                if (!visited.contains(nextFunc)) {
                    visited.insert(nextFunc);
                    frontier.push_back(nextFunc);
                }
            }
        });
    }
    return success();
}

struct BackpropOpPattern : public ConvertOpToLLVMPattern<BackpropOp> {
    using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

    LogicalResult matchAndRewrite(BackpropOp op, BackpropOpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override
    {
        Location loc = op.getLoc();
        MLIRContext *ctx = getContext();
        ModuleOp moduleOp = op->getParentOfType<ModuleOp>();

        for (Type type : op.getResultTypes()) {
            if (!type.isa<MemRefType>())
                return op.emitOpError("must be bufferized before lowering");
        }

        // The callee of the backprop Op
        func::FuncOp callee =
            SymbolTable::lookupNearestSymbolFrom<func::FuncOp>(op, op.getCalleeAttr());
        assert(callee && "Expected a valid callee of type func.func");

        convertToDestinationPassingStyle(callee);
        SymbolTableCollection symbolTable;
        LogicalResult traversalResult =
            traverseCallGraph(callee, symbolTable, [&](func::FuncOp func) {
                // Convert the function and all of its callers to destination passing style.
                if (llvm::any_of(func.getResultTypes(),
                                 [](Type resultType) { return isa<MemRefType>(resultType); })) {
                    convertToDestinationPassingStyle(func);

                    // Update all callees of this function to pass in output memrefs
                    std::optional<SymbolTable::UseRange> symbolUses = func.getSymbolUses(moduleOp);
                    for (auto symbolUse : *symbolUses) {
                        if (auto callOp = dyn_cast<func::CallOp>(symbolUse.getUser())) {
                            OpBuilder::InsertionGuard insertionGuard(rewriter);
                            rewriter.setInsertionPoint(callOp);

                            SmallVector<Value> newOperands{callOp.getArgOperands()};
                            SmallVector<Value> outputOperands;

                            // Hardcode this for now, it's gotta be a postorder traversal such that
                            // we visit child nodes before their parents.
                            if (callOp.getCallee() == "workflow.withparams") {
                                callOp.emitWarning() << "Using hard-coded DPS transformation";
                                outputOperands.push_back(
                                    callOp->getParentOfType<func::FuncOp>().getArguments().back());
                            }
                            else {
                                for (Type resultType : callOp.getResultTypes()) {
                                    if (auto memRefType = dyn_cast<MemRefType>(resultType)) {
                                        assert(memRefType.hasStaticShape() &&
                                               "Cannot convert a dynamically-sized memref to "
                                               "destination-passing style");
                                        outputOperands.push_back(rewriter.create<memref::AllocOp>(
                                            callOp.getLoc(), memRefType));
                                    }
                                }
                            }

                            newOperands.append(outputOperands);
                            rewriter.create<func::CallOp>(callOp.getLoc(), func, newOperands);
                            rewriter.replaceOp(callOp, outputOperands);
                        }
                    }
                }
                // Register custom gradients of quantum functions
                if (func->hasAttrOfType<FlatSymbolRefAttr>("gradient.qgrad")) {
                    auto qgradFn = SymbolTable::lookupNearestSymbolFrom<func::FuncOp>(
                        func, func->getAttrOfType<FlatSymbolRefAttr>("gradient.qgrad"));
                    auto augFwd = genAugmentedForward(func, rewriter);
                    if (failed(augFwd)) {
                        return failure();
                    }
                    auto customQGrad = genCustomQGradient(func, func.getLoc(), qgradFn, rewriter);
                    if (failed(customQGrad)) {
                        return failure();
                    }
                    insertEnzymeCustomGradient(rewriter, func->getParentOfType<ModuleOp>(),
                                               func.getLoc(), func, *augFwd, *customQGrad);
                }

                return success();
            });
        if (failed(traversalResult)) {
            return failure();
        }

        LowerToLLVMOptions options = getTypeConverter()->getOptions();
        if (options.useGenericFunctions) {
            LLVM::LLVMFuncOp allocFn = LLVM::lookupOrCreateGenericAllocFn(
                moduleOp, getTypeConverter()->getIndexType(), options.useOpaquePointers);
            LLVM::LLVMFuncOp freeFn =
                LLVM::lookupOrCreateGenericFreeFn(moduleOp, options.useOpaquePointers);

            // Register the previous functions as llvm globals (for Enzyme)
            // With the following piece of metadata, shadow memory is allocated with
            // _mlir_memref_to_llvm_alloc and shadow memory is freed with
            // _mlir_memref_to_llvm_free.
            insertEnzymeAllocationLike(rewriter, op->getParentOfType<ModuleOp>(), op.getLoc(),
                                       allocFn.getName(), freeFn.getName());

            // Register free
            // With the following piece of metadata, _mlir_memref_to_llvm_free's semantics are
            // stated to be equivalent to free.
            insertFunctionName(rewriter, op, "freename", StringRef("free", 5));
            insertEnzymeFunctionLike(rewriter, op, "__enzyme_function_like_free", "freename",
                                     freeFn.getName());
        }

        // Create the Enzyme function
        Type backpropFnSignature =
            LLVM::LLVMFunctionType::get(LLVM::LLVMVoidType::get(ctx), {}, /*isVarArg=*/true);

        LLVM::LLVMFuncOp backpropFnDecl =
            ensureFunctionDeclaration(rewriter, op, enzyme_autodiff_func_name, backpropFnSignature);

        // The first argument to Enzyme is a function pointer of the function to be differentiated
        Value calleePtr =
            rewriter.create<func::ConstantOp>(loc, callee.getFunctionType(), callee.getName());
        calleePtr = castToConvertedType(calleePtr, rewriter, loc);
        SmallVector<Value> callArgs = {calleePtr};

        std::vector<size_t> diffArgIndices = catalyst::compDiffArgIndices(op.getDiffArgIndices());
        getOrInsertEnzymeGlobal(rewriter, moduleOp, enzyme_const_key);
        getOrInsertEnzymeGlobal(rewriter, moduleOp, enzyme_dupnoneed_key);

        int index = 0;
        Value enzymeConst = rewriter.create<LLVM::AddressOfOp>(loc, LLVM::LLVMPointerType::get(ctx),
                                                               enzyme_const_key);

        // Add the arguments and their appropriate shadows
        for (Value arg : op.getArgs()) {
            std::vector<size_t>::iterator it =
                std::find(diffArgIndices.begin(), diffArgIndices.end(), index);
            if (it == diffArgIndices.end()) {
                if (isa<MemRefType>(arg.getType())) {
                    // unpackMemRef will handle the appropriate enzyme_const annotations
                    unpackMemRef(arg, /*shadow=*/nullptr, callArgs, rewriter, loc);
                }
                else {
                    callArgs.push_back(enzymeConst);
                    callArgs.push_back(castToConvertedType(arg, rewriter, loc));
                }
            }
            else {
                size_t position = std::distance(diffArgIndices.begin(), it);
                unpackMemRef(arg, op.getArgShadows()[position], callArgs, rewriter, loc,
                             {.zeroOut = true});
            }
            index++;
        }

        for (const auto &[outSpace, outShadow] : llvm::zip(op.getOutputs(), op.getOutShadows())) {
            unpackMemRef(outSpace, outShadow, callArgs, rewriter, loc, {.dupNoNeed = true});
        }

        // The results of backprop are in arg_shadows
        rewriter.create<LLVM::CallOp>(loc, backpropFnDecl, callArgs);
        rewriter.eraseOp(op);
        return success();
    }

  private:
    static FlatSymbolRefAttr getOrInsertEnzymeGlobal(OpBuilder &builder, ModuleOp moduleOp,
                                                     const char *globalName)
    {
        // Copyright (C) 2023 - Jacob Mai Peng
        // https://github.com/pengmai/lagrad/blob/main/lib/LAGrad/LowerToLLVM.cpp
        auto *context = moduleOp.getContext();
        if (moduleOp.lookupSymbol<LLVM::GlobalOp>(globalName)) {
            return SymbolRefAttr::get(context, globalName);
        }

        OpBuilder::InsertionGuard insertGuard(builder);
        builder.setInsertionPointToStart(moduleOp.getBody());
        auto shortTy = IntegerType::get(context, 8);
        builder.create<LLVM::GlobalOp>(moduleOp.getLoc(), shortTy,
                                       /*isConstant=*/true, LLVM::Linkage::Linkonce, globalName,
                                       IntegerAttr::get(shortTy, 0));
        return SymbolRefAttr::get(context, globalName);
    }

    Value castToConvertedType(Value value, OpBuilder &builder, Location loc) const
    {
        auto casted = builder.create<UnrealizedConversionCastOp>(
            loc, getTypeConverter()->convertType(value.getType()), value);
        return casted.getResult(0);
    }

    void unpackMemRef(Value memRefArg, Value shadowMemRef, SmallVectorImpl<Value> &callArgs,
                      OpBuilder &builder, Location loc,
                      EnzymeMemRefInterfaceOptions options = EnzymeMemRefInterfaceOptions()) const

    {
        auto llvmPtrType = LLVM::LLVMPointerType::get(builder.getContext());
        auto memRefType = cast<MemRefType>(memRefArg.getType());
        Value enzymeConst = builder.create<LLVM::AddressOfOp>(loc, llvmPtrType, enzyme_const_key);
        Value enzymeDupNoNeed =
            builder.create<LLVM::AddressOfOp>(loc, llvmPtrType, enzyme_dupnoneed_key);
        Value argStruct = castToConvertedType(memRefArg, builder, loc);
        MemRefDescriptor desc(argStruct);

        // Allocated pointer is always constant
        callArgs.push_back(enzymeConst);
        callArgs.push_back(desc.allocatedPtr(builder, loc));

        // Aligned pointer is active if a shadow is provided
        if (shadowMemRef) {
            if (options.dupNoNeed) {
                callArgs.push_back(enzymeDupNoNeed);
            }
            callArgs.push_back(desc.alignedPtr(builder, loc));
            Value shadowStruct = castToConvertedType(shadowMemRef, builder, loc);
            MemRefDescriptor shadowDesc(shadowStruct);
            Value shadowPtr = shadowDesc.alignedPtr(builder, loc);

            if (options.zeroOut) {
                Value bufferSizeBytes =
                    computeMemRefSizeInBytes(memRefType, shadowDesc, builder, loc);
                Value zero = builder.create<LLVM::ConstantOp>(loc, builder.getI8Type(), 0);
                builder.create<LLVM::MemsetOp>(loc, shadowPtr, zero, bufferSizeBytes,
                                               /*isVolatile=*/false);
            }
            callArgs.push_back(shadowPtr);
        }
        else {
            callArgs.push_back(enzymeConst);
            callArgs.push_back(desc.alignedPtr(builder, loc));
        }

        // Offsets, sizes, and strides
        callArgs.push_back(desc.offset(builder, loc));
        for (int64_t dim = 0; dim < memRefType.getRank(); dim++) {
            callArgs.push_back(desc.size(builder, loc, dim));
        }
        for (int64_t dim = 0; dim < memRefType.getRank(); dim++) {
            callArgs.push_back(desc.stride(builder, loc, dim));
        }
    }

    Value computeMemRefSizeInBytes(MemRefType type, MemRefDescriptor descriptor, OpBuilder &builder,
                                   Location loc) const
    {
        // element_size * (offset + sizes[0] * strides[0])
        Value bufferSize;
        Type indexType = getTypeConverter()->getIndexType();
        if (type.getRank() == 0) {
            bufferSize = builder.create<LLVM::ConstantOp>(loc, indexType, builder.getIndexAttr(1));
        }
        else {
            bufferSize = builder.create<LLVM::MulOp>(loc, descriptor.size(builder, loc, 0),
                                                     descriptor.stride(builder, loc, 0));
            bufferSize =
                builder.create<LLVM::AddOp>(loc, descriptor.offset(builder, loc), bufferSize);
        }
        Value elementByteSize = builder.create<LLVM::ConstantOp>(
            loc, indexType, builder.getIndexAttr(type.getElementTypeBitWidth() / 8));
        Value bufferSizeBytes = builder.create<LLVM::MulOp>(loc, elementByteSize, bufferSize);
        return bufferSizeBytes;
    }

    LogicalResult convertCustomGradArgumentTypes(TypeRange memRefArgTypes,
                                                 SmallVectorImpl<Type> &llvmArgTypes) const
    {
        for (Type argType : memRefArgTypes) {
            if (isa<MemRefType>(argType)) {
                SmallVector<Type> unpackedTypes;
                if (failed(
                        structFuncArgTypeConverter(*getTypeConverter(), argType, unpackedTypes))) {
                    return failure();
                }

                for (Type unpackedType : unpackedTypes) {
                    llvmArgTypes.append(2, unpackedType);
                }
            }
            else {
                llvmArgTypes.append(2, argType);
            }
        }
        return success();
    }

    func::FuncOp getPrintI64(ModuleOp moduleOp, OpBuilder &builder) const
    {
        if (auto printFn = moduleOp.lookupSymbol("printI64")) {
            return cast<func::FuncOp>(printFn);
        }
        OpBuilder::InsertionGuard insertionGuard(builder);
        builder.setInsertionPointToStart(moduleOp.getBody());
        auto printFn = builder.create<func::FuncOp>(
            moduleOp.getLoc(), "printI64",
            FunctionType::get(builder.getContext(), builder.getI64Type(), {}));
        printFn.setPrivate();
        return printFn;
    }

    void printInt(Operation *op, OpBuilder &builder, int64_t val) const
    {
        auto fn = getPrintI64(op->getParentOfType<ModuleOp>(), builder);
        Value constant =
            builder.create<arith::ConstantIntOp>(op->getLoc(), val, builder.getI64Type());
        builder.create<func::CallOp>(op->getLoc(), fn, constant);
    }

    FailureOr<func::FuncOp> genAugmentedForward(func::FuncOp qnode, OpBuilder &builder) const
    {
        assert(qnode.getNumResults() == 0 && "Expected QNode to be in destination-passing style");
        MLIRContext *ctx = builder.getContext();
        std::string augmentedName = (qnode.getName() + ".augfwd").str();
        OpBuilder::InsertionGuard insertionGuard(builder);
        builder.setInsertionPointAfter(qnode);
        // The tape type is a null pointer because we don't need to pass any data from the forward
        // pass to the reverse pass.
        auto tapeType = LLVM::LLVMPointerType::get(ctx);
        SmallVector<Type> argTypes;
        if (failed(convertCustomGradArgumentTypes(qnode.getArgumentTypes(), argTypes))) {
            return failure();
        }
        auto augmentedForward = builder.create<func::FuncOp>(
            qnode.getLoc(), augmentedName, FunctionType::get(ctx, argTypes, {tapeType}));
        augmentedForward.setPrivate();
        Location loc = qnode.getLoc();

        // TODO: May need to copy over the primal func to get correct gradient results
        Block *entry = augmentedForward.addEntryBlock();
        builder.setInsertionPointToStart(entry);

        // TODO: reduce duplication, this is copied and pasted from the custom qgrad
        size_t idx = 0;
        Region::BlockArgListType unpackedArgs = augmentedForward.getArguments();
        SmallVector<Value> reconstructedPrimals;
        SmallVector<Value> reconstructedShadows;
        for (Type argType : qnode.getArgumentTypes()) {
            // TODO: This may or may not be a MemRef
            auto memRefType = cast<MemRefType>(argType);
            int64_t rank = memRefType.getRank();

            SmallVector<Value> primalVals{unpackedArgs[idx + 0], unpackedArgs[idx + 2]};
            SmallVector<Value> shadowVals{unpackedArgs[idx + 1], unpackedArgs[idx + 3]};
            // Offset, sizes, and strides are shared between the primal and shadow.
            // Enzyme requires dummy shadows for these even though they're integers because it
            // currently assumes that all custom gradient arguments (even integers) are active.
            SmallVector<Value> offsetsSizesStrides;
            offsetsSizesStrides.push_back(unpackedArgs[idx + 4]);
            idx += 6;
            for (int64_t dim = 0; dim < rank; dim++) {
                offsetsSizesStrides.push_back(unpackedArgs[idx]);
                idx += 2;
                offsetsSizesStrides.push_back(unpackedArgs[idx]);
                idx += 2;
            }

            primalVals.append(offsetsSizesStrides);
            shadowVals.append(offsetsSizesStrides);
            Value packedPrimal =
                MemRefDescriptor::pack(builder, loc, *getTypeConverter(), memRefType, primalVals);
            Value packedShadow =
                MemRefDescriptor::pack(builder, loc, *getTypeConverter(), memRefType, shadowVals);
            reconstructedPrimals.push_back(
                builder.create<UnrealizedConversionCastOp>(loc, memRefType, packedPrimal)
                    .getResult(0));
            reconstructedShadows.push_back(
                builder.create<UnrealizedConversionCastOp>(loc, memRefType, packedShadow)
                    .getResult(0));
        }
        builder.create<func::CallOp>(loc, qnode, reconstructedPrimals);
        Value tape = builder.create<LLVM::NullOp>(loc, tapeType);
        builder.create<func::ReturnOp>(qnode.getLoc(), tape);
        return augmentedForward;
    }

    FailureOr<func::FuncOp> genCustomQGradient(func::FuncOp qnode, Location loc,
                                               func::FuncOp qgradFn, OpBuilder &builder) const
    {
        SmallVector<Type> customArgTypes;
        std::string customQGradName = (qnode.getName() + ".customqgrad").str();
        MLIRContext *ctx = builder.getContext();
        auto tapeType = LLVM::LLVMPointerType::get(ctx);
        SmallVector<Type> argTypes;
        if (failed(convertCustomGradArgumentTypes(qnode.getArgumentTypes(), argTypes))) {
            return failure();
        }
        argTypes.push_back(tapeType);

        OpBuilder::InsertionGuard insertionGuard(builder);
        builder.setInsertionPoint(qnode);
        auto funcType = FunctionType::get(ctx, argTypes, {});
        auto customQGrad = builder.create<func::FuncOp>(qnode.getLoc(), customQGradName, funcType);
        customQGrad.setPrivate();
        Block *block = customQGrad.addEntryBlock();
        builder.setInsertionPointToStart(block);

        // Reconstruct the MemRefs from the unpacked arguments
        size_t idx = 0;
        Region::BlockArgListType unpackedArgs = customQGrad.getArguments();
        SmallVector<Value> reconstructedPrimals;
        SmallVector<Value> reconstructedShadows;
        for (Type argType : qnode.getArgumentTypes()) {
            // TODO: This may or may not be a MemRef
            auto memRefType = cast<MemRefType>(argType);
            int64_t rank = memRefType.getRank();

            SmallVector<Value> primalVals{unpackedArgs[idx + 0], unpackedArgs[idx + 2]};
            SmallVector<Value> shadowVals{unpackedArgs[idx + 1], unpackedArgs[idx + 3]};
            // Offset, sizes, and strides are shared between the primal and shadow.
            // Enzyme requires dummy shadows for these even though they're integers because it
            // currently assumes that all custom gradient arguments (even integers) are active.
            SmallVector<Value> offsetsSizesStrides;
            offsetsSizesStrides.push_back(unpackedArgs[idx + 4]);
            idx += 6;
            for (int64_t dim = 0; dim < rank; dim++) {
                offsetsSizesStrides.push_back(unpackedArgs[idx]);
                idx += 2;
                offsetsSizesStrides.push_back(unpackedArgs[idx]);
                idx += 2;
            }

            primalVals.append(offsetsSizesStrides);
            shadowVals.append(offsetsSizesStrides);
            Value packedPrimal =
                MemRefDescriptor::pack(builder, loc, *getTypeConverter(), memRefType, primalVals);
            Value packedShadow =
                MemRefDescriptor::pack(builder, loc, *getTypeConverter(), memRefType, shadowVals);
            reconstructedPrimals.push_back(
                builder.create<UnrealizedConversionCastOp>(loc, memRefType, packedPrimal)
                    .getResult(0));
            reconstructedShadows.push_back(
                builder.create<UnrealizedConversionCastOp>(loc, memRefType, packedShadow)
                    .getResult(0));
        }

        // TODO: This is a bit redundant, we could just generate the quantum gradient in DPS
        // The qgrad func takes the pcount and allocates the gradient. We already have a shadow for
        // the gradient here, which we're taking the dim of to get the pcount.
        SmallVector<Value> primalInputs{
            ValueRange{reconstructedPrimals}.take_front(qgradFn.getNumArguments() - 1)};
        // TODO: ugly, but the -2 is because the gate param is the last input. The dps output is
        // also an argument.
        Value gateParamShadow = reconstructedShadows[qnode.getNumArguments() - 2];
        // The gate param list is always 1-d
        Value pcount = builder.create<memref::DimOp>(loc, gateParamShadow, 0);
        primalInputs.push_back(pcount);

        // TODO: don't know if this works in jacobian contexts
        // TODO: This is segfaulting because the original arguments are optimized to poison values.
        // Value qgrad = builder.create<func::CallOp>(loc, qgradFn, primalInputs).getResult(0);
        // builder.create<memref::CopyOp>(loc, qgrad, gateParamShadow);
        builder.create<func::ReturnOp>(loc);

        return customQGrad;
    }

    /// This registers a custom allocation and deallocation functions with Enzyme. It creates a
    /// global LLVM array that Enzyme will convert to the appropriate metadata using the
    /// `preserve-nvvm` pass.
    ///
    /// This functionality is described at:
    /// https://github.com/EnzymeAD/Enzyme/issues/930#issuecomment-1334502012
    LLVM::GlobalOp insertEnzymeAllocationLike(OpBuilder &builder, ModuleOp moduleOp, Location loc,
                                              StringRef allocFuncName, StringRef freeFuncName) const
    {
        MLIRContext *context = moduleOp.getContext();
        Type indexType = getTypeConverter()->getIndexType();
        OpBuilder::InsertionGuard insertGuard(builder);
        builder.setInsertionPointToStart(moduleOp.getBody());

        auto allocationLike = moduleOp.lookupSymbol<LLVM::GlobalOp>(enzyme_allocation_key);
        if (allocationLike) {
            return allocationLike;
        }

        auto ptrType = LLVM::LLVMPointerType::get(context);
        auto resultType = LLVM::LLVMArrayType::get(ptrType, 4);

        builder.create<LLVM::GlobalOp>(loc, LLVM::LLVMArrayType::get(builder.getI8Type(), 3), true,
                                       LLVM::Linkage::Linkonce, "dealloc_indices",
                                       builder.getStringAttr(StringRef("-1", 3)));
        allocationLike =
            builder.create<LLVM::GlobalOp>(loc, resultType,
                                           /*isConstant=*/false, LLVM::Linkage::External,
                                           enzyme_allocation_key, /*address space=*/nullptr);
        builder.createBlock(&allocationLike.getInitializerRegion());
        Value allocFn = builder.create<LLVM::AddressOfOp>(loc, ptrType, allocFuncName);
        Value sizeArgIndex =
            builder.create<LLVM::ConstantOp>(loc, indexType, builder.getIndexAttr(0));
        Value sizeArgIndexPtr = builder.create<LLVM::IntToPtrOp>(loc, ptrType, sizeArgIndex);
        Value deallocIndicesPtr =
            builder.create<LLVM::AddressOfOp>(loc, ptrType, "dealloc_indices");
        Value freeFn = builder.create<LLVM::AddressOfOp>(loc, ptrType, freeFuncName);

        Value result = builder.create<LLVM::UndefOp>(loc, resultType);
        result = builder.create<LLVM::InsertValueOp>(loc, result, allocFn, 0);
        result = builder.create<LLVM::InsertValueOp>(loc, result, sizeArgIndexPtr, 1);
        result = builder.create<LLVM::InsertValueOp>(loc, result, deallocIndicesPtr, 2);
        result = builder.create<LLVM::InsertValueOp>(loc, result, freeFn, 3);
        builder.create<LLVM::ReturnOp>(loc, result);

        return allocationLike;
    }

    static void insertFunctionName(PatternRewriter &rewriter, Operation *op, StringRef key,
                                   StringRef value)
    {
        ModuleOp moduleOp = op->getParentOfType<ModuleOp>();
        PatternRewriter::InsertionGuard insertGuard(rewriter);
        rewriter.setInsertionPointToStart(moduleOp.getBody());
        LLVM::GlobalOp glb = moduleOp.lookupSymbol<LLVM::GlobalOp>(key);
        if (!glb) {
            glb = rewriter.create<LLVM::GlobalOp>(
                moduleOp.getLoc(),
                LLVM::LLVMArrayType::get(IntegerType::get(rewriter.getContext(), 8), value.size()),
                true, LLVM::Linkage::Linkonce, key, rewriter.getStringAttr(value));
        }
    }

    static LLVM::GlobalOp insertEnzymeFunctionLike(PatternRewriter &rewriter, Operation *op,
                                                   StringRef key, StringRef name,
                                                   StringRef originalName)
    {
        ModuleOp moduleOp = op->getParentOfType<ModuleOp>();
        auto *context = moduleOp.getContext();
        PatternRewriter::InsertionGuard insertGuard(rewriter);
        rewriter.setInsertionPointToStart(moduleOp.getBody());

        LLVM::GlobalOp glb = moduleOp.lookupSymbol<LLVM::GlobalOp>(key);

        auto ptrType = LLVM::LLVMPointerType::get(context);
        if (!glb) {
            glb = rewriter.create<LLVM::GlobalOp>(
                moduleOp.getLoc(), LLVM::LLVMArrayType::get(ptrType, 2), /*isConstant=*/false,
                LLVM::Linkage::External, key, nullptr);
        }

        // Create the block and push it back in the global
        auto *contextGlb = glb.getContext();
        Block *block = new Block();
        glb.getInitializerRegion().push_back(block);
        rewriter.setInsertionPointToStart(block);

        auto llvmPtr = LLVM::LLVMPointerType::get(contextGlb);

        // Get original global name
        auto originalNameRefAttr = SymbolRefAttr::get(contextGlb, originalName);
        auto originalGlobal =
            rewriter.create<LLVM::AddressOfOp>(glb.getLoc(), llvmPtr, originalNameRefAttr);

        // Get global name
        auto nameRefAttr = SymbolRefAttr::get(contextGlb, name);
        auto enzymeGlobal = rewriter.create<LLVM::AddressOfOp>(glb.getLoc(), llvmPtr, nameRefAttr);

        auto undefArray =
            rewriter.create<LLVM::UndefOp>(glb.getLoc(), LLVM::LLVMArrayType::get(ptrType, 2));
        Value llvmInsert0 =
            rewriter.create<LLVM::InsertValueOp>(glb.getLoc(), undefArray, originalGlobal, 0);
        Value llvmInsert1 =
            rewriter.create<LLVM::InsertValueOp>(glb.getLoc(), llvmInsert0, enzymeGlobal, 1);
        rewriter.create<LLVM::ReturnOp>(glb.getLoc(), llvmInsert1);
        return glb;
    }

    static LLVM::GlobalOp insertEnzymeCustomGradient(OpBuilder &builder, ModuleOp moduleOp,
                                                     Location loc, func::FuncOp originalFunc,
                                                     func::FuncOp augmentedPrimal,
                                                     func::FuncOp gradient)
    {
        MLIRContext *context = moduleOp.getContext();
        OpBuilder::InsertionGuard insertGuard(builder);
        builder.setInsertionPointToStart(moduleOp.getBody());

        std::string key = (enzyme_custom_gradient_key + originalFunc.getName()).str();
        auto customGradient = moduleOp.lookupSymbol<LLVM::GlobalOp>(key);
        if (customGradient) {
            return customGradient;
        }

        auto ptrType = LLVM::LLVMPointerType::get(context);
        auto resultType = LLVM::LLVMArrayType::get(ptrType, 3);
        customGradient = builder.create<LLVM::GlobalOp>(
            loc, resultType,
            /*isConstant=*/false, LLVM::Linkage::External, key, /*address space=*/nullptr);
        builder.createBlock(&customGradient.getInitializerRegion());
        Value origFnPtr = builder.create<func::ConstantOp>(loc, originalFunc.getFunctionType(),
                                                           originalFunc.getName());
        Value augFnPtr = builder.create<func::ConstantOp>(loc, augmentedPrimal.getFunctionType(),
                                                          augmentedPrimal.getName());
        Value gradFnPtr =
            builder.create<func::ConstantOp>(loc, gradient.getFunctionType(), gradient.getName());
        SmallVector<Value> fnPtrs{origFnPtr, augFnPtr, gradFnPtr};
        Value result = builder.create<LLVM::UndefOp>(loc, resultType);
        for (const auto &[idx, fnPtr] : llvm::enumerate(fnPtrs)) {
            Value casted =
                builder.create<UnrealizedConversionCastOp>(loc, ptrType, fnPtr).getResult(0);
            result = builder.create<LLVM::InsertValueOp>(loc, result, casted, idx);
        }

        builder.create<LLVM::ReturnOp>(loc, result);

        return customGradient;
    }
};

} // namespace

namespace catalyst {
namespace gradient {

void populateConversionPatterns(LLVMTypeConverter &typeConverter, RewritePatternSet &patterns)
{
    patterns.add<AdjointOpPattern>(typeConverter);
    patterns.add<BackpropOpPattern>(typeConverter);
}

} // namespace gradient
} // namespace catalyst
