"""Immutable dump sessions, streaming device sync, reflection symbol lookup and IDA input preparation."""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import time
import urllib.parse
import urllib.request
import uuid

SYMBOL_FILES = ('Logs.txt', 'Objects.txt', 'Offsets.hpp', 'AIOHeader.hpp', 'SDK_Enums.hpp',
                'SDK_Structs.hpp', 'SDK_Classes.hpp', 'SDK_Offset.hpp', 'script.json')
LIBRARY_FILES = ('libUE4.so', 'libUnreal.so')
ALLOWED_FILES = frozenset(SYMBOL_FILES + LIBRARY_FILES)
SESSION_ID_PATTERN = r'[0-9]{8}-[0-9]{6}-[0-9a-f]{10}'


def _json(path: Path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def _safe_file(path: Path, boundary: Path) -> Path:
    if path.is_symlink():
        raise ValueError('Symbolic-link artifacts are not accepted.')
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(boundary) or not resolved.is_file():
        raise ValueError('Artifact must be a regular file inside the configured workspace.')
    return resolved


def _atomic_json(path: Path, value):
    partial = path.with_name(path.name + '.partial')
    try:
        with partial.open('w', encoding='utf-8', newline='\n') as output:
            json.dump(value, output, ensure_ascii=False, indent=2)
            output.flush()
            os.fsync(output.fileno())
        os.replace(partial, path)
    finally:
        partial.unlink(missing_ok=True)


def _stream(source, target: Path, maximum=8 * 1024**3):
    partial = target.with_name(target.name + '.partial')
    digest = hashlib.sha256()
    total = 0
    try:
        with partial.open('wb') as output:
            while chunk := source.read(1024 * 1024):
                total += len(chunk)
                if total > maximum:
                    raise ValueError('Artifact exceeds the configured 8 GiB size limit.')
                digest.update(chunk)
                output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        os.replace(partial, target)
    finally:
        partial.unlink(missing_ok=True)
    return {'name': target.name, 'size': total, 'sha256': digest.hexdigest()}


class ArtifactStore:
    """root is an explicit workspace boundary; immutable sessions live in root/sessions.

    Local imports must be children of root. Copy a source into the configured workspace
    before importing it; an MCP request cannot read arbitrary host directories.
    """
    def __init__(self, root: Path):
        self.root = Path(root).expanduser().resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.sessions = self.root / 'sessions'
        if self.sessions.is_symlink():
            raise ValueError('Sessions directory must not be a symbolic link.')
        self.sessions.mkdir(exist_ok=True)
        self._cache = {}
        self._snippet_cache = {}

    def _new(self):
        if self.sessions.resolve(strict=True) != self.sessions:
            raise ValueError('Sessions directory must not redirect outside its original path.')
        # Never rename a populated directory: Windows indexers can keep child
        # handles open without FILE_SHARE_DELETE. Manifest presence is the commit
        # marker, so final-named folders remain invisible while being populated.
        for _ in range(10):
            name = time.strftime('%Y%m%d-%H%M%S', time.gmtime()) + '-' + uuid.uuid4().hex[:10]
            staging = self.sessions / name
            try:
                staging.mkdir()
            except FileExistsError:
                continue
            return name, staging
        raise FileExistsError('Could not allocate a unique artifact session directory.')

    def _pending_directory(self, name, staging):
        if not isinstance(name, str) or not re.fullmatch(SESSION_ID_PATTERN, name):
            raise ValueError('Invalid pending session identifier.')
        expected = self.sessions / name
        staging = Path(staging)
        if (staging != expected or staging.is_symlink() or not staging.is_dir()
                or self.sessions.resolve(strict=True) != self.sessions
                or staging.resolve(strict=True) != expected):
            raise ValueError('Pending session must remain a direct directory inside the artifact store.')
        manifest = staging / 'manifest.json'
        if manifest.exists() or manifest.is_symlink():
            raise ValueError('Published artifact sessions are immutable.')
        return staging

    def _discard_pending(self, name, staging):
        # Only this operation's exact, still-uncommitted directory may be
        # removed. If cleanup itself is locked, leave it unlisted and preserve
        # the original error; never delete a session after its commit succeeded.
        try:
            pending = self._pending_directory(name, staging)
        except (OSError, ValueError):
            return
        shutil.rmtree(pending, ignore_errors=True)

    def _manifest(self, directory):
        directory = Path(directory)
        if (not re.fullmatch(SESSION_ID_PATTERN, directory.name)
                or directory.parent != self.sessions or directory.is_symlink()
                or directory.resolve(strict=True) != directory):
            raise ValueError('Invalid artifact session directory.')
        manifest = _json(_safe_file(directory / 'manifest.json', self.sessions))
        if (not isinstance(manifest, dict) or manifest.get('id') != directory.name
                or not isinstance(manifest.get('files'), list)):
            raise ValueError('Invalid or uncommitted artifact session manifest.')
        return manifest

    def _finish(self, name, staging, files, source, **extra):
        staging = self._pending_directory(name, staging)
        manifest = {'id': name, 'createdUtc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                    'source': source, 'files': sorted(files, key=lambda f: f['name']), **extra}
        if not any(item['name'] == 'script.json' for item in files):
            raise ValueError('A dump session requires script.json.')
        script = _json(_safe_file(staging / 'script.json', staging))
        if not isinstance(script, dict) or not isinstance(script.get('Functions'), list):
            raise ValueError('script.json must contain a Functions array.')
        # This is the sole publication point. _atomic_json flushes/fsyncs a
        # separate file before replacing manifest.json; partial JSON is never
        # exposed to list_sessions() or exact-ID lookup. No writes follow commit.
        _atomic_json(staging / 'manifest.json', manifest)
        return {**manifest, 'path': str(staging)}

    def import_directory(self, path, symbols_only=False):
        original = Path(path).expanduser()
        if not original.is_absolute():
            original = self.root / original
        if original.is_symlink():
            raise ValueError('Import directory must not be a symbolic link.')
        source = original.resolve(strict=True)
        if not source.is_relative_to(self.root) or not source.is_dir():
            raise ValueError('Imports are restricted to the configured workspace.')
        name, staging = self._new()
        files = []
        try:
            for filename in SYMBOL_FILES + (() if symbols_only else LIBRARY_FILES):
                candidate = source / filename
                if not candidate.exists():
                    continue
                candidate = _safe_file(candidate, self.root)
                with candidate.open('rb') as stream:
                    files.append(_stream(stream, staging / filename))
            return self._finish(name, staging, files, str(source), symbolsOnly=bool(symbols_only))
        except BaseException:
            self._discard_pending(name, staging)
            raise

    def sync_from_device(self, device_url, symbols_only=False):
        parsed = urllib.parse.urlsplit(str(device_url))
        if parsed.scheme not in ('http', 'https') or parsed.hostname not in ('127.0.0.1', 'localhost', '::1') or parsed.username or parsed.password:
            raise ValueError('Device URL must be an ADB-forwarded loopback HTTP endpoint.')
        if parsed.path not in ('', '/') or parsed.query or parsed.fragment:
            raise ValueError('Use a device origin URL without a path, query or fragment.')
        origin = str(device_url).rstrip('/')
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *args, **kwargs):
                raise ValueError('Artifact download redirects are not accepted.')
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        def request(path):
            return opener.open(urllib.request.Request(origin + path, headers={'X-UMT-Client': '1'}), timeout=60)
        def read_json(path):
            with request(path) as response:
                body = response.read(16 * 1024 * 1024 + 1)
                if len(body) > 16 * 1024 * 1024:
                    raise ValueError('Device JSON response is too large.')
                return json.loads(body)
        before = read_json('/api/state')
        if before.get('busy'):
            raise ValueError('Wait for the device operation to finish before syncing.')
        listing = read_json('/api/files')
        if not isinstance(listing, list):
            raise ValueError('Device file list is invalid.')
        def file_identity(entries):
            if not isinstance(entries, list):
                raise ValueError('Device file list is invalid.')
            return sorted((entry.get('name'), entry.get('size')) for entry in entries
                          if isinstance(entry, dict) and entry.get('name') in ALLOWED_FILES
                          and not (symbols_only and entry.get('name') in LIBRARY_FILES))
        initial_files = file_identity(listing)
        name, staging = self._new()
        files, seen = [], set()
        try:
            for entry in listing:
                filename = entry.get('name') if isinstance(entry, dict) else None
                if filename not in ALLOWED_FILES or filename in seen or (symbols_only and filename in LIBRARY_FILES):
                    continue
                seen.add(filename)
                # Never trust the server-supplied download URL or a filesystem path.
                with request('/api/file?name=' + urllib.parse.quote(filename, safe='')) as response:
                    actual = _stream(response, staging / filename)
                expected = entry.get('size')
                if isinstance(expected, int) and actual['size'] != expected:
                    raise ValueError('Artifact changed during sync: ' + filename)
                files.append(actual)
            final_files = file_identity(read_json('/api/files'))
            after = read_json('/api/state')
            identity = ('pid', 'package', 'generation', 'resultPath')
            before_metrics, after_metrics = before.get('metrics', {}), after.get('metrics', {})
            changed_operation = any(before_metrics.get(k) != after_metrics.get(k) for k in ('dumpMs', 'libraryMs'))
            if after.get('busy') or changed_operation or initial_files != final_files or any(before.get(k) != after.get(k) for k in identity):
                raise ValueError('Device session changed during sync; retry after it becomes idle.')
            return self._finish(name, staging, files, origin, device={**{k: before.get(k) for k in identity},
                'metrics': {k: before_metrics.get(k) for k in ('dumpMs', 'libraryMs')}}, symbolsOnly=bool(symbols_only))
        except BaseException:
            self._discard_pending(name, staging)
            raise

    def list_sessions(self):
        result = []
        for directory in sorted(self.sessions.iterdir(), reverse=True):
            if directory.name.startswith('.') or directory.is_symlink() or not directory.is_dir():
                continue
            try:
                manifest = self._manifest(directory)
                result.append({**manifest, 'path': str(directory)})
            except (OSError, ValueError, TypeError):
                continue
        return result

    def _session(self, session=None):
        if session is None:
            available = self.list_sessions()
            if not available:
                raise ValueError('No artifact sessions are available.')
            session = available[0]['id']
        if not isinstance(session, str) or not re.fullmatch(SESSION_ID_PATTERN, session):
            raise ValueError('Invalid artifact session identifier.')
        directory = self.sessions / session
        self._manifest(directory)
        return directory

    def _functions(self, session=None):
        directory = self._session(session)
        key = directory.name
        if key not in self._cache:
            rows = []
            for item in _json(_safe_file(directory / 'script.json', self.sessions)).get('Functions', []):
                if not isinstance(item, dict) or not isinstance(item.get('Name'), str):
                    continue
                value = item.get('Address')
                if isinstance(value, str):
                    try:
                        value = int(value, 0)
                    except ValueError:
                        continue
                if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xffffffffffffffff:
                    continue
                rows.append({'name': item['Name'], 'rva': hex(value), 'addressKind': 'RVA', 'session': key})
            classes = directory / 'SDK_Classes.hpp'
            text = classes.read_text('utf-8', errors='replace') if classes.is_file() and not classes.is_symlink() else ''
            self._cache[key] = (rows, text)
        return self._cache[key]

    def _details(self, row, text):
        owner, separator, function = row['name'].partition('$$')
        if function.startswith('exec'):
            function = function[4:]
        if not separator or not owner or not function:
            return {**row, 'sdkSnippet': '', 'sdkSnippetMatch': 'missing', 'sdkSnippetCandidates': []}
        key = row['session']
        if key not in self._snippet_cache:
            index = {}
            sanitize = lambda name: re.sub(r'[^A-Za-z0-9_]', '_', name)
            # Bind a signature to its reflection owner, not the first matching
            # C++ method name in a potentially multi-million-line SDK header.
            markers = list(re.finditer(r'^[ \t]*//\s*(?:Object:\s*)?Function\s+([^\r\n]+)', text, re.MULTILINE))
            for marker in markers:
                full_name = marker.group(1).strip()
                parts = full_name.rsplit('.', 2)
                if len(parts) < 2:
                    continue
                class_name, method = parts[-2:]
                tail = text[marker.end():marker.end() + 8192]
                boundary = re.search(r'^[ \t]*//\s*(?:Object:\s*)?(?:Function|Class)\s|^[ \t]*};', tail, re.MULTILINE)
                if boundary:
                    tail = tail[:boundary.start()]
                prototype = re.search(r'\b' + re.escape(method) + r'\s*\([^;]*\)\s*(?:const\s*)?;', tail)
                if not prototype:
                    continue
                snippet = text[marker.start():marker.end()] + tail[:prototype.end()]
                index.setdefault((sanitize(class_name), sanitize(method)), []).append(
                    {'owner': class_name, 'function': method, 'fullName': full_name, 'snippet': snippet.strip()})
            self._snippet_cache[key] = index
        candidates = self._snippet_cache[key].get((owner, function), [])
        unique = len(candidates) == 1
        return {**row, 'sdkSnippet': candidates[0]['snippet'] if unique else '',
                'sdkSnippetMatch': 'unique' if unique else 'ambiguous' if candidates else 'missing',
                'sdkSnippetCandidates': [{k: candidate[k] for k in ('owner', 'function', 'fullName')} for candidate in candidates]}

    def search_functions(self, query, limit=50, session=None):
        if not isinstance(query, str) or len(query) > 512:
            raise ValueError('query must be a string of at most 512 characters.')
        limit = max(1, min(int(limit), 1000))
        rows, text = self._functions(session)
        key = query.strip().casefold()
        rva = hex(int(key, 16)) if re.fullmatch(r'0x[0-9a-f]{1,16}', key) else None
        selected = [row for row in rows if key in row['name'].casefold() or (rva is not None and row['rva'] == rva)][:limit]
        return [self._details(row, text) for row in selected]

    def get_function(self, name, session=None):
        rows, text = self._functions(session)
        matches = [self._details(row, text) for row in rows if row['name'] == name]
        if len(matches) == 1:
            return {'found': True, 'ambiguous': False, **matches[0]}
        return {'found': bool(matches), 'ambiguous': len(matches) > 1, 'candidates': matches}

    def prepare_ida(self, session=None, library=None):
        """Create a unique analysis copy and naming script; never open/alter an IDB."""
        directory = self._session(session)
        libraries = [name for name in LIBRARY_FILES if (directory / name).is_file()]
        if library is None:
            if len(libraries) != 1:
                raise ValueError('Specify one available library: ' + ', '.join(libraries))
            library = libraries[0]
        if library not in LIBRARY_FILES:
            raise ValueError('Unrecognized UE library filename.')
        source = _safe_file(directory / library, self.sessions)
        analysis = directory / 'analysis' / uuid.uuid4().hex
        analysis.mkdir(parents=True)
        destination = analysis / library
        mapping = memory_image_to_elf(source, destination)
        shutil.copyfile(directory / 'script.json', analysis / 'script.json')
        script = analysis / 'import_symbols.py'
        script.write_text(IDAPYTHON_NAMER, encoding='utf-8')
        _atomic_json(analysis / 'mapping.json', mapping)
        skill = Path(os.environ.get('USERPROFILE', str(Path.home()))) / '.codex' / 'skills' / 'ida-reverse' / 'scripts' / 'open.ps1'
        return {'session': directory.name, 'input': str(destination), 'analysisDirectory': str(analysis),
                'mapping': mapping, 'importScript': str(script), 'openScript': str(skill),
                'openCommand': ['powershell', '-NoProfile', '-File', str(skill), '-Path', str(destination), '-NoAutoAnalysis']}


def memory_image_to_elf(source: Path, destination: Path):
    """Rebuild file layout from Android memory image offsets = ELF vaddr - loadVaddr.

    Section headers are removed because non-PT_LOAD file data is not recoverable.
    Runtime relocations remain applied; this is an analysis image, not a runnable ELF.
    """
    source, destination = Path(source), Path(destination)
    if source.resolve() == destination.resolve():
        raise ValueError('Conversion requires a separate analysis copy.')
    size = source.stat().st_size
    with source.open('rb') as stream:
        header = bytearray(stream.read(64))
        if len(header) < 64 or header[:4] != b'\x7fELF' or header[4:6] != b'\x02\x01':
            raise ValueError('Expected a little-endian ELF64 memory image.')
        machine, = struct.unpack_from('<H', header, 18)
        if machine not in (183, 62):
            raise ValueError('Unsupported ELF machine for memory-layout conversion.')
        phoff, = struct.unpack_from('<Q', header, 32)
        phentsize, phnum = struct.unpack_from('<HH', header, 54)
        if phentsize != 56 or not 1 <= phnum <= 4096 or phoff + phentsize * phnum > min(size, 16 * 1024 * 1024):
            raise ValueError('Invalid or unsupported ELF program-header table.')
        stream.seek(phoff)
        program_headers = bytearray(stream.read(phentsize * phnum))
        descriptors = []
        segments = []
        for index in range(phnum):
            kind, flags, offset, vaddr, paddr, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', program_headers, index * 56)
            descriptors.append({'index': index, 'type': kind, 'originalFileOffset': offset,
                                'fileOffset': offset, 'virtualAddress': vaddr,
                                'originalFileSize': filesz, 'fileSize': filesz, 'memorySize': memsz})
            if kind == 1:
                if filesz > memsz or offset + filesz > 8 * 1024**3 or (align > 1 and align & (align - 1)):
                    raise ValueError('Invalid PT_LOAD segment lengths.')
                segments.append({'programHeaderIndex': index, 'originalFileOffset': offset,
                                 'fileOffset': offset, 'virtualAddress': vaddr, 'fileSize': filesz,
                                 'originalFileSize': filesz,
                                 'memorySize': memsz, 'flags': flags, 'alignment': align})
        origins = [s['virtualAddress'] for s in segments if s['fileOffset'] == 0 and s['fileSize'] >= 64]
        if len(origins) != 1:
            raise ValueError('Cannot derive unique load origin from a PT_LOAD containing the ELF header.')
        origin = origins[0]
        for segment in segments:
            segment['memoryOffset'] = segment['virtualAddress'] - origin
            if segment['memoryOffset'] < 0 or segment['memoryOffset'] + segment['fileSize'] > size:
                raise ValueError('PT_LOAD bytes fall outside the supplied memory dump.')
        # The original table is trustworthy only when it is mapped at its file offset.
        if not any(s['fileOffset'] == 0 and phoff + len(program_headers) <= s['fileSize'] for s in segments):
            raise ValueError('Program-header table is not in the first load segment.')
        # Runtime descriptors can cover bytes in a LOAD's zero-fill tail. For
        # example GNU_RELRO may include the alignment/BSS tail, and a relocated
        # DYNAMIC table may have been populated there after loading. Preserve
        # the actual captured bytes, extending only the owning LOAD's p_filesz
        # as far as needed. Never substitute zeroes for unavailable dump bytes.
        materialized = []
        for descriptor in descriptors:
            address, length = descriptor['virtualAddress'], descriptor['fileSize']
            if descriptor['type'] == 1 or not length or not address:
                continue
            if any(segment['virtualAddress'] <= address
                   and address + length <= segment['virtualAddress'] + segment['fileSize']
                   for segment in segments):
                continue
            owners = [segment for segment in segments if segment['virtualAddress'] <= address
                      and address + length <= segment['virtualAddress'] + segment['memorySize']]
            if len(owners) != 1:
                raise ValueError('Cannot uniquely remap program-header descriptor ' + str(descriptor['index']))
            segment = owners[0]
            required_size = address + length - segment['virtualAddress']
            if segment['memoryOffset'] + required_size > size:
                raise ValueError('Program-header descriptor ' + str(descriptor['index']) + ' requires uncaptured runtime tail bytes.')
            materialized.append({'programHeaderIndex': segment['programHeaderIndex'],
                                 'descriptorIndex': descriptor['index'],
                                 'memoryOffset': segment['memoryOffset'] + segment['fileSize'],
                                 'size': required_size - segment['fileSize']})
            segment['fileSize'] = required_size
        def overlaps(items, offset_key):
            end = 0
            for item in sorted((item for item in items if item['fileSize']), key=lambda item: item[offset_key]):
                if item[offset_key] < end:
                    return True
                end = item[offset_key] + item['fileSize']
            return False
        original_overlap = overlaps([
            {**segment, 'fileSize': segment['originalFileSize']} for segment in segments], 'originalFileOffset')
        memory_layout = overlaps(segments, 'originalFileOffset')
        if overlaps(segments, 'memoryOffset'):
            raise ValueError('Overlapping runtime PT_LOAD ranges cannot be reconstructed unambiguously.')
        if memory_layout:
            # Protected ELF files can alias disk ranges which become independent
            # decrypted pages at runtime. Reusing those offsets would overwrite
            # one runtime segment with another. Preserve all live bytes by using
            # virtual-memory offsets, and rewrite every mapped descriptor.
            for segment in segments:
                segment['fileOffset'] = segment['memoryOffset']
                if segment['fileOffset'] + segment['fileSize'] > 8 * 1024**3:
                    raise ValueError('Reconstructed ELF exceeds the 8 GiB file-size limit.')
                align = segment['alignment']
                if align > 1 and segment['fileOffset'] % align != segment['virtualAddress'] % align:
                    raise ValueError('Memory-offset layout cannot preserve PT_LOAD alignment with this load origin.')
        by_index = {segment['programHeaderIndex']: segment for segment in segments}
        for descriptor in descriptors:
            if descriptor['type'] == 1:
                segment = by_index[descriptor['index']]
                descriptor['fileOffset'], descriptor['fileSize'] = segment['fileOffset'], segment['fileSize']
            elif descriptor['fileSize']:
                address, length = descriptor['virtualAddress'], descriptor['fileSize']
                candidates = [segment['fileOffset'] + address - segment['virtualAddress'] for segment in segments
                              if segment['virtualAddress'] <= address
                              and address + length <= segment['virtualAddress'] + segment['fileSize']]
                # An unmapped descriptor with zero vaddr is recoverable only
                # if its old file range maps to exactly one original load.
                if not candidates and address == 0:
                    old = descriptor['originalFileOffset']
                    candidates = [segment['fileOffset'] + old - segment['originalFileOffset'] for segment in segments
                                  if segment['originalFileOffset'] <= old
                                  and old + length <= segment['originalFileOffset'] + segment['originalFileSize']]
                if len(set(candidates)) != 1:
                    raise ValueError('Cannot uniquely remap program-header descriptor ' + str(descriptor['index']))
                descriptor['fileOffset'] = candidates[0]
            elif memory_layout and descriptor['virtualAddress'] >= origin and descriptor['virtualAddress']:
                descriptor['fileOffset'] = descriptor['virtualAddress'] - origin
            struct.pack_into('<Q', program_headers, descriptor['index'] * 56 + 8, descriptor['fileOffset'])
            struct.pack_into('<Q', program_headers, descriptor['index'] * 56 + 32, descriptor['fileSize'])
        for segment in segments:
            segment['materializedMemoryBytes'] = segment['fileSize'] - segment['originalFileSize']
            if segment['fileOffset'] + segment['fileSize'] > 8 * 1024**3:
                raise ValueError('Reconstructed ELF exceeds the 8 GiB file-size limit.')
            align = segment['alignment']
            if align > 1 and segment['fileOffset'] % align != segment['virtualAddress'] % align:
                raise ValueError('PT_LOAD file and virtual offsets violate ELF alignment.')
        destination.parent.mkdir(parents=True, exist_ok=True)
        partial = destination.with_name(destination.name + '.partial')
        try:
            with partial.open('w+b') as output:
                for segment in segments:
                    stream.seek(segment['memoryOffset'])
                    output.seek(segment['fileOffset'])
                    remaining = segment['fileSize']
                    while remaining:
                        chunk = stream.read(min(1024 * 1024, remaining))
                        if not chunk:
                            raise ValueError('Truncated memory image.')
                        output.write(chunk)
                        remaining -= len(chunk)
                # An ELF loader must not follow unavailable original section tables.
                struct.pack_into('<Q', header, 40, 0)
                struct.pack_into('<HHH', header, 58, 0, 0, 0)
                output.seek(0)
                output.write(header)
                output.seek(phoff)
                output.write(program_headers)
                output.flush()
                os.fsync(output.fileno())
            os.replace(partial, destination)
        finally:
            partial.unlink(missing_ok=True)
    return {'format': 'ELF64-file-layout-from-runtime-memory', 'machine': machine, 'sourceMemorySize': size,
            'loadVirtualAddress': hex(origin), 'rvaToIdaRule': 'IDA image base + RVA',
            'outputLayout': 'virtual-memory-offset' if memory_layout else 'original-file-offset',
            'originalLoadFileRangesOverlap': original_overlap, 'programHeaders': descriptors,
            'materializedRuntimeTails': materialized,
            'segments': segments, 'sectionHeadersRemoved': True, 'runtimeRelocationsRemain': True,
            'warning': 'PT_LOAD bytes and descriptor-required captured runtime tails are file-backed; remaining BSS is loader-created. Unavailable section metadata was discarded.'}


IDAPYTHON_NAMER = r'''# Run explicitly in IDA after checking mapping.json and the loaded image.
import json
from pathlib import Path
import ida_name
import ida_nalt
import ida_bytes
import ida_kernwin

path = Path(__file__).resolve().with_name('script.json')
functions = json.loads(path.read_text(encoding='utf-8-sig')).get('Functions', [])
base = ida_nalt.get_imagebase()
if ida_kernwin.ask_yn(ida_kernwin.ASKBTN_NO,
    'Import {} function labels at image base 0x{:X}? Existing non-dummy names are preserved.'.format(len(functions), base)) == ida_kernwin.ASKBTN_YES:
    counts = dict(named=0, skipped=0, failed=0)
    for item in functions:
        if not isinstance(item, dict) or not isinstance(item.get('Name'), str):
            counts['skipped'] += 1
            continue
        rva = item.get('Address')
        if isinstance(rva, str):
            rva = int(rva, 0)
        if isinstance(rva, bool) or not isinstance(rva, int) or rva < 0:
            counts['skipped'] += 1
            continue
        ea = base + rva
        flags = ida_bytes.get_flags(ea)
        if not ida_bytes.is_mapped(ea) or (ida_bytes.has_name(flags) and not ida_bytes.has_dummy_name(flags)):
            counts['skipped'] += 1
            continue
        if ida_name.set_name(ea, item['Name'], ida_name.SN_CHECK | ida_name.SN_NOWARN):
            counts['named'] += 1
        else:
            counts['failed'] += 1
    print('UMT symbol import:', counts)
'''
