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

#include <cassert>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "mhlo/IR/register.h"
#include "mhlo/transforms/passes.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "stablehlo/dialect/Register.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"

#include "Catalyst/IR/CatalystDialect.h"
#include "Catalyst/Transforms/Passes.h"
#include "Driver/CatalystLLVMTarget.h"
#include "Driver/CompilerDriver.h"
#include "Driver/Support.h"
#include "Gradient/IR/GradientDialect.h"
#include "Gradient/Transforms/Passes.h"
#include "Mitigation/IR/MitigationDialect.h"
#include "Mitigation/Transforms/Passes.h"
#include "Quantum/IR/QuantumDialect.h"
#include "Quantum/Transforms/Passes.h"

#include "Enzyme.h"
#include "Timer.hpp"

using namespace mlir;
using namespace catalyst;
using namespace catalyst::driver;

namespace catalyst::utils {

/**
 * LinesCount : A utility class to count the number of lines of embedded programs
 * in different compilation stages.
 *
 * You can dump the program-size embedded in an `Operation`, `ModuleOp`, or
 * `llvm::Module` using the static methods in this class.
 *
 * To display results, run the driver with the `ENABLE_DIAGNOSTICS=ON` variable.
 * To store results in YAML format, use `DIAGNOSTICS_RESULTS_PATH=/path/to/file.yml`
 * along with `ENABLE_DIAGNOSTICS=ON`.
 */
class LinesCount {
  private:
    inline static void print(const std::string &opStrBuf, llvm::raw_ostream &stream,
                             const std::string &name)
    {
        std::ostringstream oss;
        const auto num_lines = std::count(opStrBuf.cbegin(), opStrBuf.cend(), '\n');
        if (!name.empty()) {
            oss << "[DIAGNOSTICS] After " << std::setw(25) << std::left << name;
        }
        oss << "\t" << std::fixed << "programsize: " << num_lines << std::fixed << " lines";
        stream << oss.str() << "\n";
    }

    inline static void store(const std::string &opStrBuf, const std::string &name,
                             const std::filesystem::path &file_path)
    {
        const auto num_lines = std::count(opStrBuf.cbegin(), opStrBuf.cend(), '\n');

        const std::string_view key_padding = "          ";
        const std::string_view val_padding = "              ";

        if (!std::filesystem::exists(file_path)) {
            std::ofstream ofile(file_path);
            assert(ofile.is_open() && "Invalid file to store timer results");
            if (!name.empty()) {
                ofile << key_padding << "- " << name << ":\n";
            }
            ofile << val_padding << "programsize: " << num_lines << "\n";
            ofile.close();
            return;
        }
        // else

        // Second, update the file
        std::ofstream ofile(file_path, std::ios::app);
        assert(ofile.is_open() && "Invalid file to store timer results");
        if (!name.empty()) {
            ofile << key_padding << "- " << name << ":\n";
        }
        ofile << val_padding << "programsize: " << num_lines << "\n";
        ofile.close();
    }

    inline static void dump(const std::string &opStrBuf, llvm::raw_ostream &stream = llvm::errs(),
                            const std::string &name = {})
    {
        char *file = getenv("DIAGNOSTICS_RESULTS_PATH");
        if (!file) {
            print(opStrBuf, stream, name);
            return;
        }
        // else
        store(opStrBuf, name, std::filesystem::path{file});
    }

  public:
    [[nodiscard]] inline static bool is_diagnostics_enabled()
    {
        char *value = getenv("ENABLE_DIAGNOSTICS");
        if (!value || std::string(value) != "ON") {
            return false;
        }
        return true;
    }

    static void Operation(Operation *op, llvm::raw_ostream &stream = llvm::errs(),
                          const std::string &name = {})
    {
        if (!is_diagnostics_enabled()) {
            return;
        }

        std::string opStrBuf;
        llvm::raw_string_ostream rawStrBef{opStrBuf};
        rawStrBef << *op;

        dump(opStrBuf, stream, name);
    }

    static void ModuleOp(const ModuleOp &op, llvm::raw_ostream &stream = llvm::errs(),
                         const std::string &name = {})
    {
        if (!is_diagnostics_enabled()) {
            return;
        }

        std::string modStrBef;
        llvm::raw_string_ostream rawStrBef{modStrBef};
        op->print(rawStrBef);

        dump(modStrBef, stream, name);
    }

