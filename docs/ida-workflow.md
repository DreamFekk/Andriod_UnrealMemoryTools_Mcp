# IDA / artifact workflow

The Python bridge keeps device dumps and IDA analysis inputs separate. It uses Python 3.10+ standard-library modules only. Run examples from the project root.

```python
from pathlib import Path
from bridge.artifacts import ArtifactStore
from bridge.ida_client import IDAClient

store = ArtifactStore(Path('artifacts/analysis-workspace'))
session = store.sync_from_device('http://127.0.0.1:18765', symbols_only=False)
print(session['id'], session['files'])
print(store.search_functions('execMove', session=session['id']))
print(store.get_function('Player$$execMove', session=session['id']))
```

`sync_from_device()` requests `/api/state`, `/api/files`, and known `/api/file?name=...` endpoints. It does not execute ADB commands. Start the device service and ADB forwarding through the desktop launcher first. URLs must use loopback; proxy inheritance and redirects are disabled. The file allowlist contains the nine SDK files plus `libUE4.so` / `libUnreal.so`. `symbols_only=True` skips library transfers. A valid `script.json` is required. Every stored file has a SHA-256 entry in `manifest.json`. Downloads stream in 1 MiB chunks into a staging session; the completed session appears only after validation. Device PID/package/generation/output-path changes, dump/library completion-metric changes, or differences in the final allowed-file size listing invalidate the sync. Windows indexers can prevent renaming a populated directory. Sessions now use unique final-named folders but remain invisible until manifest.json is atomically published last. Listing and exact-ID lookup require a valid committed manifest. Failed pending sessions are removed only after checking the exact unpublished child path; committed sessions are preserved.

For a local import, copy the dump into a directory under the configured workspace:

```python
store = ArtifactStore(Path('artifacts'))
session = store.import_directory('new-output', symbols_only=True)
print(store.list_sessions())
```

An MCP-supplied local path cannot import outside that configured root. Symbolic-link artifacts, unknown filenames, parent traversal and arbitrary server-supplied download URLs are rejected. Session identifiers are generated locally. Existing sessions are not overwritten.

## Address mapping

`script.json` contains UE function **RVAs**, not absolute Android pointers and not IDA addresses:

```text
Android runtime function = Android UE module base + script.json Address
IDA function             = IDA loaded image base  + script.json Address
```

`ArtifactStore.get_function()` uses an exact name. Repeated function names return `ambiguous: true` and all candidates; the bridge does not pick one. `search_functions()` returns `name`, hexadecimal `rva`, `addressKind: RVA`, `session` and a nearby SDK class-header snippet. A snippet is attached only when the reflected owner and function match a unique `// Object: Function Package.Owner.Function` block. Missing matches return an empty snippet; conflicting owner/function blocks return an empty snippet with `sdkSnippetMatch: ambiguous` and explicit candidate full names. The first same-named method from another class is never substituted. IDA's actual loaded image base must be confirmed before calling analysis tools. `IDAClient.analyze_function()` accepts an absolute IDA address and never silently adds a base.

## A runtime memory image is not the original ELF file

The device library dumper writes a contiguous runtime image where byte offset zero corresponds to the module base. ELF disk files use `PT_LOAD.p_offset`; runtime memory uses `PT_LOAD.p_vaddr`. These offsets often differ after the first segment. Opening the raw memory image as though it were the original disk ELF can put code/data at incorrect addresses.

```python
prepared = store.prepare_ida(session['id'], library='libUE4.so')
print(prepared['input'])
print(prepared['mapping'])
print(prepared['importScript'])
```

`prepare_ida()` creates a new UUID analysis subdirectory each time. It reconstructs a separate ELF file by copying, for every PT_LOAD segment:

```text
memory image source offset = p_vaddr - loadVirtualAddress
ELF output file offset     = p_offset
copy length                = original p_filesz, extended only for captured descriptor tails
```

The load origin is derived from a unique PT_LOAD segment whose file offset is zero and which contains the ELF header. Unsupported, truncated or ambiguous layouts fail explicitly. The converter supports little-endian ELF64 with AArch64 or x86-64 machine identifiers, validates table/segment bounds, strips unavailable section-header metadata, and writes `mapping.json`. BSS is not fabricated as file-backed data. If a mapped descriptor requires bytes in a LOAD's captured runtime tail, the converter preserves those actual bytes and extends that LOAD's `p_filesz` only as far as necessary; the remaining BSS is still loader-created from `p_memsz`.

