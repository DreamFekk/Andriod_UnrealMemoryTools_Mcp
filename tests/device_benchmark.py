"""Repeat real probe/dump operations through the running device service."""
import argparse
import json
from pathlib import Path
import statistics
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', default='http://127.0.0.1:18765')
    parser.add_argument('--package', required=True, help='Exact package name of the running target')
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--out', type=Path, default=Path('artifacts/device-benchmark.json'))
    args = parser.parse_args()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def api(path, data=None):
        req = urllib.request.Request(args.base + path,
            data=None if data is None else json.dumps(data).encode(),
            headers={'Content-Type': 'application/json', 'X-UMT-Client': '1'})
        with opener.open(req, timeout=30) as response:
            return json.load(response)

    matches = [p for p in api('/api/processes') if p['package'] == args.package]
    if len(matches) != 1:
        raise RuntimeError(f'Expected one target, found {len(matches)}')
    target = matches[0]
    report = {'health': api('/api/health'), 'target': target, 'runs': []}
    for number in range(args.repeat):
        run = {'iteration': number + 1}
        for action in ('probe', 'dump', 'library'):
            accepted = api('/api/action', {'action': action, 'pid': target['pid']})
            if not accepted.get('accepted'):
                raise RuntimeError(accepted)
            deadline = time.monotonic() + 300
            while True:
                state = api('/api/state')
                if not state['busy']:
                    break
                if time.monotonic() > deadline:
                    raise TimeoutError(f'{action}: device operation exceeded 300 s')
                time.sleep(.2)
            if state['error']:
                raise RuntimeError(state['error'])
            run[action] = {k: v for k, v in state['metrics'].items() if k.startswith(action)}
            print(f'{number+1}: {action} {run[action][action+"Ms"]:.2f} ms', flush=True)
        run['files'] = api('/api/files')
        report['runs'].append(run)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    report['medianMs'] = {action: statistics.median(r[action][action+'Ms'] for r in report['runs'])
                          for action in ('probe', 'dump', 'library')}
    args.out.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    print(json.dumps(report['medianMs']))


if __name__ == '__main__':
    main()
