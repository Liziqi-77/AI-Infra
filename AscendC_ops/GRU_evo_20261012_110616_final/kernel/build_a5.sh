#!/bin/bash
# Build the fused GRU AscendC kernels for the A5 target (Ascend950PR_9579 / arch35 / dav-c310).
#
#   bash kernel_a5/build_a5.sh            # incremental
#   bash kernel_a5/build_a5.sh --clean    # wipe build/ and rebuild
#
# Only per-shell environment is touched; nothing is written outside this tree.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- must be pinned, not inherited -------------------------------------------------
# This container ships a *global* SOC_VERSION=ascend950dt_9582 in its Docker ENV
# layer. Ascend950DT_9582 is a different SKU from the board installed here, and
# the CANN cmake would happily configure for it and emit a wrong-arch kernel with
# no error. The board is confirmed as Ascend950PR_9579 by:
#   npu-smi info -t board -i 0     -> NPU Name 9579 / Chip Name Ascend950PR
#   torch.npu.get_device_name(0)   -> Ascend950PR_9579
export SOC_VERSION="Ascend950PR_9579"
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0

# The image's /usr/bin/python3 is 3.10.12 and has no torch/torch_npu/pybind11.
# CMakeLists.txt shells out to a bare `python3` for all three, so 3.12.13 (which
# has them) is prepended for this shell only.
export PATH="/usr/local/python3.12.13/bin:${PATH}"

# shellcheck disable=SC1091
source /usr/local/Ascend/cann-9.1.0/set_env.sh >/dev/null 2>&1 || true

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$HERE/build"
    shift || true
fi

mkdir -p "$HERE/build"
echo "[build_a5] SOC_VERSION=$SOC_VERSION  CANN=$ASCEND_HOME_PATH  python=$(command -v python3)"

cmake -S "$HERE" -B "$HERE/build" \
    -DSOC_VERSION="$SOC_VERSION" \
    -DASCEND_CANN_PACKAGE_PATH="$ASCEND_HOME_PATH" \
    -DCMAKE_BUILD_TYPE=Release
make -C "$HERE/build" -j"$(nproc)"

echo "[build_a5] artifact:"
ls -la "$HERE"/build/gru_ext*.so
