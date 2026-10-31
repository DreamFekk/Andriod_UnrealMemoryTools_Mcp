# 手机与电脑共用界面

构建 Android 可执行文件后，双击项目中的 `desktop/start.cmd`。启动器找到 ADB 设备、检查 root、部署服务、建立 ADB 转发并打开浏览器。首次使用时在 KernelSU 等 Root 管理器中为 ADB shell 授予 root。基础 Dump 只依赖 Python 3.10+ 和 Android platform-tools，无 npm、Electron 或第三方 Python 依赖；可选 IDA 分析需另行安装 IDA / IDALib 和兼容的 `ida-pro-mcp`。以下命令均从项目根目录执行。

```powershell
python desktop/launcher.py
python desktop/launcher.py --serial YOUR_DEVICE --port 18765 --workbench-port 18768
python desktop/launcher.py --no-deploy --no-open
python desktop/launcher.py --adb ./tools/platform-tools/adb.exe
python desktop/launcher.py --su /data/adb/ksu/bin/su
python desktop/launcher.py --native
```

- 电脑访问 `http://127.0.0.1:18766`，手机接口经 ADB 转发到电脑 `18765`。`--port` 修改转发端口；`--workbench-port` 修改电脑工作台端口。
- 手机浏览器访问 `http://127.0.0.1:18765`，直接使用同一服务和任务状态。
- 选择目标 → 探测偏移 → 导出 SDK / libUE4 → 在“导出文件”下载。
- 结构页支持字段检索，日志页支持过滤。语言和主题保存在当前浏览器。
- 默认二进制路径是 `outputs/arm64-v8a/UnrealMemoryTools`，可用 `--binary` 指定。
- 手机部署目录是 `/data/local/tmp/umt-refactored`，结果目录是 `/sdcard/UnrealMemoryTools_Refactored`。
- 服务日志在部署目录的 `service.log`。启动器退出后服务保留运行；电脑访问期间保持 ADB 连接。
- 启动器只通过 pidfile 与 `/proc/PID/exe` 的完整路径校验停止自己的服务，不使用 `killall`。

页面不加载外部字体、CDN 或 JavaScript；所有文件操作通过同源接口完成。手机服务仅监听设备 loopback，电脑通过 ADB 转发访问。

电脑启动器会在后台启动 `bridge/workbench.py`，提供 Dump 会话同步、SDK 函数名搜索、IDA MCP 连接状态、打开分析镜像与函数分析。默认把库文件一起同步，以便 IDA 分析；勾选“只同步符号”可节省传输，但该会话没有库文件时不能打开 IDA 镜像。

IDA 依赖安装到启动工作台的同一 Python 环境后，在页面填写 `http://127.0.0.1:13338/mcp` 并点击 **保存并自动连接**。工作台异步复用健康 supervisor，或在后台启动已安装的服务；Windows 不弹额外终端。已有 IDA GUI 插件可保留在 13337。自动发现 IDA 安装目录成功时，子进程获得对应 `IDADIR`。

启动会在限定时间内检查服务健康；缺少依赖或许可证时显示处理指引和日志位置，不会自动安装软件。日志与进程信息在 workspace 的 `ida-service/`，地址保存在 `workbench-settings.json`。**检查 IDA 连接** 与状态轮询只查询，不启动服务。`WinError 10061` 通常表示所选端点尚无监听服务；旧版仅保存地址不会启动，现在应使用 **保存并自动连接**。

手动排查可使用同一 Python 环境运行 `python -m ida_pro_mcp.idalib_supervisor --host 127.0.0.1 --port 13338`，完整说明见 [IDA 工作流](../docs/ida-workflow.md)。

分析工作台显示反编译、引用与原始工具证据。填写 Agent 目标后可以生成并复制带会话、函数 RVA、SDK 片段和 IDA 证据的任务。模型回答由连接 MCP 的 Agent 执行；页面不会模拟未配置的模型回答。运行 `python bridge/generate_config.py` 后，本机 MCP 连接配置生成到 `bridge/mcp_config.json`；移动项目或更换 Python 后重新生成。

```powershell
python desktop/launcher.py --workbench-port 18766 --workspace ./artifacts
```

工作台日志位于 workspace 的 `workbench.log`。`artifacts/` 中的会话、设置、报告与日志，以及 `bridge/mcp_config.json` 都是本地生成文件，不纳入 Git。启动器复用匹配同一设备转发与 workspace 的 bridge；只有 pidfile、进程创建时间、Python 可执行路径与服务 PID 全部匹配时才停止先前实例，其他端口占用会明确报错。`--native` 不启动电脑 bridge。

`desktop/run-native.cmd` 或 `--native` 启动独立手机原生悬浮界面，需要构建时启用 `UMT_NATIVE_UI`。切换模式时先停止本部署目录的旧实例，因此原生界面和 HTTP 服务不会同时操作各自的内存会话。原生模式不建立新的 ADB 转发、不打开电脑浏览器；重新运行 `desktop/start.cmd` 即切回共用 Web 界面。

接口集成检查（只读或发送预期被拒绝的请求，不触发探测/导出）：

```powershell
python tests/http_integration.py --base http://127.0.0.1:18765
```
