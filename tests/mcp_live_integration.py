"""Exercise the real stdio MCP -> workbench -> IDA chain on an explicit dump session."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', required=True)
    parser.add_argument('--function', required=True)
    parser.add_argument('--url', default='http://127.0.0.1:18766')
    parser.add_argument('--open', action='store_true', help='Create and open a new analysis copy')
    parser.add_argument('--report', type=Path, help='Save this Markdown report through MCP')
    parser.add_argument('--out', type=Path, default=ROOT / 'artifacts/mcp-live-integration.json')
    args = parser.parse_args()
    child = subprocess.Popen([sys.executable, str(ROOT / 'bridge/mcp_server.py'), '--url', args.url],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding='utf-8', creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    evidence, counter = {}, 0

    def rpc(method, params, notification=False):
        nonlocal counter
        counter += 1
        request = {'jsonrpc': '2.0', 'method': method, 'params': params}
        if not notification:
            request['id'] = counter
        child.stdin.write(json.dumps(request, ensure_ascii=False) + '\n')
        child.stdin.flush()
        if notification:
            return
        response = json.loads(child.stdout.readline())
        assert response.get('id') == counter, response
        assert 'error' not in response, response
        return response['result']

    def call(name, values=None):
        result = rpc('tools/call', {'name': name, 'arguments': values or {}})
        assert not result.get('isError'), result
        return json.loads(result['content'][0]['text'])

    def job(start):
        deadline = time.monotonic() + 360
        while time.monotonic() < deadline:
            state = call('umt_job', {'id': start['jobId']})
            if state['status'] in ('done', 'error'):
                assert state['status'] == 'done', state
                return state
            time.sleep(.5)
        raise TimeoutError(start['jobId'])

    try:
        evidence['initialize'] = rpc('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
            'clientInfo': {'name': 'umt-integration-test', 'version': '1'}})
        rpc('notifications/initialized', {}, notification=True)
        evidence['tools'] = rpc('tools/list', {})
        evidence['function'] = call('umt_get_function', {'name': args.function, 'session': args.session})
        assert evidence['function'].get('found') and not evidence['function'].get('ambiguous')
        evidence['schema'] = call('umt_ida_status', {'tool': 'decompile'})
        assert evidence['schema']['ida']['toolSchema']['name'] == 'decompile'
        if args.open:
            print('Opening fresh analysis copy...', flush=True)
            evidence['open'] = job(call('umt_open_in_ida', {'session': args.session}))
            assert evidence['open']['result']['session'] == args.session
            print('IDA database:', evidence['open']['result']['database'], flush=True)
        print('Analyzing', args.function, flush=True)
        evidence['analysis'] = job(call('umt_analyze_function', {'name': args.function, 'session': args.session}))
        result = evidence['analysis']['result']
        assert result['session'] == args.session and result['rva'] == evidence['function']['rva']
        assert result['evidence'] and not result['errors'], result.get('errors')
        if args.report:
            evidence['report'] = call('umt_save_analysis', {'markdown': args.report.read_text('utf-8')})
        evidence['passed'] = True
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(evidence, ensure_ascii=False, indent=2), 'utf-8')
        print('PASS; evidence:', args.out, flush=True)
    finally:
        child.stdin.close()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.terminate()
        child.stdout.close()
        child.stderr.close()


if __name__ == '__main__':
    main()
