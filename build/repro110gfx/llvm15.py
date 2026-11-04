#!/usr/bin/env python3
"""Use the established LLVM15 source producer with its pinned GitHub Xcode 27 profile."""
import argparse
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE.parent / 'repro109dxmt'))
import build_llvm15 as producer

PROFILE = 'github-xcode27-arm64'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=['recipe', 'source', 'toolchain', 'build'], required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args(argv)
    producer.PROFILE_MINUTES[PROFILE] = 330
    if args.stage == 'build':
        producer.build(args.work, 2, PROFILE)
        result = json.loads((args.work / 'reports/RESULT.json').read_text())
        print(json.dumps({k: result.get(k) for k in ['status', 'first_failure', 'source_built']}), flush=True)
        return 0 if result.get('source_built') else 1
    pin = producer.llvm_source_pin()
    full = producer.load('release110_llvm_full', producer.REPO / 'repro109wine/build_full.py')
    dep = producer.load('release110_llvm_deps', producer.REPO / 'repro109deps/build_deps.py')
    lock = full.apply_profile(dep.read_lock(), PROFILE)
    result = dict(stage=args.stage, profile=PROFILE, llvm_source=pin, toolchain=lock['toolchain'], install='skipped')
    if args.stage == 'toolchain':
        full.cloud_guard(PROFILE)
        args.work.mkdir(parents=True, exist_ok=False)
        sdk, cc, cxx = dep.toolchain_preflight(lock['toolchain'], args.work)
        result.update(sdk=str(sdk), cc=str(cc), cxx=str(cxx))
    print(json.dumps(result, sort_keys=True), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
