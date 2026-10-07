#!/bin/bash
# Run one existing producer operation. The scheduler owns independent axes.
set -euo pipefail
axis=${1:?One source axis is required}
recipe=${REPRO109_RECIPE:?Recipe root is required}
python=${REPRO109_DRIVER_PYTHON:-python3}
case "$axis" in
  native-source)
    "$python" -I -B "$recipe/check_pe_preparation.py" --kind native --out native-control
    ;;
  pe-source)
    "$python" -I -B "$recipe/check_pe_preparation.py" --kind pe --out pe-control
    ;;
  directx-headers)
    "$python" -I -B "$recipe/prepare_native.py" --out native
    "$python" -I -B "$recipe/freeze_version.py" --prepared native --kind native
    "$python" -I -B "$recipe/llvm_link.py" --prepared native --kind native
    "$python" -I -B "$recipe/prepare_directx.py" --prepared native --kind native \
      --checkout headers --fetch --profile github-macos15-arm64
    ;;
  llvm-source|toolchain)
    "$python" -I -B "$recipe/check_cloud_input.py" "$axis"
    ;;
  *)
    printf 'Unknown source axis\n' >&2
    exit 2
    ;;
esac
