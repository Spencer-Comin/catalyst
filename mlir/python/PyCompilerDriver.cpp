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

#include <iostream>
#include "Catalyst/Driver/CompilerDriver.h"
#include "mlir/Bindings/Python/PybindAdaptors.h"
#include "mlir/IR/BuiltinTypes.h"
#include "llvm/Support/raw_ostream.h"

namespace py = pybind11;
using namespace mlir::python::adaptors;

PYBIND11_MODULE(_catalystDriver, m)
{
    //===--------------------------------------------------------------------===//
    // Catalyst Compiler Driver
    //===--------------------------------------------------------------------===//

    m.def(
        "compile_asm",
        [](const char *source, const char *workspace, const char *moduleName,
           bool inferFunctionAttrs, bool keepIntermediate, bool verbose) {
            FunctionAttributes inferredAttributes;
            mlir::MLIRContext ctx;
            std::string errors;
            Verbosity verbosity = verbose ? CO_VERB_ALL : CO_VERB_SILENT;
            llvm::raw_string_ostream errStream{errors};

            CompilerOptions options{.ctx = &ctx,
                                    .source = source,
                                    .workspace = workspace,
                                    .moduleName = moduleName,
                                    .diagnosticStream = errStream,
                                    .keepIntermediate = keepIntermediate,
                                    .verbosity = verbosity};

            if (mlir::failed(QuantumDriverMain(options, inferredAttributes))) {
                throw std::runtime_error("Compilation failed:\n" + errors);
            }
            if (verbosity > CO_VERB_SILENT && !errors.empty()) {
                // TODO: There must be warnings/debug messages. We need to print them to the correct
                // stream.
                std::cerr << errors;
            }

            return std::make_tuple(options.getObjectFile(), inferredAttributes.llvmir,
                                   inferredAttributes.functionName, inferredAttributes.returnType);
        },
        py::arg("source"), py::arg("workspace"), py::arg("module_name") = "jit source",
        py::arg("infer_function_attrs") = false, py::arg("keep_intermediate") = false,
        py::arg("verbose") = false);

    m.def(
        "mlir_run_pipeline",
        [](const char *source, const char *pipeline) {
            auto result = RunPassPipeline(source, pipeline);
            if (mlir::failed(result)) {
                throw std::runtime_error("Pass pipeline failed");
            }
            return result.value();
        },
        py::arg("source"), py::arg("pipeline"));
}