    static void Module(const llvm::Module &llvmModule, llvm::raw_ostream &stream = llvm::errs(),
                       const std::string &name = {})
    {
        if (!is_diagnostics_enabled()) {
            return;
        }

        std::string modStrBef;
        llvm::raw_string_ostream rawStrBef{modStrBef};
        llvmModule.print(rawStrBef, nullptr);

        dump(modStrBef, stream, name);
    }
};

} // namespace catalyst::utils

namespace {

std::string joinPasses(const Pipeline::PassList &passes)
{
    std::string joined;
    llvm::raw_string_ostream stream{joined};
    llvm::interleaveComma(passes, stream);
    return joined;
}

struct CatalystIRPrinterConfig : public PassManager::IRPrinterConfig {
    typedef std::function<LogicalResult(Pass *, PrintCallbackFn print)> PrintHandler;
    PrintHandler printHandler;

    CatalystIRPrinterConfig(PrintHandler printHandler)
        : IRPrinterConfig(/*printModuleScope=*/true), printHandler(printHandler)
    {
    }

    void printAfterIfEnabled(Pass *pass, Operation *operation, PrintCallbackFn printCallback) final
    {
        if (failed(printHandler(pass, printCallback))) {
            operation->emitError("IR printing failed");
        }
    }
};

struct CatalystPassInstrumentation : public PassInstrumentation {
    typedef std::function<void(Pass *pass, Operation *operation)> PassCallback;
    PassCallback beforePassCallback;
    PassCallback afterPassCallback;
    PassCallback afterPassFailedCallback;

    CatalystPassInstrumentation(PassCallback beforePassCallback, PassCallback afterPassCallback,
                                PassCallback afterPassFailedCallback)
        : beforePassCallback(beforePassCallback), afterPassCallback(afterPassCallback),
          afterPassFailedCallback(afterPassFailedCallback)
    {
    }

    void runBeforePass(Pass *pass, Operation *operation) override
    {
        this->beforePassCallback(pass, operation);
    }

    void runAfterPass(Pass *pass, Operation *operation) override
    {
        this->afterPassCallback(pass, operation);
    }

    void runAfterPassFailed(Pass *pass, Operation *operation) override
    {
        this->afterPassFailedCallback(pass, operation);
    }
};

// Run the callback with stack printing disabled
void withoutStackTrace(MLIRContext *ctx, std::function<void()> callback)
{
    auto old = ctx->shouldPrintStackTraceOnDiagnostic();
    ctx->printStackTraceOnDiagnostic(false);
    callback();
    ctx->printStackTraceOnDiagnostic(old);
}

} // namespace

namespace {
/// Parse an MLIR module given in textual ASM representation. Any errors during parsing will be
/// output to diagnosticStream.
OwningOpRef<ModuleOp> parseMLIRSource(MLIRContext *ctx, const llvm::SourceMgr &sourceMgr)
{
    FallbackAsmResourceMap fallbackResourceMap;
    ParserConfig parserConfig{ctx, /*verifyAfterParse=*/true, &fallbackResourceMap};

    return parseSourceFile<ModuleOp>(sourceMgr, parserConfig);
}

/// Parse an LLVM module given in textual representation. Any parse errors will be output to
/// the provided SMDiagnostic.
std::shared_ptr<llvm::Module> parseLLVMSource(llvm::LLVMContext &context, StringRef source,
                                              StringRef moduleName, llvm::SMDiagnostic &err)
{
    auto moduleBuffer = llvm::MemoryBuffer::getMemBufferCopy(source, moduleName);
    return llvm::parseIR(llvm::MemoryBufferRef(*moduleBuffer), err, context);
}

/// Register all dialects required by the Catalyst compiler.
void registerAllCatalystDialects(DialectRegistry &registry)
{
    // MLIR Core dialects
    registerAllDialects(registry);
    registerAllExtensions(registry);

    // HLO
    mhlo::registerAllMhloDialects(registry);
    stablehlo::registerAllDialects(registry);

    // Catalyst
    registry.insert<CatalystDialect>();
    registry.insert<quantum::QuantumDialect>();
    registry.insert<gradient::GradientDialect>();
    registry.insert<mitigation::MitigationDialect>();
}
} // namespace

