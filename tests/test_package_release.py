"""Source archives must work in a fresh checkout without local binaries or data."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SPEC = importlib.util.spec_from_file_location(
    'package_release', Path(__file__).resolve().parents[1] / 'scripts/package_release.py')
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


class SourcePackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.root_patch = patch.object(package, 'ROOT', self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
        for name in ('LICENSE', 'THIRD_PARTY.md', 'CMakeLists.txt', 'build.ps1',
                     'README.md', 'CONTRIBUTING.md', '.gitignore', '.gitattributes', '.editorconfig'):
            self.put(name, b'fixture\n')

    def put(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_github_keeps_source_dependencies_and_excludes_local_data(self):
        keep = ['.github/workflows/ci.yml', 'src/App.cpp', 'tests/example.py',
                'src/GUI/ImGui/misc/git_freetype/arm64-v8a/libfreetype.a',
                'docs/screenshots/example.png', 'release/USER-GUIDE.md']
        drop = ['artifacts/session/Objects.txt', 'outputs/arm64-v8a/UnrealMemoryTools',
                'bridge/mcp_config.json', 'scripts/script.json', 'desktop/screenshots/fixture.png',
                'src/__pycache__/test.pyc', 'docs/notes.log', 'scripts/.env',
                'scripts/.env.local', 'src/old.cpp.bak', 'src/dump.so', 'src/cache.idb',
                'scripts/bundle.zip', 'build/CMakeCache.txt']
        for name in keep + drop:
            self.put(name, b'fixture')
        actual = {p.relative_to(self.root).as_posix() for p in package.release_files('github')}
        self.assertTrue(set(keep) <= actual)
        self.assertFalse(set(drop) & actual)
        self.assertIn('.gitignore', actual)

    def test_github_zip_needs_no_binary_and_contains_no_release_launchers(self):
        output = self.root / 'source.zip'
        with patch.object(sys, 'argv', ['package_release.py', '--kind', 'github', '--output', str(output)]):
            package.main()
        with zipfile.ZipFile(output) as archive:
            self.assertIsNone(archive.testzip())
            names = {name.split('/', 1)[1] for name in archive.namelist()}
            self.assertIn('README.md', names)
            self.assertIn('.gitattributes', names)
            self.assertNotIn('RELEASE-INFO.json', names)
            self.assertNotIn('02-START-DESKTOP.cmd', names)
        self.assertTrue(output.with_suffix('.zip.sha256').is_file())

    def test_runnable_release_still_requires_valid_arm64_binary(self):
        with self.assertRaises(FileNotFoundError):
            package.release_files('user')
        binary = self.put('outputs/arm64-v8a/UnrealMemoryTools', b'not an ELF')
        with self.assertRaises(ValueError):
            package.release_files('user')
        header = bytearray(20)
        header[:6] = bytes.fromhex('7f454c460201')
        struct.pack_into('<H', header, 18, 183)
        binary.write_bytes(header)
        self.assertIn(binary, package.release_files('user'))


if __name__ == '__main__':
    unittest.main()
