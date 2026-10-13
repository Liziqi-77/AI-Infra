#!/usr/bin/env python3
"""AscendC implementation wrapper for the fused GRU operator.

Loads the torch extension built from `kernel/` (build/kernel/build_a5.sh) and
dispatches to the two registered ops:

    torch.ops.npu.gru(x, w_ih, w_hh, b_ih, b_hh) -> out
    torch.ops.npu.gru_backward(grad_out, x, out, w_ih, w_hh, b_ih, b_hh)
        -> (dW_ih, dW_hh, db_ih, db_hh)

`ModelNew.forward` mirrors `Model.forward` in model.py: it returns the 5-tuple
(out, dW_ih, dW_hh, db_ih, db_hh) so the fused forward + backward step is the
unit of work for both accuracy and timing.

The input/output/parameter signatures are FROZEN — they must not change.
"""

import glob
import os
import torch
import torch.nn as nn

_SO_CACHE = None


def _find_so():
    """Locate the extension .so built from this variant's kernel/ tree."""
    global _SO_CACHE
    if _SO_CACHE is not None:
        return _SO_CACHE
    here = os.path.dirname(os.path.abspath(__file__))
    patterns = [
        os.path.join(here, "kernel", "build", "gru_ext*.so"),
        os.path.join(here, "kernel", "build", "*", "gru_ext*.so"),
    ]
    found = []
    for p in patterns:
        found.extend(glob.glob(p))
    if not found:
        # Fall back: any gru_ext*.so under this variant directory.
        found = glob.glob(os.path.join(here, "**", "gru_ext*.so"), recursive=True)
    if not found:
        raise RuntimeError(
            "model_new_ascendc: no gru_ext*.so found; build kernel/ first "
            "(bash kernel/build_a5.sh --clean)"
        )
    so = sorted(found)[0]
    torch.ops.load_library(so)
    _SO_CACHE = so
    return so


class ModelNew(nn.Module):
    def forward(self, x, w_ih, w_hh, b_ih, b_hh, grad_out):
        _find_so()
        out = torch.ops.npu.gru(x, w_ih, w_hh, b_ih, b_hh)
        dW_ih, dW_hh, db_ih, db_hh = torch.ops.npu.gru_backward(
            grad_out.to(torch.float32),
            x.to(torch.float32),
            out.to(torch.float32),
            w_ih.to(torch.float32),
            w_hh.to(torch.float32),
            b_ih.to(torch.float32),
            b_hh.to(torch.float32),
        )
        return out, dW_ih, dW_hh, db_ih, db_hh
