#!/usr/bin/env python3
"""msprof target: 1 forward + 1 backward at the production shape (backward pipe check)."""
import os, time
os.environ.setdefault("ASCEND_RT_VISIBLE_DEVICES", "0")
import torch, torch_npu
torch.ops.load_library(os.environ["GRU_SO"])
B, S, I, H = 8000, 256, 128, 512
torch.manual_seed(0)
x = torch.randn(B, S, I).npu(); w_ih = torch.randn(3*H, I).npu()
w_hh = torch.randn(3*H, H).npu(); b_ih = torch.randn(3*H).npu(); b_hh = torch.randn(3*H).npu()
out = torch.ops.npu.gru(x, w_ih, w_hh, b_ih, b_hh)
g = torch.randn(B, S, H).npu()
t0 = time.time()
gw = torch.ops.npu.gru_backward(g, x, out, w_ih, w_hh, b_ih, b_hh)
torch.npu.synchronize()
print("bwd done in %.1f ms" % ((time.time() - t0) * 1e3), flush=True)
time.sleep(0.2)
