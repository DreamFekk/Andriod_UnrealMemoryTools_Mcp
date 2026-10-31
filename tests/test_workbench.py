"""Workbench regression tests use temporary workspaces and fake IDA evidence only."""
from __future__ import annotations

import http.client
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import MagicMock, patch

from bridge.workbench import Workbench, Handler, IDAError


class WorkbenchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.app = Workbench(self.root)

    def tearDown(self):
        self.app.executor.shutdown(wait=True)
        self.temp.cleanup()

    def wait_job(self, identifier):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            job = self.app.job(identifier)
            if job['status'] in ('done', 'error') and (self.app.reports / (identifier + '.json')).is_file():
                return job
            time.sleep(0.01)
        self.fail('Fixture job did not finish')

    def test_quarantined_history_cannot_feed_new_analysis(self):
        self.app.reviews = {'old-session': {'status': 'invalid', 'reason': 'cross-process name pollution'}}
        for command in ('search_functions', 'get_function'):
            with self.assertRaisesRegex(ValueError, 'quarantined'):
                self.app.command({'command': command, 'session': 'old-session', 'name': 'X', 'query': 'X'})
        for method in (self.app.open_ida, self.app.analyze):
            with self.assertRaisesRegex(ValueError, 'quarantined'):
                method({'session': 'old-session', 'name': 'X'})

    def test_ida_schema_is_explicitly_available_to_agent(self):
        schema = {'name': 'decompile', 'inputSchema': {'type': 'object', 'properties': {'addr': {'type': 'string'}}}}
        self.app.ida = MagicMock()
        self.app.ida.status.return_value = {'connected': True, 'tools': [schema]}
        result = self.app.command({'command': 'ida_status', 'tool': 'decompile'})
        self.assertEqual(result['ida']['toolSchema'], schema)
        self.assertEqual(self.app.status()['ida']['tools'], ['decompile'])
        with self.assertRaisesRegex(ValueError, 'does not advertise'):
            self.app.ida_status('unknown')

    def test_success_and_error_jobs_are_persisted_and_reloaded(self):
        good = self.app.submit('fixture', lambda: {'answer': '真实中文证据'})
        bad = self.app.submit('fixture-error', lambda: (_ for _ in ()).throw(ValueError('expected fixture failure')))
        self.assertEqual(self.wait_job(good['jobId'])['result']['answer'], '真实中文证据')
        self.assertEqual(self.wait_job(bad['jobId'])['status'], 'error')
        self.assertIn('expected fixture failure', self.app.job(bad['jobId'])['error'])
        restarted = Workbench(self.root)
        try:
            self.assertEqual({job['id'] for job in restarted.status()['jobs']}, {good['jobId'], bad['jobId']})
            self.assertEqual(restarted.job(good['jobId'])['result'], {'answer': '真实中文证据'})
        finally:
            restarted.executor.shutdown(wait=True)

    def test_bounded_queue_rejects_ninth_pending_job(self):
        release = threading.Event()
        try:
            jobs = [self.app.submit('fixture-wait', lambda: release.wait(5)) for _ in range(8)]
            with self.assertRaisesRegex(ValueError, 'queue is full'):
                self.app.submit('fixture-too-many', lambda: None)
        finally:
            release.set()
        for job in jobs:
            self.assertEqual(self.wait_job(job['jobId'])['status'], 'done')

    def test_ambiguous_function_and_rva_mismatch_do_not_call_ida(self):
        self.app.ida = MagicMock()
        with patch.object(self.app.store, 'get_function', return_value={'found': True, 'ambiguous': True, 'candidates': [1, 2]}):
            with self.assertRaisesRegex(ValueError, 'exact unique'):
                self.app.analyze({'name': 'Shared$$execMove'})
        with patch.object(self.app.store, 'get_function', return_value={'found': True, 'ambiguous': False, 'rva': '0x120', 'session': 'fixture'}):
            with self.assertRaisesRegex(ValueError, 'does not match'):
                self.app.analyze({'name': 'Player$$execMove', 'rva': '0x124'})
        self.app.ida.call_tool.assert_not_called()

    def test_analysis_uses_exact_mapping_and_preserves_evidence(self):
        row = {'found': True, 'ambiguous': False, 'session': 'fixture-session', 'name': 'Actor$$execToggle', 'rva': '0x120', 'sdkSnippet': 'void Toggle();'}
        binding = {'database': 'fixture-db', 'input': str(self.root / 'libUE4.so'), 'mapping': {'segments': [{'memoryOffset': 0x100, 'fileSize': 0x100, 'flags': 5}]}}
        evidence = {'address': '0xF123456789000120', 'evidence': {'decompile': {'result': {'content': [{'type': 'text', 'text': 'fixture C code'}]}}}, 'errors': []}
        ida = MagicMock()
        ida.analyze_function.return_value = evidence
        self.app.ida = ida
        with patch.object(self.app.store, 'get_function', return_value=row), patch.object(self.app, '_select', return_value=(binding, 0xF123456789000000)), patch.object(self.app, 'ida_status', return_value={'connected': True}):
            answer = self.app.analyze({'name': row['name'], 'rva': '0x120'})
        ida.analyze_function.assert_called_once_with('0xf123456789000120')
        self.assertEqual(answer['evidence'], evidence['evidence'])
        self.assertEqual(answer['database'], 'fixture-db')
        self.assertEqual(answer['rva'], '0x120')
        self.assertIn('Process.getModuleByName', answer['traceScript'])
        self.assertIn('const rva = ptr("0x120")', answer['traceScript'])
        self.assertTrue(Path(answer['tracePath']).is_relative_to(self.app.reports))
        calls = ida.call_tool.call_args_list
        self.assertEqual(calls[1].args[0], 'define_func')
        self.assertEqual(calls[2].args[0], 'rename')

    def test_rva_outside_executable_mapping_rejected(self):
        row = {'found': True, 'ambiguous': False, 'session': 'fixture', 'name': 'Actor$$execToggle', 'rva': '0x120'}
        binding = {'mapping': {'segments': [{'memoryOffset': 0x100, 'fileSize': 0x100, 'flags': 6}]}}
        self.app.ida = MagicMock()
        with patch.object(self.app.store, 'get_function', return_value=row), patch.object(self.app, '_select', return_value=(binding, 0)):
            with self.assertRaisesRegex(ValueError, 'executable PT_LOAD'):
                self.app.analyze({'name': row['name']})
        self.app.ida.call_tool.assert_not_called()

    def test_database_selection_rejects_wrong_input_and_busy_database(self):
        identifier = 'fixture-session'
        binding = {'database': 'db-1', 'input': str(self.root / 'expected.so')}
        self.app.bindings[identifier] = binding
        self.app.ida = MagicMock()
        self.app.ida._content.return_value = {'imagebase': '0x0', 'input_path': str(self.root / 'other.so')}
        with patch.object(self.app.store, '_session', return_value=Path(identifier)):
            with self.assertRaisesRegex(IDAError, 'differs'):
                self.app._select(identifier)
            self.app.ida._content.return_value = {'status': 'busy', 'imagebase': '0x0'}
            with self.assertRaisesRegex(IDAError, 'busy'):
                self.app._select(identifier)
        self.app.ida.select_database.assert_called_with('db-1')

    def test_open_ida_keeps_artifact_session_and_database_session_distinct(self):
        prepared = {'session': 'artifact-session', 'input': str(self.root / 'libUE4.so'), 'mapping': {'segments': []}}
        opened = {'database': 'ida-database-id', 'session': {'session_id': 'ida-database-id', 'mode': 'headless'}}
        self.app.ida = MagicMock()
        self.app.ida.open_database.return_value = opened
        self.app.ida._content.return_value = {'imagebase': '0x0'}
        with patch.object(self.app.store, 'prepare_ida', return_value=prepared), patch.object(self.app, 'ida_status'):
            result = self.app.open_ida({'session': 'artifact-session'})
        self.assertEqual(result['session'], 'artifact-session')
        self.assertEqual(result['idaSession']['session_id'], 'ida-database-id')
        self.assertEqual(self.app.bindings['artifact-session']['database'], 'ida-database-id')

    def test_save_analysis_and_invalid_report_id(self):
        result = self.app.command({'command': 'save_analysis', 'markdown': '# Fixture analysis\n\nActual observed result.'})
        self.assertEqual(Path(result['path']).read_text(encoding='utf-8'), '# Fixture analysis\n\nActual observed result.')
        self.assertTrue(Path(result['path']).is_relative_to(self.app.reports))
        self.assertEqual(self.app.job(result['id'])['kind'], 'agent_report')
        self.assertEqual(self.app.job(result['id'])['result']['markdown'], Path(result['path']).read_text('utf-8'))
        self.assertIn(result['id'], [job['id'] for job in self.app.status()['jobs']])
        with self.assertRaises(ValueError):
            self.app.command({'command': 'save_analysis', 'markdown': ''})
        with self.assertRaises(ValueError):
            self.app.job('../outside')

    def test_ida_gui_endpoint_is_rejected_before_copying_large_library(self):
        with patch.object(self.app,'ida_status',return_value={'connected':True,'canOpenDatabase':False,'message':'GUI has no idb_open'}), patch.object(self.app.store,'prepare_ida') as prepare:
            with self.assertRaisesRegex(IDAError,'idb_open'):
                self.app.open_ida({})
            prepare.assert_not_called()

    def test_ida_endpoint_setting_is_loopback_and_survives_restart(self):
        with patch.object(self.app,'ida_status',return_value={'connected':True,'canOpenDatabase':True}):
            self.app.command({'command':'configure_ida','url':'http://127.0.0.1:13338/mcp'})
        restarted=Workbench(self.root)
        try:
            self.assertEqual(restarted.ida_url,'http://127.0.0.1:13338/mcp')
        finally:
            restarted.executor.shutdown(wait=True)
        with self.assertRaises(ValueError):
            self.app.command({'command':'configure_ida','url':'http://example.com/mcp'})
        self.assertEqual(self.app.ida_url,'http://127.0.0.1:13338/mcp')

    def test_device_mutations_are_not_fired_by_status(self):
        with patch.object(self.app, 'device') as device:
            status = self.app.status()
        device.assert_not_called()
        self.assertEqual(status['service'], 'umt-workbench')
        self.assertIn('--url', status['mcpConfig']['mcpServers']['unreal-memory-tools']['args'])

    def test_start_ida_is_async_and_persists_endpoint(self):
        url = 'http://127.0.0.1:13338/mcp'
        ready = {'connected': True, 'canOpenDatabase': True, 'canSelectDatabase': True, 'url': url}
        with patch.object(self.app.ida_service, 'ensure', return_value={'started': True, 'url': url}) as start, \
             patch.object(self.app, 'ida_status', return_value=ready):
            response = self.app.command({'command': 'start_ida', 'url': url})
            result = self.wait_job(response['jobId'])
        self.assertEqual(result['status'], 'done')
        self.assertEqual(result['result']['ida'], ready)
        self.assertTrue(result['result']['service']['started'])
        start.assert_called_once_with(url)
        self.assertEqual(json.loads((self.root / 'workbench-settings.json').read_text('utf-8'))['idaUrl'], url)

    def test_start_ida_failure_is_visible_and_passive_status_does_not_spawn(self):
        with patch.object(self.app.ida_service, 'ensure', side_effect=RuntimeError('IDALib installation missing')) as start:
            self.app.status()
            start.assert_not_called()
            result = self.wait_job(self.app.command({'command': 'start_ida'})['jobId'])
        self.assertEqual(result['status'], 'error')
        self.assertIn('IDALib', self.app.status()['ida']['error'])
        self.assertFalse(self.app.status()['ida']['connected'])

    def test_start_ida_rejects_remote_endpoint_before_saving(self):
        with patch.object(self.app.ida_service, 'ensure') as start:
            with self.assertRaises(ValueError):
                self.app.command({'command': 'start_ida', 'url': 'http://example.com:13338/mcp'})
            start.assert_not_called()
        self.assertFalse((self.root / 'workbench-settings.json').exists())

    def test_start_ida_blocks_duplicate_or_database_operations(self):
        self.app.jobs['fixture'] = {'kind': 'start_ida', 'status': 'running'}
        with patch.object(self.app.ida_service, 'ensure') as start:
            for command in ('start_ida', 'configure_ida', 'open_in_ida', 'analyze_function', 'ida_tool'):
                with self.assertRaisesRegex(ValueError, 'IDA'):
                    self.app.command({'command': command})
            start.assert_not_called()

    def test_start_ida_waits_for_existing_jobs(self):
        self.app.jobs['fixture'] = {'kind': 'analyze_function', 'status': 'running'}
        with self.assertRaisesRegex(ValueError, 'Wait for running jobs'):
            self.app.command({'command': 'start_ida'})

    def test_start_ida_rejects_workbench_and_device_ports(self):
        for port in (18765, 18766):
            with self.assertRaisesRegex(ValueError, '端口'):
                self.app.command({'command': 'start_ida', 'url': f'http://127.0.0.1:{port}/mcp'})


class WorkbenchHttpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.app = Workbench(Path(self.temp.name))
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.app = self.app
        self.app.port = self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        self.app.executor.shutdown(wait=True)
        self.temp.cleanup()

    def request(self, method='GET', path='/api/workbench/status', body=None, headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=5)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_status_and_assets_are_served_locally(self):
        code, headers, body = self.request()
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)['pid'], self.app.status()['pid'])
        self.assertEqual(int(headers['Content-Length']), len(body))
        self.assertIn("frame-ancestors 'none'", headers['Content-Security-Policy'])
        code, _, body = self.request(path='/app.js')
        self.assertEqual(code, 200)
        self.assertIn(b'/api/workbench/command', body)

    def test_origin_host_header_and_body_validation(self):
        headers = {'Content-Type': 'application/json', 'X-UMT-Client': '1'}
        for additions in ({'Origin': 'http://attacker.invalid'}, {'Host': 'attacker.invalid'}, {'Host': '127.0.0.1:1'}, {'X-UMT-Client': '0'}):
            code, _, body = self.request('POST', '/api/workbench/command', b'{"command":"ida_status"}', {**headers, **additions})
            self.assertEqual(code, 400, body)
            self.assertIn('error', json.loads(body))
        code, _, _ = self.request('POST', '/api/workbench/command', b'{', headers)
        self.assertEqual(code, 400)

    def test_allowed_command_and_job_polling(self):
        self.app.ida_status = MagicMock(return_value={'connected': True, 'tools': ['decompile']})
        code, _, body = self.request('POST', '/api/workbench/command', json.dumps({'command': 'ida_status'}), {'Content-Type': 'application/json', 'X-UMT-Client': '1'})
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)['ida']['tools'], ['decompile'])
        job = self.app.submit('fixture', lambda: {'actual': 'done'})
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            code, _, body = self.request(path='/api/workbench/job?id=' + job['jobId'])
            if json.loads(body)['status'] == 'done':
                break
            time.sleep(0.01)
        self.assertEqual(json.loads(body)['result'], {'actual': 'done'})


if __name__ == '__main__':
    unittest.main()