namespace catalyst::driver {

std::string CompilerOutput::nextDumpFilename(std::string filenameBase, std::string ext)
{
    return std::filesystem::path(std::to_string(this->pipelineCounter++) + "_" + filenameBase)
        .replace_extension(ext);
}

std::string CompilerOutput::pipelineDumpFilename(Pipeline::Name pipelineName, size_t pipelineIdx)
{
    return std::filesystem::path(std::to_string(this->pipelineCounter) + "_" +
                                 std::to_string(pipelineIdx) + "_" + pipelineName)
        .replace_extension(".mlir");
}

} // namespace catalyst::driver

FailureOr<llvm::Function *> getJITFunction(MLIRContext *ctx, llvm::Module &llvmModule)
{
    Location loc = NameLoc::get(StringAttr::get(ctx, llvmModule.getName()));
    std::list<StringRef> visited;
    for (auto &function : llvmModule.functions()) {
        visited.push_back(function.getName());
        if (function.getName().starts_with("catalyst.entry_point")) {
            return &function;
        }
    }
    withoutStackTrace(ctx, [&]() {
        auto noteStream =
            emitRemark(loc, "Failed to find entry-point function among the following: ");
        llvm::interleaveComma(visited, noteStream, [&](StringRef t) { noteStream << t; });
    });

    return failure();
}

LogicalResult inferMLIRReturnTypes(MLIRContext *ctx, llvm::Type *returnType,
                                   Type assumedElementType,
                                   SmallVectorImpl<RankedTensorType> &inferredTypes)
{
    auto inferSingleMemRef = [&](llvm::StructType *descriptorType) {
        SmallVector<int64_t> resultShape;
        assert(descriptorType->getNumElements() >= 3 &&
               "Expected MemRef descriptor struct to have at least 3 entries");
        // WARNING: Assumption follows
        //
        // In this piece of code we are making the assumption that the user will
        // return something that may have been an MLIR tensor once. This is
        // likely to be true, however, there are no hard guarantees.
        //
        // The assumption gives the following invariants:
        // * The structure we are "parsing" will be a memref with the following fields
        // * void* allocated_ptr
        // * void* aligned_ptr
        // * int offset
        // * int[rank] sizes
        // * int[rank] strides
        //
        // Please note that strides might be zero which means that the fields sizes
        // and stride are optional and not required to be defined.
        // sizes is defined iff strides is defined.
        // strides is defined iff sizes is defined.
        bool hasSizes = 5 == descriptorType->getNumElements();
        auto *sizes = hasSizes ? cast<llvm::ArrayType>(descriptorType->getTypeAtIndex(3)) : NULL;
        size_t rank = hasSizes ? sizes->getNumElements() : 0;
        for (size_t i = 0; i < rank; i++) {
            resultShape.push_back(ShapedType::kDynamic);
        }
        return RankedTensorType::get(resultShape, assumedElementType);
    };
    if (returnType->isVoidTy()) {
        return failure();
    }
    if (auto *structType = dyn_cast<llvm::StructType>(returnType)) {
        // The return type could be a single memref descriptor or a struct of multiple memref
        // descriptors.
        if (isa<llvm::StructType>(structType->getElementType(0))) {
            for (size_t i = 0; i < structType->getNumElements(); i++) {
                inferredTypes.push_back(
                    inferSingleMemRef(cast<llvm::StructType>(structType->getTypeAtIndex(i))));
            }
        }
        else {
            // Assume the function returns a single memref
            inferredTypes.push_back(inferSingleMemRef(structType));
        }
        return success();
    }
    return failure();
}

