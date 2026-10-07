#!/bin/bash
# Retain the complete driver output and original exit code, including preflight refusal.
set -uo pipefail
profile=${1:-github-macos15-arm64}
work=${2:?Supply an owned fresh work path}
driver_python=${REPRO109_PYTHON:-python3}
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
"$driver_python" -I -B "$script_dir/build_llvm15.py" --build \
  --profile "$profile" --jobs 3 --work "$work" > "${work}.driver.log" 2>&1
build_rc=$?
printf '%s\n' "$build_rc" > "${work}.rc.txt"
printf 'LLVM source build rc=%s; full driver log retained\n' "$build_rc"
exit "$build_rc"
