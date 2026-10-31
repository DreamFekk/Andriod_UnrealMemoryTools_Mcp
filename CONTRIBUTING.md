# Contributing / 贡献指南

Keep changes focused and include the commands used to verify them. Preserve the original author attribution and dependency notices. Game-profile compatibility is version dependent; distinguish a successful build, a synthetic regression test and validation against a running game.

提交应围绕一个明确问题，说明改动和验证命令。保留原作者署名及依赖许可证。专用 Profile 与游戏版本相关，应区分编译通过、构造数据回归通过和对应游戏版本实机验证。

## Source layout / 源码结构

| Path | Purpose / 用途 |
|---|---|
| `src/` | Android C++ application, CLI, HTTP service and native UI / Android 主程序 |
| `src/UE/UEGameProfiles/` | Dedicated game profiles / 游戏专用适配 |
| `desktop/` | ADB deployment and desktop launcher / 桌面启动器 |
| `bridge/` | Workbench, artifact sessions, MCP and IDA integration / 分析工作台 |
| `desktop/web/` | Local Web interface / 本地 Web 界面 |
| `tests/` | Reusable Python and C++ regression tests, device checks / 回归与设备检查 |
| `docs/` | Usage and compatibility documentation / 使用与兼容说明 |
| `KittyMemoryEx/`, `deps/` | Bundled build dependencies / 随源码保留的构建依赖 |
| `build.ps1`, `CMakeLists.txt`, `scripts/` | Build and release tools / 构建与打包工具 |

## Build / 构建

Use Python 3.10+ for the desktop tools and tests. Android builds require NDK r25+, CMake 3.22+, Ninja and C++20; the supported target is ARM64 Android 11 / API 30+. Set `ANDROID_NDK_HOME` when the NDK is not discovered automatically.

桌面工具和测试需要 Python 3.10+。Android 构建需要 NDK r25+、CMake 3.22+、Ninja 和 C++20，目标平台为 ARM64 Android 11 / API 30+。未自动发现 NDK 时设置 `ANDROID_NDK_HOME`。

Run from the repository root / 在项目根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 -SkipPackage
```

The binary is written to `outputs/arm64-v8a/UnrealMemoryTools`. Use `-Headless -BuildDir build-headless` for the Web / CLI variant. See the [README](README.md) for deployment and release packaging. These variants share the default binary output path, so rebuild the intended variant before packaging.

二进制输出到 `outputs/arm64-v8a/UnrealMemoryTools`。Web / CLI 版本增加 `-Headless -BuildDir build-headless`。部署和发布包命令见 [README](README.md)。两种版本默认共用二进制输出路径，打包前应重新构建所需版本。

Create a source-only archive for GitHub without requiring a compiled Android binary:

生成 GitHub 纯源码包，无需提前编译 Android 二进制：

```powershell
python scripts/package_release.py --kind github
```

The package keeps build dependencies, documentation and regression tests while excluding generated outputs and local workspace data. It is written to the sibling `releases/` directory by default; `--output` selects a different ZIP path. Commit the source files to a repository; a release ZIP is a separate delivery artifact.

打包保留构建依赖、文档和回归测试，排除生成产物与本地工作区数据。默认输出到工程同级 `releases/`，`--output` 可指定 ZIP 路径。Git 仓库提交源码文件；发布 ZIP 是独立交付物。

## Regression checks / 回归检查

The Python suite uses the standard library and requires no phone. Browser cases are optional and require Playwright and Microsoft Edge.

Python 回归使用标准库，无需连接手机。浏览器用例为可选项，需要 Playwright 和 Microsoft Edge。

```powershell
python -m unittest discover -s tests -p "test_*.py" -v

# Optional browser checks / 可选浏览器检查
python -m pip install playwright
$env:UMT_BROWSER_TEST = '1'
python -m unittest discover -s tests -p test_desktop.py -v
Remove-Item Env:UMT_BROWSER_TEST
```

Android C++ checks must be cross-compiled with the NDK and run on an ARM64 Android device. After configuring the application build, its regression targets can be built explicitly:

Android C++ 检查需要使用 NDK 交叉编译并在 ARM64 Android 设备执行。配置主工程后，可单独构建以下回归目标：

```powershell
cmake --build build --target umt_native_checks umt_engine_compat_test umt_object_array_layout_test
```

Additional C++ tests have a standalone CMake project:

其他 C++ 测试使用独立 CMake 工程：

```powershell
cmake -S tests -B build-tests -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$env:ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
cmake --build build-tests --parallel 8
adb push ./build-tests/umt_read_cache_test /data/local/tmp/umt_read_cache_test
adb shell chmod 755 /data/local/tmp/umt_read_cache_test
adb shell /data/local/tmp/umt_read_cache_test
```

For live integration checks, start the service and probe the target first. Record the package, game version, Android version, build variant and exact commands; treat results as specific to that environment.

实机集成检查前先启动服务并完成探测。记录包名、游戏版本、Android 版本、构建类型和完整命令；结论仅适用于对应环境。

```powershell
python tests/http_integration.py --base http://127.0.0.1:18765
python tests/explorer_integration.py --base http://127.0.0.1:18765
python tests/world_integration.py --base http://127.0.0.1:18766
```

## Pull requests / 提交说明

- Describe the problem, resulting behavior and validation; include screenshots for visible UI changes. / 说明问题、改后行为与验证结果；界面改动附截图。
- Keep reusable regression tests, and update them when behavior changes. / 保留可重复运行的回归测试，行为变化时同步维护。
- Document profile package IDs and unsupported fields; label untested game versions clearly. / 说明 Profile 包名和未支持字段，明确标注未实测版本。
- Exclude build directories, binaries, dumps, logs, local MCP configuration and workspace data. / 不提交构建目录、二进制、Dump、日志、本机 MCP 配置和工作区数据。
- Preserve third-party source and license notices needed to build and redistribute the project. / 保留构建与分发所需的第三方源码及许可证。

Before committing, use `git diff --check` and `git status --short` to review whitespace and the exact files being submitted. Generated paths such as `outputs/`, `artifacts/` and `bridge/mcp_config.json` are created locally and are not required in a fresh source checkout.

提交前运行 `git diff --check` 和 `git status --short`，检查空白问题与实际提交文件。`outputs/`、`artifacts/` 和 `bridge/mcp_config.json` 等路径会在本地生成，首次检出源码时不需要已有文件。
