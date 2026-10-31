# 原生 GUI 2.4 实现与验证

## 改动

原生 SDKExplorer 改为消费 App::Explore JSON 数据，而不是保留另一套不完整的内存解释逻辑。后台 std::future 执行有界读取，绘制帧只展示完成结果；generation 改变后丢弃旧任务结果。退出时等待读取任务完成，不留后台任务引用已销毁状态。

手机新增独立“世界对象列表”标签：World 发现、GameInstance 候选、Level 选择、Actor 分页/搜索、实时列表。SDK 页保留全局对象分页/搜索。右侧共享实例/类型/结构检查、成员类型跳转、实际指针、FVector、字段元数据、函数、容器、运行时、返回、标签和实时值高亮。

所有文本入口提供虚拟键盘。原故障是 OpenPopup 在 child + PushID 中调用，而 BeginPopupModal 在主窗口调用，导致 ID 不匹配；现在按钮仅排队请求，弹窗在同一主窗口作用域打开、绘制，并按主窗口居中。

## 使用与验证

通过 `desktop/run-native.cmd` 启动手机原生界面。原生模式不启动 HTTP 服务，电脑 Web 的设备连接此时可能显示断开；已有 Dump 会话和 IDA 数据库不受影响。

验证时依次执行 World 发现、Level 选择、Actor 分页、实例检查、进入结构和返回。搜索框应能调出虚拟键盘，并正确处理输入、退格、清空和关闭。切换目标或 generation 后，旧的后台结果不应重新填回页面。

`tests/native_inspector_ui_test.cpp` 使用真实 ImGui Context 和可控数据，覆盖 World/子 Level、实例导航、实时值变化、generation 失效以及键盘 ID 作用域和目标缓冲区绑定。它验证界面逻辑，不能替代真机上的窗口、触摸、字体和渲染检查。构建与测试入口见 [贡献指南](../CONTRIBUTING.md)。

真机截图可参照 [世界对象检查](screenshots/native-world-inspector-2.4.png) 和 [虚拟键盘](screenshots/native-keyboard-2.4.png)。对象数量与显示值取决于当前游戏版本和场景。
