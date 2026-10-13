#!/usr/bin/env python3
"""Turn an msprof PROF_GROUP directory into a flat pipeline-ratio dict.

Usage:
    python3 parse_msprof_pipeline.py <prof_root> <out_json>

Ratio keys emitted (duration-weighted average over the op_summary rows):
    aiv_mte2_ratio, aiv_mte3_ratio, aiv_vec_ratio, aiv_scalar_ratio,
    aic_mac_ratio, aic_mte2_ratio, kernel_ms_total
"""

import csv
import glob
import json
import os
import sys

RATIO_COLS = [
    "aiv_mte2_ratio", "aiv_mte3_ratio", "aiv_vec_ratio", "aiv_scalar_ratio",
    "aic_mac_ratio", "aic_mte2_ratio",
]


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def main():
    prof_root, out_json = sys.argv[1], sys.argv[2]
    csvs = glob.glob(os.path.join(prof_root, "**", "mindstudio_profiler_output",
                                  "op_summary_*.csv"), recursive=True)
    if not csvs:
        os.makedirs(os.path.dirname(out_json), exist_ok=True)
        with open(out_json, "w") as f:
            json.dump({"pipeline": None, "error": "no op_summary csv found"}, f)
        print("no op_summary csv found", file=sys.stderr)
        return 1

    # Prefer the plain op_summary_*.csv (base metrics) over per-metric variants.
    csvs.sort(key=lambda p: (len(os.path.basename(p)), p))
    path = csvs[0]

    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return 1

    keys = list(rows[0].keys())
    dur_col = None
    for cand in ("Task Duration(us)", "Task Duration(μs)", "Task Duration"):
        if cand in keys:
            dur_col = cand
            break
    present = [c for c in RATIO_COLS if c in keys]

    total_w = 0.0
    acc = {c: 0.0 for c in present}
    total_ms = 0.0
    for r in rows:
        d = num(r.get(dur_col, "")) if dur_col else None
        if d is None or d <= 0:
            d = 0.0
        total_ms += d / 1e3
        w = d if d > 0 else 1.0
        total_w += w
        for c in present:
            v = num(r.get(c, ""))
            if v is not None:
                acc[c] += v * w

    pipeline = {}
    if total_w > 0:
        for c in present:
            pipeline[c] = round(acc[c] / total_w, 5)
    pipeline["kernel_ms_total"] = round(total_ms, 3)

    os.makedirs(os.path.dirname(out_json), exist_ok=True)
    with open(out_json, "w") as f:
        json.dump({"pipeline": pipeline, "op_summary": path,
                   "n_rows": len(rows)}, f, indent=2)
    print(json.dumps(pipeline))
    return 0


if __name__ == "__main__":
    sys.exit(main())
