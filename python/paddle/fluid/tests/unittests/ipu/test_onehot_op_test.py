#  Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import unittest

import numpy as np
import paddle
import paddle.fluid.compiler as compiler
import paddle.fluid.contrib.mixed_precision.fp16_utils as fp16_utils
from paddle.fluid.tests.unittests.ipu.op_test_ipu import IPUOpTest, ExecutionMode


@unittest.skipIf(not paddle.is_compiled_with_ipu(),
                 "core is not compiled with IPU")
class TestBase(IPUOpTest):
    def setUp(self):
        self.set_atol()
        self.set_training()
        self.set_data_feed()
        self.set_feed_attr()
        self.set_op_attrs()

    @property
    def fp16_enabled(self):
        return True

    def set_data_feed(self):
        data1 = np.array([[1], [1], [3], [0]])

        self.feed = {'x': data1.astype(np.int32)}

    def set_feed_attr(self):
        self.feed_shape = [x.shape for x in self.feed.values()]
        self.feed_list = list(self.feed.keys())

    def set_op_attrs(self):
        self.attrs = {"depth": 4, "allow_out_of_range": False}

    def _test_base(self, exec_mode):
        scope = paddle.fluid.core.Scope()
        main_prog = paddle.static.Program()
        startup_prog = paddle.static.Program()
        main_prog.random_seed = self.SEED
        startup_prog.random_seed = self.SEED

        with paddle.fluid.scope_guard(scope):
            with paddle.static.program_guard(main_prog, startup_prog):
                x = paddle.static.data(
                    name=self.feed_list[0],
                    shape=self.feed_shape[0],
                    dtype='int32')

                with paddle.static.amp.fp16_guard():
                    out = paddle.fluid.layers.one_hot(x, **self.attrs)

                fetch_list = [out.name]

            if exec_mode == ExecutionMode.CPU_FP32:
                place = paddle.CPUPlace()
            else:
                place = paddle.IPUPlace()

            if exec_mode == ExecutionMode.IPU_PADDLE_FP16:
                fp16_utils.rewrite_program_v2(
                    startup_prog=startup_prog,
                    main_prog=main_prog,
                    amp_lists=self.amp_list)

            exe = paddle.static.Executor(place)
            exe.run(startup_prog)

            if exec_mode != ExecutionMode.CPU_FP32:
                feed_list = self.feed_list
                ipu_strategy = compiler.get_ipu_strategy()
                ipu_strategy.is_training = self.is_training
                if exec_mode == ExecutionMode.IPU_POPART_FP16:
                    ipu_strategy.enable_fp16 = True
                program = compiler.IpuCompiler(
                    main_prog,
                    ipu_strategy=ipu_strategy).compile(feed_list, fetch_list)
            else:
                program = main_prog

            feed = self.feed

            result = exe.run(program, feed=feed, fetch_list=fetch_list)

            return result[0]

    def test_base(self):
        output_dict = {}
        for mode in ExecutionMode:
            if (mode > ExecutionMode.IPU_FP32 and not self.fp16_enabled):
                break
            output_dict[mode] = self._test_base(mode).flatten()

        self.check(output_dict)


@unittest.skip('')
class TestCase1(TestBase):
    def set_op_attrs(self):
        self.attrs = {"depth": 4, "allow_out_of_range": True}


if __name__ == "__main__":
    unittest.main()