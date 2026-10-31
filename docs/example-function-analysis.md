# 函数分析示例：ActorComponent$$execToggleActive

本例展示从 SDK 声明、ARM64 指令到 exec thunk 调用语义的分析方法。摘录来自和平精英 `com.tencent.tmgp.pubgmhd` 的历史构建，不能把本例 RVA 当作当前版本通用偏移。原始 Dump 和本地 IDA 数据库不随仓库发布。

## 输入与地址

- 在自己的 Dump 会话 `manifest.json` 中记录源库 SHA-256，后续查询和分析使用同一明确会话 ID。
- 本例函数 RVA：`0x1288eee0`；摘录时 IDA imagebase 为 `0x0`，因此 EA 同为 `0x1288eee0`。
- ELF 分析布局：`virtual-memory-offset`。保护样本存在旧 p_offset 重叠，已按虚拟地址布局重建并正确映射 RELRO 尾部捕获字节。

复现时按 [MCP 使用指南](mcp-usage.md) 执行函数查询、打开独立分析副本、提交分析任务并读取 `umt_job` 结果。确认实际 input、imagebase、RVA 和函数指令后再套用下列解释；只收到任务 ID 不等于分析成功。

## SDK 与 IDA 摘录

```cpp
// Object: Function Engine.ActorComponent.ToggleActive
	// Flags: [Native|Public|BlueprintCallable]
	// Offset: 0x1288eee0
	// Params: [ Num(0) Size(0x0) ]
	void ToggleActive();
```

```asm
LDR X8, [X1,#0x20]
CMP X8, #0
CINC X8, X8, NE
STR X8, [X1,#0x20]
LDR X8, [X0]
LDR X1, [X8,#0x3D0]
BR X1
```

```cpp
__int64 __fastcall ActorComponent__execToggleActive(__int64 a1, __int64 a2)
{
  __int64 v2; // x8

  v2 = *(_QWORD *)(a2 + 32); /*0x1288eee0*/
  if ( v2 ) /*0x1288eee8*/
    ++v2; /*0x1288eee8*/
  *(_QWORD *)(a2 + 32) = v2; /*0x1288eeec*/
  return (*(__int64 (__fastcall **)(__int64))(*(_QWORD *)a1 + 976LL))(a1);
}
```

## 能确定的作用

1. x1 所指结构的 +0x20 字段不为零时加 1，并写回；结合 UE exec wrapper 结构，可推断为 FFrame 的 Code 前进，但字段身份仍需对应版本结构校验。
2. 从 x0 所指对象取 vtable，在 vtable +0x3D0 读函数指针，尾跳转过去；ARM64 每项 8 字节，对应槽位 122。
3. 当前函数是反射调用的 exec thunk，不是已经完整分析的 ToggleActive 原生实现。名字与 SDK 声明对应切换组件激活状态；具体是否更新 tick、广播事件等，需要继续分析实际虚函数实现，不能凭名字断言。
4. IDA 推断的 __int64 返回类型不是已确认 ABI。SDK 声明 void，包装函数尾跳到虚函数，因此不能按反编译器默认返回类型生成可调用原型。

## Hook 观测方法

下列观察脚本定位 libUE4.so 的当前基址，加本例 RVA，验证可执行页，采样前 30 次入口／返回。记录的是原始寄存器值，不把 x1 误标成业务参数。使用前必须从自己的 Dump 确认并更新 RVA；页面可执行并不能证明它仍是同一函数。仓库包含脚本示例，没有附带已执行 Hook 的运行记录。

```javascript
// Generated for this exact dump. Verify the same game build before attaching.
// x0..x3 are raw ARM64 argument registers, not recovered typed arguments.
'use strict';
const moduleName = "libUE4.so";
const rva = ptr("0x1288eee0");
const image = Process.getModuleByName(moduleName);
const address = image.base.add(rva);
const range = Process.findRangeByAddress(address);
if (!range || !range.protection.includes('x')) throw new Error('RVA is not executable');
let remaining = 30;
Interceptor.attach(address, {
  onEnter(args) {
    this.sample = remaining-- > 0;
    if (this.sample) console.log(JSON.stringify({name:"ActorComponent$$execToggleActive", address:address.toString(), thread:this.threadId,
      x0:args[0].toString(), x1:args[1].toString(), x2:args[2].toString(), x3:args[3].toString()}));
  },
  onLeave(value) { if (this.sample) console.log('return(raw): ' + value); }
});

```

更新 [示例脚本](example-toggleactive-trace.js) 中的 RVA，并确认目标构建与可用 Frida runtime 后运行：

```powershell
frida -U -p <当前游戏PID> -l docs/example-toggleactive-trace.js
```

Frida runtime 的部署与连接需自行配置；命令是脚本运行入口，不表示当前游戏构建已验证可用。

## 调用方法与下一步分析

- 不要把 `base + 0x1288eee0` 直接当成 `void (*)(UObject*)`：它还使用 FFrame，缺少有效 x1 会访问错误地址。
- 反射路线：找到有效 ActorComponent 实例和对应 UFunction，按本版本 ProcessEvent ABI 在正确线程分发。本例 SDK NumParams=0、ParamSize=0，但对象有效性、UFunction 与线程仍需核验。
- 原生路线：从具体实例的 vtable 读取槽位 122，再分析那个地址及可能的 override。确认实际签名、返回类型和线程要求后才能构造直接调用，不能把接口槽位当成固定全局函数。
- 局部分析时 xrefs/callees 数组可能为空，这不表示运行时没有调用者。虚调用本来也不保证形成直接 call xref。需要完整引用恢复时显式启用全库自动分析，或继续使用 umt_ida_tool 查询调用链和类型。

## 给 Agent 的任务示例

> 使用刚同步完成的明确 Dump 会话 ID，查询 ActorComponent$$execToggleActive。在 IDA 中核对包装函数与虚调用目标，说明参数、调用链与副作用；将已证实的 ABI 与推断分开，给出观测 Hook 以及调用实现所需条件，最后用 umt_save_analysis 保存报告。

报告应分别记录输入身份、观察到的指令、已验证的结构字段和仍需确认的 ABI。函数名称、SDK 声明及一个历史 RVA 都不足以证明当前版本的运行行为。
