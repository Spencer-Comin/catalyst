# Copyright 2024 Xanadu Quantum Technologies Inc.

# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at

#     http://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
This module contains the decomposition functions to pre-process tapes for
compilation & execution on devices.
"""

import jax
import pennylane as qml
from pennylane import transform
from pennylane.devices.preprocess import decompose
from pennylane.measurements import (
    CountsMP,
    ExpectationMP,
    MidMeasureMP,
    ProbabilityMP,
    VarianceMP,
)
from pennylane.tape.tape import (
    _validate_computational_basis_sampling,
    rotations_and_diagonal_measurements,
)

from catalyst.api_extensions.quantum_operators import QCtrl
from catalyst.jax_tracer import HybridOpRegion, has_nested_tapes
from catalyst.tracing.contexts import EvaluationContext
from catalyst.utils.exceptions import CompileError


def catalyst_decomposer(op):
    """A decomposer for catalyst, to be passed to the decompose transform. Takes an operator and
    returns the default decomposition, unless the operator should decompose to a QubitUnitary.
    Raises a CompileError for MidMeasureMP"""
    if isinstance(op, MidMeasureMP):
        raise CompileError("Must use 'measure' from Catalyst instead of PennyLane.")
    if op.name in {"MultiControlledX", "BlockEncode"} or isinstance(op, qml.ops.Controlled):
        return _decompose_to_matrix(op)
    return op.decomposition()


@transform
def catalyst_decompose(
    tape: qml.tape.QuantumTape,
    ctx,
    stopping_condition,
    decomposer=catalyst_decomposer,
    max_expansion=None,
):
    """Decompose operations until the stopping condition is met.

    In a single call of the catalyst_decompose function, the PennyLane operations are decomposed
    in the same manner as in PennyLane (for each operator on the tape, checking if the operator
    passes the stopping_condition, and using its `decompostion` method if not, called recursively
    until a supported operation is found or an error is hit, then moving on to the next operator
    on the tape.)

    Once all operators on the tape are supported operators, the resulting tape is iterated over,
    and for each HybridOp, the catalyst_decompose function is called on each of it's regions.
    This continues to call catalyst_decompose recursively until the tapes on all
    the HybridOps have been passed to the decompose function.
    """

    (toplevel_tape,), _ = decompose(
        tape,
        stopping_condition,
        skip_initial_state_prep=False,
        decomposer=decomposer,
        max_expansion=max_expansion,
        name="catalyst on this device",
        error=CompileError,
    )

    new_ops = []
    for op in toplevel_tape.operations:
        if has_nested_tapes(op):
            op = _decompose_nested_tapes(op, ctx, stopping_condition, decomposer, max_expansion)
        new_ops.append(op)
    tape = qml.tape.QuantumScript(new_ops, tape.measurements, shots=tape.shots)

    return (tape,), lambda x: x[0]


def _decompose_to_matrix(op):
    try:
        mat = op.matrix()
    except Exception as e:
        raise CompileError(
            f"Operation {op} could not be decomposed, it might be unsupported."
        ) from e
    op = qml.QubitUnitary(mat, wires=op.wires)
    return [op]


def _decompose_nested_tapes(op, ctx, stopping_condition, decomposer, max_expansion):
    new_regions = []
    for region in op.regions:
        if region.quantum_tape is None:
            new_tape = None
        else:
            with EvaluationContext.frame_tracing_context(ctx, region.trace):
                tapes, _ = catalyst_decompose(
                    region.quantum_tape,
                    ctx=ctx,
                    stopping_condition=stopping_condition,
                    decomposer=decomposer,
                    max_expansion=max_expansion,
                )
                new_tape = tapes[0]
        new_regions.append(
            HybridOpRegion(
                region.trace, new_tape, region.arg_classical_tracers, region.res_classical_tracers
            )
        )

    new_op = op.__class__(op.in_classical_tracers, op.out_classical_tracers, new_regions)
    return new_op


@transform
def decompose_ops_to_unitary(tape, convert_to_matrix_ops):
    r"""Quantum transform that decomposes operations to unitary given a list of operations name.

    Args:
        tape (QNode or QuantumTape or Callable): A quantum circuit.
        convert_to_matrix_ops (list[str]): The list of operation names to be converted to unitary.

    Returns:
        qnode (QNode) or quantum function (Callable) or tuple[List[QuantumTape], function]: The
        transformed circuit as described in :func:`qml.transform <pennylane.transform>`.
    """
    new_operations = []

    for op in tape.operations:
        if op.name in convert_to_matrix_ops or isinstance(op, QCtrl):
            try:
                mat = op.matrix()
            except Exception as e:
                raise CompileError(
                    f"Operation {op} could not be decomposed, it might be unsupported."
                ) from e
            op = qml.QubitUnitary(mat, wires=op.wires)
        new_operations.append(op)

    new_tape = type(tape)(new_operations, tape.measurements, shots=tape.shots)

    def null_postprocessing(results):
        """A postprocesing function returned by a transform that only converts the batch of results
        into a result for a single ``QuantumTape``.
        """
        return results[0]

    return [new_tape], null_postprocessing


def catalyst_acceptance(op: qml.operation.Operator, operations) -> bool:
    """Specify whether or not an Operator is supported."""
    return op.name in operations


@transform
def measurements_from_counts(tape):
    r"""Replace all measurements from a tape with a single count measurement, it adds postprocessing
    functions for each original measurement.

    Args:
        tape (QNode or QuantumTape or Callable): A quantum circuit.

    Returns:
        qnode (QNode) or quantum function (Callable) or tuple[List[QuantumTape], function]: The
        transformed circuit as described in :func:`qml.transform <pennylane.transform>`.

    .. note::

        Samples are not supported.
    """
    if tape.samples_computational_basis and len(tape.measurements) > 1:
        _validate_computational_basis_sampling(tape)
    diagonalizing_gates, diagonal_measurements = rotations_and_diagonal_measurements(tape)
    for i, m in enumerate(diagonal_measurements):
        if m.obs is not None:
            diagonalizing_gates.extend(m.obs.diagonalizing_gates())
            diagonal_measurements[i] = type(m)(eigvals=m.eigvals(), wires=m.wires)
    # Add diagonalizing gates
    news_operations = tape.operations
    news_operations.extend(diagonalizing_gates)
    # Transform tape
    measured_wires = set()
    for m in diagonal_measurements:
        measured_wires.update(m.wires.tolist())

    new_measurements = [qml.counts(wires=list(measured_wires))]
    new_tape = type(tape)(news_operations, new_measurements, shots=tape.shots)

    def postprocessing_counts_to_expval(results):
        """A processing function to get expecation values from counts."""
        states = results[0][0]
        counts_outcomes = results[0][1]
        results_processed = []
        for m in tape.measurements:
            mapped_counts_outcome = _map_counts(
                counts_outcomes, m.wires, qml.wires.Wires(list(measured_wires))
            )
            if isinstance(m, ExpectationMP):
                probs = _get_probs(mapped_counts_outcome)
                results_processed.append(_get_expval(eigvals=m.eigvals(), prob_vector=probs))
            elif isinstance(m, VarianceMP):
                probs = _get_probs(mapped_counts_outcome)
                results_processed.append(_get_var(eigvals=m.eigvals(), prob_vector=probs))
            elif isinstance(m, ProbabilityMP):
                probs = _get_probs(mapped_counts_outcome)
                results_processed.append(probs)
            elif isinstance(m, CountsMP):
                results_processed.append(
                    tuple([states[0 : 2 ** len(m.wires)], mapped_counts_outcome])
                )
        if len(tape.measurements) == 1:
            results_processed = results_processed[0]
        else:
            results_processed = tuple(results_processed)
        return results_processed

    return [new_tape], postprocessing_counts_to_expval


def _get_probs(counts_outcome):
    """From the counts outcome, calculate the probability vector."""
    prob_vector = []
    num_shots = jax.numpy.sum(counts_outcome)
    for count in counts_outcome:
        prob = count / num_shots
        prob_vector.append(prob)
    return jax.numpy.array(prob_vector)


def _get_expval(eigvals, prob_vector):
    """From the observable eigenvalues and the probability vector
    it calculates the expectation value."""
    expval = jax.numpy.dot(jax.numpy.array(eigvals), prob_vector)
    return expval


def _get_var(eigvals, prob_vector):
    """From the observable eigenvalues and the probability vector
    it calculates the variance."""
    var = jax.numpy.dot(prob_vector, (eigvals**2)) - jax.numpy.dot(prob_vector, eigvals)
    return var


def _map_counts(counts, sub_wires, wire_order):
    """Map the count outcome given a wires and wire order."""
    wire_map = dict(zip(wire_order, range(len(wire_order))))
    mapped_wires = [wire_map[w] for w in sub_wires]

    mapped_counts = {}
    num_wires = len(wire_order)
    for outcome, occurrence in enumerate(counts):
        binary_outcome = format(outcome, f"0{num_wires}b")
        mapped_outcome = "".join(binary_outcome[i] for i in mapped_wires)
        mapped_counts[mapped_outcome] = mapped_counts.get(mapped_outcome, 0) + occurrence

    return jax.numpy.array(list(mapped_counts.values()))
