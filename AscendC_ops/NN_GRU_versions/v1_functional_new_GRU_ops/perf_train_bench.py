#!/usr/bin/env python3
"""Training-step benchmark harness for the AscendC GRU (fp32).

Measures the three phases of one training step separately so a regression can be
attributed to forward vs backward:

    forward   : model(X)                 (no grad, fused torch.ops.npu.gru)
    backward  : loss.backward()          (BPTT + param grads)
    opt step  : optimizer.step()         (Adam)

Protocol (per the task's timing rules):
    * >= 1 warmup step (default 2), then >= 3 measured steps (default 5)
    * torch.npu.synchronize() on both sides of every timed region
    * report min / median / max to expose noise

Env knobs:
    GRU_SO, GRU_DEV, BATCH_SIZE(8000), SEQ_LEN(256), N_FEATURES(128),
    HIDDEN_SIZE(512), WARMUP(2), REPEATS(5), DATA_SEED(0)

Prints one JSON line prefixed with RESULT_JSON so callers can parse it.
"""

import glob
import json
import os
import statistics
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

os.environ.setdefault("ASCEND_RT_VISIBLE_DEVICES", os.environ.get("GRU_DEV", "3"))

import torch  # noqa: E402
import torch.nn as nn  # noqa: E402
import torch.optim as optim  # noqa: E402
import torch_npu  # noqa: E402  (registers the npu device)

from model_new_ascendc import GRURegressorAscendC  # noqa: E402

_SO = os.environ.get("GRU_SO") or sorted(
    glob.glob(str(_HERE / "kernel" / "build" / "gru_ext*.so"))
)[-1]
torch.ops.load_library(_SO)

DEVICE = "npu:0"
DTYPE = torch.float32

BATCH_SIZE = int(os.environ.get("BATCH_SIZE", 8000))
SEQ_LEN = int(os.environ.get("SEQ_LEN", 256))
N_FEATURES = int(os.environ.get("N_FEATURES", 128))
HIDDEN_SIZE = int(os.environ.get("HIDDEN_SIZE", 512))
WARMUP = int(os.environ.get("WARMUP", 2))
REPEATS = int(os.environ.get("REPEATS", 5))
DATA_SEED = int(os.environ.get("DATA_SEED", 0))


def _stat(xs):
    return {
        "min": min(xs),
        "median": statistics.median(xs),
        "max": max(xs),
        "mean": sum(xs) / len(xs),
        "n": len(xs),
    }


def main():
    g = torch.Generator().manual_seed(DATA_SEED)
    X = torch.randn(BATCH_SIZE, SEQ_LEN, N_FEATURES, generator=g)
    y = X.mean(dim=(1, 2), keepdim=False).unsqueeze(1) + 0.1 * torch.randn(
        BATCH_SIZE, 1, generator=g
    )

    torch.manual_seed(DATA_SEED)
    model = GRURegressorAscendC(N_FEATURES, HIDDEN_SIZE, 1).to(DEVICE)
    criterion = nn.MSELoss()
    optimizer = optim.Adam(model.parameters(), lr=1e-3, eps=1e-4)

    X, y = X.to(DEVICE), y.to(DEVICE)
    model.train()

    fwd_t, bwd_t, step_t = [], [], []

    def one_step(measure):
        if measure:
            torch.npu.synchronize()
            t0 = time.time()
        out = model(X)
        if measure:
            torch.npu.synchronize()
            dt_f = time.time() - t0
        loss = criterion(out, y)
        if measure:
            torch.npu.synchronize()
            t1 = time.time()
        optimizer.zero_grad()
        loss.backward()
        if measure:
            torch.npu.synchronize()
            dt_b = time.time() - t1
        optimizer.step()
        if measure:
            torch.npu.synchronize()
            dt_s = time.time() - t0
            return dt_f, dt_b, dt_s, loss.item()
        return 0.0, 0.0, 0.0, loss.item()

    for _ in range(WARMUP):
        one_step(False)

    torch.npu.reset_peak_memory_stats(DEVICE)
    for _ in range(REPEATS):
        df, db, ds, _ = one_step(True)
        fwd_t.append(df)
        bwd_t.append(db)
        step_t.append(ds)

    peak_alloc = torch.npu.max_memory_allocated(DEVICE) / 1024**2
    peak_res = torch.npu.max_memory_reserved(DEVICE) / 1024**2

    res = {
        "so": _SO,
        "config": {"B": BATCH_SIZE, "S": SEQ_LEN, "I": N_FEATURES, "H": HIDDEN_SIZE},
        "warmup": WARMUP,
        "repeats": REPEATS,
        "forward_s": _stat(fwd_t),
        "backward_s": _stat(bwd_t),
        "step_s": _stat(step_t),
        "peak_allocated_mb": peak_alloc,
        "peak_reserved_mb": peak_res,
    }
    print("RESULT_JSON " + json.dumps(res))


if __name__ == "__main__":
    main()
