"""Local service lifecycle checks use mock children, never import IDALib."""
from contextlib import ExitStack, contextmanager
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

from bridge import ida_service as service


READY = {'connected': True, 'canOpenDatabase': True, 'canSelectDatabase': True, 'sessions': []}
OFFLINE = {'connected': False, 'error': 'connection refused'}
URL = 'http://127.0.0.1:13338/mcp'


class IDAServiceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.manager = service.LocalIDAService(self.root / 'workspace')

    def tearDown(self):
        self.temporary.cleanup()

    @contextmanager
    def startup(self, statuses=None):
        child = Mock(pid=12345, returncode=None)
        child.poll.return_value = None
        child.wait.return_value = 0
        directory = self.root / 'IDA installation'
        directory.mkdir(exist_ok=True)
        with ExitStack() as stack:
            status = stack.enter_context(patch.object(self.manager, '_status', side_effect=statuses or [OFFLINE, READY]))
            available = stack.enter_context(patch.object(self.manager, '_available', return_value=True))
            deps = stack.enter_context(patch.object(self.manager, '_dependencies'))
            stack.enter_context(patch.object(service, 'discover_ida_directory', return_value=directory))
            stack.enter_context(patch.dict(os.environ, {'IDADIR': ''}))
            preflight = stack.enter_context(patch.object(service.subprocess, 'run', return_value=Mock(returncode=0)))
            spawn = stack.enter_context(patch.object(service.subprocess, 'Popen', return_value=child))
            yield child, spawn, preflight, status, available, deps

    def test_url_validation_and_canonical_form(self):
        self.assertEqual(service.normalize_url('http://LOCALHOST:13338/mcp'), 'http://localhost:13338/mcp')
        self.assertEqual(service.LocalIDAService.normalize_url('http://[::1]:13338/mcp'), 'http://[::1]:13338/mcp')
        for url in (None, True, '', 'https://127.0.0.1:13338/mcp', 'http://example.com:13338/mcp',
                    'http://127.0.0.1/mcp', 'http://127.0.0.1:80/mcp', 'http://127.0.0.1:65536/mcp',
                    'http://127.0.0.1:13338/', 'http://127.0.0.1:13338/mcp/',
                    'http://user@127.0.0.1:13338/mcp', 'http://127.0.0.1:13338/mcp?x=1',
                    'http://127.0.0.1:13338/mcp#x', 'http://127.0.0.1:13338/mcp?',
                    'http://127.0.0.1:13338/mcp\n', 'http://127.0.0.1:13338/mcp --shell'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                service.normalize_url(url)

    def test_existing_supervisor_is_reused_without_dependency_or_process_changes(self):
        with patch.object(self.manager, '_status', return_value=READY), \
                patch.object(self.manager, '_dependencies') as dependencies, \
                patch.object(service, 'discover_ida_directory') as discovery, \
                patch.object(service.subprocess, 'Popen') as spawn:
            result = self.manager.ensure(URL)
        self.assertTrue(result['reused'])
        self.assertFalse(result['started'])
        self.assertNotIn('pid', result)
        dependencies.assert_not_called()
        discovery.assert_not_called()
        spawn.assert_not_called()
        self.assertFalse(self.manager.root.exists())

    def test_gui_or_incomplete_supervisor_is_not_replaced(self):
        for status in ({'connected': True, 'canOpenDatabase': False},
                       {'connected': True, 'canOpenDatabase': True, 'canSelectDatabase': True},
                       {**READY, 'canSelectDatabase': False}):
            with self.subTest(status=status), patch.object(self.manager, '_status', return_value=status), \
                    patch.object(service.subprocess, 'Popen') as spawn:
                with self.assertRaisesRegex(service.IDAServiceError, 'existing service was left running'):
                    self.manager.ensure(URL)
                spawn.assert_not_called()

    def test_occupied_port_is_not_replaced(self):
        with self.startup() as (_, spawn, preflight, _, available, dependencies):
            available.return_value = False
            with self.assertRaisesRegex(service.IDAServiceError, 'already occupied'):
                self.manager.ensure(URL)
            spawn.assert_not_called()
            preflight.assert_not_called()
            dependencies.assert_not_called()

    def test_missing_dependency_reports_exact_interpreter_and_does_not_import_idapro(self):
        with patch.object(service.importlib.util, 'find_spec', return_value=None) as find, \
                patch.object(service.subprocess, 'Popen') as spawn:
            with self.assertRaisesRegex(service.IDAServiceError, 'ida_pro_mcp.idalib_supervisor') as error:
                self.manager._dependencies()
        self.assertIn(sys.executable, str(error.exception))
        find.assert_called_once_with('ida_pro_mcp.idalib_supervisor')
        spawn.assert_not_called()

    def test_startup_uses_argument_list_child_environment_and_saves_ready_metadata(self):
        with self.startup() as (child, spawn, preflight, status, _, _):
            result = self.manager.ensure(URL)
            self.assertTrue(result['started'])
            self.assertFalse(result['reused'])
            self.assertEqual(result['pid'], 12345)
            args, options = spawn.call_args
            self.assertEqual(args[0], [sys.executable, '-m', 'ida_pro_mcp.idalib_supervisor', '--host', '127.0.0.1', '--port', '13338'])
            self.assertIs(options['shell'], False)
            self.assertEqual(options['stdin'], subprocess.DEVNULL)
            self.assertEqual(options['creationflags'], getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self.assertEqual(options['env']['IDADIR'], str(self.root / 'IDA installation'))
            self.assertEqual(os.environ['IDADIR'], '')
            self.assertEqual(preflight.call_args.args[0][:2], [sys.executable, '-c'])
            self.assertIn('import idapro', preflight.call_args.args[0][2])
            self.assertEqual(preflight.call_args.kwargs['timeout'], self.manager.PREFLIGHT_TIMEOUT)
            metadata = json.loads((self.manager.root / 'ida-supervisor-13338.json').read_text('utf-8'))
            self.assertEqual(metadata['state'], 'ready')
            self.assertEqual(metadata['pid'], child.pid)
            self.assertTrue(Path(result['logPath']).is_file())
            child.terminate.assert_not_called()
            status.side_effect = None
            status.return_value = READY
            reused = self.manager.ensure(URL)
            self.assertTrue(reused['reused'])
            self.assertEqual(reused['pid'], child.pid)
            spawn.assert_called_once()

    def test_preflight_failure_keeps_diagnostic_and_never_spawns_supervisor(self):
        with self.startup() as (_, spawn, preflight, *_):
            def fail(*args, **kwargs):
                kwargs['stdout'].write(b'Cannot initialize IDALib: license unavailable\n')
                return Mock(returncode=1)
            preflight.side_effect = fail
            with self.assertRaisesRegex(service.IDAServiceError, 'license unavailable') as error:
                self.manager.ensure(URL)
            self.assertIn('Log:', str(error.exception))
            spawn.assert_not_called()
            self.assertEqual(json.loads((self.manager.root / 'ida-supervisor-13338.json').read_text())['state'], 'failed')

    def test_preflight_timeout_is_reported(self):
        with self.startup() as (_, spawn, preflight, *_):
            preflight.side_effect = subprocess.TimeoutExpired('python', 10)
            with self.assertRaisesRegex(service.IDAServiceError, 'initialization timed out'):
                self.manager.ensure(URL)
            spawn.assert_not_called()

    def test_early_exit_is_reported_and_dead_child_is_not_terminated(self):
        with self.startup() as (child, spawn, _, *_):
            child.poll.return_value = 7
            child.returncode = 7
            def launch(*args, **kwargs):
                kwargs['stdout'].write(b'supervisor import failed\n')
                return child
            spawn.side_effect = launch
            with self.assertRaisesRegex(service.IDAServiceError, 'exit 7') as error:
                self.manager.ensure(URL)
            self.assertIn('supervisor import failed', str(error.exception))
            child.terminate.assert_not_called()

    def test_readiness_timeout_only_stops_child_created_by_this_call(self):
        with self.startup() as (child, _, _, status, *_):
            self.manager.START_TIMEOUT = 0.01
            status.side_effect = None
            status.return_value = OFFLINE
            with self.assertRaisesRegex(service.IDAServiceError, 'did not become ready'):
                self.manager.ensure(URL)
            child.terminate.assert_called_once()
            child.wait.assert_called_once_with(timeout=2)
            child.kill.assert_not_called()
            self.assertEqual(self.manager._children, {})

    def test_metadata_pid_is_not_used_as_process_ownership(self):
        self.manager.root.mkdir()
        (self.manager.root / 'ida-supervisor-13338.json').write_text('{"pid": 999, "state": "ready"}')
        with patch.object(self.manager, '_status', return_value=READY), patch.object(service.subprocess, 'Popen') as spawn:
            result = self.manager.ensure(URL)
        self.assertNotIn('pid', result)
        spawn.assert_not_called()

    def test_concurrent_callers_launch_only_one_supervisor(self):
        second = service.LocalIDAService(self.manager.root)
        online = []
        with self.startup() as (child, spawn, _, status, *_):
            status.side_effect = lambda *args: READY if online else OFFLINE
            def launch(*args, **kwargs):
                online.append(True)
                return child
            spawn.side_effect = launch
            with patch.object(second, '_status', side_effect=lambda *args: READY if online else OFFLINE), \
                    patch.object(second, '_dependencies'), patch.object(second, '_available', return_value=True), \
                    ThreadPoolExecutor(max_workers=2) as pool:
                first = pool.submit(self.manager.ensure, URL)
                other = pool.submit(second.ensure, URL)
                results = [first.result(timeout=3), other.result(timeout=3)]
            self.assertEqual(sum(item['started'] for item in results), 1)
            self.assertEqual(sum(item['reused'] for item in results), 1)
            spawn.assert_called_once()

    def test_discovery_preserves_explicit_idadir_and_reports_ambiguity(self):
        first, second = self.root / 'IDA one', self.root / 'IDA two'
        name = 'idalib.dll' if os.name == 'nt' else 'libidalib.so'
        for directory in (first, second):
            directory.mkdir()
            (directory / name).write_bytes(b'fixture, not loaded')
        with patch.dict(os.environ, {'IDADIR': str(first)}):
            self.assertEqual(service.discover_ida_directory(), first.resolve())
        with patch.dict(os.environ, {'IDADIR': str(self.root / 'missing')}):
            with self.assertRaisesRegex(service.IDAServiceError, 'IDADIR'):
                service.discover_ida_directory()
        with patch.dict(os.environ, {'IDADIR': '', 'IDAUSR': str(self.root / 'no-config')}), \
                patch.object(service.shutil, 'which', return_value=None), \
                patch.object(service, '_windows_registry_directories', return_value=[]), \
                patch.object(service, '_portable_roots', return_value=[self.root]):
            with self.assertRaisesRegex(service.IDAServiceError, 'Multiple IDA installations'):
                service.discover_ida_directory()

    def test_configured_ida_directory_is_read_without_loading_native_library(self):
        directory = self.root / 'Configured IDA'
        directory.mkdir()
        (directory / ('idalib.dll' if os.name == 'nt' else 'libidalib.so')).write_bytes(b'fixture')
        (self.root / 'ida-config.json').write_text(json.dumps({'Paths': {'ida-install-dir': str(directory)}}))
        with patch.dict(os.environ, {'IDADIR': '', 'IDAUSR': str(self.root)}):
            self.assertEqual(service.discover_ida_directory(), directory.resolve())


if __name__ == '__main__':
    unittest.main()
