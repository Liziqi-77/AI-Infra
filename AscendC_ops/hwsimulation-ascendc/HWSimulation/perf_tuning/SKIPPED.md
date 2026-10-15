# Phase 3.5 — TileLang performance iteration: SKIPPED (framework unavailable)

**Verdict:** Phase 3 (TileLang design expression) could not be executed at all in this
environment, so there is no TileLang implementation to tune. Phase 3 Step 4
(`tilelang-perf-optimization`) is therefore legally skipped under the 4a branch
("TileLang 不可执行且有对照实验证据"). The operator was instead realised directly on
the AscendC path.

## Evidence

Command (interpreter is the same one the whole workflow uses — the one
`evaluate_ascendc.sh` / `verification_ascendc.py` run under):

```
$ which python python3
/usr/local/python3.11.10/bin/python
/usr/local/python3.11.10/bin/python3

$ python -c "import torch,sys; print(sys.executable, torch.__version__)"
/usr/local/python3.11.10/bin/python 2.10.0+cpu

$ python -c "import tilelang; print(tilelang.__file__)"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
ModuleNotFoundError: No module named 'tilelang'

$ python3 -c "import tilelang"
Traceback (most recent call last):
  File "<string>", line 1, in <module>
ModuleNotFoundError: No module named 'tilelang'

$ pip show tilelang
WARNING: Package(s) not found: tilelang
$ python -m pip list | grep -i tile
(no output)
```

A filesystem sweep for a `tilelang` package tree under the workspace and the
usual interpreter prefixes returned nothing, so this is not a `PYTHONPATH`
problem: the TileLang front end is simply not installed.

## Consequences for the workflow

| Phase | Planned artefact | Actual |
|-------|------------------|--------|
| 3-C Step 1 | `design/block_level/*.py` | not produced — `tilelang2ascend-tilelang-designer` cannot run |
| 3-C Step 2 | `design/tile_level/*.py` | not produced |
| 3-C Step 3 | `model_new_tilelang.py` + `evaluate_tilelang.sh` | not produced — nothing to compile or verify |
| 3-C Step 3.5 | `perf_tuning/{baseline.json,optimization_log.md,final_report.md}` | this `SKIPPED.md` instead |
| 4-C | TileLang → AscendC translation | `tilelang2ascend-translator` path taken explicitly (the prompt routes *any* non-PASS result, including `UNAVAILABLE`, to the translator) |

`tl_validation = UNAVAILABLE`. The operator classification (`complex`,
`tiling_design_category = null`) and the block-level algorithm were still worked out
by hand and are recorded in `docs/DESIGN.md`; the AscendC kernel in `kernel/` follows
that design.

## What the TileLang design would have had to express

Recorded so the decision is auditable rather than a silent skip. This operator is a
*sequential* scan along the time axis (9 dependent steps), vectorised across the path
axis (16 lanes). TileLang's block/tile model targets statically-unrollable tile
programs; expressing a 9-deep carry dependency as a `T.serial` loop over tiles with a
loop-carried UB state vector is exactly the shape the unroll-and-vectorise design
would have had to produce. Since the front end is absent, no attempt was made.
