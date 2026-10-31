# UnrealMemoryTools {{VERSION}} — 先看这里

**1.0.1 功能：UE5 名称 / 反射兼容增强、PUBG Mobile 名称管理器链适配、NEW STATE Mobile 专用解码 Profile。** 使用流程不变，连接设备后重新探测即可。NEW STATE 的参考密钥与实际版本不符时会明确失败，不退回缺少解码的普通模式。

验证范围：和平精英、无畏契约和 UE5 目标 com.hottagames.yh.laohu 已完成 SDK 实机验证；PUBG 国际版与 NEW STATE 当前以算法 / 构造数据测试为主，尚未完成对应真实版本的完整导出验证。Profile 存在不等于所有版本都兼容。

**1.0.1 修复 UE5 ERROR_EMPTY_PACKAGES / None 占位问题：重新启动工具后，先重新探测，再导出 SDK。已有旧 Dump 不会自动修正。枚举中的 None = 0 等可以是正常定义。**

**版本：{{VARIANT}}。请先把 ZIP 全部解压，再运行；不要在压缩软件中直接双击程序。**

这是 Android Unreal 内存导出工具，带电脑网页界面和可选的 IDA / Agent 分析功能。不是 APK；手机程序为 ARM64 ELF。

## 一、第一次用：按编号来

1. 安装 Windows x64 Python，推荐 3.11 / 3.12，安装时勾选 Add Python to PATH。关闭再打开启动器。
2. 下载 Google Android SDK Platform-Tools，完整解压。把 platform-tools 文件夹放进本包 tools 文件夹，即 tools/platform-tools/adb.exe；也可把 adb 加入 PATH。不要只复制 adb.exe，它需要同目录 DLL。
3. 手机要求 Android 11 / API 30 或更高、arm64-v8a、有 Root。开启开发者选项和 USB 调试，连接数据线，在手机接受电脑 RSA 授权。在 KernelSU 中允许 shell 获取 Root。
4. 双击 **01-CHECK-ENV.cmd（检查环境）**。PASS=通过，WARN=可选组件缺失或设备未就绪，FAIL=需要处理。检查不会自动安装软件或修改游戏。
5. 打开目标游戏，双击 **02-START-DESKTOP.cmd（启动电脑界面）**。浏览器自动打开 http://127.0.0.1:18766 。
6. 网页里选择正确进程 → 探测偏移 → 导出 SDK → 导出动态库。等待任务完成，再下载文件。

**只需要 Dump / SDK 浏览：做到这里就行，不需要 IDA，不需要 MCP，不需要模型 API Key。**

想用手机悬浮界面，双击 03-START-PHONE.cmd。headless 包不提供悬浮界面，使用电脑网页即可。原生界面和手机 Web 服务互斥，切换会停止本工具的旧实例，不停止游戏。

## 二、需要安装哪些依赖？

| 功能 | 必需依赖 | 是否随包附带 |
|---|---|---|
| 电脑控制、SDK / lib 导出 | Windows、Python 3.10+、ADB、USB Root ARM64 Android 11+ | 本工具附带；Python / ADB 另装 |
| 本工具 Web / stdio MCP | Python 标准库 | 代码附带，无需 pip install mcp |
| IDA 反编译 | IDA Pro 9+、可用 ARM64 Hex-Rays 反编译器、IDALib Python、ida-pro-mcp | 不附带，需用户自行安装配置 |
| 自然语言 Agent | 支持 stdio MCP 的客户端及其模型配置 | 不附带，模型账号／费用由客户端管理 |
| 修改源码重新编译 | Android NDK、CMake、Ninja、Python（打包） | 普通用户不需要 |

完整分析环境建议统一用 Python 3.11 / 3.12 x64；本机已验证的 ida-pro-mcp 包要求 Python >=3.11。IDA Free 不能视为上述 IDALib / ARM64 反编译环境的等价替代。

## 三、官方下载入口（点链接，不要下载不明整合包）

- Python 官方：https://www.python.org/downloads/windows/
- Google ADB / Platform-Tools 官方：https://developer.android.com/tools/releases/platform-tools
- IDA Pro / Hex-Rays 厂商官网：https://hex-rays.com/ida-pro
- Hex-Rays 官方文档：https://docs.hex-rays.com/
- **IDA Pro MCP 作者维护仓库：https://github.com/mrexodia/ida-pro-mcp**
- IDA Pro MCP 安装说明：https://github.com/mrexodia/ida-pro-mcp#readme
- IDA Pro MCP 问题反馈：https://github.com/mrexodia/ida-pro-mcp/issues
- MCP 协议官网：https://modelcontextprotocol.io/
- Android NDK 官方：https://developer.android.com/ndk/downloads
- CMake 官方：https://cmake.org/download/
- Ninja 官方：https://github.com/ninja-build/ninja/releases

