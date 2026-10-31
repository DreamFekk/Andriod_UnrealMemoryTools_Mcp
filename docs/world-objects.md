# 2.3 世界对象列表

## 怎么用

电脑和手机 Web 进入“对象列表”。先探测目标，再扫描 UWorld。多个世界时可手动选择，工具按是否具有 GameInstance 和已加载 Level 数量排序候选，界面显示名称与地址。

Actor 数据来自 PersistentLevel 与 World.Levels 中的 ULevel.Actors。只在发现 World 时扫描全局 UObject 表；场景列表不是全局 UObject 搜索结果。选择 Level 可只看一个关卡；“全部已加载 Level”汇总去重后的 Level 地址。

列表包含 Actor 名称、Class、对象地址、来源 Level 与槽位，支持 50/100/250 分页和名称/Class 搜索。点 Actor 后右侧复用 SDK 检查器：类型/父类、成员偏移/地址/值、函数、结构/指针跳转、返回、标签和 FVector 浮点数。

## 实时行为

左侧“实时遍历”每 1/2/5 秒重新读取当前页及 Level/Actors 信息；右侧“实时刷新”按 0.5/1/2/5 秒采样选中对象数值并高亮变化。页面隐藏、离开对应页和设备任务忙时暂停，新请求等待前一个结束。

目标或 generation 改变会清除旧 World 和页面。Level/Actors 基址、数量、容量参与 topology 校验；数组变化时从头重扫。返回前绕过缓存再读 Actor 数组头，发现途中变化返回 changed。World/Actor 失效会报错。多客户端只读锁竞争有界重试，不自动重试设备变更操作。

实时读取不冻结游戏，数组头一致也不能保证全部元素来自同一帧。总槽位数与有效 Actor 数分开显示；空、失效及非 Actor 槽位跳过。

## 验证列表来源

PersistentLevel 的 Actors 可能为空而其他 Level 非空，因此验证时应检查全部已加载 Level。场景切换后重新发现 World，不复用旧地址。

1. 遍历全部页，按 Actor 地址检查去重，并保留来源 Level 和 slotAddress。
2. 从开头、中间、末尾抽样，将 slotAddress 解引用后与返回 Actor 地址对照。
3. 打开一个 Actor，核对成员、函数及来源 Level；再测试 Level 筛选和跨页搜索。
4. 切换目标或场景后，确认过期 generation / topology 会报错或触发重扫。
5. 分别检查 Web 和 MCP；若启用原生模式，按 [原生 GUI](native-gui.md) 验证其独立入口。

`tests/test_desktop.py` 中的可控浏览器数据用于验证分页、筛选、数组变化与界面状态。真实进程的对象数量、地址和耗时随版本与场景变化，应以自己的测试记录为准。

界面示例：[电脑列表](screenshots/world-actors-valorant-desktop.png) · [手机列表](screenshots/world-actors-valorant-mobile.png)。截图展示界面，不保证当前版本返回相同对象或地址。

## MCP

工具仍为 umt_explorer，总数 15 个。

1. action=worlds，cursor=0、limit=64、scanLimit=8192；用 nextCursor 继续直到 complete。
2. action=actors，明确 worldAddress，cursor=0、limit=100、scanLimit=4096。后续带相同 generation、expectedTopology，继续 nextCursor。
3. 可加 levelAddress 筛关卡、query 搜 Actor。changed=true 则重扫，不能拼接旧页。
4. action=inspect、kind=instance、address=选中 Actor 地址，读取实际结构和值。

所有地址是十六进制字符串，避免浮点精度丢失。

## 范围

只遍历客户端当前已加载 Level 的 Actors，不含未加载关卡、服务器对象和全部 UObject。不可读关卡通过 warnings 明示。完整列表可在电脑/手机 Web、MCP 和原生界面的“世界对象列表”标签中使用。实例可能随场景切换消失，不能将测试地址当永久偏移。
