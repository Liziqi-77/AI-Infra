#!/usr/bin/env python3
"""Hull-White short-rate Monte-Carlo path simulation --- reference implementation.

This is the PyTorch re-expression of the CUDA kernel in ``GPU_Test/kernel.cu``
(``HWMCKernel``), which is the per-path short-rate simulation stage of the
CallableCLN pricing program (SOFR discounting + BNP/BOC credit, 10Y callable CLN,
16 paths x 10 steps).

CUDA kernel signature
---------------------
    HWMCKernel(const int* step, const double* hwparams, const double* accrual,
               const double* f1, const double* f2, const double* nrnd, double* rate)

    i = path index (0 .. NPath-1, one thread per path)
    rate[i][0] = r0
    for j in 0 .. MStep-2:
        dt        = accrual[j+1] - accrual[j]
        t1, t2    = accrual[j], accrual[j+1]
        F1, F2    = f1[j], f2[j]
        alpha_k   = 0.5 * ((sig/a) * (1 - exp(-a*t_k)))^2 + F_k        (k = 1, 2)
        Variance  = 0.5*sig^2/a * (1 - exp(-2*a*dt))        (sig^2*dt if a ~ 0)
        StdDev    = sqrt(Variance)
        Expect    = rate[i][j]*exp(-a*dt) + alpha_2 - alpha_1*exp(-a*dt)
        rate[i][j+1] = Expect + StdDev * nrnd[i][j]

The recurrence is sequential along the time axis and fully independent across
paths, so the whole tensor is computed with one vector operation per time step,
acting over the path axis.

dtype
-----
The original kernel runs in ``double``.  The AscendC target has no fp64 vector
path, so this reference and the candidate both evaluate in ``float32``; the
1e-2 absolute tolerance of the fp32 accuracy standard absorbs the ~1e-7
round-off this introduces (values are of order 1e-2 .. 1e-1).
"""

import json
import math
import os

import torch
import torch.nn as nn

CASE_JSON = "kernel_case.json"


class Model(nn.Module):
    """Hull-White short-rate path simulation (one vector step per time index)."""

    def __init__(self):
        super(Model, self).__init__()

    def forward(self, step, hwparams, accrual, f1, f2, nrnd):
        """
        Args:
            step      (torch.Tensor): int[1]     number of time steps MStep
            hwparams  (torch.Tensor): float[3]   {r0, a (mean reversion), sigma (vol)}
            accrual   (torch.Tensor): float[MStep]
            f1        (torch.Tensor): float[MStep-1]
            f2        (torch.Tensor): float[MStep-1]
            nrnd      (torch.Tensor): float[NPath, MStep-1]  standard-normal draws

        Returns:
            torch.Tensor: rate, float[NPath, MStep]
        """
        n_path = nrnd.shape[0]
        m_step = int(step.reshape(-1)[0].item())
        dtype = nrnd.dtype
        device = nrnd.device

        r0 = hwparams[0]
        a = hwparams[1]
        sig = hwparams[2]

        rate = torch.empty((n_path, m_step), dtype=dtype, device=device)
        rate[:, 0] = r0
        for j in range(m_step - 1):
            dt = accrual[j + 1] - accrual[j]
            t1 = accrual[j]
            t2 = accrual[j + 1]
            f_1 = f1[j]
            f_2 = f2[j]

            alpha_2 = (sig / a) * (1.0 - torch.exp(-a * t2))
            alpha_2 = alpha_2 * 0.5 * alpha_2 + f_2
            alpha_1 = (sig / a) * (1.0 - torch.exp(-a * t1))
            alpha_1 = alpha_1 * 0.5 * alpha_1 + f_1

            decay = torch.exp(-a * dt)
            if float(a) < 1e-10:
                variance = sig * sig * dt
            else:
                variance = 0.5 * sig * sig / a * (1.0 - torch.exp(-2.0 * a * dt))
            std_dev = torch.sqrt(variance)

            expect = rate[:, j] * decay + alpha_2 - alpha_1 * decay
            rate[:, j + 1] = expect + std_dev * nrnd[:, j]
        return rate


def get_input_groups():
    json_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), CASE_JSON)
    with open(json_path, "r") as f:
        case = json.load(f)

    inputs = case["inputs"]
    step = torch.tensor(inputs["step"], dtype=torch.int32)
    hwparams = torch.tensor(inputs["hwparams"], dtype=torch.float32)
    accrual = torch.tensor(inputs["accrual"], dtype=torch.float32)
    f1 = torch.tensor(inputs["f1"], dtype=torch.float32)
    f2 = torch.tensor(inputs["f2"], dtype=torch.float32)
    nrnd = torch.tensor(inputs["nrnd"], dtype=torch.float32)
    return [[step, hwparams, accrual, f1, f2, nrnd]]


def get_init_inputs():
    return []


if __name__ == "__main__":
    m = Model()
    for group in get_input_groups():
        out = m(*group)
        print("rate", tuple(out.shape), out.dtype)
        print(out[0, :4])
