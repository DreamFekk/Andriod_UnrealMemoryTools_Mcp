"""Read-only release readiness checks. No installer, device restart or IDA mutation."""
from __future__ import annotations
import argparse
import importlib.metadata
import importlib.util
import json
from pathlib import Path
import struct
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from desktop.launcher import find_adb, command, parse_devices, LaunchError
from bridge.ida_client import IDAClient


def check(ida_required=False, serial=None):
    rows = []
    def add(name, status, detail):
        rows.append({'name': name, 'status': status, 'detail': detail})
    add('Python', 'PASS' if sys.version_info >= (3, 10) else 'FAIL', sys.version.split()[0] + ' | ' + sys.executable)
    binary = ROOT / 'outputs/arm64-v8a/UnrealMemoryTools'
    try:
        with binary.open('rb') as stream:
            header = stream.read(20)
        valid = len(header) == 20 and header[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', header, 18)[0] == 183
        add('Android ARM64 program', 'PASS' if valid else 'FAIL', str(binary))
    except OSError as error:
        add('Android ARM64 program', 'FAIL', str(error))
    try:
        adb = find_adb()
        add('ADB', 'PASS', adb)
        devices = parse_devices(command([adb, 'devices', '-l'], timeout=8))
        ready = [name for name, state in devices if state == 'device']
        add('USB debugging', 'PASS' if ready else 'WARN', str(devices) if devices else 'No device. Connect USB, enable debugging, accept the phone RSA prompt.')
        if serial and serial not in ready:
            add('Selected device', 'FAIL', 'Specified serial is not ready: ' + serial)
        elif serial or len(ready) == 1:
            selected = serial or ready[0]
            abi = command([adb, '-s', selected, 'shell', 'getprop', 'ro.product.cpu.abi'], timeout=5)
            sdk = command([adb, '-s', selected, 'shell', 'getprop', 'ro.build.version.sdk'], timeout=5)
            add('Android version / ABI', 'PASS' if abi == 'arm64-v8a' and sdk.isdigit() and int(sdk) >= 30 else 'FAIL', 'API ' + sdk + ' / ' + abi + '; needs API 30+ and arm64-v8a')
        elif len(ready) > 1:
            add('Device selection', 'WARN', 'Multiple devices: run desktop/launcher.py --serial SERIAL')
        add('Root / KernelSU', 'INFO', 'Not requested by this checker. The launcher verifies su; allow shell in KernelSU when starting.')
    except (OSError, LaunchError) as error:
        add('ADB / device', 'FAIL', str(error))
    try:
        version = importlib.metadata.version('ida-pro-mcp')
        add('Optional ida-pro-mcp package', 'PASS', version)
    except importlib.metadata.PackageNotFoundError:
        add('Optional ida-pro-mcp package', 'WARN', 'Not installed in this Python. Needed only for IDA analysis, not Dump.')
    add('Optional idapro / IDALib', 'PASS' if importlib.util.find_spec('idapro') else 'WARN', 'Install the idapro wheel supplied with IDA Pro and activate its installation path; see 00-START-HERE.html.')
    settings=ROOT / 'artifacts/workbench-settings.json'
    try:
        ida_url=json.loads(settings.read_text('utf-8')).get('idaUrl','http://127.0.0.1:13338/mcp') if settings.is_file() else 'http://127.0.0.1:13338/mcp'
    except (OSError,ValueError):
        ida_url='http://127.0.0.1:13338/mcp'
    try:
        names = {entry['name'] for entry in IDAClient(ida_url,timeout=3).list_tools()}
        required = {'idb_open', 'idb_list', 'decompile', 'disasm'}
        missing = sorted(required - names)
        add('IDA MCP '+ida_url, 'FAIL' if missing and ida_required else 'WARN' if missing else 'PASS', 'Missing tools: ' + ', '.join(missing) if missing else str(len(names)) + ' tools; database open + analysis API available')
    except Exception as error:
        add('IDA MCP '+ida_url, 'FAIL' if ida_required else 'WARN', str(error) + ' | Optional for Dump; required for IDA analysis.')
    try:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open('http://127.0.0.1:18766/api/workbench/status', timeout=3) as response:
            status = json.load(response)
        add('Desktop workbench :18766', 'PASS' if status.get('service') == 'umt-workbench' else 'WARN', 'Start with 02-START-DESKTOP.cmd. Device port 18765 is not the desktop workbench.')
    except Exception:
        add('Desktop workbench :18766', 'INFO', 'Not started. Double-click 02-START-DESKTOP.cmd after checking dependencies.')
    return {'ok': not any(row['status'] == 'FAIL' for row in rows), 'checks': rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ida', action='store_true', help='Treat missing IDA service as a required failure')
    parser.add_argument('--serial')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    result = check(args.ida, args.serial)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print('UMT RELEASE CHECK | PASS=ready, WARN=optional/missing device, FAIL=fix before continuing\n')
        for row in result['checks']:
            print('[{}] {}\n       {}'.format(row['status'], row['name'], row['detail']))
        print('\nOpen 00-START-HERE.html for Chinese step-by-step instructions and official download links.')
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    raise SystemExit(main())
