#!/usr/bin/env python3
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Wheel packaging for the fused GRU AscendC kernels.

Self-contained equivalent of `workflows/templates/archive_tasks/_kernel_setup_common.py`:
builds `kernel/build/gru_ext*.so` with cmake+make when absent, then lets
bdist_wheel pick the artifact up from `build_lib`.

The produced extension has no `PyInit_` symbol (it registers itself through
TORCH_LIBRARY), so it is loaded with `torch.ops.load_library` rather than
`import`.
"""

import glob
import os
import subprocess
from pathlib import Path

import setuptools
from setuptools.command.build_ext import build_ext
from torch_npu.utils.cpp_extension import NpuExtension

OP_NAME = "gru"
EXT_NAME = "gru_ext"

here = Path(__file__).resolve().parent
build_dir = here / "build"


class _BuildExt(build_ext):
    def run(self):
        so_files = glob.glob(str(build_dir / f"{EXT_NAME}*.so"))
        if not so_files:
            os.makedirs(build_dir, exist_ok=True)
            soc_ver = os.environ.get("SOC_VERSION", "Ascend910B2")
            ascend = os.environ.get("ASCEND_HOME_PATH", "")
            subprocess.check_call(
                ["cmake", str(here),
                 f"-DSOC_VERSION={soc_ver}",
                 f"-DASCEND_CANN_PACKAGE_PATH={ascend}",
                 "-DCMAKE_BUILD_TYPE=Release"],
                cwd=build_dir)
            subprocess.check_call(["make", f"-j{os.cpu_count()}"], cwd=build_dir)

        self.build_lib = str(build_dir)
        super().run()


setuptools.setup(
    name=OP_NAME,
    version="0.1.0",
    description="gru AscendC kernel",
    ext_modules=[NpuExtension(EXT_NAME, sources=[])],
    cmdclass={"build_ext": _BuildExt},
    license="BSD 3-Clause",
    python_requires=">=3.8",
)
