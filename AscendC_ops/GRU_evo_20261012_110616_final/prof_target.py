#!/usr/bin/env python3
"""msprof target: fwd + bwd of the fused GRU at the production shape (case 0)."""

import importlib.util
import os
import sys
import time

os.environ.setdefault("ASCEND_RT_VISIBLE_DEVICES", "0")

import torch
import torch_npu  # noqa: F401

WORK = os.path.dirname(os.path.abspath(__file__))
ITERS = int(os.environ.get("PROF_ITERS", "3"))


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    sys.modules[name] = m
    spec.loader.exec_module(m)
    return m


ref = _load(os.path.join(WORK, "model.py"), "gru_ref_inputs")
new = _load(os.path.join(WORK, "model_new_ascendc.py"), "gru_new")

dev = torch.device("npu:0")
x, w_ih, w_hh, b_ih, b_hh, grad_out = ref.get_input_groups()[0]
x, w_ih, w_hh, b_ih, b_hh, grad_out = (t.to(dev) for t in (x, w_ih, w_hh, b_ih, b_hh, grad_out))

model = new.ModelNew().to(dev).eval()
for _ in range(ITERS):
    with torch.no_grad():
        model(x, w_ih, w_hh, b_ih, b_hh, grad_out)
torch.npu.synchronize()
print("prof_target done", flush=True)
time.sleep(0.2)
