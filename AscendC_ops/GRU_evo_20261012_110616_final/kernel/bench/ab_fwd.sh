#!/bin/bash
set -uo pipefail
export PATH="/usr/local/python3.12.13/bin:${PATH}"
export SOC_VERSION=Ascend950PR_9579
export ASCEND_RT_VISIBLE_DEVICES=0
source /usr/local/Ascend/cann-9.1.0/set_env.sh >/dev/null 2>&1 || true
BASE=/workspace/Quantitave_firm/NN_GRU_ascendc_final/kernel_a5/build/gru_ext.cpython-312-x86_64-linux-gnu.so
SPLIT=/workspace/Quantitave_firm/kernel_a5_split/build/gru_ext.cpython-312-x86_64-linux-gnu.so
for round in 1 2; do
  echo "--- round $round ---"
  GRU_SO=$BASE  python3 /workspace/Quantitave_firm/kernel_a5_hf32/bench/bench_fwd.py 3 10
  GRU_SO=$SPLIT python3 /workspace/Quantitave_firm/kernel_a5_hf32/bench/bench_fwd.py 3 10
done
