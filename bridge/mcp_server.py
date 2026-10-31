"""MCP stdio adapter for the UMT local workbench (JSON Lines transport)."""
import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


def tool(name, description, properties=None, required=None, readonly=True):
    return {'name': name, 'description': description,
            'inputSchema': {'type': 'object', 'properties': properties or {}, 'required': required or [], 'additionalProperties': False},
            'annotations': {'readOnlyHint': readonly, 'destructiveHint': False, 'openWorldHint': False}}


S = {'type': 'string'}
I = {'type': 'integer'}
B = {'type': 'boolean'}
TOOLS = [
    tool('umt_status', 'Read device/desktop sessions, current jobs, IDA availability and MCP config.'),
    tool('umt_processes', 'List Unreal target processes on the connected Android device.'),
    tool('umt_device_state', 'Read current probe/dump progress, error, offsets and structures.'),
    tool('umt_device_action', 'Start probe, SDK dump, or library export. Poll umt_device_state until idle.',
         {'action': {'type': 'string', 'enum': ['probe', 'dump', 'library']}, 'pid': I}, ['action', 'pid'], False),
    tool('umt_explorer', 'Read live objects or inspect instances/structures. worlds discovers live UWorlds; actors traverses PersistentLevel and loaded World.Levels through ULevel.Actors. Use cursors, generation and expectedTopology for guarded pagination.',
         {'action': {'type': 'string', 'enum': ['list', 'inspect', 'worlds', 'actors']}, 'cursor': I, 'limit': I, 'scanLimit': I,
          'query': S, 'address': S, 'kind': {'type': 'string', 'enum': ['object', 'instance', 'struct', 'field']}, 'maxRows': I, 'generation': I,
          'expectedIdentity': S, 'typeAddress': S, 'dereference': B, 'worldAddress': S, 'levelAddress': S, 'expectedTopology': S}, ['action']),
    tool('umt_sync_dump', 'Copy SDK+library into a hashed immutable desktop session; returns jobId. Poll umt_job.',
         {'symbolsOnly': B}, [], False),
    tool('umt_import_dump', 'Import an existing dump directory within the configured artifact workspace. Returns jobId.',
         {'path': S, 'symbolsOnly': B}, ['path'], False),
    tool('umt_search_functions', 'Search script.json symbol names and recover owner-specific SDK declarations. RVAs are strings.',
         {'query': S, 'limit': I, 'session': S}, ['query']),
    tool('umt_get_function', 'Find an exact function name, reporting duplicate-name ambiguity.', {'name': S, 'session': S}, ['name']),
    tool('umt_ida_status', 'Discover actual IDA MCP tools and database sessions. Pass tool to retrieve its full advertised input schema.', {'tool': S}),
    tool('umt_open_in_ida', 'Prepare a separate mapped ELF analysis image, open it using IDA MCP, and bind the exact returned database ID. Poll umt_job.',
         {'session': S, 'library': S, 'autoAnalysis': B}, [], False),
    tool('umt_analyze_function', 'Resolve exact exported function RVA to matching IDA database; collect decompilation, assembly, xrefs and callees, ABI notes and a raw Frida trace script. Defines/names that function in the analysis copy by default. Poll umt_job.',
         {'name': S, 'rva': S, 'session': S, 'defineFunction': B}, ['name'], False),
    tool('umt_ida_tool', 'Explicit IDA analysis/comment/type/rename tool call on the selected artifact database. Tool names/schema are available from umt_ida_status; arguments exclude database.',
         {'name': S, 'arguments': {'type': 'object'}, 'session': S}, ['name', 'arguments'], False),
    tool('umt_job', 'Read an asynchronous job, including its actual result or failure.', {'id': S}, ['id']),
    tool('umt_save_analysis', 'Persist the Agent final Markdown analysis, hook/call code and verification steps for Web download.',
         {'markdown': S}, ['markdown'], False),
]
PROMPT = '''Use umt_status and umt_device_state to identify the target and artifact session. If needed, run probe, dump and library in order and poll completion. Sync artifacts, search the exact exported function, open the matching analysis image, then analyze it and inspect relevant callers/callees through umt_ida_tool. Distinguish the UE exec thunk ABI from the SDK reflected method signature. Verify current module base + RVA, this/object lifetime, parameter structure, ProcessEvent versus direct native calls and thread affinity. Explain observed behavior with evidence, state unresolved assumptions, then implement the user's requested hook/call or equivalent feature and its validation steps. Do not invent successful execution. Save the final report and code with umt_save_analysis.'''


