# Capabilities and profile compatibility

UnrealMemoryTools provides an Android ARM64 UE reflection reader, SDK exporter, native inspector, desktop workbench and MCP bridge. The profiles below are registered in `src/App.cpp`; package matching selects a dedicated profile before the generic AutoFix path. A registered profile is an implementation, not a guarantee that every current game build is compatible.

## Capability map

| Area | Available behavior |
|---|---|
| Process discovery | Lists known packages and processes with `libUE4.so` / `libUnreal.so`; retains separate PIDs |
| Memory access | Root-backed reads with syscall and proc-memory paths; checks process start time before reusing a probe |
| Engine recovery | Legacy GNames, FNamePool, object arrays and runtime offset recovery, with profile-specific decoding callbacks |
| Export | SDK headers, object list, structure offsets, function RVAs and optional contiguous runtime library image |
| SDK browser | Search, pagination, class hierarchy, properties, functions and navigation through type/object pointers |
| Instance inspection | UObject and reflected structure values, FVector decoding, refresh, history and tags |
| World inspection | UWorld discovery and Actor lists from persistent and loaded Levels |
| Interfaces | Android native overlay, desktop/mobile Web UI and a stdio MCP server |
| IDA bridge | Immutable dump sessions, SHA-256 manifests, analysis ELF preparation and explicit database selection |

Background actions are serialized. Status snapshots remain queryable during work, and the explorer shares the reflection-operation lock. Each probe creates a fresh profile, while process/generation changes invalidate navigation and cached state. Export writes use temporary files and do not delete a previously dumped library.

## Registered profiles

| Profile | Package IDs |
|---|---|
| Arena Breakout | `com.tencent.mf.uam` |
| Delta Force | `com.tencent.tmgp.dfm` |
| Farlight 84 | `com.lilithgames.solarland.android.cn` |
| 枪战特训2 | `com.ShuiSha.FPS2` |
| Valorant | `com.tencent.tmgp.codev` |
| 洛克王国世界 | `com.tencent.nrc` |
| 和平精英 | `com.tencent.tmgp.pubgmhd` |
| PUBG Mobile variants | `com.tencent.ig`, `com.rekoo.pubgm`, `com.pubg.imobile`, `com.pubg.krmobile`, `com.vng.pubgmobile` |
| NEW STATE Mobile | `com.pubg.newstate` |
| Wuthering Waves / 鸣潮 | `com.kurogame.mingchao`, `com.kurogame.wutheringwaves.global` |

There are **10 dedicated profiles** plus the generic AutoFix fallback. `Lineage2.hpp` remains available as source but is not registered by `App::Profiles()`.

Wuthering Waves uses dedicated GUObjectArray / NamePool patterns and a Kuro name-record index (`uint32_t id`, `uint16_t header`, character data, 4-byte alignment). Its instance-owned index uses the resolved NamePool when `GNameBlocksDebug` is unavailable. It does not resolve Matrix / PhysX / FrameCount, and its wide-name conversion currently substitutes non-ASCII characters with `?`. Compatibility with a live game build is not yet verified.

The [UE5 / PUBG / NEW STATE guide](ue5-profile-support.md) describes engine layouts, decoder behavior and current limits. Dedicated decoding can fail after game updates; AutoFix is not a substitute for missing or changed proprietary decoding.

### Delta Force object-array fix

Object-array discovery now rejects the high 32 bits of the `Objects` pointer being mistaken for a count of 110, along with false prefix/compacted-table candidates. It supports a count slot at signed relative offset `-12` from the `Objects` field while preserving nonnegative exported structure offsets.

On 2026-11-01, `com.tencent.tmgp.dfm` version `1.201.37117.67` (versionCode `2207`) completed `RunFixup` successfully (`1`) and exported 101,912 objects, 434 packages, 12,515 classes, 8,672 structs, 3,569 enums and 50,173 nonzero function RVAs; SDK export took approximately 733 ms. These results describe that running build, not all Delta Force versions. Validation also passed all 20 [object-array layout cases](../tests/object_array_layout_test.cpp) and 66 [engine compatibility checks](../tests/engine_compat_test.cpp) on the device.

## Exported files

The default device output is `/sdcard/UnrealMemoryTools_Refactored/<package>/`; the launcher can configure a different root.

| File | Contents |
|---|---|
| `Logs.txt` | SDK generation details |
| `Objects.txt` | `[index] [address]: FullName` records |
| `Offsets.hpp` | Engine structure layout and global-pointer RVAs |
| `AIOHeader.hpp` | Includes for split enum, struct and class headers |
| `SDK_Enums.hpp`, `SDK_Structs.hpp`, `SDK_Classes.hpp` | Reflection output and available vtable annotations |
| `SDK_Offset.hpp` | Sizes, inheritance, member offsets and function RVAs |
| `script.json` | Metadata and function names with base-relative addresses |
| `libUE4.so` / `libUnreal.so` | Optional runtime memory image; see [IDA workflow](ida-workflow.md) before loading |

## Validation boundaries

The repository includes repeatable Python, portable C++ and Android regression tests. Build/test instructions are in the [project README](../README.md) and [contribution guide](../CONTRIBUTING.md). Generated dumps, device logs and local analysis sessions are not distributed with the source.

Unit tests and a successful ARM64 build do not establish live compatibility for every profile. Runtime object reads are not atomic snapshots. Set/Map previews summarize their headers rather than traversing every sparse-container entry, and private native structures require additional type information. See [instance inspection](instance-inspector.md), [world objects](world-objects.md), [native GUI](native-gui.md) and [MCP usage](mcp-usage.md) for operation-specific behavior.
