# Copyright 2023 Xanadu Quantum Technologies Inc.

# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at

#     http://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import jax.numpy as jnp
import pennylane as qml
import pytest

from catalyst import cond, measure, qjit


class TestReturnValues:
    """Test QJIT workflows with different return value data-types."""

    def test_return_value_float(self, backend):
        """Test constant."""

        @qml.qnode(qml.device(backend, wires=2))
        def circuit1(params):
            qml.RX(params[0], wires=0)
            qml.RX(params[1], wires=1)
            return qml.expval(qml.PauliZ(0) @ qml.PauliZ(1))

        jitted_fn = qjit(circuit1)

        params = [0.4, 0.8]
        expected = 0.64170937
        result = jitted_fn(params)
        assert jnp.allclose(result, expected)

        @qml.qnode(qml.device(backend, wires=2))
        def circuit2():
            return measure(0)

        jitted_fn = qjit(circuit2)

        result = jitted_fn()
        assert not result

    def test_return_value_arrays(self, backend):
        """Test arrays."""

        @qml.qnode(qml.device(backend, wires=2))
        def circuit1(params):
            qml.RX(params[0], wires=0)
            qml.RX(params[1], wires=1)
            return qml.state()

        jitted_fn = qjit(circuit1)

        params = [0.4, 0.8]
        result = jitted_fn(params)
        ip_result = circuit1(params)
        assert jnp.allclose(result, ip_result)

        @qml.qnode(qml.device(backend, wires=2))
        def circuit2(params):
            qml.RX(params[0], wires=0)
            qml.RX(params[1], wires=1)
            return [jnp.pi, qml.state()]

        jitted_fn = qjit(circuit2)

        params = [0.4, 0.8]
        result = jitted_fn(params)
        assert isinstance(result, list)
        assert jnp.allclose(result[0], jnp.pi)
        assert jnp.allclose(result[1], ip_result)

    def test_return_value_tuples(self, backend):
        """Test tuples."""

        @qml.qnode(qml.device(backend, wires=2))
        def circuit1():
            qml.Hadamard(0)
            qml.CNOT(wires=[0, 1])
            m0 = measure(0)
            m1 = measure(1)
            return (m0, m1)

        jitted_fn = qjit(circuit1)

        result = jitted_fn()
        assert isinstance(result, tuple)

        @qml.qnode(qml.device(backend, wires=2))
        def circuit2():
            qml.Hadamard(0)
            qml.CNOT(wires=[0, 1])
            m0 = measure(0)
            m1 = measure(1)
            return (((m0, m1), m0 + m1), m0 * m1)

        jitted_fn = qjit(circuit2)
        result = jitted_fn()
        assert isinstance(result, tuple)
        assert isinstance(result[0], tuple)
        assert isinstance(result[0][0], tuple)
        assert result[0][0][0] + result[0][0][1] == result[0][1]
        assert result[0][0][0] * result[0][0][1] == result[1]

        @qjit
        def workflow1(param):
            return {"w": jnp.sin(param), "q": jnp.cos(param)}

        result = workflow1(jnp.pi / 2)
        assert isinstance(result, dict)
        assert result["w"] == 1

    def test_return_value_hybrid(self, backend):
        """Test tuples."""

        @qml.qnode(qml.device(backend, wires=2))
        def circuit1():
            qml.Hadamard(0)
            qml.CNOT(wires=[0, 1])
            return qml.var(qml.PauliZ(1))

        @qjit
        def workflow1(param):
            a = circuit1()
            return (a, [jnp.sin(param), jnp.cos(param)])

        result = workflow1(1.27)
        assert isinstance(result, tuple)

    def test_return_value_cond(self, backend):
        """Test conditionals."""

        # QFunc Path.
        @qjit
        @qml.qnode(qml.device(backend, wires=1))
        def circuit1(n):
            @cond(n > 4)
            def cond_fn():
                return n**2

            @cond_fn.otherwise
            def else_fn():
                return n

            return cond_fn()

        assert circuit1(0) == 0
        assert circuit1(2) == 2
        assert circuit1(5) == 25

        # Classical Path.
        @qjit
        def circuit2(n):
            @cond(n > 4)
            def cond_fn():
                return n**2

            @cond_fn.otherwise
            def else_fn():
                return n

            return cond_fn()

        assert circuit2(0) == 0
        assert circuit2(2) == 2
        assert circuit2(5) == 25

    def test_return_value_dict(self, backend):
        """Test dictionaries."""

        @qml.qnode(qml.device(backend, wires=2))
        def circuit(params):
            qml.RX(params[0], wires=0)
            qml.RX(params[1], wires=1)
            return {
                "w0": qml.expval(qml.PauliZ(0)),
                "w1": qml.expval(qml.PauliZ(1)),
            }

        jitted_fn = qjit(circuit)

        params = [0.2, 0.6]
        expected = {"w0": 0.98006658, "w1": 0.82533561}
        result = jitted_fn(params)
        assert isinstance(result, dict)
        assert jnp.allclose(result["w0"], expected["w0"])
        assert jnp.allclose(result["w1"], expected["w1"])

    # def test_return_value_dict(self):
    #     """Test a function with returning a nonhomogenous dictionary."""

    #     @qml.qnode(qml.device("lightning.qubit", wires=2))
    #     def circuit(params):
    #         qml.RX(params[0], wires=0)
    #         qml.RX(params[1], wires=1)
    #         return { 'w0' : qml.expval(qml.PauliZ(0)),
    #                  'w1' : qml.expval(qml.PauliZ(1)),
    #         }

    #     params = [0.2, 0.6]
    #     jitted_fn = qjit(circuit)
    #     result = jitted_fn(params)
    #     assert isinstance(result, dict)

    #     expected = {
    #         'w0': 0.98006658,
    #         'w1': 0.82533561
    #     }

    #     assert jnp.allclose(result['w0'], expected['w0'])
    #     assert jnp.allclose(result['w1'], expected['w1'])


if __name__ == "__main__":
    pytest.main(["-x", __file__])