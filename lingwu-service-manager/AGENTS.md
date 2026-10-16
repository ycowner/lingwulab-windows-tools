# AGENTS.md — lingwu-service-manager

给 AI agent 的硬约束与踩坑清单。**动手前先读完这一份**，里面每一条都是实测踩出来的，
不是从文档抄的。

同仓库的 [`lingwu-sysmon/AGENTS.md`](../lingwu-sysmon/AGENTS.md) 讲的是共用约定
（编码、构建链、WinUI 3 通病），这里只记**本工具特有的**东西。两边冲突时以本文为准。

---

## 1. 不可协商的硬约束

| 约束 | 原因 |
|---|---|
| 源码 UTF-8 + `/utf-8` 编译开关 | 中文注释不写 `/utf-8` 会 C4819，而本工程 `/WX` |
| `.bat` / `.idl` / `.ps1` 纯 ASCII，`.bat` 还要 CRLF | MIDL 按系统 ANSI 码页读 `.idl`（zh-CN 是 936）；`.bat` 里非 ASCII 会被 cmd 按 OEM 码页解释 |
| `/W3 /WX` 零警告 | 见上 |
| `/MT` 静态 CRT | 免安装分发，目标机不一定有 VC 运行库 |
| `volatile LONG` 而非 `bool` 做跨线程标志 | `bool` 的非原子读写在优化下会被提升 |
| `core/` 不依赖 UI | 见第 4 节，`--selftest` 靠这条 |
| XML 注释里禁止连续 ASCII 连字符 `--` | 触发 `MSB4025`，且报错指向文件头不指向那一行 |
| UI 验收脚本必须用 `pwsh`，不能用 `powershell` 5.1 | 5.1 的 UIA COM 绑定不同，`AndCondition` 会静默退化成单条件 |

---

## 2. SCM 层的四个致命坑（本节最贵，全部实测）

### 2.1 `EnumServicesStatusExW` 在非提权下必然失败

```
EnumServicesStatusExW(..., pcbBytesNeeded = nullptr)      -> err=487  「试图访问无效的地址」
EnumServicesStatusExW(..., pcbBytesNeeded = &need)        -> err=87   「参数错误」
   换 dwServiceType / dwServiceState / 缓冲区大小，六种组合全是 err=87
EnumServicesStatusW（经典）                                -> 成功，345 条
```

`pcbBytesNeeded` 是 `_Out_` **非可选**参数，传 `nullptr` 会让 SCM 往空指针写 —— 487 就是
这么来的。改对之后仍报 87，原因未确定，但足以判定：**本工具的默认模式就是非提权，
所以 Ex 变体在主用路径上不可用。**

**现行方案**：`EnumServicesStatusW` 分页枚举 + 对运行中的服务逐个
`QueryServiceStatusEx(SC_STATUS_PROCESS_INFO)` 取 PID / `dwServiceFlags`。
实测 158 个运行中服务全部取到，整轮耗时低于 `GetTickCount64` 的分辨率。
**不要再回去尝试 Ex。**

注意 `SERVICE_STATUS` 里**没有** `dwProcessId` 和 `dwServiceFlags`，只有
`SERVICE_STATUS_PROCESS` 有。经典枚举给不了这两个字段。

### 2.2 缓冲区有硬上限：超过就 `err=1783`

实测这台机器上：

| API | 成功 | 失败 |
|---|---|---|
| `QueryServiceConfigW` | cb ≤ **8192** | cb ≥ 8193 → `err=1783` |
| `EnumServicesStatusW` | cb ≤ 256 KB | cb ≥ 512 KB → `err=1783` |
| `EnumServicesStatusW` | 见下 | cb = 512 KB → 也可能 `err=1783` |

`1783` = 「占位程序接收到错误数据」。**它长得像网络错误，完全看不出跟缓冲区大小有关。**

曾经的实际后果：两个调用点各写死 `std::vector<BYTE> buf(16 * 1024)`，正好踩在 8192 之外，
于是**每一个**服务的启动类型都查不到、列表那一列全空，日志只有一条看不出所以然的 1783。

**判据**：
- `QueryServiceConfigW` 申请 2 KB 起步、翻倍封顶 8 KB。真实需求只有三四百字节
  （`RpcSs` 实测 `need=388`）。
- 枚举用**分页**（`lpResumeHandle`）而不是给大缓冲。本机 345 条在 64 KB 一页里装得下，
  只调一次。
- `ERROR_MORE_DATA` **不是**致命错误：缓冲区不够时 SCM 返回 FALSE，但 `read` 与 `resume`
  **都是可用的**。实测 16 KB 一页时三次调用 134 + 141 + 70 = 345，与 256 KB 单次结果
  完全一致，无重复无遗漏。别把 MORE_DATA 当成失败直接放弃。

