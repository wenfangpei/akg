# Copyright 2020 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""fused_relu_grad_bn_update_grad"""
from __future__ import absolute_import
import akg
import akg.topi as topi
from akg.ops.fused_gpu.fused_relu_grad_bn_update_grad import fused_relu_grad_bn_update_grad

@akg.schedule(topi.cuda.schedule_reduce)
def fused_relu_grad_bn_update_grad_manual(data_sum, in_bn, head_active, in_active, layout="NHWC",  out_dtype="float32"):
    """fused_relu_grad_bn_update_grad with manual schedule."""
    return fused_relu_grad_bn_update_grad(data_sum, in_bn, head_active, in_active, layout, out_dtype)

def fused_relu_grad_bn_update_grad_auto(data_sum, in_bn, head_active, in_active, layout="NHWC", out_dtype="float32"):
    """fused_relu_grad_bn_update_grad with auto schedule."""
    return fused_relu_grad_bn_update_grad(data_sum, in_bn, head_active, in_active, layout, out_dtype)
