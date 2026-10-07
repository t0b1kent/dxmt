#!/usr/bin/env python3
"""Cloud orchestration of the existing DXMT producer with pinned Wine/LLVM outputs."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import time

sys.dont_write_bytecode = True
SOURCE_REPO = 't0b1kent/dxmt'
DXMT_REVISION = '398b8411c5bcdc51e2dc86bda7c64513d5b6328c'
WINE_REVISION = 'cb9d08f9a429385338aaa61ba8a3127009d635d4'
WINE_RUN = '37616083395'
LLVM_RUN = '37567821557'
LLVM_REVISION = 'f804f27c7a3ee61e2d7882ae12600cfeb9e05bc5'
LLVM_ARTIFACT_ID = 11460272198
LLVM_ARCHIVE_SHA = '4ce2e2c69d9f768d9b9c887b9a10cbd87299fce333d1808f6ba6de489f32e0b6'
LLVM_RESULT_SHA = 'ede9dc86c65034eb94fc37fc007a92f8a561f32cc2431fed94bcacb120bf4e63'
PROFILE = 'github-xcode27-arm64'
PIN_NAMES = ('wine_result', 'deps_manifest', 'wine_dist', 'deps_prefix')


def require(value, message):
    if not value:
        raise ValueError(message)


def cloud_guard():
    require(platform.system() == 'Darwin' and platform.machine() == 'arm64'
            and os.environ.get('GITHUB_ACTIONS') == 'true'
            and os.environ.get('GITHUB_REPOSITORY') == SOURCE_REPO
            and os.environ.get('GITHUB_REF') == 'refs/heads/macrunner-d3d12'
            and re.fullmatch(r'[a-f0-9]{40}', os.environ.get('GITHUB_SHA', '')),
            'DXMT ARM64 cloud execution at an immutable branch revision required; local execution refused')


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def pins_from_environment():
    result = {name: os.environ.get(name.upper() + '_SHA256', '') for name in PIN_NAMES}
    require(all(re.fullmatch(r'[a-f0-9]{64}', value) for value in result.values()),
            'Four measured Wine archive/receipt SHA256 inputs are required')
    return result


def sealed(path, expected, json_file=False):
    require(path.is_file() and not path.is_symlink() and sha(path) == expected,
            'Pinned input bytes differ: ' + path.name)
    return json.loads(path.read_bytes()) if json_file else path


def run_metadata(record, revision, branch=None):
    require(type(record) is dict and record.get('status') == 'completed'
            and record.get('conclusion') == 'success' and record.get('headSha') == revision
            and (branch is None or record.get('headBranch') == branch),
            'Source run must be successful at the pinned revision')


def artifact_metadata(record, name, expected_id=None):
    require(type(record) is dict and type(record.get('artifacts')) is list,
            'Artifact metadata must contain a list')
    found = [row for row in record['artifacts'] if type(row) is dict and row.get('name') == name]
    require(len(found) == 1, 'Expected exactly one named source artifact')
    row = found[0]
    require(row.get('expired') is False and type(row.get('id')) is int
            and (expected_id is None or row['id'] == expected_id),
            'Source artifact expired or identity differs')
    return {key: row.get(key) for key in ('id', 'name', 'size_in_bytes', 'digest', 'expired')}


def command(argv, out, name, timeout=300):
    with (out / (name + '.log')).open('xb') as stream:
        process = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=stream,
                                 stderr=subprocess.STDOUT, timeout=timeout)
    require(process.returncode == 0, name + ' failed; complete log retained')
    path = out / (name + '.log')
    require(path.stat().st_size <= 2 * 1024**2, name + ' output exceeds cap; raw log retained')
    return path.read_text()


def download(repo, run_id, revision, kind, out, dest, artifact_id=None):
    name = ('repro109-wine-c9-' if kind == 'wine' else 'repro109-llvm15-') + revision
    record = json.loads(command(['gh', 'run', 'view', run_id, '--repo', repo,
                                 '--json', 'status,conclusion,headSha,headBranch'], out, kind + '-run'))
    run_metadata(record, revision, 'main' if kind == 'wine' else 'macrunner-d3d12')
    listing = json.loads(command(['gh', 'api', f'repos/{repo}/actions/runs/{run_id}/artifacts'],
                                  out, kind + '-artifacts'))
    artifact = artifact_metadata(listing, name, artifact_id)
    command(['gh', 'run', 'download', run_id, '--repo', repo, '--name', name,
             '--dir', str(dest)], out, kind + '-download', timeout=1200)
    return dict(repository=repo, run=run_id, revision=revision, artifact=artifact)


def unpack(archive, expected, destination):
    sealed(archive, expected)
    require(not destination.exists() and not destination.is_symlink(), 'Owned prefix collision')
    with tarfile.open(archive, 'r:gz') as stream:
        members = stream.getmembers()
        require(len(members) <= 300000 and sum(row.size for row in members) <= 8 * 1024**3,
                'Prefix archive exceeds declared extraction caps')
        destination.mkdir(parents=True)
        stream.extractall(destination, members=members, filter='data')


def load_builder(dxmt, wine, out):
    recipe_revision = os.environ.get('GITHUB_SHA', '')
    require(re.fullmatch(r'[a-f0-9]{40}', recipe_revision), 'Immutable DXMT recipe revision required')
    for directory, revision in ((dxmt, recipe_revision), (wine, WINE_REVISION)):
        name = 'dxmt-checkout' if directory == dxmt else 'wine-checkout'
        require(command(['git', '-C', str(directory), 'rev-parse', 'HEAD'], out, name).strip() == revision,
                'Checkout revision differs')
        require(not command(['git', '-C', str(directory), 'status', '--porcelain',
                             '--untracked-files=all'], out, name + '-status').strip(),
                'Checkout is not clean')
    os.environ['MACRUNNER_WINE_RECIPE_ROOT'] = str(wine / 'build')
    os.environ['MACRUNNER_DXMT_SOURCE_REPOSITORY'] = str(dxmt)
    root = dxmt / 'build/repro109dxmt'
    sys.path.insert(0, str(root))
    spec = importlib.util.spec_from_file_location('source_dxmt_builder', root / 'build_dxmt.py')
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    builder.full.cloud_guard(PROFILE)
    builder.check_inputs()
    # Use the producer's exact preflight in both phases before artifact access.
    dep = builder.full.load_driver('source_dxmt_toolchain', builder.REPO / 'repro109deps/build_deps.py')
    tool = builder.full.apply_profile(dep.read_lock(), PROFILE)['toolchain']
    dep.toolchain_preflight(tool, out)
    return builder


def adapter_argv(dxmt, wine, args, pins, root):
    argv = [sys.executable, '-I', '-B', str(dxmt / 'build/repro109dxmt/build_dxmt.py'),
            '--build', '--profile', PROFILE, '--jobs', '3']
    for name in ('wine_prefix', 'wine_result', 'deps_prefix', 'deps_manifest', 'llvm_prefix', 'llvm_result'):
        argv += ['--' + name.replace('_', '-'), str(getattr(args, name))]
    return argv + ['--wine-result-sha256', pins['wine_result'], '--deps-manifest-sha256',
                   pins['deps_manifest'], '--wine-files', str(args.wine_files),
                   '--wine-files-sha256', args.wine_files_sha256,
                   '--llvm-result-sha256', LLVM_RESULT_SHA,
                   '--work', str(root / 'dxmt-work')]


def execute(args):
    cloud_guard()  # Before paths, tools, network, writes, or loading source producers.
    pins = pins_from_environment()
    temp = Path(os.environ['RUNNER_TEMP']).resolve()
    root = temp / 'repro109-dxmt-after-wine27'
    require(not root.exists() and not root.is_symlink(), 'Fresh owned orchestration path required')
    out = root / 'reports'; out.mkdir(parents=True)
    result = dict(schema=1, status='STARTED', first_failure=None, phase=args.phase,
                  comparison='NOT_ENABLED', stands='NOT_ENABLED', games='NOT_ENABLED',
                  signing='NOT_ENABLED', notarization='NOT_ENABLED', input_pins=pins)
    phase = 'SOURCE_INPUTS'
    try:
        dxmt, wine = args.dxmt_checkout.resolve(), args.wine_checkout.resolve()
        builder = load_builder(dxmt, wine, out)
        phase = 'ARTIFACT_ACCESS'
        wine_dir, llvm_dir = root / 'wine-artifact', root / 'llvm-artifact'
        sources = [download('t0b1kent/macrunner-wine', WINE_RUN, WINE_REVISION, 'wine', out, wine_dir),
                   download('t0b1kent/dxmt', LLVM_RUN, LLVM_REVISION, 'llvm', out, llvm_dir, LLVM_ARTIFACT_ID)]
        wr = wine_dir / 'repro109-wine-c9-results'
        lr = llvm_dir / 'repro109-llvm15-results'
        phase = 'SEALED_RECEIPTS'
        args.wine_result = wr / 'wine/reports/RESULT.json'
        args.deps_manifest = wr / 'reports/composition/dependency-manifest.json'
        args.llvm_result = lr / 'reports/RESULT.json'
        child = sealed(args.wine_result, pins['wine_result'], True)
        sealed(args.deps_manifest, pins['deps_manifest'], True)
        sealed(args.llvm_result, LLVM_RESULT_SHA, True)
        parent = json.loads((wr / 'reports/RESULT.json').read_bytes())
        require(type(parent) is dict and parent.get('status') == 'FULL_WINE_SOURCE_BUILT_NOT_ACCEPTED'
                and parent.get('first_failure', 'MISSING') is None and parent.get('wine') == child
                and type(parent.get('install_skipped')) is int and parent['install_skipped'] == 0,
                'Complete parent Wine source receipt differs from child')
        phase = 'PREFIXES'
        # build_full.py requires dependencies to stay in place (CPython/Meson paths).
        args.wine_prefix = temp / 'repro109-wine-c9-work/wine/install'
        args.deps_prefix = temp / 'repro109-wine-c9-work/deps/prefix'
        args.llvm_prefix = temp / 'repro109-llvm15-work/prefix'
        unpack(wr / 'wine-dist.tar.gz', pins['wine_dist'], args.wine_prefix)
        unpack(wr / 'dependency-prefix.tar.gz', pins['deps_prefix'], args.deps_prefix)
        unpack(lr / 'llvm15-prefix.tar.gz', LLVM_ARCHIVE_SHA, args.llvm_prefix)
        for name in ('wine_result', 'deps_manifest', 'llvm_result'):
            shutil.copyfile(getattr(args, name), out / (name + '.json'))
        phase = 'CONSUMER_INPUTS'
        dep = builder.full.load_driver('source_dxmt_deps', builder.REPO / 'repro109deps/build_deps.py')
        wine_driver = builder.full.load_driver('source_dxmt_wine', builder.REPO / 'repro109wine/build_wine.py')
        args.wine_files = root / 'preflight-wine-files.json'
        args.wine_files.write_text(json.dumps(dep.inventory(args.wine_prefix), indent=2) + '\n')
        args.wine_files_sha256 = sha(args.wine_files)
        args.wine_result_sha256, args.deps_manifest_sha256 = pins['wine_result'], pins['deps_manifest']
        args.llvm_result_sha256 = LLVM_RESULT_SHA
        result['wine_inputs'] = builder.validate_wine(args, dep, wine_driver)
        result['llvm_inputs'] = builder.validate_llvm_prefix.validate(args.llvm_prefix, args.llvm_result, LLVM_RESULT_SHA)
        for name, version in (('meson', '1.11.0'), ('ninja', '1.13.2')):
            tool = args.deps_prefix / 'bin' / name
            require(tool.is_file(), 'Source-built tool missing: ' + name)
            require(command([str(tool), '--version'], out, name + '-version').strip() == version,
                    'Source-built tool version differs: ' + name)
        manifest = dict(schema=1, sources=sources, pins=pins, llvm_archive_sha256=LLVM_ARCHIVE_SHA,
                        llvm_result_sha256=LLVM_RESULT_SHA, profile=PROFILE,
                        dxmt_revision=os.environ['GITHUB_SHA'], dxmt_source_base=DXMT_REVISION,
                        wine_inventory_sha256=args.wine_files_sha256,
                        prefixes={name: str(getattr(args, name)) for name in ('wine_prefix', 'deps_prefix', 'llvm_prefix')})
        with (out / 'INPUTS.json').open('x') as stream:
            stream.write(json.dumps(manifest, indent=2) + '\n')
        result['status'] = 'DXMT_INPUTS_VERIFIED_NOT_COMPILED'
        if args.phase == 'full':
            phase = 'DXMT_PRODUCER'
            dep.command(adapter_argv(dxmt, wine, args, pins, root), root, dict(os.environ),
                        out / 'dxmt-driver.log', out, 'dxmt-driver', time.monotonic() + 101 * 60, timeout=100 * 60)
            result['producer'] = json.loads((root / 'dxmt-work/reports/RESULT.json').read_bytes())
            require(result['producer'].get('status') == 'D3D11_SOURCE_BUILT_NOT_FUNCTION_SECTION_ACCEPTED'
                    and result['producer'].get('first_failure', 'MISSING') is None,
                    'DXMT producer did not complete')
            result['status'] = 'D3D11_SOURCE_BUILT_NOT_FUNCTION_SECTION_ACCEPTED'
    except Exception as error:
        result.update(status='FAILED', first_failure=dict(phase=phase, error=str(error)[:1200]))
        raise
    finally:
        result['install_skipped'] = sum('install skipped' in line.lower()
                                      for path in out.glob('*.log') for line in path.read_text(errors='replace').splitlines())
        (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({key: result[key] for key in ('status', 'first_failure', 'install_skipped')}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', choices=('inputs', 'full'), default='inputs')
    parser.add_argument('--dxmt-checkout', required=True, type=Path)
    parser.add_argument('--wine-checkout', required=True, type=Path)
    execute(parser.parse_args())


if __name__ == '__main__':
    main()