注意：ida-pro-mcp 是 mrexodia 维护的第三方开源项目，不是 Hex-Rays 官方产品。以上仓库地址由本地已安装包的 Project-URL 元数据核对；本次网络 DNS 不通，未声称在线核验了最新 README。后续安装选项以仓库当前说明为准。

## 四、可选：配置 IDA Pro MCP

### 1. 准备 IDA / IDALib

先安装 IDA Pro 9+ 并启动一次，完成许可证配置；确认具有 ARM64 反编译器。用与后续 MCP 相同的 Python 安装 IDA 安装目录提供的 idapro wheel，然后运行其 py-activate-idalib.py。这一步是注册 IDALib 安装路径，不是绕过或生成 IDA 许可证。

在 PowerShell 执行（弹出提示后粘贴自己的 IDA 安装目录）：

```powershell
$Ida = Read-Host '请输入 IDA Pro 安装目录，不要带引号'
$Wheels = @(Get-ChildItem -LiteralPath (Join-Path $Ida 'idalib/python') -Filter 'idapro*.whl')
if ($Wheels.Count -ne 1) { throw '未找到唯一 idapro wheel，请查看 IDA 自带 idalib/README.txt' }
python -m pip install $Wheels[0].FullName
python (Join-Path $Ida 'idalib/python/py-activate-idalib.py') -d $Ida
```

上述路径按本机 IDA 9.3 自带文件验证；若自己的版本目录不同，以安装目录 idalib/README.txt 为准。

### 2. 安装作者维护的 MCP 服务

```powershell
python -m pip install --upgrade https://github.com/mrexodia/ida-pro-mcp/archive/refs/heads/main.zip
```

该命令需要联网，不会随本工具启动自动执行。也可按作者 README 指定的版本／提交安装。

### 3. 在工作台自动连接 IDALib 服务

依赖安装到启动工作台的同一 Python 环境后，打开网页的 IDA 连接设置，使用默认 `http://127.0.0.1:13338/mcp`，点击 **保存并自动连接**。等待异步启动任务结束，无需另外开 PowerShell 窗口。已有 IDA GUI 插件可继续使用 13337。

工作台复用健康的本机 supervisor，或在后台启动已安装的服务；Windows 不弹额外终端。能自动找到 IDA 安装目录时会为子进程设置 `IDADIR`，随后在限定时间内检查健康状态。它不会自动安装 Python 包或配置许可证。缺少 IDA、IDALib、`ida-pro-mcp` 或许可证时，任务会显示具体错误、处理指引和日志位置。

日志与进程信息位于 `artifacts/ida-service/`，端点设置保存在 `artifacts/workbench-settings.json`，重启工作台后保留。网页 **检查 IDA 连接**、**05-CHECK-IDA.cmd** 和背景状态轮询只做检查，不会自动启动服务。

需要手动排查时，用启动工作台的同一 Python 环境运行以下后备命令：

```powershell
python -m ida_pro_mcp.idalib_supervisor --host 127.0.0.1 --port 13338
```

使用这个前台后备命令时保持窗口运行，Ctrl+C 会结束服务。这里显式指定工作台默认端口 13338，避免与可能使用 13337 的 GUI 插件冲突；上游命令自身的默认端口可能不同。不要在分析任务进行中关闭服务。

本工作台自动打开库依赖服务提供 idb_open / idb_list，并支持对指定 database 调用分析工具。仅启动旧版 IDA GUI 插件不一定具备这些能力。若提示模块或工具不存在，按作者 README 安装兼容的 supervisor / IDALib 版本；不是把端口改对就一定兼容。

启动完成后可通过网页“检查 IDA 连接”复查。工具可发现不等于许可证／反编译器一定有效，最终以打开实际库并得到反编译结果为准。

## 五、可选：让 Agent 通过 MCP 干活

1. 先运行 02-START-DESKTOP.cmd，保持 ADB 连接。
2. 双击 **04-GENERATE-MCP-CONFIG.cmd**，生成 bridge/mcp_config.json，写入当前电脑 Python 和解压目录的绝对路径。**换电脑、移动目录或更换 Python 后必须重新生成。**
3. 打开自己的 Agent 客户端 MCP 设置，导入此文件的 mcpServers 条目；如果客户端要求逐项填写，类型选 stdio，command 和 args 照文件填写。不要覆盖其他已有 MCP。
4. 启用后新建会话，要求实际调用 umt_status。正常会返回 service: umt-workbench；工具列表应出现 15 个 umt_* 工具。仅复制 JSON 到聊天框不等于完成配置。
5. 如要 IDA 分析，完成上一节，网页同步手机 Dump（不要勾“只同步符号”）→ 选择会话 → 搜索函数 → 在 IDA 打开库 → 分析。
6. 可以问 Agent：“查询我选择会话中的 ActorComponent$$execToggleActive，核对 RVA 和 IDA 代码，说明参数、作用与调用条件，并用 umt_save_analysis 保存报告。”

