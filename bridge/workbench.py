"""Local desktop proxy, immutable dump sessions, IDA jobs and Agent reports. No pip dependencies."""
from __future__ import annotations
import argparse
import concurrent.futures
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import mimetypes
import os
from pathlib import Path
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
if __package__ in (None, ''):
    from artifacts import ArtifactStore
    from ida_client import IDAClient, IDAError
    from ida_service import LocalIDAService, IDAServiceError
else:
    from .artifacts import ArtifactStore
    from .ida_client import IDAClient, IDAError
    from .ida_service import LocalIDAService, IDAServiceError

PROJECT = Path(__file__).resolve().parent.parent
VERSION = '1.0.1'


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.partial')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    os.replace(temporary, path)


class Workbench:
    def __init__(self, workspace, device_url='http://127.0.0.1:18765', ida_url=None, port=18766):
        parsed = urllib.parse.urlsplit(device_url)
        if parsed.scheme != 'http' or parsed.hostname not in ('localhost', '127.0.0.1', '::1') or parsed.path not in ('', '/'):
            raise ValueError('Use an ADB-forwarded loopback device origin.')
        self.store = ArtifactStore(Path(workspace))
        self.root = self.store.root
        settings = self.root / 'workbench-settings.json'
        saved = json.loads(settings.read_text('utf-8')) if settings.is_file() else {}
        ida_url = ida_url or saved.get('idaUrl') or 'http://127.0.0.1:13338/mcp'
        self.device_url, self.ida_url = device_url.rstrip('/'), ida_url
        self.reports = self.root / 'reports'
        self.reports.mkdir(exist_ok=True)
        self.ida = IDAClient(ida_url, timeout=300)
        self.ida_service = LocalIDAService(self.root / 'ida-service')
        self.instance_id, self.port = uuid.uuid4().hex, port
        self.jobs, self.lock = {}, threading.RLock()
        for saved in sorted(self.reports.glob('*.json'), key=lambda p: p.stat().st_mtime)[-50:]:
            try:
                job = json.loads(saved.read_text('utf-8'))
                if re.fullmatch('[0-9a-f]{32}', job.get('id', '')) and job.get('status') in ('done', 'error'):
                    self.jobs[job['id']] = job
            except (ValueError, OSError):
                pass
        self.ida_lock = threading.RLock()
        self.executor = concurrent.futures.ThreadPoolExecutor(max_workers=2, thread_name_prefix='umt-job')
        self._ida_status = {'connected': False, 'url': ida_url, 'message': 'Use Check IDA connection.'}
        self.bindings = {}
        review_path = self.root / 'session-reviews.json'
        self.reviews = json.loads(review_path.read_text('utf-8')) if review_path.is_file() else {}
        state = self.root / 'ida-bindings.json'
        if state.is_file():
            try:
                self.bindings = json.loads(state.read_text('utf-8'))
            except (ValueError, OSError):
                pass
        self.shutdown_callback = None
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def device(self, path, body=None):
        req = urllib.request.Request(self.device_url + path,
            data=None if body is None else json.dumps(body).encode(),
            headers={'Content-Type': 'application/json', 'X-UMT-Client': '1'})
        try:
            with self.opener.open(req, timeout=30) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            detail = error.read(65536).decode('utf-8', 'replace')
            raise ValueError('Device rejected request: ' + detail) from error

    def configuration(self):
        return {'mcpServers': {'unreal-memory-tools': {'command': sys.executable,
            'args': [str(PROJECT / 'bridge/mcp_server.py'), '--url', f'http://127.0.0.1:{self.port}']}}}

    def status(self):
        with self.lock:
            jobs = [{key: value for key, value in job.items() if key != 'result'} for job in self.jobs.values()]
            return {'service': 'umt-workbench', 'version': VERSION, 'pid': os.getpid(),
                'instanceId': self.instance_id, 'workspace': str(self.root), 'artifactRoot': str(self.root),
                'deviceUrl': self.device_url, 'idaUrl': self.ida_url, 'ida': self._ida_status,
                'sessions': [{**item, 'review': self.reviews.get(item['id'])} for item in self.store.list_sessions()], 'jobs': jobs[-30:], 'mcpConfig': self.configuration()}

    def job(self, identifier):
        with self.lock:
            if identifier not in self.jobs:
                saved = self.reports / (identifier + '.json')
                if not re.fullmatch('[0-9a-f]{32}', identifier) or not saved.is_file():
                    raise ValueError('Unknown job.')
                return json.loads(saved.read_text('utf-8'))
            return dict(self.jobs[identifier])

    def submit(self, kind, function):
        identifier = uuid.uuid4().hex
        with self.lock:
            if sum(job['status'] in ('running', 'queued') for job in self.jobs.values()) >= 8:
                raise ValueError('Job queue is full; wait for an existing job.')
            self.jobs[identifier] = {'id': identifier, 'kind': kind, 'status': 'queued', 'created': time.time()}

        def run():
            with self.lock:
                self.jobs[identifier]['status'] = 'running'
            try:
                result = function()
                with self.lock:
                    self.jobs[identifier].update(status='done', result=result, finished=time.time())
            except Exception as error:
                with self.lock:
                    self.jobs[identifier].update(status='error', error=str(error), finished=time.time())
            with self.lock:
                snapshot = dict(self.jobs[identifier])
            write_json(self.reports / (identifier + '.json'), snapshot)
        self.executor.submit(run)
        return {'jobId': identifier, 'status': 'running'}

    def ida_status(self, tool=None):
        with self.ida_lock:
            previous = self.ida.timeout
            self.ida.timeout = 5
            try:
                state = dict(self.ida.status())
            finally:
                self.ida.timeout = previous
            schemas = state.get('tools', [])
            state['tools'] = [item['name'] for item in schemas]
            with self.lock:
                self._ida_status = state
            if tool:
                schema = next((item for item in schemas if item['name'] == tool), None)
                if schema is None:
                    raise ValueError('IDA does not advertise tool: ' + str(tool))
                return {**state, 'toolSchema': schema}
            return state

    def _check_review(self, session):
        if not self.reviews:
            return
        identifier = session or self.store._session(None).name
        review = self.reviews.get(identifier, {})
        if review.get('status') == 'invalid':
            raise ValueError('Historical dump is quarantined: ' + review.get('reason', 'Resync a fresh dump.'))

    def open_ida(self, request):
        self._check_review(request.get('session'))
        with self.ida_lock:
            state = self.ida_status()
            if state.get('canOpenDatabase') is False:
                raise IDAError(state.get('message', 'IDA endpoint has no idb_open; connect an IDALib supervisor.'))
            if state.get('connected') is False:
                raise IDAError(state.get('error', 'IDA is not connected.'))
            prepared = self.store.prepare_ida(request.get('session'), request.get('library'))
            opened = self.ida.open_database(prepared['input'], bool(request.get('autoAnalysis', False)))
            health = self.ida._content(self.ida.call_tool('server_health', {}))
            if 'imagebase' not in health:
                raise IDAError('IDA opened, but its image base is unavailable; wait for database readiness.')
            binding = {**prepared, **opened, 'session': prepared['session'],
                       'idaSession': opened.get('session'), 'imagebase': health['imagebase']}
            self.bindings[prepared['session']] = binding
            write_json(self.root / 'ida-bindings.json', self.bindings)
            self.ida_status()
            return binding

    def _select(self, session):
        session_id = self.store._session(session).name
        self._check_review(session_id)
        binding = self.bindings.get(session_id)
        if not binding:
            raise ValueError('Open this dump session in IDA first.')
        self.ida.select_database(binding['database'])
        health = self.ida._content(self.ida.call_tool('server_health', {}))
        if health.get('status') == 'busy' or 'imagebase' not in health:
            raise IDAError('IDA is busy; retry when analysis finishes.')
        if Path(health.get('input_path', '')).resolve() != Path(binding['input']).resolve():
            raise IDAError('IDA input differs from the selected dump; reopen the matching analysis copy.')
        return binding, int(health['imagebase'], 0)

    def analyze(self, request):
        self._check_review(request.get('session'))
        with self.ida_lock:
            row = self.store.get_function(request.get('name', ''), request.get('session'))
            if not row.get('found') or row.get('ambiguous'):
                raise ValueError('Select an exact unique function name. Lookup: ' + json.dumps(row))
            if request.get('rva') is not None and int(request['rva'], 0) != int(row['rva'], 0):
                raise ValueError('Selected RVA does not match script.json.')
            binding, imagebase = self._select(row['session'])
            rva = int(row['rva'], 0)
            if not rva or not any(segment['memoryOffset'] <= rva < segment['memoryOffset'] + segment['fileSize']
                                  and segment['flags'] & 1 for segment in binding['mapping']['segments']):
                raise ValueError('Function RVA is not inside a dumped executable PT_LOAD segment.')
            address = hex(imagebase + rva)
            setup = {}
            lookup = self.ida.call_tool('lookup_funcs', {'queries': address})
            setup['lookup'] = lookup
            # Only the selected analysis copy is changed; source dumps remain immutable.
            if request.get('defineFunction', True):
                setup['define'] = self.ida.call_tool('define_func', {'items': {'addr': address}})
                setup['rename'] = self.ida.call_tool('rename', {'batch': {'func': {'addr': address, 'name': row['name']}, 'allow_overwrite': False}})
            evidence = self.ida.analyze_function(address)
            result = {**evidence, 'function': row, 'session': row['session'], 'database': binding['database'],
                'idaImagebase': hex(imagebase), 'rva': row['rva'], 'setupEvidence': setup,
                'runtimeAddressRule': 'current module base + RVA',
                'callingNotes': self.calling_notes(row), 'traceScript': self.trace_script(row, Path(binding['input']).name)}
            digest = hashlib.sha256((row['session'] + row['name']).encode()).hexdigest()[:20]
            trace = self.reports / (digest + '-trace.js')
            trace.write_text(result['traceScript'], encoding='utf-8')
            result['tracePath'] = str(trace)
            self.ida_status()
            return result

    @staticmethod
    def calling_notes(function):
        return ['script.json Address is an RVA; IDA EA and live runtime addresses use different bases.',
            'Names containing $$exec generally identify Unreal VM thunks. Their ABI typically takes UObject*, FFrame&, and result storage; the SDK method signature is not the thunk ABI.',
            'Confirm the recovered prototype, argument widths, AArch64 register/stack use, object lifetime and game-thread affinity before a NativeFunction or C++ call.',
            'For reflected functions, recover the UFunction and parameter structure (offsets, flags, return slot) and use the matching ProcessEvent signature. The export alone does not establish a callable object instance.',
            'The supplied trace script observes entry/return registers; it does not invent typed arguments or execute the target. Use the IDA evidence to derive the requested hook/call implementation.']

    @staticmethod
    def trace_script(function, module):
        return """// Generated for this exact dump. Verify the same game build before attaching.
// x0..x3 are raw ARM64 argument registers, not recovered typed arguments.
'use strict';
const moduleName = %s;
const rva = ptr(%s);
const image = Process.getModuleByName(moduleName);
const address = image.base.add(rva);
const range = Process.findRangeByAddress(address);
if (!range || !range.protection.includes('x')) throw new Error('RVA is not executable');
let remaining = 30;
Interceptor.attach(address, {
  onEnter(args) {
    this.sample = remaining-- > 0;
    if (this.sample) console.log(JSON.stringify({name:%s, address:address.toString(), thread:this.threadId,
      x0:args[0].toString(), x1:args[1].toString(), x2:args[2].toString(), x3:args[3].toString()}));
  },
  onLeave(value) { if (this.sample) console.log('return(raw): ' + value); }
});
""" % (json.dumps(module), json.dumps(function['rva']), json.dumps(function['name']))

    def command(self, request):
        if not isinstance(request, dict):
            raise ValueError('Expected an object.')
        kind = request.get('command')
        with self.lock:
            starting_ida = any(job['kind'] == 'start_ida' and job['status'] in ('queued', 'running')
                               for job in self.jobs.values())
            if starting_ida and kind in ('start_ida', 'configure_ida', 'open_in_ida', 'analyze_function', 'ida_tool'):
                raise ValueError('IDA 正在启动，请等待连接检查完成后再操作。')
        session = request.get('session') or None
        request = {**request, 'session': session}
        if kind == 'sync_dump':
            def sync():
                files = self.device('/api/files')
                if not request.get('symbolsOnly') and not any(f['name'] in ('libUE4.so', 'libUnreal.so') for f in files):
                    raise ValueError('Export the runtime library on the device, or select symbols only.')
                return self.store.sync_from_device(self.device_url, bool(request.get('symbolsOnly', False)))
            return self.submit(kind, sync)
        if kind == 'import_dump':
            return self.submit(kind, lambda: self.store.import_directory(request['path'], bool(request.get('symbolsOnly', False))))
        if kind == 'search_functions':
            self._check_review(session)
            return {'functions': self.store.search_functions(request.get('query', ''), request.get('limit', 50), session)}
        if kind == 'get_function':
            self._check_review(session)
            return self.store.get_function(request['name'], session)
        if kind == 'ida_status':
            return {'ida': self.ida_status(request.get('tool'))}
        if kind == 'start_ida':
            url = self.ida_service.normalize_url(request.get('url', self.ida_url))
            if urllib.parse.urlsplit(url).port in (self.port, urllib.parse.urlsplit(self.device_url).port or 80):
                raise ValueError('IDA MCP 端口不能与电脑工作台或手机转发端口相同。')
            with self.ida_lock:
                with self.lock:
                    if any(job['status'] in ('queued', 'running') for job in self.jobs.values()):
                        raise ValueError('Wait for running jobs before starting or changing the IDA service.')
                    write_json(self.root / 'workbench-settings.json', {'idaUrl': url})
                    self.ida, self.ida_url = IDAClient(url, timeout=300), url
                    self._ida_status = {'connected': False, 'url': url, 'starting': True,
                                        'message': '正在检查本机 IDA 并启动连接服务…'}

                    def start():
                        try:
                            service = self.ida_service.ensure(url)
                            state = self.ida_status()
                            if not state.get('connected') or not state.get('canOpenDatabase') or not state.get('canSelectDatabase'):
                                raise IDAServiceError(state.get('error') or state.get('message') or 'IDA service did not become ready.')
                            return {'ida': state, 'idaUrl': url, 'service': service}
                        except Exception as error:
                            with self.lock:
                                self._ida_status = {'connected': False, 'url': url, 'starting': False,
                                                    'message': str(error), 'error': str(error)}
                            raise

                    return self.submit(kind, start)
        if kind == 'configure_ida':
            candidate = IDAClient(request.get('url', ''), timeout=300)
            with self.ida_lock:
                with self.lock:
                    if any(job['status'] in ('queued', 'running') for job in self.jobs.values()):
                        raise ValueError('Wait for running jobs before changing the IDA endpoint.')
                write_json(self.root / 'workbench-settings.json', {'idaUrl': candidate.url})
                self.ida, self.ida_url = candidate, candidate.url
                return {'ida': self.ida_status(), 'idaUrl': self.ida_url}
        if kind == 'open_in_ida':
            return self.submit(kind, lambda: self.open_ida(request))
        if kind == 'analyze_function':
            return self.submit(kind, lambda: self.analyze(request))
        if kind == 'ida_tool':
            allowed = {'lookup_funcs', 'decompile', 'disasm', 'xrefs_to', 'xref_query', 'callees', 'callgraph',
                'analyze_function', 'analyze_component', 'trace_data_flow', 'get_bytes', 'get_int', 'get_string',
                'survey_binary', 'type_inspect', 'read_struct', 'func_profile', 'set_comments', 'append_comments',
                'rename', 'set_type', 'declare_type', 'define_func'}
            if request.get('name') not in allowed:
                raise ValueError('Tool is not exposed by this analysis adapter; use the dedicated IDA MCP connection for other tools.')
            def execute():
                with self.ida_lock:
                    self._select(session)
                    args = dict(request.get('arguments', {}))
                    args.pop('database', None)
                    return self.ida.call_tool(request['name'], args)
            return self.submit(kind, execute)
        if kind == 'save_analysis':
            content = request.get('markdown', '')
            if not isinstance(content, str) or not content.strip() or len(content.encode()) > 2 * 1024**2:
                raise ValueError('Provide a nonempty Markdown report under 2 MiB.')
            identifier = uuid.uuid4().hex
            path = self.reports / (identifier + '.md')
            path.write_text(content, encoding='utf-8')
            saved = {'id': identifier, 'kind': 'agent_report', 'status': 'done',
                     'created': time.time(), 'finished': time.time(),
                     'result': {'markdown': content, 'reportUrl': '/api/workbench/report?id=' + identifier}}
            write_json(self.reports / (identifier + '.json'), saved)
            with self.lock:
                self.jobs[identifier] = saved
            return {'id': identifier, 'path': str(path), 'url': '/api/workbench/report?id=' + identifier}
        if kind == 'shutdown' and request.get('instanceId') == self.instance_id and self.shutdown_callback:
            threading.Thread(target=self.shutdown_callback, daemon=True).start()
            return {'stopping': True}
        raise ValueError('Unknown workbench command.')


