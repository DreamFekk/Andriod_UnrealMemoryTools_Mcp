"""Small dependency-free MCP JSON-RPC client for the installed IDA server.

Only explicit call_tool calls can mutate a database. analyze_function chooses
read-only tools from the actual advertised schemas and preserves raw evidence.
"""
from __future__ import annotations
import json
import errno
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request
import uuid


class IDAError(RuntimeError):
    pass


class IDAClient:
    def __init__(self, url='http://127.0.0.1:13337/mcp', timeout=30, database=None):
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme not in ('http', 'https') or parsed.hostname not in ('127.0.0.1', 'localhost', '::1') or parsed.username or parsed.password:
            raise ValueError('IDA MCP must be a loopback HTTP endpoint.')
        self.url, self.timeout = url, timeout
        self.session_id = None
        self.protocol = '2024-11-05'
        self.initialized = False
        self.server_info = None
        self.tools = None
        self.database = database
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *args, **kwargs):
                raise IDAError('IDA endpoint redirects are not accepted.')
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def _rpc(self, method, params=None, notification=False):
        request_id = uuid.uuid4().hex
        body = {'jsonrpc': '2.0', 'method': method}
        if not notification:
            body['id'] = request_id
        if params is not None:
            body['params'] = params
        headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream'}
        if self.session_id:
            headers['Mcp-Session-Id'] = self.session_id
            headers['MCP-Protocol-Version'] = self.protocol
        request = urllib.request.Request(self.url, data=json.dumps(body).encode(), headers=headers)
        try:
            with self.opener.open(request, timeout=self.timeout) as response:
                self.session_id = response.headers.get('Mcp-Session-Id', self.session_id)
                content_type = response.headers.get('Content-Type', '').lower()
                if notification or response.status in (202, 204):
                    response.read(1024)
                    return None
                if 'text/event-stream' in content_type:
                    reply = self._sse(response, request_id)
                else:
                    payload = response.read(32 * 1024 * 1024 + 1)
                    if len(payload) > 32 * 1024 * 1024:
                        raise IDAError('IDA response exceeds 32 MiB.')
                    reply = json.loads(payload)
        except urllib.error.HTTPError as exc:
            detail = exc.read(4096).decode('utf-8', errors='replace')
            raise IDAError('IDA HTTP {}: {}'.format(exc.code, detail)) from exc
        except (OSError, ValueError) as exc:
            reason = getattr(exc, 'reason', exc)
            if isinstance(reason, OSError) and (reason.errno in (errno.ECONNREFUSED, 10061) or getattr(reason, 'winerror', None) == 10061):
                raise IDAError('IDA MCP 地址 ' + self.url + ' 拒绝连接：该端口没有运行中的服务。'
                               '在电脑工作台点击“保存并自动连接”启动已安装的 IDA MCP。') from exc
            raise IDAError('IDA transport: ' + str(exc)) from exc
        if not isinstance(reply, dict) or reply.get('id') != request_id:
            raise IDAError('IDA returned an invalid or mismatched JSON-RPC response.')
        if 'error' in reply:
            raise IDAError(json.dumps(reply['error'], ensure_ascii=False))
        if 'result' not in reply:
            raise IDAError('IDA JSON-RPC response has no result.')
        return reply['result']

    @staticmethod
    def _sse(response, request_id):
        lines, total = [], 0
        while True:
            line = response.readline(1024 * 1024 + 1)
            if not line:
                if lines:
                    message = json.loads('\n'.join(lines))
                    if isinstance(message, dict) and message.get('id') == request_id:
                        return message
                break
            total += len(line)
            if len(line) > 1024 * 1024 or total > 32 * 1024 * 1024:
                raise IDAError('IDA SSE response is too large.')
            text = line.decode('utf-8').rstrip('\r\n')
            if not text:
                if lines:
                    message = json.loads('\n'.join(lines))
                    if isinstance(message, dict) and message.get('id') == request_id:
                        return message
                    lines = []
            elif text.startswith('data:'):
                lines.append(text[5:].lstrip(' '))
        raise IDAError('IDA SSE stream ended before its JSON-RPC response.')

    def initialize(self):
        if self.initialized:
            return self.server_info
        result = self._rpc('initialize', {'protocolVersion': self.protocol, 'capabilities': {},
            'clientInfo': {'name': 'UnrealMemoryTools-IDA-Bridge', 'version': '1.0.0'}})
        if not isinstance(result, dict):
            raise IDAError('Invalid MCP initialize result.')
        self.protocol = result.get('protocolVersion', self.protocol)
        self.server_info = result
        self._rpc('notifications/initialized', notification=True)
        self.initialized = True
        return result

    def list_tools(self, refresh=False):
        self.initialize()
        if self.tools is None or refresh:
            tools, cursor, seen = [], None, set()
            for _ in range(100):
                result = self._rpc('tools/list', {} if cursor is None else {'cursor': cursor})
                if not isinstance(result, dict) or not isinstance(result.get('tools'), list):
                    raise IDAError('Invalid MCP tools/list result.')
                tools.extend(result['tools'])
                cursor = result.get('nextCursor')
                if not cursor:
                    break
                if cursor in seen:
                    raise IDAError('MCP tools/list repeated a pagination cursor.')
                seen.add(cursor)
            else:
                raise IDAError('MCP tools/list pagination limit exceeded.')
            self.tools = tools
        return self.tools

    def call_tool(self, name, args):
        if not isinstance(name, str) or not isinstance(args, dict):
            raise ValueError('Tool calls require a tool name and an argument object.')
        tools = self.list_tools()
        tool = next((item for item in tools if item.get('name') == name), None)
        if tool is None:
            raise IDAError('IDA does not advertise tool: ' + name)
        schema = tool.get('inputSchema', {})
        args = dict(args)
        if self.database is not None and 'database' in schema.get('properties', {}):
            args.setdefault('database', self.database)
        if 'database' in schema.get('required', []) and not args.get('database'):
            raise IDAError('Select an IDA database session before calling this tool (idb_list / idb_open).')
        return self._rpc('tools/call', {'name': name, 'arguments': args})

    def status(self):
        try:
            tools = self.list_tools(refresh=True)
            status = {'connected': True, 'url': self.url, 'server': self.server_info,
                    'sessionId': self.session_id, 'database': self.database, 'tools': tools, 'toolCount': len(tools)}
            status['canOpenDatabase'] = any(self._matches(t.get('name', ''), ('idb_open',)) for t in tools)
            status['canSelectDatabase'] = any(self._matches(t.get('name', ''), ('idb_list',)) for t in tools)
            status['endpointMode'] = 'database_manager' if status['canOpenDatabase'] else 'current_database'
            if not status['canOpenDatabase']:
                status['message'] = ('已连接到仅提供当前数据库工具的 IDA 端点；缺少 idb_open，不能自动打开新库。'
                    '请连接 IDALib supervisor。若 GUI 插件占用 13337，可将 supervisor 启动在 13338，并修改下方 IDA 地址。')
            listing = next((tool['name'] for tool in tools if self._matches(tool.get('name', ''), ('idb_list', 'idalib_list'))), None)
            if listing:
                raw = self.call_tool(listing, {})
                status['sessionsEvidence'] = raw
                status['sessions'] = self._content(raw).get('sessions', [])
            if self.database:
                health = next((tool['name'] for tool in tools if self._matches(tool.get('name', ''), ('server_health',))), None)
                if health:
                    raw = self.call_tool(health, {})
                    status['databaseHealthEvidence'] = raw
                    status['databaseHealth'] = self._content(raw)
            return status
        except (IDAError, ValueError) as exc:
            return {'connected': False, 'url': self.url, 'error': str(exc)}

    @staticmethod
    def _content(result):
        if not isinstance(result, dict):
            return {}
        if isinstance(result.get('structuredContent'), dict):
            return result['structuredContent']
        for block in result.get('content', []):
            if isinstance(block, dict) and block.get('type') == 'text':
                try:
                    parsed = json.loads(block.get('text', ''))
                    if isinstance(parsed, dict):
                        return parsed
                except ValueError:
                    pass
        return result

    def select_database(self, database):
        if not isinstance(database, str) or not database:
            raise ValueError('Select an exact IDA database session ID.')
        tools = self.list_tools()
        listing = next((tool['name'] for tool in tools if self._matches(tool.get('name', ''), ('idb_list', 'idalib_list'))), None)
        if not listing:
            raise IDAError('This endpoint does not advertise database session enumeration.')
        raw = self.call_tool(listing, {})
        sessions = self._content(raw).get('sessions', [])
        selected = next((session for session in sessions if session.get('session_id') == database), None)
        if selected is None:
            raise IDAError('Requested database session is not advertised by IDA.')
        self.database = database
        return {'database': database, 'session': selected, 'evidence': raw}

    def open_database(self, path, auto_analysis=False):
        path = Path(path).expanduser().resolve(strict=True)
        if not path.is_file() or not (path.parent / 'mapping.json').is_file() or not (path.parent / 'script.json').is_file():
            raise ValueError('Open a unique analysis copy produced by ArtifactStore.prepare_ida().')
        tool = next((tool for tool in self.list_tools(refresh=True) if self._matches(tool.get('name', ''), ('idb_open',))), None)
        if tool is None:
            raise IDAError('当前 IDA 端点 ' + self.url + ' 没有 idb_open，通常是 GUI 插件而不是 IDALib supervisor。'
                '请启动 python -m ida_pro_mcp.idalib_supervisor --host 127.0.0.1 --port 13338，'
                '再把工作台 IDA 地址设为 http://127.0.0.1:13338/mcp。无需关闭现有 IDA 窗口。')
        properties = tool.get('inputSchema', {}).get('properties', {})
        if 'input_path' not in properties:
            raise IDAError('Unrecognized idb_open path schema.')
        arguments = {'input_path': str(path)}
        options = {'mode': 'prefer_headless', 'run_auto_analysis': bool(auto_analysis),
                   'build_caches': bool(auto_analysis), 'init_hexrays': True}
        arguments.update({key: value for key, value in options.items() if key in properties})
        raw = self.call_tool(tool['name'], arguments)
        decoded = self._content(raw)
        database = decoded.get('session', {}).get('session_id')
        if not decoded.get('success') or not isinstance(database, str) or not database:
            raise IDAError('IDA did not return a successful database session: ' + json.dumps(raw, ensure_ascii=False))
        self.database = database
        return {'database': database, 'session': decoded['session'], 'evidence': raw}

    @staticmethod
    def _matches(name, aliases):
        lower = name.lower()
        return any(lower == alias or lower.endswith('_' + alias) for alias in aliases)

    def _arguments(self, tool, address):
        schema = tool.get('inputSchema', {})
        properties = schema.get('properties', {})
        required = schema.get('required', [])
        arguments = {}
        address_keys = ('addr', 'address', 'ea', 'addrs', 'addresses', 'targets', 'queries', 'query', 'roots')
        selected = next((key for key in address_keys if key in properties), None)
        if selected is None:
            raise IDAError('No recognized address field in tool schema: ' + tool['name'])
        def address_value(prop, depth=0):
            if depth > 4:
                raise IDAError('Tool address schema nesting is too deep.')
            if 'anyOf' in prop or 'oneOf' in prop:
                for option in prop.get('anyOf', prop.get('oneOf', [])):
                    try:
                        return address_value(option, depth + 1)
                    except IDAError:
                        continue
                raise IDAError('No supported address schema alternative.')
            kind = prop.get('type')
            if kind == 'array':
                return [address_value(prop.get('items', {}), depth + 1)]
            if kind == 'object':
                nested = prop.get('properties', {})
                key = next((key for key in ('addr', 'address', 'ea') if key in nested), None)
                if key is None:
                    raise IDAError('Unsupported nested address schema.')
                value = {key: address_value(nested[key], depth + 1)}
                for name in prop.get('required', []):
                    if name == key:
                        continue
                    if 'default' in nested.get(name, {}):
                        value[name] = nested[name]['default']
                    else:
                        raise IDAError('Unsupported required nested field: ' + name)
                return value
            if kind == 'integer':
                return int(address, 16)
            if kind in ('string', None):
                return address
            raise IDAError('Unsupported address field type.')
        arguments[selected] = address_value(properties[selected])
        for name in required:
            if name in arguments:
                continue
            prop = properties.get(name, {})
            if 'default' in prop:
                arguments[name] = prop['default']
            elif name == 'database' and self.database is not None:
                arguments[name] = self.database
            elif name in ('max_instructions', 'count', 'limit'):
                arguments[name] = min(100, prop.get('maximum', 100))
            elif name == 'include_asm':
                arguments[name] = False
            elif name == 'direction' and 'to' in prop.get('enum', []):
                arguments[name] = 'to'
            else:
                raise IDAError('Unrecognized required tool argument: ' + name)
        if 'max_instructions' in properties:
            arguments['max_instructions'] = 100
        return arguments

    def analyze_function(self, address):
        if isinstance(address, bool):
            raise ValueError('address must be an absolute IDA address, not an RVA.')
        if isinstance(address, int):
            address = hex(address)
        if not isinstance(address, str) or not re.fullmatch(r'0[xX][0-9a-fA-F]{1,16}', address) or int(address, 16) == 0:
            raise ValueError('address must be an absolute hexadecimal IDA address.')
        tools = self.list_tools()
        output = {'address': address, 'addressKind': 'IDA_EA', 'evidence': {}, 'errors': []}
        families = {'decompile': ('decompile',), 'disassembly': ('disasm', 'disassemble'),
                    'xrefs': ('xrefs_to', 'get_xrefs_to', 'xref_query'), 'callees': ('callees', 'get_callees')}
        for category, aliases in families.items():
            candidates = [tool for tool in tools if self._matches(tool.get('name', ''), aliases)]
            if not candidates:
                output['errors'].append({'kind': category, 'error': 'No matching tool is advertised.'})
                continue
            tool = candidates[0]
            try:
                args = self._arguments(tool, address)
                result = self.call_tool(tool['name'], args)
                output['evidence'][category] = {'tool': tool['name'], 'arguments': args, 'result': result}
                if isinstance(result, dict) and result.get('isError'):
                    output['errors'].append({'kind': category, 'tool': tool['name'], 'error': 'Tool returned isError; raw evidence is retained.'})
            except (IDAError, ValueError) as exc:
                output['errors'].append({'kind': category, 'tool': tool['name'], 'error': str(exc)})
        return output
