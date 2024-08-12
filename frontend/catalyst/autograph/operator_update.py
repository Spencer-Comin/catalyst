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

"""Converter for array element operator assignment."""

import gast
from malt.core import converter
from malt.pyct import templates


# The methods from this class should be migrated to the SliceTransformer class in DiastaticMalt
class SingleIndexArrayOperatorUpdateTransformer(converter.Base):
    """Converts array element operator assignment statements into calls to update_item_with_{op},
    where op is one of the following:

    - `add` corresponding to `+=`
    - `sub` to `-=`
    - `mult` to `*=`
    - `div` to `/=`
    - `pow` to `**=`
    """

    def _process_single_update(self, target, op, value):
        if not isinstance(target, gast.Subscript):
            return None
        s = target.slice
        if isinstance(s, (gast.Tuple, gast.Slice)):
            return None
        if not isinstance(op, (gast.Mult, gast.Add, gast.Sub, gast.Div, gast.Pow)):
            return None

        template = f"""
            target = ag__.update_item_with_{type(op).__name__.lower()}(target, i, x)
        """

        return templates.replace(template, target=target.value, i=target.slice, x=value)

    def visit_AugAssign(self, node):
        """The AugAssign node is replaced with a call to ag__.update_item_with_{op}
        when its target is a single index array subscript and its op is an arithmetic
        operator (i.e. Add, Sub, Mult, Div, or Pow), otherwise the node is left as is.

        Example:
            `x[i] += y` is replaced with `x = ag__.update_item_with(x, i, y)`
            `x[i] ^= y` remains unchanged
        """
        node = self.generic_visit(node)
        replacement = self._process_single_update(node.target, node.op, node.value)
        if replacement is not None:
            return replacement
        return node


def transform(node, ctx):
    """Replace an AugAssign node with a call to ag__.update_item_with_{op}
    when the its target is a single index array subscript and its op is an arithmetic
    operator (i.e. Add, Sub, Mult, Div, or Pow), otherwise the node is left as is.

    Example:
        `x[i] += y` is replaced with `x = ag__.update_item_with(x, i, y)`
        `x[i] ^= y` remains unchanged
    """
    return SingleIndexArrayOperatorUpdateTransformer(ctx).visit(node)