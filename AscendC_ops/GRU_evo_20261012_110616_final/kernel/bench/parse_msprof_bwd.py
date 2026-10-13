#!/usr/bin/env python3
"""Backward msprof summary: per-op count/avg + AIV/AIC pipe breakdown of the MIX task.

Usage: parse_msprof_bwd.py <msprof_output_dir> [label]
"""
import csv, glob, os, sys, statistics as st

root, label = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "run")
d = glob.glob(os.path.join(root, "*", "mindstudio_profiler_output")) or \
    glob.glob(os.path.join(root, "mindstudio_profiler_output"))
d = d[0]

print(f"### {label}")
print("-- op_statistic --")
for f in glob.glob(os.path.join(d, "op_statistic_*.csv")):
    for row in csv.DictReader(open(f)):
        if "gru" in row.get("OP Type", ""):
            print("  {OP Type:<22} {Core Type:<12} count={Count:<6} total={Total Time(us):>11}"
                  " min={Min Time(us):>9} avg={Avg Time(us):>9} max={Max Time(us):>10}".format(**row))

rows = []
for f in glob.glob(os.path.join(d, "op_summary_*.csv")):
    rows += list(csv.DictReader(open(f)))
by_op = {}
for r in rows:
    by_op.setdefault(r["OP Type"], []).append(r)

def mean(rs, field):
    v = [float(r[field]) for r in rs if r.get(field) not in (None, "", "N/A")]
    return st.mean(v) if v else float("nan")

print("-- op_summary (per op) --")
for op, rs in sorted(by_op.items()):
    if "gru" not in op:
        continue
    dur = [float(r["Task Duration(us)"]) for r in rs]
    print(f"  {op:<22} n={len(rs):<5} avg={st.mean(dur):>10.2f}us total={sum(dur)/1000:>8.2f}ms")
    fields = ["aic_time(us)", "aiv_time(us)", "aic_mac_ratio", "aic_mte2_ratio",
              "aic_mte3_ratio", "aic_fixpipe_ratio", "aic_scalar_ratio",
              "aiv_vec_ratio", "aiv_mte2_ratio", "aiv_mte3_ratio", "aiv_scalar_ratio",
              "cube_utilization(%)"]
    present = [f for f in fields if any(r.get(f) not in (None, "", "N/A") for r in rs)]
    if present:
        print("    " + "  ".join(f"{f.split('(')[0]}={mean(rs, f):.3f}" for f in present))