### 2.3 枚举缓冲区的布局：结构体数组在前，字符串统一堆在尾部

```
实测（345 条，buf.size() = 262144）：
  前 48*n 字节是 ENUM_SERVICE_STATUSW 数组，第 i 条在 i*48
  两个名字指针指向**缓冲区末尾**的字符串区，而且是**从后往前**排的：
      [0] nameOff=262132  displayOff=262074
      [1] nameOff=262060  displayOff=262042
      [7] nameOff=261704  displayOff=261658
  （第一条名字正好结束在最后一个字节）
```

所以「`sizeof(结构体)` + 两个字符串长度」这种步长推算是**错的** —— 它假设字符串紧跟在自己
那条结构体后面。照那么写，第一次迭代就越界，症状是**访问冲突 0xC0000005**，
而不是读到空名字：越界处的指针值本身就是个非法地址，`printf("%ls")` 当场崩。

**正确写法只有一个：把缓冲区当结构体数组直接下标取** `arr[i]`，并用
`servicesRead * sizeof(ENUM_SERVICE_STATUSW) <= buf.size()` 做上界断言。

### 2.4 `ENUM_SERVICE_STATUSW` 的真实成员名

```c
typedef struct _ENUM_SERVICE_STATUSW {
    LPWSTR          lpServiceName;   // 不是 szServiceName
    LPWSTR          lpDisplayName;
    SERVICE_STATUS  ServiceStatus;   // 不是顶层 dwCurrentState
} ENUM_SERVICE_STATUSW;
```

写成 `e->dwCurrentState` / `e->szServiceName` 会得到一批「不是某结构体的成员」，
看着像 `winsvc.h` 没包含，实际是**记错了结构布局**。

另外 `SC_ENUM_PROCESS_INFO` 在本机 SDK 里等于 **0**（`_SC_ENUM_TYPE` 只有一个成员），
传符号就好，别传字面量。

---

## 3. WinUI 3 / cppwinrt 在本工程的坑

### 3.1 IDL 里写不出「一个方法带多个 [out] 参数」

```idl
void ReadInputs([out] String a, [out] String b, ...);
```
→ **MIDL5005**「method parameters cannot have both [in] and [out] attributes」，七个参数全报，
另外配七条 MIDL5163 说这个 `out` 是冗余的（因为 out 已经是默认）。

```idl
void ReadInputs(String a, String b, ...);   // 去掉 [out]
```
→ 编译通过，但 cppwinrt 生成的 ABI shim 传的是 **`hstring const&`**，而 C++ 实现签名是
`hstring&` → **C2664**，报错位置在**生成的头文件**里、指着你写对的签名。

**结论：这个形态根本表达不出来。改成只读属性 `{ get; }`。** `ServiceRow.idl` 已经
证明 `{ get; }` 在这套 MIDL/winmd 配置下可靠，而且它按值返回。

顺带：`{ get; }` 返回的成员**不能加 `const`** —— XAML 编译器为 `x:Name` 生成的访问器
（`ServiceNameBox()` 等）是非 const 成员，const 成员函数里调不动。

### 3.2 每个 runtimeclass 都要有 `factory_implementation`

`module.g.cpp` 会为每个 runtimeclass 生成工厂缓存条目，缺了就是：
- 编译期：`X.g.cpp` 里三条错（成员不存在 / 类型未声明 / `make<>()` 找不到匹配重载）
- 链接期：一堆 unresolved external symbol

```cpp
namespace winrt::LingwuServiceManager::factory_implementation {
struct CreateServiceWindow : CreateServiceWindowT<CreateServiceWindow, implementation::CreateServiceWindow> {};
}
```

复用了 sysmon 的 `ServiceRow.h` 那段说明，连注释一起搬过来了。

### 3.3 其它零散但会浪费半小时的

| 现象 | 原因 |
|---|---|
| `DispatcherQueueTimer` 没有默认构造函数 | 用 `DispatcherQueue().CreateTimer()`。UWP 样例里全是 `DispatcherQueueTimer()`，那是运行时可构造类型 |
| 定时器周期成员叫 `Interval`，不是 `TickInterval` | |
| `Items()` 是**返回值**不是引用成员 | 写 `auto& items = Box().Items();` 报 C2440 |
| `Text()` 收 `hstring const&` | 喂 `box_value(...)`（`IInspectable`）报 C2665 |
| `IsChecked()` 的投影类型是 `IReference<bool>` | 有 `Value()`；`IPropertyValue` 没有 `Value()`，只有 `GetBoolean()` |
| `hstring(x)` 用圆括号 | `hstring{ std::wstring }` 是花括号初始化，非法，报语法错误 |
| `hstring` → `std::wstring` 要显式 | `std::wstring s = box.Text();` 报 C2440，写 `std::wstring s{ box.Text().c_str() }` |
| `RotateTransform` 在 `Microsoft::UI::Xaml::Media` | `Controls` 不是它的父命名空间，漏 using 报「意外的 RotateTransform」 |
| `IShellItem::GetDisplayName` 第二参是 `LPWSTR*` | 它 `CoTaskMemAlloc` 自己长度的内存，不能塞进 `wchar_t[MAX_PATH]`，用 `CoTaskMemFree` 释放 |
| `ContentDialog.Title/Content` 是 `IInspectable` | 必须 `box_value(...)`，直接传 `L"..."` 报 C2665 |
| `AsyncStatus` → `winrt::Windows::Foundation` | 少一个 using 就是「未声明的标识符」 |

