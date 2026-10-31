# UE5、PUBG 与 NEW STATE 支持说明（2.5.1）

## 参考版本

MobileDumper-7：<https://github.com/MJx0/MobileDumper-7>，提交
`595655004a8e1d56fd0e708fe64649f1864d7066`。下面的实现说明对应此参考版本，不表示自动跟随上游最新改动。

主要参考：

- `Dumper/Profile/CustomProfiles/Shared/PUBG.h`
- `Dumper/Profile/CustomProfiles/Shared/NewState.h`（MJx0 / droidxrx）
- `Dumper/Engine/Unreal/UnrealTypes.cpp`、`UnrealObjects.cpp`
- `Dumper/Engine/OffsetFinder/Offsets.cpp`

相关 MIT 许可保存在 `src/UE/UEGameProfiles/LICENSE.MobileDumper-7`，发布脚本也会收集进 licenses。

## UE5 公共链路

| 改动 | 实际行为 |
|---|---|
| 版本识别 | 有界解析 UE4 / UE5 Release / Main 分支字符串，优先模块只读数据。库名只作为无法读到版本时的提示，不再把所有 libUnreal.so 认定为精确 5.3 |
| 布局隔离 | 默认偏移工厂每次根据传入配置生成，取消第一次调用后固定配置的 static 缓存；移除对含 std::function 的对象执行 memset |
| FName | 支持 ANSI / UTF-16、代理对、大小写保留 DisplayIndex、池内编号名称；编号引用允许指向 ID 0，检测循环与读取边界 |
| 名称池发现 | 校验连续 None / ByteProperty，按实际头部与步长计算第二条记录；使用 Profile 的名称解码回调验证加密名称，不再写死 block + 8 |
| 编号名称配置 | 不从 UE 版本直接推出 outline-number；依据 UObject 布局和实际编号条目验证 |
| 扫描效率 | 名称和对象表发现使用 256 KiB 分块读取模块段；Legacy 字符串分批读取，避免逐字符系统调用 |
| UObject | Class、Outer、Index 的读取提供实例级回调；SDK、浏览器、偏移修复与函数解析共用这些回调 |
| FField | 支持 UE5 owner 低位标记与运行时布局校验 |
| 对齐 | 支持 UE5.6 的 16 位 UStruct MinAlignment，不把相邻字段读进对齐值 |
| 属性类型 | 识别 ObjectPtrProperty / ClassPtrProperty、OptionalProperty、4 / 8 字节 LWC RealProperty；Optional 递归有深度与自引用检查 |
| 枚举 | 共用读取器处理传统 TArray<TPair<FName,int64>> 及 UE5.6 动态 tagged FName / value 分离数组；SDK 和浏览器均保留负数、别名与稀疏取值 |

枚举原实现会把非递增枚举强行改成 0、1、2；现已删除这一行为。读取不到有效枚举条目时返回空结果，不伪造顺序值。

数据流程：

```mermaid
flowchart LR
    A[包名匹配 Profile] --> B[版本提示与初始布局]
    B --> C[验证名称池或名称表]
    C --> D[分块发现并验证对象表]
    D --> E[共享名称与对象解码回调]
    E --> F[运行时偏移修复]
    F --> G[SDK 与 script.json]
    F --> H[Web / 原生实例检查]
    G --> I[MCP / IDA 分析会话]
```

## PUBG Mobile

包名保持：`com.tencent.ig`、`com.rekoo.pubgm`、`com.pubg.imobile`、`com.pubg.krmobile`、`com.vng.pubgmobile`。

优先扫描模块可读、可写且不可执行的段，定位 global → manager → manager + 0x110 → name array。通过 None 和另一个内建属性名验证，并处理名称表外层包装与条目头部差异。

旧描述符路径作为后备保留，但链长度限于 1～16；检查溢出、循环、无效和不可读地址。删除原代码直接用未经校验的 `(Code - 100) / 3` 下标写入栈数组的做法。包名不再触发公共 GetNameEntry 内的重复解引用。

当前交付为 ARM64；代码保留上游 32 位 manager offset 描述，但不代表本包新增 ARMv7 支持。

## NEW STATE Mobile

新增 `NewStateProfile`，注册包名 `com.pubg.newstate`。

- UObject Index：3 个 uint32 的异或与旋转解码。
- UObject Class / Outer：3 个 uint64 的异或与旋转解码。
- 名称：ANSI 字节变换，以及保留 UTF-16 高字节的对应变换。
- 回调属于该 Profile 生命周期，换游戏不会继续使用上一目标的解码。
- 要求核心布局验证通过；失败返回明确错误，不用普通 AutoFix 悄悄绕过专有解码。

