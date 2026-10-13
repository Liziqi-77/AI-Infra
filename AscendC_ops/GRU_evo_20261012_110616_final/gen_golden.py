#!/usr/bin/env python3
"""One-time setup: emit test cases (GRU.json / .bak / call_spec.json) and the
frozen golden outputs (torch fp32 reference) into shared/.

Run once, on the NPU, before the evolution rounds:

    python3 gen_golden.py
"""

import json
import os
import sys

import torch
import torch_npu  # noqa: F401  (registers the NPU backend)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from model import CASES, get_input_groups, gru_recurrence  # noqa: E402

DT_STR = {"fp32": "fp32", "fp16": "fp16", "bf16": "bf16"}


def write_cases():
    lines = []
    for (B, S, I, H, name) in CASES:
        case = {
            "inputs": [
                {"name": "x", "type": "tensor", "shape": [B, S, I], "dtype": DT_STR[name]},
                {"name": "w_ih", "type": "tensor", "shape": [3 * H, I], "dtype": DT_STR[name]},
                {"name": "w_hh", "type": "tensor", "shape": [3 * H, H], "dtype": DT_STR[name]},
                {"name": "b_ih", "type": "tensor", "shape": [3 * H], "dtype": DT_STR[name]},
                {"name": "b_hh", "type": "tensor", "shape": [3 * H], "dtype": DT_STR[name]},
                {"name": "grad_out", "type": "tensor", "shape": [B, S, H], "dtype": "fp32"},
            ]
        }
        lines.append(json.dumps(case))
    body = "\n".join(lines) + "\n"
    with open(os.path.join(HERE, "GRU.json"), "w") as f:
        f.write(body)
    with open(os.path.join(HERE, "GRU.json.bak"), "w") as f:
        f.write(body)
    print(f"[gen_golden] wrote GRU.json / GRU.json.bak ({len(lines)} cases)")

    spec = {
        "inputs": [
            {"name": "x", "shape": [CASES[0][0], CASES[0][1], CASES[0][2]], "dtype": "float32"},
        ],
        "scalar_args": {},
        "tensor_kwargs": {},
        "case_count": len(lines),
    }
    with open(os.path.join(HERE, "call_spec.json"), "w") as f:
        json.dump(spec, f, indent=2)
    print("[gen_golden] wrote call_spec.json")


def write_golden():
    dev = torch.device("npu:0")
    gdir = os.path.join(HERE, "golden")
    os.makedirs(gdir, exist_ok=True)
    groups = get_input_groups()
    for idx, (x, w_ih, w_hh, b_ih, b_hh, grad_out) in enumerate(groups):
        xd = x.to(dev)
        wid = w_ih.to(torch.float32).to(dev).requires_grad_(True)
        whd = w_hh.to(torch.float32).to(dev).requires_grad_(True)
        bid = b_ih.to(torch.float32).to(dev).requires_grad_(True)
        bhd = b_hh.to(torch.float32).to(dev).requires_grad_(True)
        with torch.enable_grad():
            out = gru_recurrence(xd, wid, whd, bid, bhd)
            grads = torch.autograd.grad(out, [wid, whd, bid, bhd],
                                        grad_outputs=grad_out.to(dev))
        payload = {
            "out": out.detach().to(torch.float32).cpu(),
            "dW_ih": grads[0].detach().cpu(),
            "dW_hh": grads[1].detach().cpu(),
            "db_ih": grads[2].detach().cpu(),
            "db_hh": grads[3].detach().cpu(),
        }
        torch.save(payload, os.path.join(gdir, f"case_{idx}.pt"))
        print(f"[gen_golden] case {idx} B={CASES[idx][0]} S={CASES[idx][1]} "
              f"H={CASES[idx][3]} {CASES[idx][4]}: out {tuple(payload['out'].shape)} "
              f"|out|max={payload['out'].abs().max().item():.4e}")


if __name__ == "__main__":
    write_cases()
    write_golden()
    print("[gen_golden] done")
