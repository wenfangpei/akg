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

"""cast"""
import akg
from akg.ops.math_gpu import cast
import akg.topi as topi

@akg.schedule(topi.cuda.schedule_injective)
def Cast(x, dst_type):
    """cast."""
    if x.dtype == "int64" and dst_type == "float16":
        x = cast.cast(x, "float32")
    if x.dtype == "float16" and dst_type == "int32":
        x = topi.trunc(x)
    return cast.cast(x, dst_type)