class Handler(BaseHTTPRequestHandler):
    server_version = 'UMT/' + VERSION
    protocol_version = 'HTTP/1.1'

    @property
    def app(self):
        return self.server.app

    def log_message(self, *args):
        pass

    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def headers_ok(self):
        hosts = self.headers.get_all('Host', [])
        if len(hosts) != 1:
            raise ValueError('Exactly one Host is required.')
        authority = urllib.parse.urlsplit('http://' + hosts[0])
        if authority.hostname not in ('localhost', '127.0.0.1', '::1') or authority.username or authority.password:
            raise ValueError('Invalid Host.')
        if authority.port not in (None, self.server.server_port):
            raise ValueError('Unexpected port.')
        if self.command == 'POST':
            if self.headers.get('X-UMT-Client') != '1' or self.headers.get_content_type() != 'application/json':
                raise ValueError('Expected JSON and X-UMT-Client: 1.')
            origin = self.headers.get('Origin')
            if origin is not None and origin != 'http://' + hosts[0]:
                raise ValueError('Origin must match Host.')
        if self.headers.get('Transfer-Encoding') or len(self.headers.get_all('Content-Length', [])) > 1:
            raise ValueError('Unsupported request framing.')

    def respond(self, code, data, content_type='application/json; charset=utf-8'):
        raw = data if isinstance(data, bytes) else json.dumps(data, ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(raw)))
        self.send_header('Connection', 'close')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Security-Policy', "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(raw)
        self.close_connection = True

    def proxy(self, body=None):
        allowed = ('/api/state', '/api/processes', '/api/action', '/api/explorer', '/api/files', '/api/file', '/api/health')
        path = urllib.parse.urlsplit(self.path).path
        if path not in allowed:
            self.respond(404, {'error': 'Unknown endpoint.'})
            return
        req = urllib.request.Request(self.app.device_url + self.path, data=body,
            headers={'Content-Type': 'application/json', 'X-UMT-Client': '1'})
        try:
            remote = self.app.opener.open(req, timeout=60)
        except urllib.error.HTTPError as error:
            remote = error
        with remote:
            self.send_response(remote.status)
            for key in ('Content-Type', 'Content-Length', 'Content-Disposition'):
                if remote.headers.get(key):
                    self.send_header(key, remote.headers[key])
            self.send_header('Connection', 'close')
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            while chunk := remote.read(1024 * 1024):
                self.wfile.write(chunk)
        self.close_connection = True

    def do_GET(self):
        try:
            self.headers_ok()
            parsed = urllib.parse.urlsplit(self.path)
            if parsed.path == '/api/workbench/status':
                self.respond(200, self.app.status())
            elif parsed.path == '/api/workbench/job':
                self.respond(200, self.app.job(urllib.parse.parse_qs(parsed.query).get('id', [''])[0]))
            elif parsed.path == '/api/workbench/report':
                identifier = urllib.parse.parse_qs(parsed.query).get('id', [''])[0]
                if not re.fullmatch('[0-9a-f]{32}', identifier):
                    raise ValueError('Invalid report id.')
                path = self.app.reports / (identifier + '.md')
                if path.is_file():
                    self.respond(200, path.read_bytes(), 'text/markdown; charset=utf-8')
                else:
                    self.respond(200, self.app.job(identifier))
            elif parsed.path.startswith('/api/'):
                self.proxy()
            else:
                assets = {'/': 'index.html', '/index.html': 'index.html', '/app.js': 'app.js', '/style.css': 'style.css'}
                filename = assets.get(parsed.path)
                if not filename:
                    self.respond(404, {'error': 'Not found.'})
                    return
                data = (PROJECT / 'desktop/web' / filename).read_bytes()
                mime = {'html': 'text/html', 'js': 'text/javascript', 'css': 'text/css'}[filename.rsplit('.', 1)[1]]
                self.respond(200, data, mime + '; charset=utf-8')
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        except Exception as error:
            self.respond(400, {'error': str(error)})

    def do_POST(self):
        try:
            self.headers_ok()
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 3 * 1024**2:
                raise ValueError('Body must be 1 byte to 3 MiB.')
            body = self.rfile.read(length)
            if len(body) != length:
                raise ValueError('Incomplete body.')
            if self.path == '/api/workbench/command':
                self.respond(200, self.app.command(json.loads(body)))
            else:
                self.proxy(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        except Exception as error:
            self.respond(400, {'error': str(error)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device-url', default='http://127.0.0.1:18765')
    parser.add_argument('--ida-url', default=None, help='Override the saved workspace IDA endpoint')
    parser.add_argument('--workspace', type=Path, default=PROJECT / 'artifacts')
    parser.add_argument('--port', type=int, default=18766)
    args = parser.parse_args()
    app = Workbench(args.workspace, args.device_url, args.ida_url, args.port)
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.app = app
    app.shutdown_callback = server.shutdown
    print('UMT Workbench http://127.0.0.1:' + str(args.port), flush=True)
    try:
        server.serve_forever(poll_interval=.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        app.executor.shutdown(wait=True)


if __name__ == '__main__':
    main()
