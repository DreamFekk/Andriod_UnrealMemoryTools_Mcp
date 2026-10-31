import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('umt_mcp', ROOT / 'bridge/mcp_server.py')
mcp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mcp)


class MCPTest(unittest.TestCase):
    def setUp(self):
        self.adapter = mcp.Adapter('http://127.0.0.1:18766')

    def test_stdio_framing_and_notifications(self):
        requests = [dict(jsonrpc='2.0', id=1, method='initialize', params={}),
                    dict(jsonrpc='2.0', method='notifications/initialized'),
                    dict(jsonrpc='2.0', id=2, method='tools/list'),
                    dict(jsonrpc='2.0', id=3, method='prompts/list')]
        result = subprocess.run([sys.executable, str(ROOT / 'bridge/mcp_server.py')],
            input='\n'.join(json.dumps(r) for r in requests)+'\n', capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([r['id'] for r in replies], [1, 2, 3])
        self.assertEqual(len(replies[1]['result']['tools']), 15)

    def test_route_to_workbench(self):
        with patch.object(self.adapter, 'http', return_value={'jobId': '123'}) as http:
            self.adapter.call('umt_analyze_function', {'name': 'Foo$$execBar', 'session': 'test'})
            self.assertEqual(http.call_args.args, ('/api/workbench/command',
                {'command': 'analyze_function', 'name': 'Foo$$execBar', 'session': 'test'}))

    def test_exact_argument_types(self):
        for args in ({'action': 'probe', 'pid': True}, {'action': 'probe', 'pid': 1, 'shell': 'bad'},
                     {'action': 'unknown', 'pid': 1}, {'pid': 1}):
            with self.assertRaises(ValueError):
                self.adapter.call('umt_device_action', args)

    def test_struct_inspection_parameters_are_forwarded_exactly(self):
        args={'action':'inspect','kind':'struct','address':'0xF123456789ABCDEF','typeAddress':'0x1234',
              'dereference':True,'generation':7,'expectedIdentity':'exact-instance'}
        with patch.object(self.adapter,'http',return_value={'properties':[]}) as http:
            self.adapter.call('umt_explorer',args)
            self.assertEqual(http.call_args.args,('/api/explorer',args))

    def test_world_actor_scan_preserves_64bit_pointer_and_cursor_guards(self):
        args={'action':'actors','worldAddress':'0xF123456789ABCDEF','levelAddress':'0xF123456789AA0000',
              'cursor':4096,'limit':100,'scanLimit':4096,'generation':9,'expectedTopology':'0xABCD'}
        with patch.object(self.adapter,'http',return_value={'items':[]}) as http:
            self.adapter.call('umt_explorer',args)
            self.assertEqual(http.call_args.args,('/api/explorer',args))

    def test_explorer_retries_temporary_read_lock_but_device_actions_do_not(self):
        busy = ValueError('{"busy":true,"error":"read lock held"}')
        with patch.object(self.adapter,'http',side_effect=[busy,{'items':[]}]) as http, patch.object(mcp.time,'sleep') as sleep:
            self.assertEqual(self.adapter.call('umt_explorer',{'action':'worlds'}),{'items':[]})
            self.assertEqual(http.call_count,2)
            sleep.assert_called_once_with(.15)
        with patch.object(self.adapter,'http',side_effect=busy) as http, patch.object(mcp.time,'sleep') as sleep:
            with self.assertRaises(ValueError):
                self.adapter.call('umt_device_action',{'action':'probe','pid':123})
            self.assertEqual(http.call_count,1)
            sleep.assert_not_called()

    def test_tool_failure_is_reported_not_success(self):
        with patch.object(self.adapter, 'http', side_effect=OSError('unreachable')):
            response = self.adapter.handle({'id': 1, 'method': 'tools/call', 'params': {'name': 'umt_status'}})
        self.assertTrue(response['result']['isError'])
        self.assertIn('unreachable', response['result']['content'][0]['text'])

    def test_unknown_method(self):
        response = self.adapter.handle({'id': 3, 'method': 'shell/execute'})
        self.assertEqual(response['error']['code'], -32601)

    def test_loopback_only(self):
        with self.assertRaises(ValueError):
            mcp.Adapter('http://example.com')


if __name__ == '__main__':
    unittest.main()