密钥对应上面列出的上游提交。实际游戏版本更换密钥、布局或访问方式后仍可能需要更新 Profile。NEW STATE 的支持来自其专用配置，不把该游戏简单归类为“UE5 游戏”。

## 验证范围

引擎兼容回归覆盖布局工厂、名称编码、PUBG 名称链、NEW STATE 读取回调和枚举。可移植读取器测试覆盖失败读取、越界、循环和加密名称固定答案；对象表测试覆盖 flat/chunked 布局、空洞、误判、item stride 与 Profile 解码回调。

这些测试使用可控数据。真实目标还需执行探测、SDK 导出、对象浏览、实例检查，并核对输出中的对象索引、名称和函数 RVA。构建成功或固定答案通过不表示所有 PUBG、NEW STATE 或 UE5 构建均已通过实机验证。

## 当前边界

- 已针对 UE5 目标 `com.hottagames.yh.laohu` 的对象表误判修复发现与验证逻辑；历史设备输出未随源码发布。PUBG 国际版和 NEW STATE 的对应实际版本仍需完成整体验证。
- UE5.6 的静态 UTF-8 枚举容器、任意自定义 packed object array / 对象句柄、UE5.7+ 特殊反射变体未作全覆盖保证。
- 名称池 BlockBits、块表偏移及 UObject item stride 仍需匹配目标布局；常见配置与已知 Profile 已支持，不能由库名推断所有自定义变体。
- Optional 当前补的是反射类型导出与类型信息；不声称任意 Optional 的运行时值表示都能完整展开。
- 新版执行器与旧版的 Web / 原生 / MCP 使用方式、输出文件名保持兼容。新 State 解码失败时会停止，防止导出看似成功的错误 SDK。

## 复现命令

```powershell
# ARM64 主程序与引擎回归执行器
# 先按 README 配置 Android NDK 的 build 目录
cmake --build build --target UnrealMemoryTools umt_engine_compat_test --parallel 8
adb push outputs/arm64-v8a/umt_engine_compat_test /data/local/tmp/
adb shell chmod 755 /data/local/tmp/umt_engine_compat_test
adb shell /data/local/tmp/umt_engine_compat_test

# 无设备也可以验证边界与解码算法（C++20）
New-Item -ItemType Directory -Force artifacts | Out-Null
g++ -std=c++20 -O2 -Isrc tests/runtime_codec_test.cpp -o artifacts/runtime_codec_test.exe
./artifacts/runtime_codec_test.exe

# 编译并生成完整用户 / 源码包
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 -SourcePackage
```

原生与 Headless 测试需要不同输出目录时，可通过 CMake 的 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 显式指定，避免两种构建互相覆盖。


## 2.5.1：UE5 空 SDK 与 None 名称修复

用户提供的 UE5 目标 `com.hottagames.yh.laohu` 实测暴露了原对象表识别的误判：某 UObject 预分配内存区也包含合法名称和对象指针，仅凭 `/Script/CoreUObject` 名称锚点会把它当成 flat GUObjectArray。随后 InternalIndex 与数组槽位不匹配，GatherUObjects 提前返回，报 ERROR_EMPTY_PACKAGES。

现在通过多个活对象的 InternalIndex / 槽位关系验证；同时探测 item stride / Object 偏移、NumElements / capacity、flat / chunked 形式，并验证远端槽位与 65536 分块边界。识别出的真实目标表为 chunked、item stride 0x18、NumElements 偏移 0x14。没有把目标绝对地址写进 Profile。

导出前也改成多槽位检查，允许 slot 1 为 GC 空洞。空表与索引不一致分别返回 ERROR_OBJECT_ARRAY_EMPTY / ERROR_OBJECT_ARRAY_INDEX_MISMATCH，避免再全部表现为 ERROR_EMPTY_PACKAGES。

验证导出质量时，应检查 SDK_Classes.hpp 和 SDK_Structs.hpp 中异常的 None / FNone / UNone 占位词，并回到原始名称解析定位原因；SDK_Enums.hpp 中合法的 None 枚举成员应保留，不能通过字符串替换掩盖解析错误。

`tests/object_array_layout_test.cpp` 覆盖误判、空洞、远端索引、item 布局、chunk 头、超大数量与 Profile 解码回调。该回归使用构造数据，设备集成需用对应游戏版本另行验证。
