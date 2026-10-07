"""Small durable stage receipts; reuse the existing private results publisher."""
from datetime import datetime, timezone
import json
import os
import argparse
from pathlib import Path
import subprocess
import signal
import sys
import time
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from wine_layout import recipe_root
sys.path.insert(0, str(recipe_root() / 'repro109deps'))
import failure_evidence

PUBLISH_SECONDS = 120
CLEANUP_SECONDS = 10


class LiveResults:
    def __init__(self, reports, repo, profile, *, publish_dir=None, publisher=None, clock=time.monotonic, job='repro109-llvm15', initial_publish_required=True):
        self.reports, self.repo, self.profile = Path(reports), Path(repo), profile
        self.clock, self.started, self.sequence = clock, clock(), 0
        self.job = job
        self.publish_dir = Path(publish_dir) if publish_dir else None
        self.publisher = publisher or self._publish
        self.initial_publish_required = initial_publish_required
        self.reports.mkdir(parents=True, exist_ok=True)
        if self.publish_dir:
            self.publish_dir.mkdir(parents=True, exist_ok=True)

    def _publish(self, path):
        for attempt in (1, 2):
            try:
                return self._publish_once(path)
            except subprocess.TimeoutExpired:
                with (self.reports / 'checkpoint-publish.log').open('a') as stream:
                    stream.write(f'WARNING checkpoint publisher timeout attempt={attempt}/2; '
                                 + ('retrying\n' if attempt == 1 else 'exhausted\n'))
                if attempt == 2:
                    raise

    def _publish_once(self, path):
        # Only the tiny checkpoint folder is pushed, never prefix/archive/raw logs.
        remote = os.environ.get('RESULTS_REMOTE', '')
        if not remote:
            raise ValueError('Explicit RESULTS_REMOTE is required for checkpoint publication')
        log = self.reports / 'checkpoint-publish.log'
        with log.open('ab') as stream:
            argv = ['bash', str(self.repo / 'ci_scripts/publish-results.sh'), str(path),
                    self.job + '-checkpoint']
            child = subprocess.Popen(argv, cwd=self.repo, start_new_session=True,
                                     stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT)
            try:
                rc = child.wait(timeout=PUBLISH_SECONDS)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(child.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                except OSError:
                    raise RuntimeError('Owned checkpoint publisher TERM refused; retry refused') from None
                try:
                    child.wait(timeout=CLEANUP_SECONDS)
                except subprocess.TimeoutExpired:
                    # Never signal a group using a leader PID already reaped by wait.
                    try:
                        os.killpg(child.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    except OSError:
                        raise RuntimeError('Owned checkpoint publisher KILL refused; retry refused') from None
                    try:
                        child.wait(timeout=CLEANUP_SECONDS)
                    except subprocess.TimeoutExpired:
                        raise RuntimeError('Owned checkpoint publisher did not stop; retry refused') from None
                raise
            if rc:
                raise subprocess.CalledProcessError(rc, argv)

    def record(self, phase, state, *, status='STARTED', failure=None):
        self.sequence += 1
        row = dict(schema=1, job=self.job, profile=self.profile, sequence=self.sequence,
                   phase=phase, state=state, status=status,
                   utc=datetime.now(timezone.utc).isoformat(), elapsed_seconds=self.clock() - self.started,
                   source_revision=os.environ.get('GITHUB_SHA', os.environ.get('CI_COMMIT', 'UNAVAILABLE')),
                   build=os.environ.get('GITHUB_RUN_ID', os.environ.get('CI_BUILD_NUMBER', 'UNAVAILABLE')),
                    failure=failure, publish='ENABLED' if self.publish_dir else 'NOT_ENABLED')
        if status == 'FAILED' or state in ('FAILED', 'COMMAND_FAILED'):
            component = (failure.get('component') or failure.get('phase') or phase) if isinstance(failure, dict) else phase
            row['command_evidence'] = failure_evidence.latest(self.reports, since=self.started, component=component)
        data = json.dumps(row, sort_keys=True, indent=2) + '\n'
        (self.reports / 'PROGRESS.json').write_text(data)
        if self.publish_dir:
            (self.publish_dir / 'PROGRESS.json').write_text(data)
            (self.publish_dir / f'{self.sequence:04d}-{phase}-{state}.json').write_text(data)
        try:
            if self.publish_dir:
                self.publisher(self.publish_dir)
        except (subprocess.SubprocessError, OSError) as error:
            required = self.initial_publish_required and self.sequence == 1
            row['publish'] = 'FAILED_REQUIRED' if required else 'FAILED_CONTINUING'
            row['publish_failure'] = dict(kind=type(error).__name__)
            if isinstance(error, subprocess.CalledProcessError):
                row['publish_failure']['rc'] = error.returncode
            elif isinstance(error, subprocess.TimeoutExpired):
                row['publish_failure']['timeout_seconds'] = error.timeout
            # Do not print argv, environment, or exception text (may contain secrets).
            with (self.reports / 'checkpoint-publish.log').open('a') as stream:
                stream.write(f'WARNING checkpoint publication {row["publish"]} '
                             f'sequence={self.sequence} kind={type(error).__name__}\n')
            if required:
                raise
        finally:
            data = json.dumps(row, sort_keys=True, indent=2) + '\n'
            (self.reports / 'PROGRESS.json').write_text(data)
            with (self.reports / 'PROGRESS.jsonl').open('a') as stream:
                stream.write(json.dumps(row, sort_keys=True) + '\n')
            if self.publish_dir:
                (self.publish_dir / 'PROGRESS.json').write_text(data)
                (self.publish_dir / f'{self.sequence:04d}-{phase}-{state}.json').write_text(data)
        return row


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--reports', type=Path, required=True)
    parser.add_argument('--publish-dir', type=Path, required=True)
    parser.add_argument('--profile', choices=['github', 'xcode-cloud'], required=True)
    parser.add_argument('--job', choices=['repro109-llvm15', 'repro109-deps'], default='repro109-llvm15')
    args = parser.parse_args()
    LiveResults(args.reports, Path(__file__).resolve().parent.parent, args.profile,
                publish_dir=args.publish_dir, job=args.job).record('PRECHECK', 'JOB_STARTED')
