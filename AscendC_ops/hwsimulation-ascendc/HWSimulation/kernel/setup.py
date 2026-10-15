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
"""Wheel packaging for the Hull-White short-rate simulation AscendC kernel.

The extension produced by cmake carries no `PyInit_` symbol (it registers
itself through TORCH_LIBRARY), so it cannot be imported as a plain extension
module.  It is therefore wrapped in a Python package: `_BuildExt` builds the
shared object, drops it next to a generated `__init__.py` under `build/hwm/`,
and points `build_lib` at the build tree so bdist_wheel picks up

    hwm/__init__.py
    hwm/_kernel.<ext-suffix>.so

and `import hwm` performs the `torch.ops.load_library` registration.
"""

import glob
import os
import shutil
import subprocess
from pathlib import Path

import setuptools
from setuptools.command.build_ext import build_ext
from torch_npu.utils.cpp_extension import NpuExtension

OP_NAME = "hwm"
EXT_NAME = "hwm_ext"

here = Path(__file__).resolve().parent
build_dir = here / "build"
pkg_dir = build_dir / OP_NAME

_INIT_PY = '''"""AscendC kernels for the Hull-White short-rate Monte-Carlo simulation.

Importing this package registers ``torch.ops.npu.hwm``.
"""

import glob as _glob
import os as _os

import torch as _torch

_DIR = _os.path.dirname(_os.path.abspath(__file__))
_LIBS = sorted(_glob.glob(_os.path.join(_DIR, "_kernel*.so")))
if not _LIBS:
    raise ImportError(f"no AscendC shared object found in {_DIR}")
_torch.ops.load_library(_LIBS[0])

__all__ = []
'''


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
            so_files = glob.glob(str(build_dir / f"{EXT_NAME}*.so"))

        # Wrap the shared object in an importable package.
        if so_files:
            pkg_dir.mkdir(parents=True, exist_ok=True)
            (pkg_dir / "__init__.py").write_text(_INIT_PY)
            suffix = Path(so_files[0]).suffix
            shutil.copyfile(so_files[0], pkg_dir / f"_kernel{suffix}")

        self.build_lib = str(build_dir)
        super().run()


setuptools.setup(
    name=OP_NAME,
    version="0.1.0",
    description="hwm AscendC kernel",
    ext_modules=[NpuExtension(EXT_NAME, sources=[])],
    cmdclass={"build_ext": _BuildExt},
    license="BSD 3-Clause",
    python_requires=">=3.8",
)