This produces an **analysis ELF**, not a pristine or runnable copy. Runtime relocations remain applied. Unmapped section contents cannot be recovered. Dynamic metadata may contain runtime pointers, and a loader may require additional segment/base configuration. Inspect IDA's segment virtual addresses against `mapping.json` before trusting symbols. If IDA's loader rebases the library, use its current `get_imagebase()`; do not use the Android runtime base as an IDA EA. For unusual module origins, verify the first loaded segment versus the recorded `loadVirtualAddress` before symbol import.

## Open only the unique analysis copy

Open `prepared['input']` with **File → Open** in IDA, or use the explicit MCP adapter below. Keep the generated `mapping.json`, `script.json` and `import_symbols.py` beside that analysis input. Work in the unique analysis directory so existing databases and original dumps remain separate.

`prepare_ida()` prepares files without starting IDA. Its optional `openScript` / `openCommand` fields refer to a user-installed Codex IDA skill, which is not included in this repository. They are conveniences for compatible installations, not prerequisites. The manual workflow and `IDAClient.open_database()` do not require that skill.

## Import names explicitly

`prepare_ida()` places `script.json` and `import_symbols.py` beside the analysis input. Open the input, verify its image base/segments, and explicitly run the script in IDA. The script calls `ida_nalt.get_imagebase()` at runtime, validates mapped addresses, preserves existing non-dummy names, and asks for confirmation before applying labels. It does not patch bytes, undefine ranges, force functions or overwrite existing user labels. Conflicting symbol names are counted as failed rather than forced into another address. It does not claim that reflection Func pointers are always complete native implementations.

## Query the installed IDA MCP server

```python
ida = IDAClient('http://127.0.0.1:13338/mcp', database=None)
status = ida.status()
print(status['connected'], status.get('toolCount'), status.get('error'))
# Pass an absolute EA only after confirming the IDA image base.
result = ida.analyze_function('0x12345678')
print(result['evidence'])
print(result['errors'])
```

The client performs MCP initialize / initialized negotiation, retains the MCP session header, discovers paginated tool schemas, accepts JSON and SSE replies, and chooses the advertised read tools for decompilation, disassembly, inbound references and callees. It builds address arguments from the real `inputSchema`; unknown required arguments produce explicit errors rather than guessed defaults. Evidence keeps the exact tool name, arguments and raw MCP result. Missing tools, transport failures and `isError` results remain visible. This is tool evidence, not an invented analysis.

`call_tool(name, args)` is the explicit escape hatch for advertised tools. It may include mutating IDA tools if the caller deliberately names one; the analysis helper never selects rename, patch, undefine or similar tools. MCP transports that require legacy GET-event-stream plus a separate POST endpoint are not supported; the HTTP client handles the common streamable HTTP POST response in JSON or SSE form.

## Validation

```powershell
python -m unittest discover -s tests -p test_bridge_artifacts.py -v
python -m unittest discover -s tests -p test_ida_client.py -v
```

Tests cover immutable imports, SHA-256 manifests, duplicate names, symbols-only allowlisting, workspace restriction, streaming device sync, exact segment mapping, failure preserving an existing analysis file, unique analysis copies, MCP initialization, JSON/SSE results, real tool-schema address argument construction and explicit unsupported-schema errors. They use local temporary fixtures and mock loopback services. A passing test run does not verify a particular IDA installation or game build; check the selected database, loaded segments and a known function against your own input.

IDA supervisor versions may advertise `idb_open`, `idb_list`, and a required `database` field on worker tools. Enumerate sessions explicitly and use `select_database()` before analysis. The client injects this selection only where the actual schema has a database field. It does not silently open or choose a database. GUI/native worker endpoints may omit that field.

## Supervisor open/select adapter

The installed server's `tools/list` is authoritative. The adapter expects `idb_open`, `idb_list` and the advertised worker schemas. A GUI-only endpoint can analyze its existing database without exposing a tool to open another one.

After installing IDA / IDALib and compatible `ida-pro-mcp` in the workbench's Python environment, set the Web UI endpoint to `http://127.0.0.1:13338/mcp` and click **Save & auto-connect**. This submits an asynchronous `start_ida` command. It reuses a healthy local supervisor or launches `sys.executable -m ida_pro_mcp.idalib_supervisor` without a visible Windows terminal, then performs bounded health checks. An existing GUI plugin may remain on port 13337.

When IDA discovery succeeds, the child process receives the matching `IDADIR`. Dependencies and licenses must already be installed; startup does not run pip or obtain a license. Failures report the missing component or startup error and local log location. Service logs/process metadata are under the configured workspace's `ida-service/` directory (by default `artifacts/ida-service/`), and the endpoint is saved in `workbench-settings.json`. **Check IDA connection**, `umt_ida_status` and status polling are passive; they do not launch a service.

