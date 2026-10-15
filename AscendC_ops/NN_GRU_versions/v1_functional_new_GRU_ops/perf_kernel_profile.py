#!/usr/bin/env python3
"""Single training step, for kernel-level profiling under msprof.

Runs WARMUP steps then exactly ONE measured forward+backward (no optimizer), so
the msprof op_summary captures one step's kernel mix. Env: same knobs as
perf_train_bench.py plus GRU_BWD_CHUNK.
"""

import glob
import os
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

os.environ.setdefault("ASCEND_RT_VISIBLE_DEVICES", os.environ.get("GRU_DEV", "3"))

import torch  # noqa: E402
import torch.nn as nn  # noqa: E402
import torch_npu  # noqa: E402

from model_new_ascendc import GRURegressorAscendC  # noqa: E402

_SO = os.environ.get("GRU_SO") or sorted(
    glob.glob(str(_HERE / "kernel" / "build" / "gru_ext*.so"))
)[-1]
torch.ops.load_library(_SO)

DEVICE = "npu:0"
B = int(os.environ.get("BATCH_SIZE", 8000))
S = int(os.environ.get("SEQ_LEN", 256))
I = int(os.environ.get("N_FEATURES", 128))
H = int(os.environ.get("HIDDEN_SIZE", 512))
WARMUP = int(os.environ.get("WARMUP", 2))


def main():
    g = torch.Generator().manual_seed(0)
    X = torch.randn(B, S, I, generator=g)
    y = X.mean(dim=(1, 2)).unsqueeze(1)

    torch.manual_seed(0)
    model = GRURegressorAscendC(I, H, 1).to(DEVICE)
    criterion = nn.MSELoss()
    X, y = X.to(DEVICE), y.to(DEVICE)
    model.train()

    def step():
        out = model(X)
        loss = criterion(out, y)
        loss.backward()

    for _ in range(WARMUP):
        step()
    torch.npu.synchronize()

    model.zero_grad(set_to_none=False)
    step()  # the profiled step
    torch.npu.synchronize()
    print("done")


if __name__ == "__main__":
    main()
