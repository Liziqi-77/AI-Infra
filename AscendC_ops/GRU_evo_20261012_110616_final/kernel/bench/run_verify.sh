#!/bin/bash
set -euo pipefail
export PATH="/usr/local/python3.12.13/bin:${PATH}"
export SOC_VERSION=Ascend950PR_9579
export ASCEND_RT_VISIBLE_DEVICES=0
source /usr/local/Ascend/cann-9.1.0/set_env.sh >/dev/null 2>&1 || true
export GRU_SO="${GRU_SO:?set GRU_SO}"
export GOLDEN_DIR=/workspace/Quantitave_firm/_port_setup/npubench_gru
exec python3 /workspace/Quantitave_firm/_port_setup/a5_verify_forward.py
