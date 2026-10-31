"""Start a local IDALib supervisor without importing IDA into the workbench."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
import uuid

if __package__ in (None, ''):
    from ida_client import IDAClient
else:
    from .ida_client import IDAClient


class IDAServiceError(RuntimeError):
    pass


def normalize_url(url: str) -> str:
    """Accept an explicit local supervisor endpoint, never command fragments."""
    try:
        if not isinstance(url, str) or any(c.isspace() for c in url):
            raise ValueError()
        parsed = urllib.parse.urlsplit(url)
        port = parsed.port
        if (parsed.scheme != 'http' or parsed.hostname not in ('127.0.0.1', 'localhost', '::1')
                or parsed.username is not None or parsed.password is not None
                or parsed.path != '/mcp' or parsed.query or parsed.fragment
                or '?' in url or '#' in url or port is None or not 1024 <= port <= 65535):
            raise ValueError()
    except (ValueError, TypeError) as error:
        raise ValueError('Use http://127.0.0.1:13338/mcp (HTTP loopback, explicit port 1024–65535, /mcp, no credentials or query).') from error
    host = '[::1]' if parsed.hostname == '::1' else parsed.hostname
    return f'http://{host}:{port}/mcp'


def _valid_ida_directory(value) -> Path | None:
    if not value:
        return None
    path = Path(value).expanduser()
    if path.is_file():
        path = path.parent
    names = ('idalib.dll',) if os.name == 'nt' else ('libidalib.so', 'libidalib.dylib')
    if any((path / name).is_file() for name in names):
        return path.resolve()
    return None


def _windows_registry_directories():
    if os.name != 'nt':
        return []
    import winreg
    result = []
    for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
            try:
                with winreg.OpenKey(hive, r'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 0,
                                    winreg.KEY_READ | view) as parent:
                    for index in range(min(winreg.QueryInfoKey(parent)[0], 4096)):
                        try:
                            with winreg.OpenKey(parent, winreg.EnumKey(parent, index)) as key:
                                name = str(winreg.QueryValueEx(key, 'DisplayName')[0]).lower()
                                if 'ida' not in name or not ('hex-rays' in name or 'ida pro' in name):
                                    continue
                                result.append(winreg.QueryValueEx(key, 'InstallLocation')[0])
                        except OSError:
                            continue
            except OSError:
                continue
    return result


def _portable_roots():
    if os.name != 'nt':
        return []
    roots = []
    for variable in ('ProgramFiles', 'ProgramFiles(x86)', 'LOCALAPPDATA'):
        if os.environ.get(variable):
            base = Path(os.environ[variable])
            roots.append(base / 'Programs' if variable == 'LOCALAPPDATA' else base)
    # Only inspect fixed conventional directories, then one child level.
    # This deliberately does not recurse over drives or user documents.
    for letter in 'CDEFGHIJKLMNOPQRSTUVWXYZ':
        for folder in ('IDA', 'APP/IDA', 'Apps/IDA', 'Tools/IDA'):
            candidate = Path(f'{letter}:/') / folder
            if candidate.is_dir():
                roots.append(candidate)
    return roots


def discover_ida_directory() -> Path | None:
    explicit = os.environ.get('IDADIR')
    if explicit:
        directory = _valid_ida_directory(explicit)
        if directory is None:
            raise IDAServiceError('IDADIR does not contain the IDALib library. Set IDADIR to the IDA 9+ installation directory containing idalib.dll, then restart the workbench.')
        return directory
    config_root = os.environ.get('IDAUSR')
    if not config_root:
        appdata = os.environ.get('APPDATA')
        config_root = str(Path(appdata) / 'Hex-Rays/IDA Pro') if os.name == 'nt' and appdata else str(Path.home() / '.idapro')
    try:
        config = json.loads((Path(config_root) / 'ida-config.json').read_text('utf-8-sig'))
        directory = _valid_ida_directory(config.get('Paths', {}).get('ida-install-dir'))
        if directory:
            return directory
    except (OSError, ValueError, AttributeError, TypeError):
        pass
    candidates = set()
    for binary in ('ida.exe', 'ida64.exe', 'idat.exe', 'ida', 'idat'):
        found = shutil.which(binary)
        if found:
            directory = _valid_ida_directory(Path(found).parent)
            if directory:
                candidates.add(directory)
    for value in _windows_registry_directories():
        directory = _valid_ida_directory(value)
        if directory:
            candidates.add(directory)
    for root in _portable_roots():
        directory = _valid_ida_directory(root)
        if directory:
            candidates.add(directory)
        try:
            children = sorted(root.iterdir(), key=lambda p: p.name.lower())[:256]
        except OSError:
            continue
        for child in children:
            if root.name.lower() not in ('ida',) and 'ida' not in child.name.lower():
                continue
            directory = _valid_ida_directory(child)
            if directory:
                candidates.add(directory)
    if len(candidates) > 1:
        raise IDAServiceError('Multiple IDA installations found: ' + ', '.join(map(str, sorted(candidates)))
                              + '. Set IDADIR to the intended installation and restart the workbench.')
    return next(iter(candidates), None)


_START_LOCK = threading.Lock()


class LocalIDAService:
    START_TIMEOUT = 15.0
    PREFLIGHT_TIMEOUT = 10.0
    POLL_INTERVAL = 0.2
    PROBE_TIMEOUT = 0.6
    normalize_url = staticmethod(normalize_url)

    def __init__(self, workspace, ida_dir=None):
        self.root = Path(workspace).expanduser().resolve()
        self.ida_dir = ida_dir
        self._children = {}

    @staticmethod
    def _ready(status):
        return (status.get('connected') is True and status.get('canOpenDatabase') is True
                and status.get('canSelectDatabase') is True and isinstance(status.get('sessions'), list))

    def _status(self, url, timeout=None):
        try:
            return IDAClient(url, timeout=timeout or self.PROBE_TIMEOUT).status()
        except Exception as error:
            return {'connected': False, 'error': str(error)}

    @staticmethod
    def _available(host, port):
        # Check the literal address used by the child, without DNS resolution.
        family = socket.AF_INET6 if host == '::1' else socket.AF_INET
        with socket.socket(family, socket.SOCK_STREAM) as listener:
            if os.name == 'nt' and hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            try:
                listener.bind((host, port))
                return True
            except OSError:
                return False

    @staticmethod
    def _dependencies():
        for module in ('ida_pro_mcp.idalib_supervisor', 'idapro'):
            try:
                found = importlib.util.find_spec(module)
            except (ModuleNotFoundError, ValueError, ImportError):
                found = None
            if found is None:
                raise IDAServiceError(f'{module} is missing from {sys.executable}. Install ida-pro-mcp and the IDALib Python package supplied with IDA 9+ into this Python, then restart the workbench. Nothing was installed automatically.')

    @staticmethod
    def _hidden_options():
        return {'creationflags': getattr(subprocess, 'CREATE_NO_WINDOW', 0)}

    @staticmethod
    def _tail(path):
        try:
            with path.open('rb') as stream:
                stream.seek(0, 2)
                stream.seek(max(0, stream.tell() - 6000))
                return stream.read().decode('utf-8', 'replace').strip()
        except OSError:
            return ''

    def _preflight(self, environment, log):
        # Import only in an isolated child: IDALib initializes a native kernel.
        try:
            result = subprocess.run([sys.executable, '-c', 'import idapro; print("UMT_IDALIB_READY", flush=True)'],
                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, shell=False,
                cwd=str(self.root), env=environment, timeout=self.PREFLIGHT_TIMEOUT, **self._hidden_options())
        except subprocess.TimeoutExpired as error:
            raise IDAServiceError('IDALib initialization timed out. Check the existing IDA installation and license; close any license dialog and retry.') from error
        except OSError as error:
            raise IDAServiceError(f'Cannot start IDALib preflight using {sys.executable}: {error}') from error
        if result.returncode:
            raise IDAServiceError('IDALib initialization failed. Verify the IDA 9+ installation, its Python package and existing license. See the startup log for the native loader error.')

    @staticmethod
    def _stop_child(child):
        # Ownership comes exclusively from our Popen, never persisted PID data.
        if child.poll() is not None:
            return
        child.terminate()
        try:
            child.wait(timeout=2)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=2)

    def _save(self, path, metadata):
        temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
        try:
            temporary.write_text(json.dumps(metadata, ensure_ascii=False, indent=2), 'utf-8')
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)

    def ensure(self, url):
        url = normalize_url(url)
        with _START_LOCK:
            status = self._status(url)
            if self._ready(status):
                result = {'url': url, 'started': False, 'reused': True,
                          'message': 'Existing IDALib supervisor is ready; current database sessions were preserved.'}
                known = self._children.get(url)
                if known and known[0].poll() is None:
                    result.update(pid=known[0].pid, logPath=str(known[1]))
                return result
            if status.get('connected'):
                raise IDAServiceError('This endpoint is already serving MCP but lacks a working idb_open/idb_list supervisor session list. Select another local port, such as 13338; the existing service was left running.')
            parsed = urllib.parse.urlsplit(url)
            host = '127.0.0.1' if parsed.hostname == 'localhost' else parsed.hostname
            if not self._available(host, parsed.port):
                raise IDAServiceError(f'Port {parsed.port} is already occupied or unavailable. Choose another local supervisor port; no existing process was stopped. ' + status.get('error', ''))
            self._dependencies()
            directory = (discover_ida_directory() if os.environ.get('IDADIR') or self.ida_dir is None
                         else _valid_ida_directory(self.ida_dir))
            if directory is None:
                raise IDAServiceError('IDA 9+ with IDALib was not found. Set IDADIR to the installation directory containing idalib.dll (or libidalib.so/libidalib.dylib), then restart the workbench.')
            environment = os.environ.copy()
            # An explicitly configured IDADIR wins; discovery never edits host env/config.
            if not environment.get('IDADIR'):
                environment['IDADIR'] = str(directory)
            self.root.mkdir(parents=True, exist_ok=True)
            log_path = self.root / f'ida-supervisor-{parsed.port}-{uuid.uuid4().hex[:12]}.log'
            metadata_path = self.root / f'ida-supervisor-{parsed.port}.json'
            child = None
            metadata = {'url': url, 'python': sys.executable, 'idaDirectory': str(directory),
                        'logPath': str(log_path), 'state': 'starting', 'createdAt': time.time()}
            try:
                with log_path.open('ab', buffering=0) as log:
                    self._preflight(environment, log)
                    command = [sys.executable, '-m', 'ida_pro_mcp.idalib_supervisor', '--host', host, '--port', str(parsed.port)]
                    child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                        shell=False, cwd=str(self.root), env=environment, start_new_session=os.name != 'nt',
                        **self._hidden_options())
                    metadata['pid'] = child.pid
                    self._save(metadata_path, metadata)
                    deadline = time.monotonic() + self.START_TIMEOUT
                    while time.monotonic() < deadline:
                        if child.poll() is not None:
                            raise IDAServiceError(f'IDALib supervisor exited during startup (exit {child.returncode}).')
                        remaining = deadline - time.monotonic()
                        status = self._status(url, min(self.PROBE_TIMEOUT, max(0.05, remaining / 5)))
                        if self._ready(status) and child.poll() is None:
                            metadata['state'] = 'ready'
                            self._save(metadata_path, metadata)
                            self._children[url] = (child, log_path)
                            return {'url': url, 'started': True, 'reused': False, 'pid': child.pid,
                                    'logPath': str(log_path), 'message': 'IDALib supervisor started and its database session API is ready.'}
                        time.sleep(min(self.POLL_INTERVAL, max(0, deadline - time.monotonic())))
                    raise IDAServiceError('IDALib supervisor did not become ready within 15 seconds. ' + status.get('error', 'Check idb_open/idb_list support in the installed ida-pro-mcp version.'))
            except Exception as error:
                cleanup_error = ''
                if child is not None:
                    try:
                        self._stop_child(child)
                    except (OSError, subprocess.TimeoutExpired) as cleanup:
                        cleanup_error = f'\nThe startup child could not be stopped: {cleanup}'
                metadata.update(state='failed', error=str(error))
                try:
                    self._save(metadata_path, metadata)
                except OSError:
                    pass
                detail = self._tail(log_path)
                raise IDAServiceError(f'{error}{cleanup_error}\nLog: {log_path}' + (f'\n{detail}' if detail else '')) from error
