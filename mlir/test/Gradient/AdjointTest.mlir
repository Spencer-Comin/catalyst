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

// RUN: quantum-opt %s --lower-gradients=only=adj --split-input-file --verify-diagnostics | FileCheck %s

// Check tensor to tensor case
func.func private @funcTensorTensor(%arg0: tensor<7x3x2x1xf64>) -> tensor<2xf64> attributes {qnode, diff_method = "adjoint"} {
    %0 = quantum.alloc(1) : !quantum.reg
    quantum.dealloc %0 : !quantum.reg
    %c0 = arith.constant 0.0 : f64
    %res = tensor.from_elements %c0, %c0 : tensor<2xf64>
    return %res : tensor<2xf64>
}

// CHECK-LABEL: @funcTensorTensor.adjoint(%arg0: tensor<7x3x2x1xf64>, %arg1: index) -> tensor<?x2xf64>
    // CHECK-NEXT:   [[GRAD:%.+]] = gradient.adjoint @funcTensorTensor.nodealloc(%arg0)
    // CHECK-NEXT:   return [[GRAD]]
// }

// CHECK-LABEL: @funcTensorTensor.fullgrad0adj(%arg0: tensor<7x3x2x1xf64>) -> tensor<7x3x2x1x2xf64>

// CHECK-LABEL: @funcTensorTensor.argmap(%arg0: tensor<7x3x2x1xf64>) -> tensor<?xf64>

// CHECK-LABEL: @gradCallTensorTensor
func.func @gradCallTensorTensor(%arg0: tensor<7x3x2x1xf64>) -> tensor<7x3x2x1x2xf64> {
    // CHECK:   [[GRAD:%.+]] = call @funcTensorTensor.fullgrad0adj(%arg0) : (tensor<7x3x2x1xf64>) -> tensor<7x3x2x1x2xf64>
    %2 = gradient.grad "defer" @funcTensorTensor(%arg0) : (tensor<7x3x2x1xf64>) -> tensor<7x3x2x1x2xf64>

    // CHECK:   return [[GRAD]]
    func.return %2 : tensor<7x3x2x1x2xf64>
}


// -----

// Check the multiple results case
func.func @funcMultiRes(%arg0: tensor<f64>) -> (tensor<f64>, tensor<f64>) attributes {qnode, diff_method = "adjoint"} {
    %0 = quantum.alloc(1) : !quantum.reg
    quantum.dealloc %0 : !quantum.reg
    func.return %arg0, %arg0 : tensor<f64>, tensor<f64>
}

// CHECK-LABEL: @funcMultiRes.adjoint(%arg0: tensor<f64>, %arg1: index) -> (tensor<?xf64>, tensor<?xf64>)
    // CHECK-NEXT:   [[GRAD:%.+]]:2 = gradient.adjoint @funcMultiRes.nodealloc(%arg0)
    // CHECK-NEXT:   return [[GRAD]]#0, [[GRAD]]#1
// }

// CHECK-LABEL: @funcMultiRes.fullgrad0adj(%arg0: tensor<f64>) -> (tensor<f64>, tensor<f64>)

// CHECK-LABEL: @funcMultiRes.argmap(%arg0: tensor<f64>) -> tensor<?xf64>

// CHECK-LABEL: @gradCallMultiRes
func.func @gradCallMultiRes(%arg0: tensor<f64>) -> (tensor<f64>, tensor<f64>)  {
    // CHECK:   [[GRAD:%.+]]:2 = call @funcMultiRes.fullgrad0adj(%arg0) : (tensor<f64>) -> (tensor<f64>, tensor<f64>)
    %0:2 = gradient.grad "defer" @funcMultiRes(%arg0) : (tensor<f64>) -> (tensor<f64>, tensor<f64>)

    // CHECK:   return [[GRAD]]#0, [[GRAD]]#1
    func.return %0#0, %0#1 : tensor<f64>, tensor<f64>
}
// -----

// Check the multiple arguments case
func.func @funcMultiArg(%arg0: tensor<f64>, %arg1: tensor<2xf64>) -> tensor<f64> attributes {qnode, diff_method = "adjoint"} {
    %0 = quantum.alloc(1) : !quantum.reg
    quantum.dealloc %0 : !quantum.reg
    func.return %arg0 : tensor<f64>
}

// CHECK-LABEL: @funcMultiArg.adjoint(%arg0: tensor<f64>, %arg1: tensor<2xf64>, %arg2: index) -> tensor<?xf64>
    // CHECK-NEXT:   [[GRAD:%.+]] = gradient.adjoint @funcMultiArg.nodealloc(%arg0, %arg1)
    // CHECK-NEXT:   return [[GRAD]]
// }

// CHECK-LABEL: @funcMultiArg.fullgrad0adj(%arg0: tensor<f64>, %arg1: tensor<2xf64>) -> tensor<f64>

