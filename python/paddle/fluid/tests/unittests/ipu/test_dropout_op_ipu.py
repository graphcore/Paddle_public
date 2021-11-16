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
        self.set_feed()
        self.set_feed_attr()
        self.set_attrs()

    def set_feed(self):
        data = np.random.uniform(size=[1, 3, 10, 10])
        self.feed_fp32 = {'x': data.astype(np.float32)}
        self.feed_fp16 = {'x': data.astype(np.float16)}

    def set_feed_attr(self):
        self.feed_shape = [x.shape for x in self.feed_fp32.values()]
        self.feed_list = list(self.feed_fp32.keys())

    def set_attrs(self):
        self.attrs = {
            "dropout_prob": 0.5,
            "is_test": True,
            "dropout_implementation": "downgrade_in_infer"
        }

    def _test_base(self, run_mode=0):
        scope = paddle.fluid.core.Scope()
        main_prog = paddle.static.Program()
        startup_prog = paddle.static.Program()
        SEED = self.SEED
        main_prog.random_seed = SEED
        startup_prog.random_seed = SEED
        dtype = 'float32' if run_mode != self.TEST_IPU_FP16 else 'float16'
        feed = self.feed_fp16 if run_mode == self.TEST_IPU_FP16 else self.feed_fp32

        with paddle.fluid.scope_guard(scope):
            with paddle.static.program_guard(main_prog, startup_prog):
                with paddle.static.amp.fp16_guard():
                    x = paddle.static.data(
                        name=self.feed_list[0],
                        shape=self.feed_shape[0],
                        dtype=dtype)
                    dropout = paddle.fluid.layers.dropout(x, **self.attrs)
                    out = paddle.fluid.layers.elementwise_add(dropout, dropout)

                fetch_list = [out.name]

            if run_mode != self.TEST_CPU_FP32:
                place = paddle.IPUPlace()
            else:
                place = paddle.CPUPlace()
            if run_mode == self.TEST_IPU_FP16:
                fp16_utils.rewrite_program_v2(
                    startup_prog=startup_prog,
                    main_prog=main_prog,
                    amp_lists=self.amp_list)
            exe = paddle.static.Executor(place)
            exe.run(startup_prog)

            if run_mode != self.TEST_CPU_FP32:
                feed_list = self.feed_list
                ipu_strategy = compiler.get_ipu_strategy()
                ipu_strategy.is_training = self.is_training
                program = compiler.IpuCompiler(
                    main_prog,
                    ipu_strategy=ipu_strategy).compile(feed_list, fetch_list)
            else:
                program = main_prog

            result = exe.run(program, feed=feed, fetch_list=fetch_list)
            return result[0]

    def test_base(self):
        res0 = self._test_base(self.TEST_CPU_FP32)
        res1 = self._test_base(self.TEST_IPU_FP32)
        res2 = self._test_base(self.TEST_IPU_FP16)

        self.assertTrue(
            np.allclose(
                res0.flatten(), res1.flatten(), atol=self.atol))
        self.assertTrue(
            np.allclose(
                res1.flatten(), res2.flatten(), atol=self.atol_fp16))


class TestCase1(TestBase):
    def set_attrs(self):
        self.attrs = {
            "dropout_prob": 0.5,
            "is_test": True,
            "dropout_implementation": "upscale_in_train"
        }


class TestCase2(TestBase):
    def set_attrs(self):
        self.attrs = {
            "dropout_prob": 0.0,
            "is_test": False,
            "dropout_implementation": "upscale_in_train"
        }


if __name__ == "__main__":
    unittest.main()
