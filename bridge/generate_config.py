"""Generate a portable-to-this-machine MCP configuration without editing any app settings."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
from urllib.parse import urlsplit


def loopback_origin(value: str) -> str:
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError as error:
        raise argparse.ArgumentTypeError('Use a valid HTTP loopback origin and port.') from error
    if (parsed.scheme != 'http' or parsed.hostname not in ('localhost', '127.0.0.1', '::1')
            or parsed.username is not None or parsed.password is not None
            or parsed.path not in ('', '/') or parsed.query or parsed.fragment
            or port == 0 or any(character.isspace() for character in value)):
        raise argparse.ArgumentTypeError('Use an HTTP loopback origin, e.g. http://127.0.0.1:18766.')
    host = '[::1]' if parsed.hostname == '::1' else parsed.hostname
    return f'http://{host}' + (f':{port}' if port is not None else '')


def configuration(url: str = 'http://127.0.0.1:18766') -> dict:
    script = Path(__file__).resolve().with_name('mcp_server.py')
    if not script.is_file():
        raise FileNotFoundError(f'MCP server is missing: {script}')
    return {'mcpServers': {'unreal-memory-tools': {
        'command': str(Path(sys.executable).resolve()),
        'args': [str(script), '--url', loopback_origin(url)],
    }}}


def write_configuration(path: Path, config: dict) -> None:
    path = path.expanduser().resolve()
    if path == Path(__file__).resolve() or path == Path(__file__).resolve().with_name('mcp_server.py'):
        raise ValueError('The output must not overwrite a Python source file.')
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                                         dir=path.parent, prefix=path.name + '.', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(config, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', type=loopback_origin, default='http://127.0.0.1:18766', help='Local workbench origin')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().with_name('mcp_config.json'), help='Generated JSON file, not the application settings file')
    parser.add_argument('--print', dest='print_only', action='store_true', help='Print JSON without writing files')
    args = parser.parse_args(argv)
    try:
        config = configuration(args.url)
        if args.print_only:
            print(json.dumps(config, ensure_ascii=False, indent=2))
        else:
            write_configuration(args.output, config)
            print(f'Generated: {args.output.expanduser().resolve()}')
            print('Import or merge this JSON in your MCP client. No application settings were modified.')
        return 0
    except (OSError, ValueError) as error:
        print(f'Configuration generation failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    raise SystemExit(main())
