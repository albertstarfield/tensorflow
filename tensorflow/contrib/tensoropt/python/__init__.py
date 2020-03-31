# Copyright 2018 The TensorFlow Authors. All Rights Reserved.
# Copyright (C) Codeplay Software Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# =============================================================================
"""Exposes the python wrapper for TensorOpt graph transforms."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

# pylint: disable=unused-import,line-too-long
from tensorflow.contrib.tensoropt.python.ops import topt_engine_op
from tensorflow.contrib.tensoropt.python.topt_convert import create_inference_graph
# pylint: enable=unused-import,line-too-long