LogicalResult runLLVMPasses(const CompilerOptions &options,
                            std::shared_ptr<llvm::Module> llvmModule, CompilerOutput &output)
{
    // opt -O2
    // As seen here:
    // https://llvm.org/docs/NewPassManager.html#just-tell-me-how-to-run-the-default-optimization-pipeline-with-the-new-pass-manager

    auto &outputs = output.pipelineOutputs;
    // Create the analysis managers.
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    // Create the new pass manager builder.
    // Take a look at the PassBuilder constructor parameters for more
    // customization, e.g. specifying a TargetMachine or various debugging
    // options.
    llvm::PassBuilder PB;
    // Register all the basic analyses with the managers.
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    // Create the pass manager.
    // This one corresponds to a typical -O2 optimization pipeline.
    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);

    // Optimize the IR!
    MPM.run(*llvmModule.get(), MAM);

    if (options.keepIntermediate) {
        llvm::raw_string_ostream rawStringOstream{outputs["PreEnzymeOpt"]};
        llvmModule->print(rawStringOstream, nullptr);
        auto outFile = output.nextDumpFilename("PreEnzymeOpt", ".ll");
        dumpToFile(options, outFile, outputs["PreEnzymeOpt"]);
    }

    return success();
}

LogicalResult runEnzymePasses(const CompilerOptions &options,
                              std::shared_ptr<llvm::Module> llvmModule, CompilerOutput &output)
{
    auto &outputs = output.pipelineOutputs;
    // Create the new pass manager builder.
    // Take a look at the PassBuilder constructor parameters for more
    // customization, e.g. specifying a TargetMachine or various debugging
    // options.
    llvm::PassBuilder PB;

    // Create the analysis managers.
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    // Register all the basic analyses with the managers.
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    // Call Enzyme specific augmentPassBuilder which will add Enzyme passes.
    augmentPassBuilder(PB);

    // Create the pass manager.
    // This one corresponds to a typical -O2 optimization pipeline.
    llvm::ModulePassManager MPM = PB.buildModuleOptimizationPipeline(
        llvm::OptimizationLevel::O2, llvm::ThinOrFullLTOPhase::None);

    // Optimize the IR!
    MPM.run(*llvmModule.get(), MAM);

    if (options.keepIntermediate) {
        llvm::raw_string_ostream rawStringOstream{outputs["Enzyme"]};
        llvmModule->print(rawStringOstream, nullptr);
        auto outFile = output.nextDumpFilename("Enzyme", ".ll");
        dumpToFile(options, outFile, outputs["Enzyme"]);
    }

    return success();
}

LogicalResult runLowering(const CompilerOptions &options, MLIRContext *ctx, ModuleOp moduleOp,
                          CompilerOutput &output)

{
    using std::pair;
    using Timer = catalyst::utils::Timer<llvm::raw_ostream>;
    auto &outputs = output.pipelineOutputs;
    auto pm = PassManager::on<ModuleOp>(ctx, PassManager::Nesting::Implicit);

    if (options.verbosity >= Verbosity::Timing) {
        auto tm = std::make_unique<DefaultTimingManager>();
        tm->setOutput(options.diagnosticStream);
        tm->setEnabled(true);
        pm.enableTiming(std::move(tm));
    }

    // Maps a pass to zero or one pipelines ended by this pass
    // Maps a pass to its owning pipeline
    std::unordered_map<const Pass *, pair<Pipeline::Name, size_t>> pipelineTailMarkers;
    std::unordered_map<const Pass *, pair<Pipeline::Name, size_t>> passPipelineNames;

    // Fill all the pipe-to-pipeline mappings
    {
        size_t pipelineIdx = 0;
        for (const auto &pipeline : options.pipelinesCfg) {
            size_t existingPasses = pm.size();
            if (failed(
                    parsePassPipeline(joinPasses(pipeline.passes), pm, options.diagnosticStream))) {
                return failure();
            }
            if (existingPasses != pm.size()) {
                const Pass *pass = nullptr;
                for (size_t pn = existingPasses; pn < pm.size(); pn++) {
                    pass = &(*(pm.begin() + pn));
                    passPipelineNames[pass] = pair(pipeline.name, pipelineIdx);
                }
                assert(pass != nullptr);
                pipelineTailMarkers[pass] = pair(pipeline.name, pipelineIdx);
            }
            pipelineIdx++;
        }
    }

    if (options.keepIntermediate) {
        std::string tmp;
        llvm::raw_string_ostream s{tmp};
        s << moduleOp;
        dumpToFile(options, output.nextDumpFilename(options.moduleName.str(), ".mlir"), tmp);
    }

    Timer timer{};

    auto beforePassCallback = [&](Pass *pass, Operation *op) {
        if (!timer.is_active()) {
            timer.start();
        }
    };

    // For each pipeline-terminating pass, print the IR into the corresponding dump file and
    // into a diagnostic output buffer. Note that one pass can terminate multiple pipelines.
    auto afterPassCallback = [&](Pass *pass, Operation *op) {
        auto res = pipelineTailMarkers.find(pass);
        if (res != pipelineTailMarkers.end()) {
            timer.dump(res->second.first, options.diagnosticStream, /*add_endl */ false);
            catalyst::utils::LinesCount::Operation(op, options.diagnosticStream);
        }

        if (options.keepIntermediate && res != pipelineTailMarkers.end()) {
            auto pipelineName = res->second.first;
            auto pipelineIdx = res->second.second;
            llvm::raw_string_ostream s{outputs[pipelineName]};
            s << *op;
            dumpToFile(options, output.pipelineDumpFilename(pipelineName, pipelineIdx),
                       outputs[pipelineName]);
        }
    };

    // For each failed pass, print the owner pipeline name into a diagnostic stream.
    auto afterPassFailedCallback = [&](Pass *pass, Operation *op) {
        auto res = passPipelineNames.find(pass);
        assert(res != passPipelineNames.end() && "Unexpected pass");
        auto pipelineName = res->second.first;
        auto pipelineIdx = res->second.second;
        options.diagnosticStream << "While processing '" << pass->getName() << "' pass "
                                 << "of the '" << pipelineName << "' pipeline\n";
        llvm::raw_string_ostream s{outputs[pipelineName]};
        s << *op;
        if (options.keepIntermediate) {
            dumpToFile(options, output.pipelineDumpFilename(pipelineName + "_FAILED", pipelineIdx),
                       outputs[pipelineName]);
        }
    };

    // Output pipeline names on failures
    pm.addInstrumentation(std::unique_ptr<PassInstrumentation>(new CatalystPassInstrumentation(
        beforePassCallback, afterPassCallback, afterPassFailedCallback)));

    // Run the lowering pipelines
    if (failed(pm.run(moduleOp))) {
        return failure();
    }

    output.pipelineCounter++;
    return success();
}

