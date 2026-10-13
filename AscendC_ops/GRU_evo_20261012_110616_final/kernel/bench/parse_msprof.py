#!/usr/bin/env python3
"""Summarise an msprof output dir for the GRU forward: op_statistic rows plus a
per-op/per-shape split of op_summary (input projection vs recurrent launches).

Usage: parse_msprof.py <msprof_output_dir> [label]
"""
import csv, glob, os, sys, statistics as st

root = sys.argv[1]
label = sys.argv[2] if len(sys.argv) > 2 else os.path.basename(root.rstrip("/"))
d = glob.glob(os.path.join(root, "*", "mindstudio_profiler_output"))
if not d:
    d = glob.glob(os.path.join(root, "mindstudio_profiler_output"))
d = d[0]

print(f"### {label}")
print("-- op_statistic --")
for f in glob.glob(os.path.join(d, "op_statistic_*.csv")):
    for row in csv.DictReader(open(f)):
        if "gru" in row.get("OP Type", ""):
            print("  {OP Type:<16} {Core Type:<16} count={Count:<6} total={Total Time(us):>12} "
                  "min={Min Time(us):>10} avg={Avg Time(us):>10} max={Max Time(us):>11}".format(**row))

rows = []
for f in glob.glob(os.path.join(d, "op_summary_*.csv")):
    rows += list(csv.DictReader(open(f)))
by_op = {}
for r in rows:
    by_op.setdefault(r["OP Type"], []).append(r)
gg = by_op.pop("gru_gemm", [])
if gg:
    dur = sorted(float(r["Task Duration(us)"]) for r in gg)
    # the input projection is the single M = rows*S launch per iteration; make a
    # generic launcher-group analysis keyed on the op name instead.
    print("-- op_summary: gru_gemm (all launches) --")
    print(f"  count={len(gg)} min={dur[0]:.1f} med={st.median(dur):.1f} max={dur[-1]:.1f} us")
for op, rs in sorted(by_op.items()):
    dur = sorted(float(r["Task Duration(us)"]) for r in rs)
    med = st.median(dur)
    # the single large-M input projection dominates max; report both modes
    big = [x for x in dur if x > 5 * med]
    small = [x for x in dur if x <= 5 * med]
    def ratio(field, sample):
        vals = [float(r.get(field) or 0) for r in rs if float(r["Task Duration(us)"]) in sample]
        return st.mean(vals) if vals and any(field in r for r in rs) else float("nan")
    def g(field, keep):
        v = [float(r[field]) for r in rs if (float(r["Task Duration(us)"]) > 5 * med) == keep
             and r.get(field) not in (None, "", "N/A")]
        return st.mean(v) if v else float("nan")
    print(f"-- op_summary: {op} --")
    print(f"  launches={len(rs)}  median={med:.1f} us  total={sum(dur)/1000:.1f} ms/3-iters")
    if big:
        print(f"  big-M launches (input projection): n={len(big)} "
              f"avg={st.mean(big):.1f} total={sum(big)/1000:.1f} ms  "
              f"aic_mac_ratio={g('aic_mac_ratio', True):.3f} cube_util={g('cube_utilization(%)', True):.2f}%")
    print(f"  other launches (recurrent): n={len(small)} avg={st.mean(small):.1f} us "
          f"total={sum(small)/1000:.1f} ms  min={min(small):.1f} max={max(small):.1f}")
    print(f"    aic_mac_ratio={g('aic_mac_ratio', False):.3f} "
          f"aic_mte2_ratio={g('aic_mte2_ratio', False):.3f} "
          f"aic_mte3_ratio={g('aic_mte3_ratio', False):.3f} "
          f"aic_scalar_ratio={g('aic_scalar_ratio', False):.3f} "
          f"aic_fixpipe_ratio={g('aic_fixpipe_ratio', False):.3f} "
          f"cube_util={g('cube_utilization(%)', False):.2f}%"
          + (f"  aiv_vec_ratio={g('aiv_vec_ratio', False):.3f}" if False else ""))