`WinError 10061` means the selected endpoint did not accept a connection, usually because no service is listening. Older save/check behavior only changed the URL. Use **Save & auto-connect** to request startup; after a timeout, inspect its task result and log, then use the passive check to see whether startup completed later.

For manual diagnosis or a separately managed service, use the same Python environment as the workbench:

```powershell
python -m ida_pro_mcp.idalib_supervisor --host 127.0.0.1 --port 13338
```

This foreground fallback must remain running while used. The repository does not bundle IDA. If another service occupies the chosen port, use an available local port and update the client URL. Then prepare and open an explicit session:

```python
prepared = store.prepare_ida(session['id'], 'libUE4.so')
ida = IDAClient('http://127.0.0.1:13338/mcp', timeout=120)
opened = ida.open_database(prepared['input'], auto_analysis=False)
# The only selected database ID is the server-returned session.session_id.
print(opened['database'])
print(ida.status()['databaseHealth'])   # includes the actual imagebase when available
# For an already open session, select an exact advertised ID:
ida.select_database(opened['database'])
```

`open_database()` only accepts an analysis input accompanied by mapping.json and script.json. It chooses `mode=prefer_headless`, suppresses full auto-analysis and cache-building by default, and never removes or overwrites an IDB. Its raw open result remains available as evidence. `select_database()` validates the ID against `idb_list`; it does not choose the first session. `status()` enumerates sessions and, after selection, reports `server_health` and the actual database image base. Pass `auto_analysis=True` explicitly when full analysis is desired; runtime and decompiler availability are reported by IDA rather than assumed.
## Aliased PT_LOAD file ranges in protected images

Some libraries have several PT_LOAD records sharing or overlapping their original file offsets, while their live virtual ranges hold different bytes after initialization. Copying those live pages back to the original offsets would overwrite earlier segments. The converter detects any overlap of nonempty original load-file ranges and switches to `outputLayout: virtual-memory-offset`: every load segment gets `p_offset = p_vaddr - loadVirtualAddress`. All original virtual addresses and permissions remain unchanged.

The converter rewrites the binary program-header table, including PT_DYNAMIC, PT_NOTE, PT_PHDR, and other mapped descriptors, so their offsets point into the relocated file-backed range. Descriptors without a usable virtual mapping must have exactly one recoverable original file mapping or conversion fails. The converter verifies `p_offset % p_align == p_vaddr % p_align`; a load origin that prevents this layout is rejected rather than emitting a misaligned ELF. Overlapping live file-backed virtual ranges are likewise rejected as ambiguous.

Each segment mapping records `originalFileOffset` and the actual new `fileOffset`, plus `memoryOffset`, `virtualAddress`, and the original program-header index. `programHeaders` records all descriptor remappings. This does not change the symbol formula: IDA image base plus RVA. Tests include distinct A/B runtime pages whose original disk offsets alias, dependent DYNAMIC/NOTE/PHDR relocation, and preservation of existing output when alignment cannot be satisfied.

## Descriptors extending into captured runtime tails

A runtime descriptor such as `PT_DYNAMIC` or `PT_GNU_RELRO` can be fully contained in a LOAD's `p_memsz` while extending beyond its original `p_filesz`. Rejecting it as unmappable drops valid captured data; merely assigning a new descriptor offset would leave it outside the rebuilt file-backed LOAD. The converter therefore:

1. Finds the unique LOAD whose original virtual memory range contains the entire descriptor.
2. Verifies the required range exists in the supplied memory dump. Missing bytes are an error, never silently zero-filled.
3. Extends that LOAD's file size to the descriptor end, without changing `p_memsz`, virtual addresses or permissions.
4. Rechecks file-range overlap after extension and switches to virtual-memory-offset layout when necessary; rejects overlap of the resulting file-backed virtual ranges.
5. Copies the actual runtime bytes and rewrites LOAD `p_filesz` and descriptor offsets in the ELF table.

Mappings retain `originalFileSize`, new `fileSize`, per-segment `materializedMemoryBytes`, and detailed `materializedRuntimeTails` entries. This is materialization of captured runtime state for analysis, not recovery of the original disk image's BSS.

### Verify a converted image

Check that every nonempty descriptor lies inside the output and agrees with its containing LOAD. Verify LOAD lengths and alignment against `mapping.json`, then compare copied bytes with the source memory ranges, excluding intentionally rewritten ELF/program headers. Retain the source SHA-256 from the session manifest. These checks validate conversion; opening IDA and confirming the loaded virtual addresses are separate steps.