// CHECK-LABEL: @funcMultiArg.fullgrad1adj(%arg0: tensor<f64>, %arg1: tensor<2xf64>) -> tensor<2xf64>

// CHECK-LABEL: @funcMultiArg.fullgrad01adj(%arg0: tensor<f64>, %arg1: tensor<2xf64>) -> (tensor<f64>, tensor<2xf64>)

// CHECK-LABEL: @funcMultiArg.argmap(%arg0: tensor<f64>, %arg1: tensor<2xf64>) -> tensor<?xf64>

// CHECK-LABEL: @gradCallMultiArg
func.func @gradCallMultiArg(%arg0: tensor<f64>, %arg1: tensor<2xf64>) -> (tensor<f64>, tensor<2xf64>, tensor<f64>, tensor<2xf64>) {
    // CHECK:   [[GRAD0:%.+]] = call @funcMultiArg.fullgrad0adj(%arg0, %arg1) : (tensor<f64>, tensor<2xf64>) -> tensor<f64>
    %0 = gradient.grad "defer"  @funcMultiArg(%arg0, %arg1) : (tensor<f64>, tensor<2xf64>) -> tensor<f64>
    // CHECK:   [[GRAD1:%.+]] = call @funcMultiArg.fullgrad1adj(%arg0, %arg1) : (tensor<f64>, tensor<2xf64>) -> tensor<2xf64>
    %1 = gradient.grad "defer"  @funcMultiArg(%arg0, %arg1) {diffArgIndices = dense<[1]> : tensor<1xindex>} : (tensor<f64>, tensor<2xf64>) -> tensor<2xf64>
    // CHECK:   [[GRAD2:%.+]]:2 = call @funcMultiArg.fullgrad01adj(%arg0, %arg1) : (tensor<f64>, tensor<2xf64>) -> (tensor<f64>, tensor<2xf64>)
    %2:2 = gradient.grad "defer" @funcMultiArg(%arg0, %arg1) {diffArgIndices = dense<[0, 1]> : tensor<2xindex>} : (tensor<f64>, tensor<2xf64>) -> (tensor<f64>, tensor<2xf64>)

    // CHECK:   return [[GRAD0]], [[GRAD1]], [[GRAD2]]#0, [[GRAD2]]#1
    func.return %0, %1, %2#0, %2#1 : tensor<f64>, tensor<2xf64>, tensor<f64>, tensor<2xf64>
}

// -----

// Check multiple grad calls to same function
func.func private @funcMultiCall(%arg0: tensor<f64>) -> tensor<f64> attributes {qnode, diff_method = "adjoint"} {
    %0 = quantum.alloc(1) : !quantum.reg
    quantum.dealloc %0 : !quantum.reg
    func.return %arg0 : tensor<f64>
}

// CHECK-LABEL: @funcMultiCall.adjoint(%arg0: tensor<f64>, %arg1: index) -> tensor<?xf64>

// CHECK-LABEL: @funcMultiCall.fullgrad0adj(%arg0: tensor<f64>) -> tensor<f64>

// CHECK-LABEL: @funcMultiCall.argmap(%arg0: tensor<f64>) -> tensor<?xf64>

// CHECK-LABEL: @gradCallMultiCall
func.func @gradCallMultiCall(%arg0: tensor<f64>) -> (tensor<f64>, tensor<f64>) {
    // CHECK:   [[GRAD0:%.+]] = call @funcMultiCall.fullgrad0adj(%arg0) : (tensor<f64>) -> tensor<f64>
    %0 = gradient.grad "defer" @funcMultiCall(%arg0) : (tensor<f64>) -> tensor<f64>
    // CHECK:   [[GRAD1:%.+]] = call @funcMultiCall.fullgrad0adj(%arg0) : (tensor<f64>) -> tensor<f64>
    %1 = gradient.grad "defer" @funcMultiCall(%arg0) : (tensor<f64>) -> tensor<f64>
    // CHECK:   return [[GRAD0]], [[GRAD1]]
    func.return %0, %1 : tensor<f64>, tensor<f64>
}

// -----

// Check invalid function with no deallocation
func.func private @funcScalarScalar(%arg0: tensor<f64>) -> tensor<f64> attributes {qnode, diff_method = "adjoint"} {
    %0 = quantum.alloc(1) : !quantum.reg
    return %arg0 : tensor<f64>
}

func.func @gradCallScalarScalar(%arg0: tensor<f64>) -> tensor<f64> {
    // expected-error@-6 {{Invalid number of quantum registers: 0}}
    %0 = gradient.grad "defer" @funcScalarScalar(%arg0) : (tensor<f64>) -> tensor<f64>
    func.return %0 : tensor<f64>
}