流程：手机 SDK/lib → 电脑不可变会话 → IDA 分析副本 → Agent 工具调用 → 报告。网页是任务和证据界面，不内置大模型，不会自行购买模型服务。Hook / 调用模板生成不代表已在手机执行，具体 ABI、对象与线程仍需验证。

## 六、端口、文件和常见问题

| 入口 | 地址 / 路径 |
|---|---|
| 电脑完整界面 | http://127.0.0.1:18766 |
| 手机服务／电脑 ADB 转发 | http://127.0.0.1:18765 |
| IDA MCP supervisor | http://127.0.0.1:13338/mcp |
| 手机结果 | /sdcard/UnrealMemoryTools_Refactored/包名/ |
| 电脑已同步 Dump | artifacts/sessions/ |
| 报告和分析任务 | artifacts/reports/ |
| 电脑日志 | artifacts/workbench.log |
| IDA 服务日志与进程信息 | artifacts/ida-service/ |
| 手机日志 | /data/local/tmp/umt-refactored/service.log |

- **工作台只有一行提示**：在电脑打开 18766，不要把 18765 当成 IDA 工作台。断线会每 4 秒重连；已有文件不会被清空。
- **没有 Python / ADB**：先安装前两项依赖；ADB 可放 tools/platform-tools。安装 Python 后重新开启动器。
- **unauthorized / offline**：手机解锁接受 USB 调试授权，检查数据线；不要只接充电线。
- **Root 失败**：在 KernelSU / Root 管理器允许 shell；确认 adb shell su -c id 返回 uid=0。
- **多台设备**：在包目录运行 python desktop/launcher.py --serial 设备序列号；序列号用 adb devices 查看。
- **IDA 未连接 / WinError 10061**：所选端点没有接受连接，通常是服务尚未监听。使用默认 13338 点击“保存并自动连接”，查看启动任务结果和 artifacts/ida-service/ 日志；不影响基本 Dump。
- **IDA 启动失败 / 超时**：按任务错误处理缺失依赖、IDALib 路径或许可证。超时后先用“检查 IDA 连接”确认是否稍后完成，不要把提交任务成功当成服务已就绪。
- **MCP 无工具**：确认 stdio command/args 是当前路径，客户端启用后新建会话。不存在适用于所有 Agent 客户端的一键全局注册。
- **无法打开库**：必须同时导出 SDK 和 lib，且同步时不要只同步符号；打开后等待 job 完成。
- **目标重启**：重新选择 PID 并探测，不复用旧地址。
- **已有端口被占用**：不要随便结束其他程序；运行 python desktop/launcher.py --port 18765 --workbench-port 18768，再运行 python bridge/generate_config.py --url http://127.0.0.1:18768。
- **运行闪退**：先运行 01-CHECK-ENV.cmd，保留完整报错、系统版本与日志。不要将其他私有 token 或模型 Key 混入发布反馈。

程序启动后服务继续后台运行；关闭浏览器不等于停止手机服务。退出手机悬浮界面可点其退出按钮。卸载前保存 artifacts 和手机导出，不要直接删正在被 IDA 打开的数据库。

## 七、给发包者

当前编译脚本默认编译后生成 user ZIP、SHA-256 与文件清单；-SourcePackage 额外生成源码包；-SkipPackage 只编译；-ReleaseDir 可指定输出目录。user 是发给普通用户的包，source 是给开发者的包。

本包不附带 Python、ADB、IDA 商业程序、IDA 许可证、模型 Key、你电脑上的 MCP 绝对路径、历史 Dump 或 IDA 数据库。新用户无需 NDK / CMake / Ninja。

原作者及依赖许可见 LICENSE、THIRD_PARTY.md 和 licenses/；发布时保留这些文件。

## 新增：对象实例与结构指针怎么查

1. 探测当前目标后进入 SDK 浏览器，输入对象实际地址，选择 Instance 实例，点击检查。
2. 查看 Class、父类、成员类型、所属类、偏移、实际地址、当前数值；函数列表包含继承函数。
3. FVector 直接显示 X / Y / Z 浮点数。点击“展开此结构”可进入成员结构，继续看 X/Y/Z 的相对偏移。对象指针可以点击继续跳转。
4. 对真正的非 UObject 结构指针：选择 Struct，填 UScriptStruct 类型定义地址；若地址栏填的是存放指针的槽地址，勾选“先解引用一次”。已经是结构数据地址就不要勾。
5. 勾选实时刷新，选择 0.5、1、2 或 5 秒；变化成员高亮。切换目标会停止，页面隐藏和探测/导出时暂停。裸结构没有 UObject 生命周期保证。
6. 用返回和标签导航。结构成员预览有深度/数量上限，可以逐级点击深入；未知原生布局要先提供类型，不能仅凭十六进制地址推断所有成员。

