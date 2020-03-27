# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
"""Unit tests for merge compiler regions."""
import tvm
from tvm import relay
from tvm.relay.op.annotation import compiler_begin, compiler_end
from tvm.relay.testing import run_opt_pass


def test_diamond_graph_fanouts():
    """
    This tests that the data dependencies present in a diamond-shaped
    graph are correctly resolved by the merging pass.

    O = supported by target
    X = not supported by target

       O         O
      / \       /               \
     O   X --> O    +       +    X
      \ /              \ /
       O                O

    Note that we can't just merge the three supported operators together,
    otherwise both subgraphs would depend on the other.
    """
    def diamond_graph_fanouts():
        data = relay.var('data', shape=(10, 10))
        cb_1 = compiler_begin(data, "test")
        O_1 = relay.abs(cb_1)
        ce_1 = compiler_end(O_1, "test")
        ce_2 = compiler_end(O_1, "test")
        cb_2 = compiler_begin(ce_1, "test")
        O_2 = relay.nn.relu(cb_2)
        ce_3 = compiler_end(O_2, "test")

        X = relay.tanh(ce_2)

        cb_3 = compiler_begin(ce_3, "test")
        cb_4 = compiler_begin(X, "test")
        O_3 = relay.add(cb_3, cb_4)
        ce_4 = compiler_end(O_3, "test")

        diamond = relay.Function([data], ce_4)
        return diamond

    def expected():
        data = relay.var('data', shape=(10, 10))
        cb_1 = compiler_begin(data, "test")
        O_1 = relay.abs(cb_1)
        ce_2 = compiler_end(O_1, "test")
        O_2 = relay.nn.relu(O_1)
        ce_3 = compiler_end(O_2, "test")

        cb_x = compiler_begin(ce_2, "default")
        X = relay.tanh(cb_x)
        ce_x1 = compiler_end(X, "default")
        ce_x2 = compiler_end(X, "default")

        cb_3 = compiler_begin(ce_3, "test")
        cb_4 = compiler_begin(ce_x1, "test")
        O_3 = relay.add(cb_3, cb_4)
        ce_4 = compiler_end(O_3, "test")

        func = relay.Function([data], ce_4)
        return func

    result = run_opt_pass(diamond_graph_fanouts(), relay.transform.MergeCompilerRegions())
    golden = run_opt_pass(expected(), relay.transform.InferType())
    assert relay.analysis.alpha_equal(result, golden)


