"""Read-only checks against a probed Android SDK explorer service."""
import argparse
import json
import re
import unittest
import urllib.error
import urllib.request

BASE = 'http://127.0.0.1:18765'


def api(path, data=None):
    request = urllib.request.Request(BASE + path,
        data=None if data is None else json.dumps(data).encode(),
        headers={'Content-Type': 'application/json', 'X-UMT-Client': '1'})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=20) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


class ExplorerIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        _, state = api('/api/state')
        if state.get('busy') or not state.get('probed'):
            raise unittest.SkipTest('Probe a target and wait for idle before testing the explorer.')
        code, cls.page = api('/api/explorer', {'action': 'list', 'cursor': 0, 'limit': 100})
        if code != 200:
            raise AssertionError(cls.page)

    def test_address_strings_and_list_schema(self):
        page = self.page
        self.assertGreater(page['total'], 0)
        self.assertGreater(len(page['items']), 0)
        self.assertLessEqual(len(page['items']), 100)
        for item in page['items']:
            self.assertRegex(item['address'], r'^0x[0-9a-fA-F]+$')
            self.assertIsInstance(item['index'], int)
            for key in ('name', 'fullName', 'className'):
                self.assertIsInstance(item[key], str)

    def test_page_cursor_has_no_duplicates(self):
        code, page = api('/api/explorer', {'action': 'list', 'cursor': self.page['nextCursor'], 'limit': 100})
        self.assertEqual(code, 200, page)
        first = {item['index'] for item in self.page['items']}
        self.assertFalse(first.intersection(item['index'] for item in page['items']))
        self.assertEqual(self.page['generation'], page['generation'])

    def test_bounded_search(self):
        code, page = api('/api/explorer', {'action': 'list', 'query': 'Object', 'cursor': 0,
                                          'limit': 25, 'scanLimit': 4096})
        self.assertEqual(code, 200, page)
        self.assertLessEqual(page['scanned'], 4096)
        self.assertLessEqual(len(page['items']), 25)
        for item in page['items']:
            self.assertIn('object', (item['name'] + ' ' + item['fullName']).lower())

    def test_limits_are_bounded(self):
        code, page = api('/api/explorer', {'action': 'list', 'cursor': 0, 'limit': 1000000, 'scanLimit': 100000000})
        self.assertEqual(code, 200, page)
        self.assertLessEqual(len(page['items']), 1000)
        self.assertLessEqual(page['scanned'], 4096)

    def test_inspect_has_all_functional_sections(self):
        item = next((entry for entry in self.page['items'] if entry['name'] == 'Object'), self.page['items'][0])
        code, obj = api('/api/explorer', {'action': 'inspect', 'address': item['address'], 'maxRows': 64})
        self.assertEqual(code, 200, obj)
        self.assertEqual(int(obj['address'], 16), int(item['address'], 16))
        self.assertEqual(obj['generation'], self.page['generation'])
        self.assertTrue(obj['metadata'])
        for key in ('metadata', 'properties', 'functions', 'containers'):
            self.assertIsInstance(obj[key], list)
        self.assertIsInstance(obj['runtime'], dict)
        for field in obj['properties']:
            self.assertRegex(field['offset'], r'^0x[0-9a-fA-F]+$')
            self.assertIsInstance(field['value'], str)

    def test_invalid_addresses_and_actions_do_not_crash(self):
        for request in ({'action': 'inspect', 'address': 'not-hex'},
                        {'action': 'inspect', 'address': '0x0'},
                        {'action': 'inspect', 'address': '0xFFFFFFFFFFFFFFFF'},
                        {'action': 'erase'}, {'action': 'list', 'cursor': 'bad'}):
            code, result = api('/api/explorer', request)
            self.assertGreaterEqual(code, 400, result)
            self.assertTrue(result.get('error'))
        code, _ = api('/api/health')
        self.assertEqual(code, 200)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', default=BASE)
    args, remaining = parser.parse_known_args()
    BASE = args.base.rstrip('/')
    unittest.main(argv=['explorer_integration.py', '-v', *remaining])