## 2.2.1：类型声明中的指针可直接进入

在 Class 的成员声明中，Pawn / APawn*、Character / ACharacter*、PlayerState / APlayerState* 等成员名和类型均可点击，自动进入对应类型定义，不需手填 UScriptStruct 地址。成员的“字段元数据”保留查看原描述符；实际实例中的非空指针值继续跳到指向的对象。类型声明没有运行时 this，不会把字段描述符地址误读成实例指针。数组 / Map 等泛型成员附带可解析的目标类型链接。

## 2.3：世界对象列表（ULevel.Actors）

先探测目标，进入“对象列表”页。工具自动发现 UWorld，优先展示有 GameInstance 的 World 候选；可以手动切换 World。左侧遍历 PersistentLevel 和 World.Levels 中所有已加载 Level 的 ULevel.Actors，主关卡 Actors 为空也会继续遍历子关卡。

列表显示 Actor 名称、Class、地址、来源 Level 和 Actor 槽位，支持名称/Class 搜索、Level 筛选和 50/100/250 分页。勾选“实时遍历”按 1/2/5 秒重新采样当前页和 Level/数组信息；翻页重新读取目标内存，搜索可跨多个有界批次。

点 Actor 后右侧显示与 SDK 浏览器相同的实例检查：Class/父类、成员类型、偏移、地址、数值、函数、结构与指针跳转、FVector 浮点数、返回和标签。右侧“实时刷新”独立控制选中实例的值。手机浏览器也可使用该页。

对象列表是目标当前加载的场景 Actors，不包含未加载 Level、服务器端不可见对象，也不把所有 UObject 当成 Actor。World 发现会扫描全局对象表一次；后续 Actor 列表仅来自 Level.Actors。多个 World 的选择会明确显示地址。数组头变化触发重扫；页内并发变化仍属于尽力读取，不是冻结游戏的原子快照。

MCP 仍为 15 个工具：umt_explorer(action=worlds) 使用 cursor 发现 World；umt_explorer(action=actors, worldAddress=...) 分页遍历，支持 levelAddress、query、generation、expectedTopology。每次响应提供 totalSlots、nextCursor、complete、levels、warnings；不要把第一页当成全部结果。

## IDA 已连接但缺少 idb_open

“已连接”仅表示 MCP 通信正常。IDA GUI 插件可提供反编译/汇编，却通常不提供打开新数据库的 idb_open。工作台现明确区分“支持打开库”和“仅当前数据库”。

如果 GUI 插件占用 13337，保留它，把工作台的 IDA MCP URL 填为 `http://127.0.0.1:13338/mcp`，点击 **保存并自动连接**。工作台会保存地址，并异步复用或启动 supervisor；无需关闭 GUI 的当前数据库，也无需手动打开额外终端。

旧版“保存并检查”只修改地址并探测连接，端口上没有服务时仍会返回 `WinError 10061`。新版按钮会请求启动并检查健康状态；独立的“检查 IDA 连接”仍是被动检查。服务与依赖报错按第四节处理。

“在 IDA 打开库”创建的是 supervisor 管理的独立分析数据库，不自动切换已打开的 IDA 图形窗口。现在会在复制大库之前检查端点能力，GUI-only 端点不会反复生成无用分析副本。

## 手机原生 GUI

03-START-PHONE.cmd 启动的是真正的 Vulkan / ImGui 原生界面。探测目标后，进入“SDK 对象浏览”或“世界对象列表”。两页共用实例检查器：成员/类型链接、结构体值、FVector 浮点、继承函数、容器、运行时、返回和标签均可使用。World 支持发现、Level 选择、Actors 分页/搜索和实时列表；右侧可独立开启实时值。

地址、结构类型、对象搜索、成员过滤均有虚拟键盘。键盘弹窗 ID 作用域已修正，按键可输入、退格、清空和完成。读取在后台执行，不在每个绘制帧重复扫描。

原生与手机 HTTP 模式互斥。原生运行时电脑 bridge 的设备 API 会离线；需要 Web/MCP 设备操作时运行 02-START-DESKTOP.cmd 切回。切换只停止本工具实例，不停止游戏、不删除 Dump。

CLI 可以使用 --worlds --pid 当前PID：探测后直接进入原生世界对象页；--explorer --pid 当前PID 直接进入 SDK 页。
