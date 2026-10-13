#!/usr/bin/env bash
# Full evaluation pipeline for one GRU variant.
#
#   bash eval_kernel.sh <variant_dir> [--skip-prof]
#
# Steps:
#   1. clean build of <variant_dir>/kernel  (bash kernel/build_a5.sh --clean)
#   2. best-effort msprof pass on the production case -> profiling/lingxi_msprof_summary.json
#   3. accuracy (vs frozen torch-fp32 golden) + device timing -> evaluation_results.json
#
# Environment traps pinned here (container defaults are wrong):
#   SOC_VERSION must be Ascend950PR_9579 (ambient value is the 950DT SKU)
#   python3 must resolve to /usr/local/python3.12.13 (image /usr/bin/python3 lacks torch)
set -uo pipefail

WORK="${1:?usage: eval_kernel.sh <variant_dir> [--skip-prof]}"
SKIP_PROF="${2:-}"
WORK="$(cd "$WORK" && pwd)"

export PATH="/usr/local/python3.12.13/bin:${PATH}"
export SOC_VERSION="Ascend950PR_9579"
export ASCEND_HOME_PATH="/usr/local/Ascend/cann-9.1.0"
export ASCEND_RT_VISIBLE_DEVICES=0
# shellcheck disable=SC1091
source /usr/local/Ascend/cann-9.1.0/set_env.sh >/dev/null 2>&1 || true

# --- global serialization on the single NPU + single CPU core -----------------
# nproc=1 and one device: concurrent variants would corrupt each other's msprof
# timings and thrash the build. All evals take a global lock.
_LOCK="/workspace/Quantitave_firm/evo_kernel_a5_hf32/GRU_evo_20261012_110616/.npu_eval.lock"
exec 9>"$_LOCK"
flock 9
echo "=== [eval_kernel] acquired global npu/CPU lock ==="

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$WORK"

echo "=== [eval_kernel] variant: $WORK ==="
echo "=== [eval_kernel] SOC_VERSION=$SOC_VERSION python3=$(command -v python3) ==="

# ---------------------------------------------------------------- 1. build
BUILD_LOG="$WORK/build.log"
rm -f "$WORK/evaluation_results.json"
if [ -f "$WORK/kernel/build_a5.sh" ]; then
    bash "$WORK/kernel/build_a5.sh" --clean > "$BUILD_LOG" 2>&1
    BUILD_RC=$?
else
    echo "kernel/build_a5.sh not found" > "$BUILD_LOG"
    BUILD_RC=1
fi

if [ "$BUILD_RC" -ne 0 ]; then
    echo "=== [eval_kernel] BUILD FAILED (rc=$BUILD_RC); tail of $BUILD_LOG ==="
    tail -30 "$BUILD_LOG"
    python3 - "$WORK" <<'PY'
import json, os, sys
work = sys.argv[1]
res = {
    "tag": "evolved", "op_name": "GRU",
    "n_cases_total": 4, "n_cases_passed": 0,
    "baseline": {"time_us": 0.0},
    "evolved": {"time_us": -1.0, "pipeline": None},
    "comparison": {"compilation_success": False, "precision_passed": False,
                   "speedup": 0.0, "precision_message": "build failed",
                   "measurement_quality": "bad"},
    "per_case": [],
}
with open(os.path.join(work, "evaluation_results.json"), "w") as f:
    json.dump(res, f, indent=2)
PY
    exit 2
fi
echo "=== [eval_kernel] build OK ==="

# --------------------------------------------------- 2. msprof pipeline (best effort)
if [ "$SKIP_PROF" != "--skip-prof" ]; then
    PROF_ROOT="$WORK/profiling/msprof"
    rm -rf "$WORK/profiling"
    mkdir -p "$WORK/profiling"
    (
        timeout 900 msprof --output="$PROF_ROOT" --ai-core=on --aic-mode=task-based \
            --application="python3 $WORK/prof_target.py" > "$WORK/profiling/msprof.log" 2>&1
    )
    python3 "$WORK/parse_msprof_pipeline.py" "$PROF_ROOT" \
        "$WORK/profiling/lingxi_msprof_summary.json" \
        > "$WORK/profiling/pipeline.txt" 2>> "$WORK/profiling/msprof.log" || true
    echo "=== [eval_kernel] pipeline: $(cat "$WORK/profiling/pipeline.txt" 2>/dev/null) ==="
fi

# --------------------------------------------------- 3. accuracy + timing
python3 "$WORK/verify_and_bench.py" --work-dir "$WORK" \
    --out "$WORK/evaluation_results.json"
RC=$?
echo "=== [eval_kernel] done rc=$RC ==="
exit $RC
