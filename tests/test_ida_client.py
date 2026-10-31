from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import unittest
import errno
import urllib.error
from unittest.mock import patch
from bridge.ida_client import IDAClient, IDAError


class IDAClientTest(unittest.TestCase):
    def test_refused_connection_explains_service_start(self):
        with patch.object(self.client.opener, 'open', side_effect=urllib.error.URLError(
                ConnectionRefusedError(errno.ECONNREFUSED, 'Connection refused'))):
            result = self.client.status()
        self.assertFalse(result['connected'])
        self.assertIn('保存并自动连接', result['error'])
        self.assertIn(self.client.url, result['error'])

    def setUp(self):
        self.calls = []
        calls = self.calls
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                calls.append(request)
                method = request['method']
                if method == 'notifications/initialized':
                    self.send_response(202)
                    self.end_headers()
                    return
                if method == 'initialize':
                    result = {'protocolVersion': '2024-11-05', 'serverInfo': {'name': 'mock-IDA'}}
                elif method == 'tools/list':
                    result = {'tools': [
                        {'name': 'idapro_decompile', 'inputSchema': {'type': 'object', 'properties': {'addr': {'type': 'string'}}, 'required': ['addr']}},
                        {'name': 'idapro_disasm', 'inputSchema': {'type': 'object', 'properties': {'addr': {'type': 'string'}, 'max_instructions': {'type': 'integer'}}, 'required': ['addr']}},
                        {'name': 'idapro_xrefs_to', 'inputSchema': {'type': 'object', 'properties': {'addrs': {'type': 'array', 'items': {'type': 'string'}}}, 'required': ['addrs']}},
                        {'name': 'idapro_callees', 'inputSchema': {'type': 'object', 'properties': {'queries': {'type': 'array', 'items': {'type': 'object', 'properties': {'address': {'type': 'integer'}}, 'required': ['address']}}}, 'required': ['queries']}}]}
                elif method == 'tools/call':
                    result = {'content': [{'type': 'text', 'text': json.dumps(request['params'])}]}
                else:
                    result = {}
                payload = json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'result': result})
                self.send_response(200)
                self.send_header('Mcp-Session-Id', 'mock-session')
                # Alternate SSE and JSON to exercise both response forms.
                if method == 'tools/call':
                    body = (': keepalive\n\nevent: message\ndata: ' + payload + '\n\n').encode()
                    self.send_header('Content-Type', 'text/event-stream')
                else:
                    body = payload.encode()
                    self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.client = IDAClient('http://127.0.0.1:' + str(self.server.server_port) + '/mcp')

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def test_initialization_status_and_real_schema(self):
        status = self.client.status()
        self.assertTrue(status['connected'])
        self.assertEqual(status['toolCount'], 4)
        self.assertFalse(status['canOpenDatabase'])
        self.assertEqual(status['endpointMode'], 'current_database')
        self.assertEqual(self.client.session_id, 'mock-session')
        self.assertEqual(sum(c['method'] == 'initialize' for c in self.calls), 1)
        self.assertTrue(any(c['method'] == 'notifications/initialized' for c in self.calls))

    def test_analysis_uses_advertised_tools_and_keeps_raw_evidence(self):
        result = self.client.analyze_function('0x1001234')
        self.assertFalse(result['errors'])
        self.assertEqual(len(result['evidence']), 4)
        self.assertEqual(result['evidence']['xrefs']['arguments'], {'addrs': ['0x1001234']})
        self.assertEqual(result['evidence']['callees']['arguments'], {'queries': [{'address': 0x1001234}]})
        self.assertIn('content', result['evidence']['decompile']['result'])
        self.assertFalse(any('patch' in c.get('params', {}).get('name', '') for c in self.calls))

    def test_missing_tool_and_invalid_address(self):
        with self.assertRaises(IDAError):
            self.client.call_tool('does_not_exist', {})
        for value in ('1234', 'garbage', '0x0', True):
            with self.assertRaises(ValueError):
                self.client.analyze_function(value)

    def test_unknown_required_schema_is_reported_not_guessed(self):
        self.client.initialize()
        self.client.tools = [{'name': 'decompile', 'inputSchema': {'properties': {'addr': {'type': 'string'}, 'danger': {'type': 'string'}}, 'required': ['addr', 'danger']}}]
        result = self.client.analyze_function('0x1234')
        self.assertEqual(result['evidence'], {})
        self.assertIn('Unrecognized required', result['errors'][0]['error'])
        self.assertFalse(any(c['method'] == 'tools/call' for c in self.calls))

    def test_supervisor_database_is_explicit(self):
        self.client.initialize()
        self.client.tools = [{'name': 'decompile', 'inputSchema': {'properties': {
            'addr': {'type': 'string'}, 'database': {'type': 'string'}}, 'required': ['addr', 'database']}}]
        missing = self.client.analyze_function('0x1234')
        self.assertTrue(missing['errors'])
        self.client.database = 'selected-idb-session'
        result = self.client.analyze_function('0x1234')
        self.assertEqual(result['evidence']['decompile']['arguments']['database'], 'selected-idb-session')

    def test_open_and_select_database_use_server_id(self):
        self.client.initialized = True
        self.client.tools = [
            {'name': 'idb_open', 'inputSchema': {'properties': {key: {} for key in
                ('input_path', 'mode', 'run_auto_analysis', 'build_caches', 'init_hexrays')}}},
            {'name': 'idb_list', 'inputSchema': {'properties': {}}}]
        recorded = []
        def rpc(method, params=None, notification=False):
            if method == 'tools/list':
                return {'tools': self.client.tools}
            recorded.append(params)
            if params['name'] == 'idb_open':
                return {'structuredContent': {'success': True, 'session': {'session_id': 'real-server-id'}}}
            return {'structuredContent': {'sessions': [{'session_id': 'real-server-id'}]}}
        self.client._rpc = rpc
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'libUE4.so'
            path.write_bytes(b'input')
            with self.assertRaises(ValueError):
                self.client.open_database(path)
            (path.parent / 'mapping.json').write_text('{}')
            (path.parent / 'script.json').write_text('{}')
            opened = self.client.open_database(path)
            self.assertEqual(opened['database'], 'real-server-id')
            self.assertFalse(recorded[0]['arguments']['run_auto_analysis'])
            self.assertEqual(recorded[0]['arguments']['mode'], 'prefer_headless')
        self.assertEqual(self.client.select_database('real-server-id')['database'], 'real-server-id')
        with self.assertRaises(IDAError):
            self.client.select_database('invented-id')


if __name__ == '__main__':
    unittest.main()
