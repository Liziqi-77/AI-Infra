#!/bin/bash
set -uo pipefail
export PATH="/usr/local/python3.12.13/bin:${PATH}"
export SOC_VERSION=Ascend950PR_9579
export ASCEND_RT_VISIBLE_DEVICES=0
source /usr/local/Ascend/cann-9.1.0/set_env.sh >/dev/null 2>&1 || true
export GRU_SO="$1"
export PROF_ITERS="${3:-3}"
OUT="$2"
rm -rf "$OUT"
msprof --output="$OUT" --ai-core=on --aic-mode=task-based \
  --application="python3 /workspace/Quantitave_firm/kernel_a5_hf32/bench/prof_target_fwd.py"
echo "msprof exit=$?"
ls "$OUT"
