import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch
from bridge.artifacts import ArtifactStore, memory_image_to_elf


def elf_memory():
    data = bytearray(0x2100)
    data[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<HHI', data, 16, 3, 183, 1)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<Q', data, 40, 0x9000)
    struct.pack_into('<HHHHHH', data, 52, 64, 56, 2, 64, 9, 8)
    struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0, 0, 0x100, 0x100, 0x1000)
    struct.pack_into('<IIQQQQQQ', data, 120, 1, 6, 0x1000, 0x2000, 0x2000, 0x100, 0x300, 0x1000)
    data[0x2000:] = b'R' * 0x100
    return data


class ArtifactsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.store = ArtifactStore(self.root)
        self.source = self.root / 'source'
        self.source.mkdir()
        (self.source / 'script.json').write_text(json.dumps({'Functions': [
            {'Name': 'Player$$execMove', 'Address': 0x1234},
            {'Name': 'Shared$$execJump', 'Address': 32},
            {'Name': 'Shared$$execJump', 'Address': 64}, None]}), 'utf-8')
        (self.source / 'SDK_Classes.hpp').write_text('// Object: Class Test.Player\nclass UPlayer {\n // Object: Function Test.Player.Move\n // Native\n void Move(float Speed);\n};', 'utf-8')
        (self.source / 'libUE4.so').write_bytes(elf_memory())

    def tearDown(self):
        self.temp.cleanup()

    def test_import_hash_search_and_duplicate_exact(self):
        session = self.store.import_directory(self.source)
        self.assertEqual(len(self.store.list_sessions()), 1)
        manifest = {file['name']: file for file in session['files']}
        self.assertEqual(manifest['script.json']['sha256'], hashlib.sha256((self.source / 'script.json').read_bytes()).hexdigest())
        matches = self.store.search_functions('move', session=session['id'])
        self.assertEqual(matches[0]['rva'], '0x1234')
        self.assertIn('void Move(float Speed)', matches[0]['sdkSnippet'])
        self.assertFalse(self.store.get_function('Player$$execMove')['ambiguous'])
        repeated = self.store.get_function('Shared$$execJump')
        self.assertTrue(repeated['ambiguous'])
        self.assertEqual(len(repeated['candidates']), 2)
        self.assertFalse(self.store.get_function('absent')['found'])

    def test_symbols_only_and_fixed_allowlist(self):
        (self.source / 'secret.txt').write_text('private')
        session = self.store.import_directory(self.source, symbols_only=True)
        names = {file['name'] for file in session['files']}
        self.assertNotIn('libUE4.so', names)
        self.assertNotIn('secret.txt', names)
        with self.assertRaises(ValueError):
            self.store.search_functions('', session='../source')

    def test_signatures_are_matched_to_exact_owner_and_ambiguity(self):
        (self.source / 'SDK_Classes.hpp').write_text(
            '// Object: Class Test.Enemy\nstruct UEnemy {\n'
            '// Object: Function Test.Enemy.Move\nvoid Move(int Damage);\n};\n'
            '// Object: Class Test.Player\nstruct UPlayer {\n'
            '// Object: Function Test.Player.Move\nvoid Move(float Speed);\n};\n', 'utf-8')
        first = self.store.import_directory(self.source)
        detail = self.store.get_function('Player$$execMove', first['id'])
        self.assertEqual(detail['sdkSnippetMatch'], 'unique')
        self.assertIn('float Speed', detail['sdkSnippet'])
        self.assertNotIn('int Damage', detail['sdkSnippet'])
        with (self.source / 'SDK_Classes.hpp').open('a', encoding='utf-8') as out:
            out.write('// Object: Function OtherPackage.Player.Move\nvoid Move(double Value);\n')
        second = self.store.import_directory(self.source)
        detail = self.store.get_function('Player$$execMove', second['id'])
        self.assertEqual(detail['sdkSnippetMatch'], 'ambiguous')
        self.assertEqual(detail['sdkSnippet'], '')
        self.assertEqual(len(detail['sdkSnippetCandidates']), 2)

    def test_publication_does_not_rename_populated_directory(self):
        with patch.object(Path, 'rename', side_effect=PermissionError('directory indexing lock')) as rename:
            result = self.store.import_directory(self.source)
        rename.assert_not_called()
        self.assertTrue(Path(result['path']).is_dir())
        self.assertEqual(self.store._session(result['id']), Path(result['path']))

    def test_unpublished_session_is_invisible_and_published_is_immutable(self):
        name, pending = self.store._new()
        (pending / 'script.json').write_text('{"Functions": []}', 'utf-8')
        self.assertEqual(self.store.list_sessions(), [])
        with self.assertRaises(OSError):
            self.store._session(name)
        (pending / 'manifest.json.partial').write_text('{', 'utf-8')
        self.assertEqual(self.store.list_sessions(), [])
        self.store._finish(name, pending, [{'name': 'script.json', 'size': 17}], 'fixture')
        self.assertEqual(self.store._session(name), pending)
        before = (pending / 'manifest.json').read_bytes()
        with self.assertRaisesRegex(ValueError, 'immutable'):
            self.store._finish(name, pending, [], 'replacement')
        self.store._discard_pending(name, pending)
        self.assertEqual((pending / 'manifest.json').read_bytes(), before)

    def test_search_exact_rva_preserves_all_bits(self):
        path = self.source / 'script.json'
        path.write_text(json.dumps({'Functions': [{'Name': 'Large$$execAddress', 'Address': 0xf123456789abcdef}]}), 'utf-8')
        session = self.store.import_directory(self.source)
        rows = self.store.search_functions('0xF123456789ABCDEF', session=session['id'])
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['rva'], '0xf123456789abcdef')
        self.assertEqual(self.store.search_functions('0xf123456789abcdee', session=session['id']), [])

    def test_workspace_and_missing_script(self):
        with tempfile.TemporaryDirectory() as external:
            with self.assertRaises(ValueError):
                self.store.import_directory(external)
        (self.source / 'script.json').unlink()
        with self.assertRaises(ValueError):
            self.store.import_directory(self.source)
        self.assertEqual(list(self.store.sessions.iterdir()), [])

    def test_memory_layout_conversion_is_exact(self):
        target = self.root / 'analysis.so'
        mapping = memory_image_to_elf(self.source / 'libUE4.so', target)
        result = target.read_bytes()
        self.assertEqual(result[0x1000:0x1100], b'R' * 0x100)
        self.assertEqual(len(result), 0x1100)
        self.assertEqual(struct.unpack_from('<Q', result, 40)[0], 0)
        self.assertEqual(struct.unpack_from('<HHH', result, 58), (0, 0, 0))
        self.assertEqual(mapping['segments'][1]['memoryOffset'], 0x2000)
        self.assertTrue(mapping['runtimeRelocationsRemain'])

    def test_bad_conversion_does_not_replace_existing(self):
        source, destination = self.root / 'bad.so', self.root / 'analysis.so'
        source.write_bytes(bytes(64))
        destination.write_bytes(b'previous-analysis')
        with self.assertRaises(ValueError):
            memory_image_to_elf(source, destination)
        self.assertEqual(destination.read_bytes(), b'previous-analysis')

    def test_overlapping_original_offsets_preserve_distinct_runtime_segments(self):
        data = bytearray(0x3100)
        data[:0x100] = elf_memory()[:0x100]
        # Three LOAD records plus DYNAMIC, NOTE and PHDR descriptors.
        struct.pack_into('<H', data, 56, 6)
        struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0, 0, 0x200, 0x200, 0x1000)
        struct.pack_into('<IIQQQQQQ', data, 120, 1, 6, 0x1000, 0x2000, 0x2000, 0x100, 0x100, 0x1000)
        struct.pack_into('<IIQQQQQQ', data, 176, 1, 6, 0x1000, 0x3000, 0x3000, 0x100, 0x100, 0x1000)
        struct.pack_into('<IIQQQQQQ', data, 232, 2, 6, 0x1020, 0x3020, 0x3020, 0x20, 0x20, 8)
        struct.pack_into('<IIQQQQQQ', data, 288, 4, 4, 0x1040, 0x2040, 0x2040, 0x20, 0x20, 4)
        struct.pack_into('<IIQQQQQQ', data, 344, 6, 4, 64, 64, 64, 336, 336, 8)
        data[0x2000:0x2100], data[0x3000:0x3100] = b'A' * 0x100, b'B' * 0x100
        source, destination = self.root / 'aliased.so', self.root / 'analysis.so'
        source.write_bytes(data)
        mapping = memory_image_to_elf(source, destination)
        result = destination.read_bytes()
        self.assertEqual(result[0x2000:0x2100], b'A' * 0x100)
        self.assertEqual(result[0x3000:0x3100], b'B' * 0x100)
        self.assertEqual(mapping['outputLayout'], 'virtual-memory-offset')
        self.assertTrue(mapping['originalLoadFileRangesOverlap'])
        self.assertEqual([segment['originalFileOffset'] for segment in mapping['segments']], [0, 0x1000, 0x1000])
        self.assertEqual([segment['fileOffset'] for segment in mapping['segments']], [0, 0x2000, 0x3000])
        self.assertEqual(struct.unpack_from('<Q', result, 232 + 8)[0], 0x3020)
        self.assertEqual(struct.unpack_from('<Q', result, 288 + 8)[0], 0x2040)
        self.assertEqual(struct.unpack_from('<Q', result, 344 + 8)[0], 64)
        for segment in mapping['segments']:
            self.assertEqual(segment['fileOffset'] % segment['alignment'], segment['virtualAddress'] % segment['alignment'])

    def test_overlap_layout_rejects_unalignable_origin_without_overwrite(self):
        data = bytearray(0x3100)
        data[:0x100] = elf_memory()[:0x100]
        struct.pack_into('<H', data, 56, 3)
        struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0x1000, 0x1000, 0x100, 0x100, 0x1000)
        struct.pack_into('<IIQQQQQQ', data, 120, 1, 6, 0x1000, 0x3000, 0x3000, 0x100, 0x100, 0x2000)
        struct.pack_into('<IIQQQQQQ', data, 176, 1, 6, 0x1000, 0x4000, 0x4000, 0x100, 0x100, 0x1000)
        source, destination = self.root / 'unalignable.so', self.root / 'analysis.so'
        source.write_bytes(data)
        destination.write_bytes(b'preserve')
        with self.assertRaisesRegex(ValueError, 'alignment'):
            memory_image_to_elf(source, destination)
        self.assertEqual(destination.read_bytes(), b'preserve')

    def test_descriptor_tail_materializes_captured_bytes_and_rewrites_filesz(self):
        for descriptor_type in (2, 0x6474e552):
            with self.subTest(descriptor_type=descriptor_type):
                data = elf_memory() + b'T' * 0x100
                struct.pack_into('<H', data, 56, 3)
                struct.pack_into('<IIQQQQQQ', data, 176, descriptor_type, 4,
                                 0x1180, 0x2180, 0x2180, 0x40, 0x40, 8)
                source, destination = self.root / 'tail.so', self.root / 'analysis.so'
                source.write_bytes(data)
                mapping = memory_image_to_elf(source, destination)
                result = destination.read_bytes()
                self.assertEqual(result[0x1100:0x11c0], b'T' * 0xc0)
                self.assertEqual(result[0x1180:0x11c0], data[0x2180:0x21c0])
                self.assertEqual(struct.unpack_from('<Q', result, 120 + 32)[0], 0x1c0)
                self.assertEqual(struct.unpack_from('<Q', result, 120 + 40)[0], 0x300)
                self.assertEqual(struct.unpack_from('<Q', result, 176 + 8)[0], 0x1180)
                self.assertEqual(mapping['segments'][1]['originalFileSize'], 0x100)
                self.assertEqual(mapping['segments'][1]['materializedMemoryBytes'], 0xc0)
                self.assertEqual(mapping['materializedRuntimeTails'][0]['descriptorIndex'], 2)
                self.assertEqual(source.read_bytes(), data)

    def test_uncaptured_descriptor_tail_fails_without_replacing_output(self):
        data = elf_memory()
        struct.pack_into('<H', data, 56, 3)
        struct.pack_into('<IIQQQQQQ', data, 176, 2, 4, 0x1180, 0x2180, 0x2180, 0x40, 0x40, 8)
        source, destination = self.root / 'missing-tail.so', self.root / 'analysis.so'
        source.write_bytes(data)
        destination.write_bytes(b'preserve')
        with self.assertRaisesRegex(ValueError, 'uncaptured runtime tail'):
            memory_image_to_elf(source, destination)
        self.assertEqual(destination.read_bytes(), b'preserve')

    def test_tail_extension_rechecks_file_and_virtual_overlap(self):
        for conflicting_virtual in (False, True):
            with self.subTest(conflicting_virtual=conflicting_virtual):
                data = bytearray(0x3200)
                data[:0x100] = elf_memory()[:0x100]
                struct.pack_into('<H', data, 56, 4)
                struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0, 0, 0x300, 0x300, 0x1000)
                struct.pack_into('<IIQQQQQQ', data, 120, 1, 6, 0x1000, 0x2000, 0x2000, 0x80, 0x300, 0x1000)
                next_va = 0x2100 if conflicting_virtual else 0x3100
                struct.pack_into('<IIQQQQQQ', data, 176, 1, 6, 0x1100, next_va, next_va, 0x100, 0x100, 0x1000)
                struct.pack_into('<IIQQQQQQ', data, 232, 0x6474e552, 4, 0x1000, 0x2000, 0x2000, 0x200, 0x200, 1)
                data[0x2000:0x2200], data[0x3100:0x3200] = b'A' * 0x200, b'B' * 0x100
                source, destination = self.root / 'extension.so', self.root / 'analysis.so'
                source.write_bytes(data)
                destination.write_bytes(b'preserve')
                if conflicting_virtual:
                    with self.assertRaisesRegex(ValueError, 'Overlapping runtime PT_LOAD'):
                        memory_image_to_elf(source, destination)
                    self.assertEqual(destination.read_bytes(), b'preserve')
                else:
                    mapping = memory_image_to_elf(source, destination)
                    result = destination.read_bytes()
                    self.assertFalse(mapping['originalLoadFileRangesOverlap'])
                    self.assertEqual(mapping['outputLayout'], 'virtual-memory-offset')
                    self.assertEqual(result[0x2000:0x2200], b'A' * 0x200)
                    self.assertEqual(result[0x3100:0x3200], b'B' * 0x100)
                    self.assertEqual(struct.unpack_from('<Q', result, 232 + 8)[0], 0x2000)

    def test_prepare_uses_unique_analysis_copy(self):
        imported = self.store.import_directory(self.source)
        first = self.store.prepare_ida(imported['id'])
        second = self.store.prepare_ida(imported['id'])
        self.assertNotEqual(first['input'], second['input'])
        self.assertIn('get_imagebase()', Path(first['importScript']).read_text('utf-8'))
        self.assertEqual(first['openCommand'][3], first['openScript'])
        self.assertEqual((self.source / 'libUE4.so').read_bytes(), elf_memory())

    def test_sync_filters_urls_and_checks_session(self):
        script = (self.source / 'script.json').read_bytes()
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_GET(self):
                if self.path == '/api/state':
                    body = json.dumps({'busy': False, 'pid': 1, 'package': 'test', 'generation': 2, 'resultPath': '/dump'}).encode()
                elif self.path == '/api/files':
                    body = json.dumps([{'name': 'script.json', 'size': len(script), 'url': 'https://untrusted.invalid/file'},
                                       {'name': '../../secret', 'size': 9}]).encode()
                elif self.path == '/api/file?name=script.json':
                    body = script
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            synced = self.store.sync_from_device('http://127.0.0.1:' + str(server.server_port))
            self.assertEqual([f['name'] for f in synced['files']], ['script.json'])
            with self.assertRaises(ValueError):
                self.store.sync_from_device('https://external.invalid')
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_sync_rejects_completed_operation_or_changed_listing(self):
        script = (self.source / 'script.json').read_bytes()
        for changed in ('metrics', 'listing'):
            counters = {'state': 0, 'files': 0}
            class Handler(BaseHTTPRequestHandler):
                def log_message(self, *args):
                    pass
                def do_GET(self):
                    if self.path == '/api/state':
                        counters['state'] += 1
                        body = json.dumps({'busy': False, 'pid': 1, 'package': 'test', 'generation': 2,
                            'resultPath': '/dump', 'metrics': {'dumpMs': 2 if changed == 'metrics' and counters['state'] > 1 else 1}}).encode()
                    elif self.path == '/api/files':
                        counters['files'] += 1
                        files = [{'name': 'script.json', 'size': len(script)}]
                        if changed == 'listing' and counters['files'] > 1:
                            files.append({'name': 'Objects.txt', 'size': 1})
                        body = json.dumps(files).encode()
                    elif self.path == '/api/file?name=script.json':
                        body = script
                    else:
                        self.send_error(404)
                        return
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
            server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                with self.assertRaisesRegex(ValueError, 'session changed'):
                    self.store.sync_from_device('http://127.0.0.1:' + str(server.server_port))
                self.assertEqual(list(self.store.sessions.iterdir()), [])
            finally:
                server.shutdown()
                server.server_close()
                thread.join()


if __name__ == '__main__':
    unittest.main()