def test_example_graph():
    """This tests the merging algorithm on the example used in the RFC.

    See the RFC here: https://discuss.tvm.ai/t/relay-improved-graph-partitioning-algorithm/5830
    Blue nodes are adds, red nodes are subtracts.
    """
    def annotated():
        in_1 = relay.var('in_1', shape=(10, 10), dtype='float32')
        in_2 = relay.var('in_2', shape=(10, 10), dtype='float32')
        in_3 = relay.var('in_3', shape=(10, 10), dtype='float32')
        in_4 = relay.var('in_4', shape=(10, 10), dtype='float32')
        in_5 = relay.var('in_5', shape=(10, 10), dtype='float32')
        in_6 = relay.var('in_6', shape=(10, 10), dtype='float32')
        in_7 = relay.var('in_7', shape=(10, 10), dtype='float32')
        in_8 = relay.var('in_8', shape=(10, 10), dtype='float32')
        in_9 = relay.var('in_9', shape=(10, 10), dtype='float32')
        in_10 = relay.var('in_10', shape=(10, 10), dtype='float32')

        begin0 = compiler_begin(in_1, "test")
        begin1 = compiler_begin(in_2, "test")
        begin2 = compiler_begin(in_3, "test")
        begin3 = compiler_begin(in_4, "test")
        node0 = relay.add(begin0, begin1)
        node1 = relay.add(begin2, begin3)
        end0 = compiler_end(node0, "test")
        end1 = compiler_end(node1, "test")
        begin4 = compiler_begin(end0, "test")
        begin5 = compiler_begin(end1, "test")
        node2 = relay.add(begin4, begin5)
        end2 = compiler_end(node2, "test")

        node3 = relay.subtract(in_5, in_6)
        node4 = relay.subtract(in_7, node3)

        begin6 = compiler_begin(end2, "test")
        begin7 = compiler_begin(node4, "test")
        node5 = relay.add(begin6, begin7)
        end3 = compiler_end(node5, "test")
        end4 = compiler_end(node5, "test")
        node6 = relay.subtract(in_8, end3)
        begin8 = compiler_begin(in_9, "test")
        begin9 = compiler_begin(end4, "test")
        node7 = relay.add(begin8, begin9)
        end5 = compiler_end(node7, "test")

        begin10 = compiler_begin(node6, "test")
        begin11 = compiler_begin(end5, "test")
        node8 = relay.add(begin10, begin11)
        end6 = compiler_end(node8, "test")
        begin12 = compiler_begin(in_10, "test")
        begin13 = compiler_begin(end6, "test")
        node9 = relay.add(begin12, begin13)
        end7 = compiler_end(node9, "test")

        f = relay.Function([in_1, in_2, in_3, in_4, in_5, in_6, in_7, in_8, in_9, in_10], end7)
        mod = tvm.IRModule.from_expr(f)
        return mod

    def expected():
        in_1 = relay.var('in_1', shape=(10, 10), dtype='float32')
        in_2 = relay.var('in_2', shape=(10, 10), dtype='float32')
        in_3 = relay.var('in_3', shape=(10, 10), dtype='float32')
        in_4 = relay.var('in_4', shape=(10, 10), dtype='float32')
        in_5 = relay.var('in_5', shape=(10, 10), dtype='float32')
        in_6 = relay.var('in_6', shape=(10, 10), dtype='float32')
        in_7 = relay.var('in_7', shape=(10, 10), dtype='float32')
        in_8 = relay.var('in_8', shape=(10, 10), dtype='float32')
        in_9 = relay.var('in_9', shape=(10, 10), dtype='float32')
        in_10 = relay.var('in_10', shape=(10, 10), dtype='float32')

        begin0 = compiler_begin(in_1, "test")
        begin1 = compiler_begin(in_2, "test")
        begin2 = compiler_begin(in_3, "test")
        begin3 = compiler_begin(in_4, "test")
        node0 = relay.add(begin0, begin1)
        node1 = relay.add(begin2, begin3)
        node2 = relay.add(node0, node1)

        begin4 = compiler_begin(in_5, "default")
        begin5 = compiler_begin(in_6, "default")
        begin6 = compiler_begin(in_7, "default")
        node3 = relay.subtract(begin4, begin5)
        node4 = relay.subtract(begin6, node3)
        end0 = compiler_end(node4, "default")

        begin7 = compiler_begin(end0, "test")
        begin8 = compiler_begin(in_9, "test")

        node5 = relay.add(node2, begin7)
        end1 = compiler_end(node5, "test")

        begin9 = compiler_begin(end1, "default")
        begin10 = compiler_begin(in_8, "default")
        node6 = relay.subtract(begin10, begin9)
        end2 = compiler_end(node6, "default")

        node7 = relay.add(begin8, node5)
        end3 = compiler_end(node7, "test")
        begin11 = compiler_begin(end3, "test")
        begin12 = compiler_begin(end2, "test")

        node8 = relay.add(begin12, begin11)

        begin13 = compiler_begin(in_10, "test")
        node9 = relay.add(begin13, node8)
        end4 = compiler_end(node9, "test")

        f = relay.Function([in_1, in_2, in_3, in_4, in_5, in_6, in_7, in_8, in_9, in_10], end4)
        mod = tvm.IRModule.from_expr(f)
        return mod

    mod = annotated()
    mod = relay.transform.MergeCompilerRegions()(mod)
    ref_mod = expected()
    assert relay.analysis.alpha_equal(mod, ref_mod)


if __name__ == "__main__":
    test_diamond_graph_fanouts()
    test_example_graph()
