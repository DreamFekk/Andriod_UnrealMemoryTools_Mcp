# MCP 与 Agent 使用指南

这套工程把 Android 实时内存、不可变 Dump 会话和本机 IDA 分析串成一个工具链。浏览器负责选择目标、查看 SDK、管理任务和展示证据；模型能力来自接入 MCP 的 Agent，网页本身没有内置 LLM，也不会把未执行的分析伪装成 AI 回答。

## 1. 先启动电脑工作台

设备连接 ADB，在 KernelSU 中允许 shell 获得 root，然后在工程根目录运行：

```powershell
python desktop/launcher.py
# 已经部署时复用手机服务：
python desktop/launcher.py --no-deploy
# 自定义两个不同端口：
python desktop/launcher.py --port 18765 --workbench-port 18768 --workspace ./artifacts
```

| 入口 | 默认地址 | 用途 |
|---|---|---|
| 手机服务 | 手机本机 `http://127.0.0.1:18765` | 探测、SDK/lib 导出、实时对象浏览器 |
| ADB 转发 | 电脑 `http://127.0.0.1:18765` | 转发上述手机服务，不提供电脑 IDA 功能 |
| 电脑工作台 | 电脑 `http://127.0.0.1:18766` | 代理手机接口，并提供 Dump 会话、IDA、分析报告 |
| IDA MCP supervisor | 电脑 `http://127.0.0.1:13338/mcp` | 本机 IDA 服务；GUI 插件可继续使用 13337 |
| UMT MCP | `python bridge/mcp_server.py --url …` | stdio JSON Lines 工具服务器，由 MCP 客户端启动 |

电脑工作台和手机服务只监听 loopback。不同设备上的 `127.0.0.1` 不是同一个地址：手机浏览器不通过电脑 bridge，所以手机上可以浏览 SDK，但不显示可操作的电脑 IDA 工作台。

`desktop/run-native.cmd` 启动手机原生悬浮界面，不启动电脑 bridge。它与手机 HTTP 服务互斥；需要 MCP/电脑工作流时使用 `desktop/start.cmd`。

## 2. 生成并导入 MCP 配置

```powershell
python bridge/generate_config.py
```

生成 `bridge/mcp_config.json`，其中 `command` 是当前 Python 的绝对路径，`args` 使用工程中 `bridge/mcp_server.py` 的绝对路径。工程搬家、更换 Python 或改变工作台端口后重新生成：

```powershell
python bridge/generate_config.py --url http://127.0.0.1:18768
# 只输出 JSON，不写文件：
python bridge/generate_config.py --print
# 写入一个自行指定的导入文件：
python bridge/generate_config.py --output ./artifacts/mcp-for-this-machine.json
```

生成器只写指定 JSON 文件，不修改 AionUi、Codex 或其他应用的设置，不安装服务，也不会自动注册 MCP。不要把 `--output` 指向已经包含其他配置的应用设置文件；生成内容是完整导入片段，不是配置合并器。

配置形状如下，实际路径由生成器填入：

```json
{
  "mcpServers": {
    "unreal-memory-tools": {
      "command": "<当前 Python 绝对路径>",
      "args": [
        "<工程绝对路径>/bridge/mcp_server.py",
        "--url",
        "http://127.0.0.1:18766"
      ]
    }
  }
}
```

### 在 MCP 客户端中启用

以 AionUi 为例，其他支持 stdio MCP 的客户端使用相同的 command/args：

1. 在 AionUi 的 MCP 设置中导入 `bridge/mcp_config.json`，或按客户端字段填写 stdio command/args。若已有其他 MCP，合并 `mcpServers` 条目，不覆盖原有配置。
2. 启用此 MCP 供所选 Agent 使用；保存后新建一个会话。部分客户端不会向已开始的会话动态注入工具。
3. 在新会话要求调用 `umt_status`，确认返回 `service: umt-workbench`、当前 workspace 和设备入口。
4. 确认 `tools/list` 出现下列 15 个工具，而不是只看到配置文字。

工作台「连接 MCP 与 IDA」中的复制按钮提供与当前端口匹配的实际配置。

## 3. 15 个工具及用途

