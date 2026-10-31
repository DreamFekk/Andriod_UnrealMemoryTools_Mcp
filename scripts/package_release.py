"""Create runnable release ZIPs or a source-only GitHub ZIP (--kind github)."""
from __future__ import annotations
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import re
import struct
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def release_files(kind):
    files = [ROOT / 'LICENSE', ROOT / 'THIRD_PARTY.md']
    previews = set(re.findall(r'!\[[^\]]*\]\((docs/screenshots/[^)\s]+)\)', (ROOT / 'README.md').read_text('utf-8')))
    dirs = ('desktop', 'bridge') if kind == 'user' else ('src', 'deps', 'KittyMemoryEx', 'desktop', 'bridge', 'tests', 'scripts', 'docs', 'release', '.github')
    if kind != 'user':
        files += [ROOT / name for name in ('CMakeLists.txt', 'build.ps1', 'README.md',
                  'CONTRIBUTING.md', '.gitignore', '.gitattributes', '.editorconfig')]
    for directory in dirs:
        for path in (ROOT / directory).rglob('*'):
            rel = path.relative_to(ROOT)
            if not path.is_file() or path.is_symlink() or any(p in ('.git', '.idea', '.vscode', '__pycache__', '.venv', '.pytest_cache') for p in rel.parts):
                continue
            if rel.parts[:2] == ('desktop', 'screenshots'):
                continue
            if kind == 'user' and 'screenshots' in rel.parts and rel.as_posix() not in previews:
                continue
            if rel.as_posix() in ('bridge/mcp_config.json', 'scripts/script.json') or path.suffix.lower() in ('.pyc', '.pyo', '.log', '.i64', '.idb', '.id0', '.id1', '.id2', '.nam', '.dmp', '.o', '.obj', '.exe', '.so', '.zip', '.partial', '.tmp', '.bak'):
                continue
            if path.name == '.env' or (path.name.startswith('.env.') and path.name != '.env.example'):
                continue
            if kind == 'user' and path.suffix.lower() not in ('.py', '.ps1', '.cmd', '.html', '.css', '.js'):
                continue
            files.append(path)
    if kind != 'github':
        binary = ROOT / 'outputs/arm64-v8a/UnrealMemoryTools'
        with binary.open('rb') as stream:
            header = stream.read(20)
        if len(header) != 20 or header[:6] != bytes.fromhex('7f454c460201') or struct.unpack_from('<H', header, 18)[0] != 183:
            raise ValueError('Expected ELF64 little-endian AArch64 binary; build first.')
        files.append(binary)
    return sorted(set(files))

def guide_html(markdown):
    lines, code, table = [], False, False
    for line in markdown.splitlines():
        if line.startswith(chr(96) * 3):
            lines.append('</code></pre>' if code else '<pre><code>')
            code = not code
            continue
        escaped = html.escape(line)
        if code:
            lines.append(escaped + chr(10))
            continue
        escaped = re.sub(r'(https?://[^\s<]+)', r'<a href="\1" target="_blank" rel="noopener">\1</a>', escaped)
        escaped = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', escaped)
        if line.startswith('|'):
            if re.fullmatch(r'[| :\-]+', line):
                continue
            if not table:
                lines.append('<div style="overflow:auto"><table style="border-collapse:collapse;width:100%;background:white"><tbody>')
                table = True
            lines.append('<tr>' + ''.join('<td style="border:1px solid #ccd2df;padding:10px">' + cell.strip() + '</td>' for cell in escaped.strip('|').split('|')) + '</tr>')
            continue
        if table:
            lines.append('</tbody></table></div>')
            table = False
        heading = re.match(r'^(#{1,3}) (.*)', escaped)
        if heading:
            level = len(heading[1])
            lines.append(f'<h{level}>{heading[2]}</h{level}>')
        elif line:
            lines.append('<p>' + escaped + '</p>')
    return '<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>先看这里 · UnrealMemoryTools</title><style>body{font:16px/1.85 system-ui,Microsoft YaHei,sans-serif;max-width:1050px;margin:30px auto;padding:24px;color:#172438;background:#f4f7fc}h1{font-size:30px}h2{border-left:5px solid #6555ce;padding:8px 15px;background:#e8e8fc;margin-top:40px}a{color:#245dc2;overflow-wrap:anywhere}pre{background:#182334;color:#ecf4ff;padding:20px;border-radius:10px;white-space:pre-wrap;overflow-wrap:anywhere}p{overflow-wrap:anywhere}.hero{padding:22px;background:white;border:2px solid #6555ce;border-radius:12px}</style><div class="hero"><b>第一次使用：全部解压 → 安装 Python / ADB → 01 检查环境 → 02 启动</b><br>只做 Dump 无需 IDA / MCP。高级分析再看第四、五节。<br><a href="http://127.0.0.1:18766/">打开电脑工作台（启动后）</a> · <a href="https://github.com/mrexodia/ida-pro-mcp">IDA Pro MCP 作者仓库</a></div>' + ''.join(lines) + '</html>'

