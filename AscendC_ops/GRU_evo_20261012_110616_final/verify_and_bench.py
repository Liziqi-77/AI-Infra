#!/usr/bin/env python3
"""Accuracy + device-side timing for one GRU variant.

Usage:
    python3 verify_and_bench.py --work-dir <parallel_dir> [--warmup 2] [--iters 3]
                                [--out evaluation_results.json]

Reads:
    <work-dir>/model.py           deterministic inputs / cases
    <work-dir>/golden/case_*.pt   frozen torch-fp32 reference outputs
    <work-dir>/kernel/build/*.so  the built extension
    <work-dir>/baseline_meta.json {"baseline_time_us": <float>}

Writes:
    <work-dir>/evaluation_results.json
"""

import argparse
import importlib.util
import json
import os
import sys
import time

import torch
import torch_npu  # noqa: F401


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    sys.modules[name] = m
    spec.loader.exec_module(m)
    return m


def _metrics(got, want):
    a = got.detach().to("cpu", torch.float32).reshape(-1)
    b = want.detach().to("cpu", torch.float32).reshape(-1)
    diff = (a - b).float()
    denom = b.norm().item()
    rel_l2 = (diff.norm().item() / denom) if denom > 0 else float(diff.norm().item())
    max_abs = diff.abs().max().item()
    cos = torch.nn.functional.cosine_similarity(a.unsqueeze(0), b.unsqueeze(0)).item()
    return {"rel_l2": rel_l2, "max_abs": max_abs, "cos": cos}


REL_L2_TOL = 1e-2
COS_TOL = 0.99
OUT_NAMES = ["out", "dW_ih", "dW_hh", "db_ih", "db_hh"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work-dir", required=True)
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--iters", type=int, default=3)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    work = os.path.abspath(args.work_dir)
    dev = torch.device("npu:0")

    ref_mod = _load(os.path.join(work, "model.py"), "gru_ref_inputs")
    new_mod = _load(os.path.join(work, "model_new_ascendc.py"), "gru_new")

    groups = ref_mod.get_input_groups()
    cases = ref_mod.CASES

    so_used = None
    compilation_success = True
    compile_message = ""
    try:
        so_used = new_mod._find_so()
    except Exception as e:  # noqa: BLE001
        compilation_success = False
        compile_message = f"{type(e).__name__}: {e}"

    per_case = []
    n_passed = 0
    times_us = []

    if compilation_success:
        model = new_mod.ModelNew().to(dev).eval()
        for idx, (x, w_ih, w_hh, b_ih, b_hh, grad_out) in enumerate(groups):
            entry = {"case": idx, "shape": [int(v) for v in x.shape],
                     "dtype": cases[idx][4], "passed": False, "error": ""}
            golden = torch.load(os.path.join(work, "golden", f"case_{idx}.pt"),
                                map_location="cpu", weights_only=False)
            xd = x.to(dev)
            wid = w_ih.to(dev)
            whd = w_hh.to(dev)
            bid = b_ih.to(dev)
            bhd = b_hh.to(dev)
            gd = grad_out.to(dev)
            try:
                with torch.no_grad():
                    res = model(xd, wid, whd, bid, bhd, gd)
                torch.npu.synchronize()
            except Exception as e:  # noqa: BLE001
                entry["error"] = f"run failed: {type(e).__name__}: {e}"
                per_case.append(entry)
                continue

            ok = True
            detail = {}
            for name, got in zip(OUT_NAMES, res):
                m = _metrics(got, golden[name])
                detail[name] = m
                if not (m["rel_l2"] <= REL_L2_TOL and m["cos"] >= COS_TOL):
                    ok = False
            entry["metrics"] = detail
            entry["passed"] = ok

            # ---- timing (fwd + bwd step) ----
            try:
                for _ in range(args.warmup):
                    with torch.no_grad():
                        model(xd, wid, whd, bid, bhd, gd)
                torch.npu.synchronize()
                best = None
                for _ in range(args.iters):
                    t0 = time.perf_counter()
                    with torch.no_grad():
                        model(xd, wid, whd, bid, bhd, gd)
                    torch.npu.synchronize()
                    dt = time.perf_counter() - t0
                    best = dt if best is None else min(best, dt)
                entry["time_ms"] = best * 1e3
                times_us.append(best * 1e6)
            except Exception as e:  # noqa: BLE001
                entry["error"] = (entry["error"] + " | " + f"timing failed: {e}").strip(" |")
                ok = False
                entry["passed"] = ok

            if ok:
                n_passed += 1
            per_case.append(entry)
            print(f"[verify] case {idx} shape={entry['shape']} {entry['dtype']} "
                  f"passed={entry['passed']} "
                  f"out_rel_l2={detail.get('out', {}).get('rel_l2', float('nan')):.3e} "
                  f"dWih_rel_l2={detail.get('dW_ih', {}).get('rel_l2', float('nan')):.3e} "
                  f"t={entry.get('time_ms', float('nan')):.3f} ms", flush=True)

    n_total = len(groups)
    precision_passed = (n_passed == n_total) and n_total > 0
    evolved_us = (sum(times_us) / len(times_us)) if times_us else -1.0

    baseline_meta_path = os.path.join(work, "baseline_meta.json")
    baseline_us = 0.0
    if os.path.exists(baseline_meta_path):
        with open(baseline_meta_path) as f:
            baseline_us = float(json.load(f).get("baseline_time_us", 0.0))
    speedup = (baseline_us / evolved_us) if (baseline_us > 0 and evolved_us > 0) else 0.0

    pipeline = None
    pipe_path = os.path.join(work, "profiling", "lingxi_msprof_summary.json")
    if os.path.exists(pipe_path):
        try:
            with open(pipe_path) as f:
                pipeline = json.load(f).get("pipeline")
        except Exception:  # noqa: BLE001
            pipeline = None

    result = {
        "tag": "evolved",
        "op_name": "GRU",
        "n_cases_total": n_total,
        "n_cases_passed": n_passed,
        "baseline": {"time_us": baseline_us},
        "evolved": {"time_us": round(evolved_us, 4), "pipeline": pipeline},
        "comparison": {
            "compilation_success": bool(compilation_success),
            "precision_passed": bool(precision_passed),
            "speedup": round(speedup, 6),
            "precision_message": compile_message if not compilation_success
                                 else (f"{n_passed}/{n_total} cases within tol"
                                       if not precision_passed else "all cases within tol"),
            "measurement_quality": "good",
        },
        "shared_object": so_used,
        "per_case": per_case,
    }

    out_path = args.out or os.path.join(work, "evaluation_results.json")
    with open(out_path, "w") as f:
        json.dump(result, f, indent=2)
    print(f"[verify] wrote {out_path}  compiled={compilation_success} "
          f"precision={precision_passed} time_us={evolved_us:.1f} speedup={speedup:.4f}x",
          flush=True)
    return 0 if compilation_success else 2


if __name__ == "__main__":
    sys.exit(main())