| 工具 | 用途 |
|---|---|
| `umt_status` | 电脑会话、任务、IDA 状态和 MCP 配置 |
| `umt_processes` | 列出设备上的 Unreal 进程 |
| `umt_device_state` | 当前目标、探测结果、进度与错误 |
| `umt_device_action` | 顺序启动 `probe`、`dump` 或 `library` |
| `umt_explorer` | 有界对象分页/搜索，或 UObject/FField 地址检查 |
| `umt_sync_dump` | 从手机流式同步 SDK/库，返回异步 job ID |
| `umt_import_dump` | 导入 workspace 内已有目录，创建独立会话 |
| `umt_search_functions` | 搜索导出函数，返回 RVA 与准确匹配的 SDK 声明 |
| `umt_get_function` | 精确名称查询，报告找不到或重名歧义 |
| `umt_ida_status` | IDA 工具、数据库及可选工具完整 schema |
| `umt_open_in_ida` | 准备独立分析 ELF 并打开对应 IDA 数据库 |
| `umt_analyze_function` | 按会话和 RVA 分析指定函数，保留真实工具证据 |
| `umt_ida_tool` | 在明确选择的数据库内调用允许的 IDA 工具 |
| `umt_job` | 查询异步任务结果或失败原因 |
| `umt_save_analysis` | 保存 Agent 的 Markdown 分析、实现与验证步骤 |

同步、导入、打开 IDA 和分析函数等耗时操作返回 `jobId`。继续调用 `umt_job`，直到 `status` 为 `done` 或 `error`；`queued` / `running` 都不是完成。手机探测和导出使用 `umt_device_state` 轮询 `busy` 与 `error`。

### 获取 IDA 参数 schema

不要凭印象构造 IDA 调用。先查看当前服务实际声明：

```text
umt_ida_status({"tool":"decompile"})
```

结果中的 `toolSchema` 包含该工具完整 `inputSchema`。不传 `tool` 则读取状态和工具名清单。调用 `umt_ida_tool` 时按 schema 提供 `arguments`；不传 `database`，bridge 根据明确的 Dump 会话选择对应 IDA 数据库并注入数据库参数。

## 4. 自然语言到可验证分析

可以把以下目标直接交给已连上 MCP 的 Agent：

> 分析当前 Unreal 目标里与组件启停有关的函数。先读取设备状态和可用 Dump 会话，选择一个明确会话；从 SDK 搜索函数并确认精确名称与 RVA，在对应 IDA 分析镜像中取反编译和引用，说明参数、调用链及反射入口，最后保存含证据和验证方法的分析报告。

推荐调用顺序：

```text
umt_status → umt_processes → umt_device_state
  → 必要时 probe → 等待 idle → dump → 等待 idle → library → 等待 idle
  → umt_sync_dump(symbolsOnly=false) → umt_job
  → 记录完成结果里的明确 artifact session ID
  → umt_search_functions(query, session)
  → umt_get_function(exact name, session)
  → umt_open_in_ida(session) → umt_job
  → umt_analyze_function(exact name, rva, session) → umt_job
  → 必要时 umt_ida_status(tool) → umt_ida_tool(name, arguments, session)
  → Agent 整理实现/证据/验证 → umt_save_analysis(markdown)
```

后续调用都携带同一个明确 `session`。不要混用“最新会话”、另一游戏版本的 SDK、旧 runtime 地址或另一个已打开的 IDA 数据库。存在重名时先处理歧义，不默认选择第一项。

`script.json` 的 `Address` 是 **RVA**：

```text
Android runtime address = 当前目标模块基址 + RVA
IDA EA                  = 对应 IDA 数据库 imagebase + RVA
```

地址在 Web/MCP 层使用十六进制字符串，避免 JavaScript 浮点精度丢失。SDK 反射方法声明也不等于 `$$exec` thunk 的机器 ABI；参数布局、this 对象、对象生命周期和线程要求应以反编译/运行时证据确认。

`umt_analyze_function` 默认可在该会话的**独立分析副本**中定义并命名选中函数；传 `defineFunction:false` 可以跳过这两个准备动作。原始 Dump 和其他已有 IDB 不会因此被覆盖。生成的原始参数 trace 脚本属于观察模板，不表示已恢复所有参数类型、已注入设备或已执行目标调用。

## 5. 启动 IDA 与版本兼容

先在启动工作台的 Python 环境中安装 IDA / IDALib 和兼容的 `ida-pro-mcp`。在工作台填写默认地址 `http://127.0.0.1:13338/mcp`，点击 **保存并自动连接**，等待异步启动任务完成。

该按钮提交 `start_ida`：复用健康的本机 supervisor，或用当前 Python 在后台启动已安装服务；Windows 不弹额外终端。找到 IDA 安装目录时会给子进程设置 `IDADIR`，随后在限定时间内检查健康状态。已有 GUI 插件可保留在 13337，无需关闭其数据库。

依赖和许可证需要预先安装；工作台不会自动执行 pip。缺少 Python 包、IDA、IDALib 或许可证时，会显示具体错误、处理指引和日志位置。日志与进程信息位于 `artifacts/ida-service/`，地址保存在 `artifacts/workbench-settings.json`。单独点击 **检查 IDA 连接**、调用 `umt_ida_status` 或后台轮询都只检查状态，不启动服务。

