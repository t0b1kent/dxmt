"""Inert requests only: no downloads, vendor code, or cloud execution."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('request_job', Path(__file__).with_name('repro109_dxmt_after_wine27.py'))
job = importlib.util.module_from_spec(spec)
spec.loader.exec_module(job)


class RequestTests(unittest.TestCase):
    def setUp(self):
        parent = Path(os.environ['REPRO109_TEST_TMP'])
        self.root = parent / ('request-' + self._testMethodName)
        self.root.mkdir()
        self.ticket = self.root / 'request.json'
        self.values = {'schema': 1, 'phase': 'inputs'}
        self.values.update({name + '_sha256': str(i + 1) * 64 for i, name in enumerate(job.PIN_NAMES)})

    def push(self, values=None, raw=None, phase=None, env=None):
        self.ticket.write_bytes(raw if raw is not None else json.dumps(values or self.values).encode())
        return job.event_request('push', phase, self.ticket, env or {})

    def manual(self, phase='inputs', values=None):
        values = values or self.values
        env = {name.upper() + '_SHA256': values[name + '_sha256'] for name in job.PIN_NAMES}
        env['PHASE'] = values['phase']
        return job.event_request('workflow_dispatch', phase, Mock(), env)

    def test_both_events_equal_for_both_phases(self):
        for phase in ('inputs', 'full'):
            values = dict(self.values, phase=phase)
            self.assertEqual(self.push(values), self.manual(phase, values))

    def test_push_ignores_manual_environment(self):
        self.assertEqual(self.push(env={'PHASE': 'full', 'WINE_DIST_SHA256': 'f' * 64})['phase'], 'inputs')

    def test_manual_never_reads_ticket(self):
        ticket = Mock()
        env = {name.upper() + '_SHA256': self.values[name + '_sha256'] for name in job.PIN_NAMES}
        env['PHASE'] = 'full'
        self.assertEqual(job.event_request('workflow_dispatch', None, ticket, env)['phase'], 'full')
        self.assertEqual(ticket.mock_calls, [])

    def test_phase_override_refused_for_push(self):
        with self.assertRaisesRegex(ValueError, 'Push phase'):
            self.push(phase='full')

    def test_missing_ticket(self):
        with self.assertRaisesRegex(ValueError, 'regular push request'):
            job.event_request('push', None, self.ticket, {})

    def test_symlink_ticket(self):
        target = self.root / 'target.json'; target.write_text(json.dumps(self.values))
        self.ticket.symlink_to(target)
        with self.assertRaisesRegex(ValueError, 'regular push request'):
            job.event_request('push', None, self.ticket, {})

    def test_oversize_ticket(self):
        with self.assertRaisesRegex(ValueError, 'bounded regular'):
            self.push(raw=b' ' * 8193)

    def test_duplicate_fields(self):
        with self.assertRaisesRegex(ValueError, 'Duplicate request'):
            self.push(raw=b'{"schema":1,"schema":1}')

    def test_wrong_schema_type_and_value(self):
        for schema in (True, '1', 0, 2):
            with self.assertRaisesRegex(ValueError, 'schema 1'):
                self.push(dict(self.values, schema=schema))

    def test_exact_fields(self):
        for value in (dict(self.values, unexpected='x'), {k:v for k,v in self.values.items() if k != 'phase'}):
            with self.assertRaisesRegex(ValueError, 'Exact push request fields'):
                self.push(value)

    def test_non_object_json(self):
        for raw in (b'[]', b'null', b'1'):
            with self.assertRaisesRegex(ValueError, 'Exact push request fields'):
                self.push(raw=raw)

    def test_malformed_json(self):
        with self.assertRaises(json.JSONDecodeError): self.push(raw=b'{')

    def test_phase_refused(self):
        for phase in ('', 'other', None, 1, []):
            with self.assertRaisesRegex(ValueError, 'Request phase'):
                self.push(dict(self.values, phase=phase))

    def test_pin_types_and_shapes(self):
        for name in job.PIN_NAMES:
            for value in ('f' * 63, 'F' * 64, 'g' * 64, None, 1, [], {}):
                with self.assertRaisesRegex(ValueError, 'Four measured'):
                    self.push(dict(self.values, **{name + '_sha256': value}))

    def test_manual_missing_pin(self):
        for name in job.PIN_NAMES:
            env = {n.upper() + '_SHA256': self.values[n + '_sha256'] for n in job.PIN_NAMES if n != name}
            with self.assertRaisesRegex(ValueError, 'Four measured'):
                job.event_request('workflow_dispatch', 'inputs', Mock(), env)

    def test_unsupported_events_before_ticket(self):
        for event in ('', 'pull_request', 'schedule'):
            ticket = Mock()
            with self.assertRaisesRegex(ValueError, 'Unsupported request event'):
                job.event_request(event, None, ticket, {})
            self.assertEqual(ticket.mock_calls, [])

    def test_main_local_guard_before_request_or_execute(self):
        argv = ['job', '--dxmt-checkout', str(self.root), '--wine-checkout', str(self.root)]
        with patch.object(sys, 'argv', argv), patch.object(job, 'cloud_guard', side_effect=ValueError('local refused')), \
             patch.object(job, 'event_request') as request, patch.object(job, 'execute') as execute:
            with self.assertRaisesRegex(ValueError, 'local refused'): job.main()
            request.assert_not_called(); execute.assert_not_called()

    def test_main_routes_both_events_into_same_producer(self):
        argv = ['job', '--dxmt-checkout', str(self.root), '--wine-checkout', str(self.root)]
        for event in ('workflow_dispatch', 'push'):
            for phase in ('inputs', 'full'):
                selected = {k:v for k,v in dict(self.values, phase=phase).items() if k != 'schema'}
                with patch.object(sys, 'argv', argv), patch.dict(os.environ, {'GITHUB_EVENT_NAME':event}), \
                     patch.object(job, 'cloud_guard'), patch.object(job, 'event_request', return_value=selected) as request, \
                     patch.object(job, 'execute') as execute:
                    job.main(); request.assert_called_once()
                    self.assertEqual(request.call_args.args[0], event)
                    self.assertEqual(execute.call_args.args[0].phase, phase)
                    for name in job.PIN_NAMES:
                        self.assertEqual(os.environ[name.upper() + '_SHA256'], selected[name + '_sha256'])

    def test_workflow_restricts_push_to_one_branch_and_one_file(self):
        path = Path(__file__).resolve().parents[2] / '.github/workflows/repro109-dxmt-after-wine27-xcode27-arm64.yml'
        text = path.read_text()
        push = text.split('  push:\n', 1)[1].split('  workflow_dispatch:', 1)[0]
        self.assertEqual(push.strip(), 'branches: [macrunner-d3d12]\n    paths: [build/repro109dxmt/dispatch-after-wine27.json]')
        self.assertIn("github.repository == 't0b1kent/dxmt' && github.ref == 'refs/heads/macrunner-d3d12'", text)
        self.assertIn('ref: ${{ github.sha }}', text)
        self.assertIn('test_dispatch_request.py', text)
        self.assertNotIn('--phase "$PHASE"', text)

    def test_main_real_requests_build_inputs_and_full(self):
        ticket = self.root / 'build/repro109dxmt/dispatch-after-wine27.json'
        ticket.parent.mkdir(parents=True)
        argv = ['job', '--dxmt-checkout', str(self.root), '--wine-checkout', str(self.root)]
        for event in ('workflow_dispatch', 'push'):
            for phase in ('inputs', 'full'):
                ticket.write_text(json.dumps(dict(self.values, phase=phase)))
                env = {'GITHUB_ACTIONS':'true', 'GITHUB_REPOSITORY':job.SOURCE_REPO,
                       'GITHUB_REF':'refs/heads/macrunner-d3d12', 'GITHUB_SHA':job.DXMT_REVISION,
                       'GITHUB_EVENT_NAME':event, 'PHASE':phase if event=='workflow_dispatch' else 'ignored'}
                env.update({name.upper()+'_SHA256':self.values[name+'_sha256'] for name in job.PIN_NAMES})
                with patch.object(sys, 'argv', argv), patch.dict(os.environ, env, clear=True), \
                     patch.object(job.platform, 'system', return_value='Darwin'), \
                     patch.object(job.platform, 'machine', return_value='arm64'), patch.object(job, 'execute') as execute:
                    job.main()
                    self.assertEqual(execute.call_args.args[0].phase, phase)
                    self.assertEqual(job.pins_from_environment(), {name:self.values[name+'_sha256'] for name in job.PIN_NAMES})

    def test_published_ticket_matches_accepted_inputs(self):
        value = json.loads(Path(__file__).with_name('dispatch-after-wine27.json').read_bytes())
        self.assertIn(value['phase'], ('inputs', 'full'))
        self.assertEqual(value, {'schema':1,'phase':value['phase'],
            'wine_result_sha256':'8d2258433d4ddf957d811565f3b1b342684dcaf930841b35f73fe99c8dee983a',
            'deps_manifest_sha256':'78a4a3699d8addb71ecc90498ebcb27f7dcea2d756bdc4af367c667dd2738740',
            'wine_dist_sha256':'398d09faf25cac2a1bb9f41d5a584f18a96ef77155f56c45158a2dc291bc19a7',
            'deps_prefix_sha256':'94bd434db9d2998ff63d8ebc4b74d254bd7195fd72d20ec799a6c46629570bb2'})

    def test_source_matrix_shares_the_exact_wine_recipe_pin(self):
        path = Path(__file__).resolve().parents[2] / '.github/workflows/repro109-dxmt-source-matrix-macos15-arm64.yml'
        text = path.read_text()
        self.assertIn('repository: t0b1kent/macrunner-wine\n          ref: ' + job.WINE_REVISION, text)
        self.assertNotIn('00cd5eaad9a1a4b5c3679d82edf62ce3835e9f63', text)
        self.assertIn('axis: [native-source, pe-source, directx-headers, llvm-source, toolchain]', text)


if __name__ == '__main__': unittest.main(verbosity=2)