### 3.4 XAML 编译器的静默陷阱

**只改 `.xaml` 里的字面量时 MSBuild 可能不重新链接 exe。** XAML 编译（MarkupCompilePass1）
和 PRI 生成都跑了，只有 Link 被增量跳过，输出里照样打印 `xxx.vcxproj -> xxx.exe`。

**判据：build 完之后比一下 exe 的 LastWriteTime 和你改的源文件的时间。exe 比源文件旧
= 正在拿旧二进制验收，后面所有 UIA / 截图 / 内存结论全部作废。** 必要时 `-t:Rebuild`。

（编译后的 XAML 字面量烘进 **`.xbf`** 而不是 `.g.h`，所以 `MainWindow.xaml.g.h` 时间戳
不变是正常的，不能用它判断有没有编过。）

---

## 4. 架构约定

```
core/       不依赖 UI，可被 --selftest 独立驱动
platform/   Win32 层（trace / 窗口装饰 / 单实例）
ui/         WinUI 3，只做呈现与投递
```

`--selftest` 走 `core/` 全链路（345 条服务、字段合法性、缓存、交叉验证），**不创建窗口**。
这是唯一能在非交互 shell 里完整验证内核的通道，也是修 SCM 层时的第一道防线：
**改 `core/` 之后先跑 `--selftest`，它绿了再去看 UI。**

### 线程契约

- `ScmSession` 归 **poll 线程独占**。UI 线程要用写操作一律走非成员函数
  （`ScmStart` / `ScmStop` / …），各自开关句柄。
- 写操作全部走 **op 线程**，UI 线程只投递与呈现。
- 跨线程通知：工作线程置原子标志，UI 侧 100 ms `DispatcherQueueTimer` 轮询。
- **`ComboBox` 的 `SelectionChanged` / `TextBox` 的 `TextChanged` 接不上**
  （本版 cppwinrt 的 `Microsoft.UI.*` delegate 模板构造漏 `inline`）。
  改用心跳比对 `SelectedIndex` / 文本内容。
- 但 `AutoSuggestBox::TextChanged` **能**接。而且它的事件参数 `Reason()` **不要用来
  做守卫** —— 见下面第 5.1 条。

### 提权顺序（顺序反了就是「点了没反应」）

```
关窗口 → 进程退出 → 放掉互斥量 → runas
```

反过来的话新实例会认为「已经有一份在跑」然后自己退出。
`core::RequestRelaunchElevated()` 只**登记**，真正 `runas` 在 `main.cpp` 的退出路径上做。

---

## 5. 验收脚本