def generated_files(version, variant, kind):
    guide = (ROOT / 'release/USER-GUIDE.md').read_text('utf-8').replace('{{VERSION}}', version).replace('{{VARIANT}}', '完整原生 + Web 版' if variant == 'native' else 'Headless / Web 版（无手机悬浮窗）')
    result = {'00-START-HERE.html': guide_html(guide).encode(), '00-先看这里-使用说明.txt': guide.encode('utf-8-sig'), 'USER-GUIDE.md': guide.encode(), 'runtime/run.ps1': (ROOT / 'release/runtime/run.ps1').read_bytes(), 'RELEASE-INFO.json': json.dumps({'version': version, 'kind': kind, 'variant': variant, 'nativeUi': variant == 'native', 'architecture': 'arm64-v8a', 'androidMinApi': 30}, indent=2).encode()}
    if kind == 'user':
        result['README.md'] = guide.encode()
    actions = {'01-CHECK-ENV': 'check', '02-START-DESKTOP': 'desktop', '03-START-PHONE': 'native', '04-GENERATE-MCP-CONFIG': 'config', '05-CHECK-IDA': 'ida'}
    for name, action in actions.items():
        cmd = '@echo off\r\nsetlocal\r\nchcp 65001 >nul\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0runtime\\run.ps1" -Action ' + action + ' %*\r\nexit /b %errorlevel%\r\n'
        result[name + '.cmd'] = cmd.encode()
    result['00-OPEN-GUIDE.cmd'] = b'@echo off\r\nstart "" "%~dp000-START-HERE.html"\r\n'
    result['tools/README.txt'] = b'Download Google Platform-Tools. Put the complete platform-tools folder here: tools/platform-tools/adb.exe. See 00-START-HERE.html.'
    for base in ('deps', 'src', 'KittyMemoryEx'):
        for path in (ROOT / base).rglob('*'):
            if path.is_file() and not path.is_symlink() and ('license' in path.name.lower() or 'copying' in path.name.lower() or path.name in ('FTL.TXT', 'GPLv2.TXT')):
                result['licenses/' + path.relative_to(ROOT).as_posix()] = path.read_bytes()
    for rel in ('src/GUI/ImGui/imgui.h', 'src/GUI/My_Utils/stb_image.cpp',
                'src/GUI/native_surface/ANativeWindowCreator.h', 'deps/fmt/format.h'):
        path = ROOT / rel
        if path.is_file():
            result['licenses/' + rel + '.txt'] = path.read_bytes()
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--release-dir', type=Path, default=ROOT.parent / 'releases')
    parser.add_argument('--kind', choices=('user', 'source', 'github'), default='user',
                        help='user/source include the built Android program; github contains only repository files')
    parser.add_argument('--variant', choices=('native', 'headless'), default='native')
    parser.add_argument('--version', default='1.0.1')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?', args.version):
        raise ValueError('Invalid release version')
    top = ('UnrealMemoryTools-' + args.version + '-github-source' if args.kind == 'github'
           else 'UnrealMemoryTools-' + args.version + '-windows-' + args.kind + ('-headless' if args.variant == 'headless' else ''))
    output = (args.output or args.release_dir / (top + '.zip')).expanduser().resolve()
    if output.suffix.lower() != '.zip':
        raise ValueError('Output must have a .zip extension')
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_suffix('.zip.partial')
    files = {p.relative_to(ROOT).as_posix(): p.read_bytes() for p in release_files(args.kind)}
    if args.kind != 'github':
        files.update(generated_files(args.version, args.variant, args.kind))
    manifest = {'version': args.version, 'kind': args.kind, 'variant': args.variant, 'createdUtc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'files': [{'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()} for name, data in sorted(files.items())]}
    try:
        with zipfile.ZipFile(partial, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for name, data in sorted(files.items()):
                archive.writestr(top + '/' + name, data)
            if args.kind != 'github':
                archive.writestr(top + '/RELEASE-MANIFEST.json', json.dumps(manifest, ensure_ascii=False, indent=2))
        with zipfile.ZipFile(partial) as archive:
            if archive.testzip():
                raise ValueError('ZIP integrity check failed')
        os.replace(partial, output)
    finally:
        partial.unlink(missing_ok=True)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix('.zip.sha256').write_text(digest + '  ' + output.name + chr(10), 'utf-8')
    print(json.dumps({'zip': str(output), 'bytes': output.stat().st_size, 'sha256': digest, 'files': len(files)}, ensure_ascii=False))

if __name__ == '__main__':
    if hasattr(__import__('sys').stdout, 'reconfigure'):
        __import__('sys').stdout.reconfigure(encoding='utf-8', errors='replace')
    main()