LogicalResult QuantumDriverMain(const CompilerOptions &options, CompilerOutput &output)
{
    using Timer = catalyst::utils::Timer<llvm::raw_ostream>;
    DialectRegistry registry;
    static bool initialized = false;
    if (!initialized) {
        registerAllPasses();
    }
    initialized |= true;
    registerAllCatalystPasses();
    mhlo::registerAllMhloPasses();

    registerAllCatalystDialects(registry);
    registerLLVMTranslations(registry);
    MLIRContext ctx(registry);
    ctx.printOpOnDiagnostic(true);
    ctx.printStackTraceOnDiagnostic(options.verbosity >= Verbosity::Debug);

    ctx.enableMultithreading(options.enableMultiThreadedCompilation);
    CO_MSG(options, Verbosity::Debug,
           "MLIR multi-threaded compilation flag: " << options.enableMultiThreadedCompilation
                                                    << " (" << ctx.getNumThreads() << " threads)"
                                                    << "\n");
    ScopedDiagnosticHandler scopedHandler(
        &ctx, [&](Diagnostic &diag) { diag.print(options.diagnosticStream); });

    llvm::LLVMContext llvmContext;
    std::shared_ptr<llvm::Module> llvmModule;

    llvm::raw_string_ostream outIRStream(output.outIR);

    auto moduleBuffer = llvm::MemoryBuffer::getMemBufferCopy(options.source, options.moduleName);
    auto sourceMgr = std::make_shared<llvm::SourceMgr>();
    sourceMgr->AddNewSourceBuffer(std::move(moduleBuffer), SMLoc());
    SourceMgrDiagnosticHandler sourceMgrHandler(*sourceMgr, &ctx, options.diagnosticStream);

    OwningOpRef<ModuleOp> op =
        Timer::timer(parseMLIRSource, "parseMLIRSource", options.diagnosticStream,
                     /* add_endl */ false, &ctx, *sourceMgr);
    catalyst::utils::LinesCount::ModuleOp(*op, options.diagnosticStream);

    if (op) {
        if (failed(Timer::timer(runLowering, "runMLIRPasses", options.diagnosticStream,
                                /* add_endl */ true, options, &ctx, *op, output))) {
            CO_MSG(options, Verbosity::Urgent, "Failed to lower MLIR module\n");
            return failure();
        }

        output.outIR.clear();
        outIRStream << *op;

        if (options.lowerToLLVM) {
            llvmModule = Timer::timer(translateModuleToLLVMIR, "translateModuleToLLVMIR",
                                      options.diagnosticStream, /* add_endl */ false, *op,
                                      llvmContext, "LLVMDialectModule");
            if (!llvmModule) {
                CO_MSG(options, Verbosity::Urgent, "Failed to translate LLVM module\n");
                return failure();
            }

            catalyst::utils::LinesCount::Module(*llvmModule, options.diagnosticStream);

            if (options.keepIntermediate) {
                dumpToFile(options, output.nextDumpFilename("llvm_ir", ".ll"), *llvmModule);
            }
        }
    }
    else {
        CO_MSG(options, Verbosity::Urgent,
               "Failed to parse module as MLIR source, retrying parsing as LLVM source\n");
        llvm::SMDiagnostic err;
        llvmModule = Timer::timer(parseLLVMSource, "parseLLVMSource", options.diagnosticStream,
                                  /* add_endl */ false, llvmContext, options.source,
                                  options.moduleName, err);
        if (!llvmModule) {
            // If both MLIR and LLVM failed to parse, exit.
            err.print(options.moduleName.data(), options.diagnosticStream);
            CO_MSG(options, Verbosity::Urgent, "Failed to parse module as LLVM source\n");
            return failure();
        }

        catalyst::utils::LinesCount::Module(*llvmModule, options.diagnosticStream);
    }

    if (llvmModule) {
        if (failed(Timer::timer(runLLVMPasses, "runLLVMPasses", options.diagnosticStream,
                                /* add_endl */ false, options, llvmModule, output))) {
            return failure();
        }

        catalyst::utils::LinesCount::Module(*llvmModule.get(), options.diagnosticStream);

        if (failed(Timer::timer(runEnzymePasses, "runEnzymePasses", options.diagnosticStream,
                                /* add_endl */ false, options, llvmModule, output))) {
            return failure();
        }

        catalyst::utils::LinesCount::Module(*llvmModule.get(), options.diagnosticStream);

        output.outIR.clear();
        outIRStream << *llvmModule;

        // Attempt to infer the name and return type of the module from LLVM IR. This information is
        // required when executing a module given as textual IR.
        auto function = getJITFunction(&ctx, *llvmModule);
        if (succeeded(function)) {
            output.inferredAttributes.functionName = function.value()->getName().str();

            CO_MSG(options, Verbosity::Debug,
                   "Inferred function name: '" << output.inferredAttributes.functionName << "'\n");

            // When inferring the return type from LLVM, assume a f64
            // element type. This is because the LLVM pointer type is
            // opaque and requires looking into its uses to infer its type.
            SmallVector<RankedTensorType> returnTypes;
            if (failed(Timer::timer(inferMLIRReturnTypes, "inferMLIRReturn",
                                    options.diagnosticStream,
                                    /* add_endl */ true, &ctx, function.value()->getReturnType(),
                                    Float64Type::get(&ctx), returnTypes))) {
                // Inferred return types are only required when compiling from textual IR. This
                // inference failing is not a problem when compiling from Python.
                CO_MSG(options, Verbosity::Urgent, "Unable to infer function return type\n");
            }
            else {
                llvm::raw_string_ostream returnTypeStream(output.inferredAttributes.returnType);
                llvm::interleaveComma(returnTypes, returnTypeStream,
                                      [&](RankedTensorType t) { t.print(returnTypeStream); });
                CO_MSG(options, Verbosity::Debug,
                       "Inferred function return type: '" << output.inferredAttributes.returnType
                                                          << "'\n");
            }
        }
        else {
            CO_MSG(options, Verbosity::Urgent,
                   "Unable to infer catalyst.entry_point* function attributes\n");
        }

        auto outfile = options.getObjectFile();
        if (failed(Timer::timer(compileObjectFile, "compileObjFile", options.diagnosticStream,
                                /* add_endl */ true, options, std::move(llvmModule), outfile))) {
            return failure();
        }
        output.objectFilename = outfile;
    }
    return success();
}