全部在 `tools\`，用 `pwsh` 跑。

| 脚本 | 覆盖 |
|---|---|
| `--selftest` | core 层 7 组断言 |
| `uicheck.ps1` | 窗口、345 条服务、**三列双向排序**、搜索、筛选、详情面板 |
| `uicreate.ps1` | 创建表单校验 + 提权拒绝路径（提权时验真的建出来） |
| `uiadmin.ps1` | 提权模型（非提权验拒绝+提权入口，提权验操作生效） |
| `uimem.ps1` | 干净实例内存，私有内存红线 160 MB |
| `make-icon.ps1` | 生成 `resources/app.ico`（几何图形，可复现） |

### 5.1 UIA 验收的六条铁律

1. **跨进程枚举必须 `AndCondition(ControlType, ProcessId)`。** 只用 `AutomationId`
   会匹到桌面上第一个同名窗口，那通常**是别的应用**。
2. **不要让 `Reason()` 守卫 `TextChanged`。** 原实现在 `OnSearchChanged` 里靠
   `args.Reason() != UserInput` 提前返回，理由是「程序性回写也会触发」。但本工程
   **没有任何地方回写搜索框**，那个风险不存在 —— 代价是 UIA 的 `ValuePattern.SetValue`
   走程序性赋值，搜索框从此对自动化完全失灵。防抖要在别处做（比对新旧文本）。
3. **ListView 是虚拟化的，`ListItem` 数量不能当断言。** 列表里有 345 条，UIA 只枚举到
   已实现的那 ~37 行，而且这个数字**会随滚动增长** —— 拿它做「操作前 / 操作后」比较
   实际测的是滚动，不是筛选。**要问「列表里有多少条」就读状态栏。**
4. **ComboBox 的项只在弹出时才是 UIA 子节点。** 关着的组合框 `FindAll` 永远返回 0，
   必须先 `ExpandCollapsePattern.Expand()`。
5. **`Expander` 折叠时子节点完全不可达。** `FindFirst` 返回 null，即使对象存在、应用本身
   也能调到它。要读里面的字段先 `Expand()`。
6. **`ContentDialog` 的树是异步物化的。** 刚弹出的那一刻拿到的按钮可能是
   「正文 TextBlock 被当成按钮 + WinUI 默认的 Close」，一秒后才变成真正的
   「以管理员身份重新启动 / 取消」。**连采两次、一致了再用**（`Get-DialogButtons`）。
   而且要按**码点**断言中文按钮，因为 `uia.ps1` 必须保持纯 ASCII。

另外：**脚本要用 `try/finally` 收尾。** 断言抛出后没有 `Stop-App`，进程会占住 exe，
下一次 `build.bat` 报 `LNK1104 无法打开文件` —— 那个错指向链接器，而真正的原因是上一轮
验收脚本没收尾。

### 5.2 能验和不能验的边界

**所有写操作都需要管理员权限，而非交互 shell 提不了权。** 所以：

- 非提权运行时，`uicreate` / `uiadmin` 验的是**拒绝路径**：不能假装成功、
  要说清原因、要给出提权入口。**静默失败的按钮**正是这些脚本要挡的东西。
- 提权运行时，同一个脚本自动改验 happy path。
- **本机无法用合成鼠标输入取证。** 所有交互走 UIA 的 `Invoke` / `Value` /
  `SelectionItem` 模式。右键菜单的弹出路径（Win32 `WM_CONTEXTMENU` + 命中测试）
  因此**只做了逻辑验证，没有端到端证据**。
- 自动隐藏的覆盖式 ScrollBar **抓不到也枚举不到**（模板给了
  `AccessibilityView="Raw"`），要验证它不遮内容只能靠量坐标区间推导。

---

## 6. trace

`%LOCALAPPDATA%\LingwuServiceManager\trace.log`。

打开方式（满足其一）：环境变量 `LINGSVC_TRACE` 非空，**或**存在标记文件
`%LOCALAPPDATA%\LingwuServiceManager\trace.on`。

**为什么同时支持标记文件**：非交互 shell 里用 `$env:` 设的环境变量不一定能传到
`Start-Process` 起的子进程（实测在本 agent 的 shell 里就传不过去，trace 静默不写，
极易误判成「代码没执行」）。文件存在性没有这种歧义。**要取证就用标记文件。**

**trace 是窄 `vsnprintf`：打印宽字符串要用 `%ls` 不是 `%s`。**
写错的表现是服务名只显示第一个字符（`ADPSvc` 显示成 `A`），看起来像「名字被截断了」，
实际是格式化串用错了。

---

## 7. 服务数据的事实

- 参考机上有 **345** 个 Win32 服务，其中约 **156–158** 个在运行。
- 枚举不按状态过滤（`SERVICE_STATE_ALL`）。传 `SERVICE_ACTIVE` 只返回活动服务，
  大约一半，界面上却同样「看起来正常」—— 所以验收脚本对行数有硬断言
  （> 100），而不是只断言「有内容」。
- `QueryServiceState` 与枚举结果做过交叉验证（`--selftest` 第 5 组）。

---

## 8. 用户确认过的产品决策（不要擅自改）

| 决策 | 结论 |
|---|---|
| 功能范围 | 列表 + 只读详情 + 启停重启 + 启动类型切换 + 创建 + 删除 |
| 权限模型 | **默认普通权限**启动，能看列表；需要提权时弹提示由用户点按钮 |
| 程序形态 | 纯按需打开，关闭即退出。不进托盘、不开机自启 |
| 运行账户 | 只支持 `LocalSystem` / `LocalService` / `NetworkService`，**不支持自定义账户与密码** |
| 依赖服务 | 不支持多选，`lpDependencies` 传 `nullptr` |
| 默认视图 | 全部服务（与 `services.msc` 一致） |
| 排序 | 三列都能**双向**排 |
| 详情面板 | **默认折叠**，不自动展开 |
| 启动类型必须两处都写 | `ChangeServiceConfigW(dwStartType)` + `ChangeServiceConfig2W(SERVICE_CONFIG_DELAYED_AUTO_START_INFO, ...)` |
| 内存红线 | **私有内存 160 MB**（工作集只记录，不卡） |
| 图标 | 与 sysmon 同一套配色（底 `#1E222B`、强调 `#F5B942`），标记是齿轮；由 `tools/make-icon.ps1` 生成 |
| 版本 | 从 `0.0.1` 起 |