class Adapter:
    def __init__(self, url):
        p = urllib.parse.urlsplit(url)
        if p.scheme != 'http' or p.hostname not in ('127.0.0.1', 'localhost', '::1'):
            raise ValueError('Workbench URL must use loopback HTTP.')
        self.url = url.rstrip('/')
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def http(self, path, data=None):
        req = urllib.request.Request(self.url + path,
            data=None if data is None else json.dumps(data).encode(),
            headers={'Content-Type': 'application/json', 'X-UMT-Client': '1'})
        try:
            with self.opener.open(req, timeout=45) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            raise ValueError(error.read(65536).decode('utf-8', 'replace')) from error

    def call(self, name, args):
        schema = next((entry for entry in TOOLS if entry['name'] == name), None)
        if schema is None:
            raise ValueError('Unknown tool.')
        if not isinstance(args, dict) or any(key not in schema['inputSchema']['properties'] for key in args):
            raise ValueError('Unexpected tool argument.')
        if any(key not in args for key in schema['inputSchema']['required']):
            raise ValueError('Required tool argument missing.')
        for key, value in args.items():
            prop = schema['inputSchema']['properties'][key]
            expected = {'string': str, 'integer': int, 'boolean': bool, 'object': dict}[prop['type']]
            if not isinstance(value, expected) or prop['type'] == 'integer' and isinstance(value, bool):
                raise ValueError('Invalid type: ' + key)
            if 'enum' in prop and value not in prop['enum']:
                raise ValueError('Invalid value: ' + key)
        direct = {'umt_status': '/api/workbench/status', 'umt_processes': '/api/processes', 'umt_device_state': '/api/state'}
        if name in direct:
            return self.http(direct[name])
        if name == 'umt_job':
            return self.http('/api/workbench/job?id=' + urllib.parse.quote(args['id'], safe=''))
        if name == 'umt_device_action':
            return self.http('/api/action', args)
        if name == 'umt_explorer':
            # A live Web inspector may briefly hold the shared read lock.
            # Retry only this read-only operation, never device mutations.
            for attempt in range(10):
                try:
                    result = self.http('/api/explorer', args)
                    if not result.get('busy') or attempt == 9:
                        return result
                except ValueError as error:
                    try:
                        detail = json.loads(str(error))
                    except ValueError:
                        raise error
                    if not isinstance(detail, dict) or not detail.get('busy') or attempt == 9:
                        raise error
                time.sleep(.15)
        return self.http('/api/workbench/command', {'command': name.removeprefix('umt_'), **args})

    def handle(self, request):
        identifier = request.get('id')
        method, params = request.get('method'), request.get('params', {})
        if identifier is None:
            return None
        try:
            if method == 'initialize':
                result = {'protocolVersion': '2024-11-05', 'capabilities': {'tools': {}, 'prompts': {}},
                          'serverInfo': {'name': 'unreal-memory-tools', 'version': '1.0.1'}, 'instructions': PROMPT}
            elif method == 'ping':
                result = {}
            elif method == 'tools/list':
                result = {'tools': TOOLS}
            elif method == 'tools/call':
                try:
                    data = self.call(params.get('name'), params.get('arguments', {}))
                    result = {'content': [{'type': 'text', 'text': json.dumps(data, ensure_ascii=False)}], 'isError': False}
                except Exception as error:
                    result = {'content': [{'type': 'text', 'text': str(error)}], 'isError': True}
            elif method == 'prompts/list':
                result = {'prompts': [{'name': 'analyze_ue_function', 'description': 'Evidence-driven UE function / hook / invocation workflow',
                                      'arguments': [{'name': 'objective', 'description': 'Desired behavior or function', 'required': True}]}]}
            elif method == 'prompts/get' and params.get('name') == 'analyze_ue_function':
                result = {'description': 'UE analysis task', 'messages': [{'role': 'user', 'content': {'type': 'text',
                    'text': str(params.get('arguments', {}).get('objective', '')) + '\n\n' + PROMPT}}]}
            else:
                return {'jsonrpc': '2.0', 'id': identifier, 'error': {'code': -32601, 'message': 'Method not found'}}
            return {'jsonrpc': '2.0', 'id': identifier, 'result': result}
        except Exception as error:
            return {'jsonrpc': '2.0', 'id': identifier, 'error': {'code': -32602, 'message': str(error)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:18766')
    args = parser.parse_args()
    adapter = Adapter(args.url)
    # stdout belongs exclusively to MCP messages, never logs or progress.
    while line := sys.stdin.buffer.readline(4 * 1024**2 + 1):
        try:
            if len(line) > 4 * 1024**2:
                raise ValueError('MCP message too large.')
            request = json.loads(line)
            if not isinstance(request, dict):
                raise ValueError('Expected JSON-RPC object.')
            response = adapter.handle(request)
        except Exception as error:
            response = {'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': str(error)}}
        if response is not None:
            sys.stdout.buffer.write((json.dumps(response, ensure_ascii=False) + '\n').encode('utf-8'))
            sys.stdout.buffer.flush()


if __name__ == '__main__':
    main()