手动排查时，在启动工作台的同一 Python 环境中运行：

```powershell
python -m ida_pro_mcp.idalib_supervisor --host 127.0.0.1 --port 13338
```

这个前台命令是手动后备方式，使用期间保持运行。工程不附带 IDA；已有分析任务运行时不要反复重启服务。

工程的 `IDAClient.open_database()` 使用 `idb_open` / `idb_list`，保存服务器返回的精确 database ID，并在每次分析前校验输入路径和 imagebase。以服务器实际公布的 tools/list 为准；仅有 GUI 插件、旧版 `idalib_open` 或不同工具 schema 时，先检查端点能力和版本。也可以按 [IDA workflow](ida-workflow.md) 手动打开分析副本。

运行时内存镜像不等于磁盘原始 ELF。工作台依据 PT_LOAD 映射生成新的分析副本，处理文件偏移/虚拟地址差异，并保留 `mapping.json`；运行时重定位和无法恢复的区段会影响分析，结果并非可直接运行的原始库。转换说明见 [IDA workflow](ida-workflow.md)。

## 6. 结果位置与常见故障

默认 workspace 为工程的 `artifacts/`：

- `sessions/<生成的会话 ID>/`：完整 Dump 文件和 SHA-256 manifest。
- `sessions/<会话 ID>/analysis/<唯一目录>/`：映射后的分析 ELF、命名脚本与映射信息。
- `reports/`：异步任务 JSON、Agent Markdown 和函数 trace 产物。
- `workbench.log`：电脑 bridge 日志。
- `ida-service/`：自动启动 supervisor 的日志与进程信息。

| 现象 | 检查方式 |
|---|---|
| MCP 配置存在但 Agent 没工具 | 检查 stdio command/args，启用后新建会话，再实际调用 `umt_status` |
| 工作台端口拒绝连接 | 先运行 `desktop/start.cmd`；配置 URL 必须是电脑工作台端口，而非手机转发端口 |
| IDA 未连接 / `WinError 10061` | 所选端点没有接受连接，通常是服务未监听；使用默认 `13338/mcp` 点击“保存并自动连接”，查看启动任务及 `ida-service/` 日志 |
| IDA 启动失败 / 超时 | 按任务错误安装缺失依赖或配置 IDALib / 许可证；超时后可先“检查 IDA 连接”，不要把提交成功当服务已就绪 |
| 无法打开 IDA 库 | 该会话必须包含 `libUE4.so` 或 `libUnreal.so`，不能只有 symbols-only 导出 |
| `Open this dump session in IDA first` | 对同一个明确 artifact session 执行 open，并等待 job 完成 |
| 数据库路径不匹配 | 不要复用其他输入的 database ID；重新为当前会话创建分析副本 |
| 函数重名或 SDK 片段为空 | 读 `ambiguous` / `sdkSnippetMatch`，确认完整 owner 和函数名；不猜一个签名 |
| 同步时设备会话变化 | 等手机探测/导出结束再同步，避免其他客户端并发更换目标 |
| Web 提示任务排队/运行 | 用任务历史或 `umt_job` 读结果，不把提交成功当分析成功 |

完整单元测试可在不连接手机、不启动真实 IDA 的情况下运行：

```powershell
python -m unittest discover -s tests -p "test_*.py" -v
```

桌面浏览器测试需要本机 Playwright 与 Edge；开启 `$env:UMT_BROWSER_TEST='1'`。设备相关测试需要正在运行的手机服务，具体命令见 [贡献指南](../CONTRIBUTING.md)。模拟服务测试通过不等于真实设备或 IDA 集成已通过。

## IDA 已连接但缺少 idb_open

“已连接”仅表示 MCP 通信正常。IDA GUI 插件可提供反编译/汇编，却通常不提供打开新数据库的 idb_open。工作台现明确区分“支持打开库”和“仅当前数据库”。

如果 GUI 插件占用 13337，保留它，把工作台的 IDA MCP URL 填为 `http://127.0.0.1:13338/mcp`，点击 **保存并自动连接**。地址持久化，启动任务会复用或拉起 supervisor，无需手动打开额外终端。

旧版“保存并检查”只保存地址并探测连接，单纯换端口没有监听服务时仍会出现 `WinError 10061`。新版按钮增加了启动操作；单独的“检查 IDA 连接”仍保持只读检查。

“在 IDA 打开库”创建的是 supervisor 管理的独立分析数据库，不自动切换已打开的 IDA 图形窗口。现在会在复制大库之前检查端点能力，GUI-only 端点不会反复生成无用分析副本。
