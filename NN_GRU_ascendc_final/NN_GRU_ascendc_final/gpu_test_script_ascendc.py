#!/usr/bin/env python3
"""End-to-end training of the GRU regressor on AscendC kernels.

GRU layer -> fused `torch.ops.npu.gru` (forward) + `torch.ops.npu.gru_backward`
(BPTT + parameter gradients). Head -> `nn.Linear`. MSELoss + Adam.

Self-contained: it depends only on files inside this directory (`model_new_ascendc.py`
and `kernel/build/gru_ext*.so`).

Env knobs (all optional):
    GRU_SO          path to gru_ext*.so   (default: kernel/build/gru_ext*.so)
    GRU_DEV         npu device id         (default 3)
    BATCH_SIZE      default 8000
    SEQ_LEN         default 256
    N_FEATURES      default 128
    HIDDEN_SIZE     default 512
    OUTPUT_SIZE     default 1
    TRAIN_STEPS     default 500
    LR              default 1e-3
    DATA_SEED       default 0
    REPORT_EVERY    print every N steps (default 10)
"""

import glob
import os
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

_SO = os.environ.get("GRU_SO") or sorted(glob.glob(str(_HERE / "kernel" / "build" / "gru_ext*.so")))[-1]
torch.ops.load_library(_SO)

DEVICE = "npu:0"
DTYPE = torch.float32  # the backward kernels are the fp32 path

BATCH_SIZE = int(os.environ.get("BATCH_SIZE", 8000))
SEQ_LEN = int(os.environ.get("SEQ_LEN", 256))
N_FEATURES = int(os.environ.get("N_FEATURES", 128))
HIDDEN_SIZE = int(os.environ.get("HIDDEN_SIZE", 512))
OUTPUT_SIZE = int(os.environ.get("OUTPUT_SIZE", 1))
TRAIN_STEPS = int(os.environ.get("TRAIN_STEPS", 500))
LR = float(os.environ.get("LR", 1e-3))
DATA_SEED = int(os.environ.get("DATA_SEED", 0))
REPORT_EVERY = int(os.environ.get("REPORT_EVERY", 10))


def generate_data(batch_size, seq_len, n_features):
    g = torch.Generator().manual_seed(DATA_SEED)
    X = torch.randn(batch_size, seq_len, n_features, generator=g)
    y = X.mean(dim=(1, 2), keepdim=False).unsqueeze(1) + 0.1 * torch.randn(
        batch_size, 1, generator=g
    )
    return X, y


def snapshot(model):
    return {n: p.detach().clone() for n, p in model.named_parameters()}


def max_delta(before, model):
    rows = []
    for n, p in model.named_parameters():
        d = (p.detach() - before[n]).abs().max().item()
        rows.append((n, d))
    return rows


def main():
    print(f"so        : {_SO}")
    print(f"device    : {DEVICE}  dtype={DTYPE}")
    print(f"config    : batch={BATCH_SIZE} seq={SEQ_LEN} I={N_FEATURES} H={HIDDEN_SIZE} "
          f"steps={TRAIN_STEPS} lr={LR}")

    X_data, y_data = generate_data(BATCH_SIZE, SEQ_LEN, N_FEATURES)

    torch.manual_seed(DATA_SEED)
    model = GRURegressorAscendC(N_FEATURES, HIDDEN_SIZE, OUTPUT_SIZE).to(DEVICE)

    criterion = nn.MSELoss()
    optimizer = optim.Adam(model.parameters(), lr=LR, eps=1e-4)

    X_data, y_data = X_data.to(DEVICE), y_data.to(DEVICE)
    model.train()

    before = snapshot(model)

    torch.npu.synchronize()
    torch.npu.reset_peak_memory_stats(DEVICE)

    start_time = time.time()
    first_loss = None
    last_loss = None
    for step in range(TRAIN_STEPS):
        optimizer.zero_grad()
        outputs = model(X_data)
        loss = criterion(outputs, y_data)
        loss.backward()
        optimizer.step()
        if step == 0:
            first_loss = loss.item()
        last_loss = loss.item()
        if REPORT_EVERY and (step % REPORT_EVERY == 0 or step == TRAIN_STEPS - 1):
            print(f"  step {step:5d}  loss={loss.item():.8f}")
    torch.npu.synchronize()
    elapsed_time = time.time() - start_time

    peak_allocated = torch.npu.max_memory_allocated(DEVICE)
    peak_reserved = torch.npu.max_memory_reserved(DEVICE)

    print()
    print(f"GRU kernel : AscendC (torch.ops.npu.gru + torch.ops.npu.gru_backward) dtype={DTYPE}")
    print(f"first loss : {first_loss:.8f}")
    print(f"final loss : {last_loss:.8f}")
    print(f"loss drop  : {first_loss - last_loss:.8f}")
    print(f"total time : {elapsed_time:.4f} s")
    print(f"per step   : {elapsed_time / max(TRAIN_STEPS, 1):.6f} s")
    print(f"peak mem   : allocated={peak_allocated / 1024**2:.2f} MB  "
          f"reserved={peak_reserved / 1024**2:.2f} MB")

    print("\nparameter movement (max |after - before|):")
    for n, d in max_delta(before, model):
        print(f"  {n:<28} {d:.6e}")


if __name__ == "__main__":
    main()
