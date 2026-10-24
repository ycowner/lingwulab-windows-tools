# AGENTS.md —— 零物工具箱（lingwu-toolbox）开发约束

面向在本目录内改代码的 AI agent / 人类开发者。**动手前必读。**

本目录是 C++/WinRT + WinUI 3 实现，版本 **0.0.1**（本项目唯一版本）。
它同时包含两个模块：**性能监视**（进程表）和**服务管理**（Windows 服务表），
两者在同一个窗口里用标题栏的导航药丸切换，共用一个 exe、一个托盘图标、一份设置。

```
lingwu-toolbox/
├─ src/
│  ├─ core/            数据层，不依赖任何 UI 框架，可独立 --selftest
│  │  ├─ monitor/      进程采样、排序、聚合、自启注册表、结束进程
│  │  ├─ service/      SCM 枚举、启停/启动类型/创建/删除
│  │  ├─ settings.*    两边共用的设置持久化（唯一事实源）
│  │  └─ selftest.*    两套断言的统一入口
│  ├─ platform/        纯 Win32：trace、窗口装饰、单实例、托盘、回调兜底
│  ├─ ui/              WinUI 3：外壳 + 两个页面 + 设置窗口 + 创建服务窗口
│  └─ main.cpp
├─ tools/              验收脚本（见「验收脚本」一节）
├─ packaging/          SFX 自解压打包
└─ resources/          app.ico / app.manifest / app.rc
```

## 1. 构建

```bat
build.bat
```

产物 `dist\LingwuToolbox.exe`。脚本通过 `vswhere` 定位 MSVC，缺依赖时不要自己拼 `cl` 命令行。

改动源码后必须**跑一次 `build.bat` 并确认零错误零警告**（`/W3 /WX`）。
`/MT` 静态 CRT 是硬要求，复验方式见 README 踩坑 15。

## 2. 硬性约束

这些是用户明确提出的预算，不要在没问过的情况下突破：

- **应用本体 ≤ 8 MB**（当前 exe 1.13 MB / 1,182,208 字节）。
  `dist\` 合计 62 MB 是**框架依赖部署**的结果（Windows App Runtime 的
  `Microsoft.UI.Xaml.dll` 等按自包含方式随包分发），不是应用代码变胖了。
- **常驻内存 220 MB 私有内存红线**（口径见第 7 节）。
- **CPU 自耗低**（默认 1 秒间隔；窗口隐藏到托盘时列表 UI 完全不刷新，见下）。
- **免安装绿色**：unpackaged + 框架依赖部署，不写 Program Files，除自启动项外不碰注册表。
- **整个程序默认以管理员身份运行**（manifest 声明 `requireAdministrator`）：
  服务管理模块要启停/删除服务，没有管理员权限根本列不全。
  **代价**：开机自启拉起时 Windows 会弹一次 UAC —— 这是提权模型的必然结果，不是 bug。
  UAC 弹在应用起来之前，所以用户先看到 UAC 确认框、确认后才看到托盘图标。

### 提权模型的连带影响（重要）

因为整个程序都是管理员，所以：

- **验收脚本也必须在提权 shell 里跑**。UI Automation 看不到跨完整性级别的界面：
  同一个窗口、同一个查询，提权客户端拿到 **451** 个后代，非提权客户端只拿到 **4** 个
  （那 4 个是窗口边框）。拿不到 AutomationId，也调不动任何 pattern。
  `tools/common/common.ps1` 的 `Require-Elevated` 会自动把脚本提权重跑，
  详见第 11 节。
- **状态栏的「管理员权限」是常量**，不是实时检测：`ServicePage` 里
  `controller_.SetElevated(true)` 是写死的。正式产物永远以管理员运行，所以这句话
  永远成立；但如果你用一个 `asInvoker` 的验证副本来跑 UI 验收，这行字会是**假的**，
  而写操作会被 OS 拒绝 —— 别把它当成应用缺陷。

## 3. 架构

```
core/       不依赖 UI，可被 --selftest 独立驱动
platform/   Win32 层（trace / 窗口装饰 / 单实例 / 托盘 / 回调兜底）
ui/         WinUI 3，只做呈现与投递
```

`--selftest` 走 `core/` 全链路（性能监视 + 服务管理两套断言），**不创建窗口**。
这是唯一能在非交互 shell 里完整验证内核的通道，也是修 `core/` 时的第一道防线：
**改 `core/` 之后先跑 `--selftest`，它绿了再去看 UI。**

### 命名空间分层

- `lingwu::core::monitor` —— 进程采样、排序、聚合、终止进程、自启注册表
- `lingwu::core::service` —— SCM 枚举、服务模型、写操作控制器
- `lingwu::core` —— 两边共用的设置层（`core/settings.h`）
- `::platform` —— 纯 Win32（**不在** `lingwu` 命名空间里，是全局的 `platform`）
- `::winrt::LingwuToolbox::implementation` —— WinUI 3 实现类

`core/monitor/metrics.h` 才有 `SortMode` 枚举，`core/settings.h` 没有。
用 `SORT_*` 的文件必须 include `core/monitor/metrics.h` 并加
`namespace core = lingwu::core::monitor;`；服务侧对应
`namespace core = lingwu::core::service;`。
**两个 `core` 别名不能同时出现在一个文件里** —— 这就是两个模块的代码不混在一个
translation unit 里的原因之一。

### 线程契约

- 服务侧的 `ScmSession` 归 **poll 线程独占**。UI 线程要用写操作一律走非成员函数
  （`ScmStart` / `ScmStop` / …），各自开关句柄。
- 写操作全部走 **op 线程**，UI 线程只投递与呈现。
- 跨线程通知：工作线程置原子标志，UI 侧 100 ms `DispatcherQueueTimer` 轮询。
- **`ComboBox` 的 `SelectionChanged` / `TextBox` 的 `TextChanged` 接不上**
  （本版 cppwinrt 的 `Microsoft.UI.*` delegate 模板构造漏 `inline`）。
  改用心跳比对 `SelectedIndex` / 文本内容。
- 但 `AutoSuggestBox::TextChanged` **能**接。而且它的事件参数 `Reason()` **不要用来
  做守卫** —— 见第 5 节第 10 条。

## 4. 源码约定

- 源码 UTF-8，编译必须带 `/utf-8`
- **`build.bat` 与所有 `.bat` 必须是纯 ASCII + CRLF**，混入中文会让 cmd 逐行丢字符
- **`.idl` 必须纯 ASCII**：midl 按系统 ANSI (936) 读，中文报 `C4819` 且撞 `/WX`
- **`.ps1` 不是纯 ASCII**：实测 `tools\` 下的脚本普遍带中文注释与中文字面量，
  编码是 **UTF-8 无 BOM + LF**。所以 `.ps1` 的约束是「**只能用 `pwsh` 跑**」
  （见第 11 节），不是「必须纯 ASCII」。本仓库源码统一 **LF** 行尾
  （只有 `.bat` 是 CRLF）
- `.ps1` 里写带变量的中文字符串时注意 `"$i:"` 会被 PowerShell 当成驱动器限定
  变量，报「没有有效的文本终止符」，要写成 `"${i}:"`
- `/DNOMINMAX` 已在编译选项里，不要移除
- 每个 `.cpp` 第一行必须 `#include "pch.h"`（`/Yu"pch.h"` 找不到指令即 `C1010`）
- 需要原子读写的成员用 `volatile LONG`，**不要用 `bool`**
  （`Interlocked*` 是 4 字节操作，直接作用在 `bool` 上会越界写坏相邻成员）
- 布局一律交给 XAML + 系统 DPI，**不要写裸像素常量**（本机 200% 缩放）
- 新代码注释写**为什么这么写**，不要复述代码在做什么。踩坑依据必须落在对应源码注释里
- **XML 注释里禁止连续 ASCII 连字符 `--`**：触发 `MSB4025`，
  且报错指向文件头不指向那一行

## 5. 改动前必读的踩坑记录

下面每一条都是「不报错但功能是坏的」类型，全部实测踩出来。

1. **`Application::Start` 的回调里必须自己 `make<implementation::App>()`** ——
   漏了它 `/OPT:REF` 会把整个 App 类裁掉，症状是「进程活着但一个窗口都没有」。
   **判据：直接在 exe 里搜 `OnLaunched` 这几个字符。**
2. **`ItemsSource` 终身只赋一次**，之后就地增删同一个 `IObservableVector`
   （`RemoveAtEnd`/`Append` 调长度，`SetAt` 写内容）。换 ItemsSource 会让私有内存
   60 秒涨 37 MB 且不收敛。**禁用 `RemoveAt`+`InsertAt` 重排** —— 那是结构性变更，
   会让 ItemsRepeater 彻底停止实现容器（症状：数据 163 行、屏幕只剩 1 行）。
   **两个页面都构造一次、常驻，切换只改 `Visibility`**：重建页面 = 新建一套行容器
   + 重新赋 ItemsSource，那条铁律就白立了，而且每次切页都会漏一批原生资源。
3. **`ICustomPropertyProvider::Type()` 必须 `TypeKind::Metadata` + 真实类名**。
   写成 `Primitive`/`"String"`（那是 `ICustomProperty::Type()` 的写法）会在启动约
   5 秒后以 `0xc000027b` 崩在 `Microsoft.UI.Xaml.dll`。
4. **`ICustomProperty` 里绝不存裸行指针**，`GetValue` 只能用引擎传进来的 `target`
   参数取行（QI 到投影类型）。存裸指针会在排序时随机 `0xc0000005`：
   `Update()` 抛 `PropertyChanged` 时绑定引擎**只打脏标记**，真正的 `GetValue`
   推迟到之后的 layout pass，而同一帧的 `SetAt` / `rowByKey_` 重建可能已把行释放。
   实测 180 秒崩 4 次，**且伪装成「测量脚本把进程杀了」**，极易白放过几轮。
   持强引用会成环泄漏（行 → event → delegate 是强引用链，COM 无循环回收），
   退休缓冲区只是把崩溃窗口推到两帧之后，都别用。
5. **CPU 排序键必须量化，且主键和次级键要一起量化**。
   `metrics.h` 的 `AppCpuSortKey()` 把 `cpuNorm` 归到 0.01% 的格子（与 `FormatCpu`
   的最小显示精度一致）。只量化 `switch` 里的主键、次级键仍比裸 `cpuNorm`，
   等于白做 —— 实测 `replaced` 依旧 153~168。还要补终极 tiebreaker（组内首个
   `pidStart`）：`std::sort` **不稳定**，比较器返回 false 就意味着「两者无序」，
   逐进程口径下大量同名进程（多个 `chrome.exe`）名称这一级也分不出胜负。
   内存键**不量化**（字节级整数，`MEM_DESC` 实测 123.7 MB 已达标）。
6. **`Microsoft.UI.*` 命名空间的委托一律接不上**（本版 cppwinrt 3.0.260818.1 的委托
   模板构造函数漏 `inline`）。只有 `Windows.Foundation` 那批
   （`RoutedEventHandler` / `TypedEventHandler`）可用。事件挂载一律绕开
   `SelectionChanged` / `RightTapped` / `Pointer*`。
7. **`X` 按钮必须用 `AppWindow.Closing` + `args.Cancel(true)` 拦截**；
   `SetWindowSubclass` 拦 `WM_CLOSE` 实测无效。
   **且 `Closing` 处理器里任何异常都会让 `Cancel(true)` 作废** —— 窗口照常关闭、
   进程直接退出，症状与退出逻辑写错完全无法区分，而 trace 里往往一条线索都没有。
   整段必须包 `try/catch` 并落 trace，trace 要打在可能抛异常的语句**之前**。
   实测踩过：第二次弹关闭框时把同一个 `CheckBox` 重新 `Append` 进一个新建的
   `StackPanel`（XAML 元素同一时刻只能有一个父节点）→ 抛异常 → 进程没了。
   ⇒ `ContentDialog` 的 `Content` **只装配一次**，绝不重复 `Append`。
   **（2026-10-21 已落实）** `MainWindow` 的 `Closing` 处理器现在整体由
   `platform::GuardCallback("window.Closing.onCloseRequested", ...)` 兜住，
   且 `a.Cancel(true)` 排在所有可能抛异常的语句**之前** —— 一旦 Cancel 生效，
   本次关闭就不成立，后面即使抛异常，窗口也不会关、进程也不会退出。
8. **WinUI 3 的 `Window` 没有 `Visibility`**：`IWindow` 只有只读的 `Visible()`，
   没有 `put_Visible`，写 `Visibility = Collapsed` 直接编不过。隐藏只有
   `AppWindow().Hide()` / `AppWindow().Show(true)` 这一套，而 `SW_RESTORE`
   对隐藏窗口是空操作 —— **所有恢复路径（`ShowAndActivate`、以及 `HostProc`
   里的单实例激活消息）都要先判 `IsWindowVisible` 再判 `IsIconic`**，
   否则「收进托盘后再点一次 exe 图标」没反应且不报错。
9. **同一个 `XamlRoot` 同时只能挂一个 `ContentDialog`**（现有 `confirm_` / `notice_` /
   `closeExit_` 三个）。开之前置 `dialogOpen_`、`Closed` 时清位，缺一处就永久锁死；
   冲突时**忽略这一次关闭并落 trace**，不要弹第二个。
10. **不要让 `Reason()` 守卫 `TextChanged`。** 原实现在 `OnSearchChanged` 里靠
    `args.Reason() != UserInput` 提前返回，理由是「程序性回写也会触发」。但本工程
    **没有任何地方回写搜索框**，那个风险不存在 —— 代价是 UIA 的 `ValuePattern.SetValue`
    走程序性赋值，搜索框从此对自动化完全失灵。防抖要在别处做（比对新旧文本）。
11. **自定义 Win32 消息在 WinUI 3 上收不到**（窗口过程由 WinUI 接管）。
    跨线程通知用「采样线程置位原子标志 + UI 侧 `DispatcherTimer` 轮询」。
12. **托盘宿主必须用 `HWND_MESSAGE` 消息窗口**，而它收不到 `HWND_BROADCAST` 广播，
    所以 `TaskbarCreated` 必须由主窗口子类接再转发。
13. **托盘左键按 `NIM_SETVERSION` 协商结果分流新旧通知码**。四类码一起接会双跑
    `ShowAndActivate`。`g_version` 必须挂在 `platform` 命名空间而非匿名命名空间。
14. **设置项必须能表达底层取值的全部范围**。排序下拉曾只列 3 项，导致 `MEM_ASC` /
    `CPU_ASC` 在设置里永远选不到，尽管它们能被主窗口表头设出来。
    同样的道理：`CloseAction` 三态以前**没有 UI 入口**（设为 1 或 2 之后只能手改
    ini 改回 0），现在补了「关闭窗口时」下拉。
15. **单实例定位不能用类名**。WinUI 3 类名全系统统一
    （`WinUIDesktopWin32WindowClass`），要共享内存双份存 HWND。
16. **Win32 回调边界（窗口过程 / 菜单回调 / Shell 回调）必须包异常兜底**。
    C++ 异常从 `LRESULT CALLBACK` 逃出去，user32 / shell 层没有 C++ handler，
    CRT 只能 `std::terminate` → `abort()` → `__fastfail` → **0xC0000409**，
    进程直接消失且现场全丢（事件日志只有一条异常码，trace 里什么都没有）。
    统一用 `platform::GuardCallback(tag, fn)`（`src/platform/callback_guard.h`），
    tag 要能直接指出是哪一条路径。已覆盖：托盘 `WM_TRAY_CB`、托盘菜单五个
    菜单项、主窗口窗口子类三条分支、`AppWindow.Closing` 处理器。
17. **WinUI 3 的 `Window` 关闭后不可复用** —— `Close()` 之后对象仍在
    （成员非空）但 `AppWindow` 已失效，再调 `Activate()` 会抛 `hresult_error`；
    而异常从托盘菜单回调一路穿过窗口过程 = 进程直接消失（2026-10-20 实测）。
    **既不能复用已关闭的窗口，也不能每次打开都新建一个** ——
    后者实测**线性泄漏**原生资源（每轮约 36 个句柄 + 1.8 MB 私有内存，
    30 轮 = +379 句柄 / +30.4 MB，越过 145 MB 红线）。
    ⇒ 设置窗口采用「**构造一次 + 隐藏/显示**」：关闭时只 `AppWindow().Hide()`，
    真正关闭放在主窗口退出时：先 `AllowClose()`（`SettingsWindow.idl` 里新增的
    方法）放行自己的关闭拦截，再 `Window::Close()`。
    **不要用 `AppWindow::Destroy()`** —— 实测踩过：它只销毁原生 HWND，
    **框架的窗口计数不会归零**，消息循环不结束，进程会僵留在「窗口全没了
    但进程还在」的状态（trace 里 `MainWindow closed` 已打过，进程仍在）。
    只有真正的 `Window::Close()` 才会把它从框架里注销掉。
    必须关：只要还有 `Window` 活着，XAML 框架的消息循环就不会结束。
    连带：`Window.Closed` 不再触发 ⇒ 设置结果改为**轮询**
    `settingsWindow_.Accepted()` 回收（`ApplyPendingSettingsResult`），
    靠 `settingsResultConsumed_` 防重复应用。
18. **IDL 里写不出「一个方法带多个 [out] 参数」**：
    `void ReadInputs([out] String a, [out] String b, ...)` → **MIDL5005**
    「method parameters cannot have both [in] and [out] attributes」，七个参数全报，
    另外配七条 MIDL5163 说这个 `out` 是冗余的（因为 out 已经是默认）。
    去掉 `[out]` 之后编译通过，但 cppwinrt 生成的 ABI shim 传的是
    **`hstring const&`**，而 C++ 实现签名是 `hstring&` → **C2664**，
    报错位置在**生成的头文件**里、指着你写对的签名。
    **结论：这个形态根本表达不出来。改成只读属性 `{ get; }`。**
    顺带：`{ get; }` 返回的成员**不能加 `const`** —— XAML 编译器为 `x:Name`
    生成的访问器（`ServiceNameBox()` 等）是非 const 成员，const 成员函数里调不动。
19. **每个 runtimeclass 都要有 `factory_implementation`**。`module.g.cpp` 会为每个
    runtimeclass 生成工厂缓存条目，缺了就是：编译期 `X.g.cpp` 里三条错
    （成员不存在 / 类型未声明 / `make<>()` 找不到匹配重载）+ 链接期一堆
    unresolved external symbol。
20. **XAML 编译器的静默陷阱**：**只改 `.xaml` 里的字面量时 MSBuild 可能不重新链接
    exe。** XAML 编译（MarkupCompilePass1）和 PRI 生成都跑了，只有 Link 被增量跳过，
    输出里照样打印 `xxx.vcxproj -> xxx.exe`。
    **判据：build 完之后比一下 exe 的 LastWriteTime 和你改的源文件的时间。exe 比源文件旧
    = 正在拿旧二进制验收，后面所有 UIA / 截图 / 内存结论全部作废。** 必要时 `-t:Rebuild`。
    （编译后的 XAML 字面量烘进 **`.xbf`** 而不是 `.g.h`，所以 `MainWindow.xaml.g.h` 时间戳
    不变是正常的，不能用它判断有没有编过。）
21. **零散的 cppwinrt 形状陷阱**（每一条都会浪费半小时）：

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

## 6. SCM 层的四个致命坑（服务模块，本节最贵）

### 6.1 `EnumServicesStatusExW` 在非提权下必然失败

```
EnumServicesStatusExW(..., pcbBytesNeeded = nullptr)      -> err=487  「试图访问无效的地址」
EnumServicesStatusExW(..., pcbBytesNeeded = &need)        -> err=87   「参数错误」
   换 dwServiceType / dwServiceState / 缓冲区大小，六种组合全是 err=87
EnumServicesStatusW（经典）                                -> 成功，345 条
```

`pcbBytesNeeded` 是 `_Out_` **非可选**参数，传 `nullptr` 会让 SCM 往空指针写 —— 487 就是
这么来的。改对之后仍报 87，原因未确定，但足以判定：**Ex 变体在主用路径上不可用。**

**现行方案**：`EnumServicesStatusW` 分页枚举 + 对运行中的服务逐个
`QueryServiceStatusEx(SC_STATUS_PROCESS_INFO)` 取 PID / `dwServiceFlags`。
实测 158 个运行中服务全部取到，整轮耗时低于 `GetTickCount64` 的分辨率。
**不要再回去尝试 Ex。**

注意 `SERVICE_STATUS` 里**没有** `dwProcessId` 和 `dwServiceFlags`，只有
`SERVICE_STATUS_PROCESS` 有。经典枚举给不了这两个字段。

### 6.2 缓冲区有硬上限：超过就 `err=1783`

实测这台机器上：

| API | 成功 | 失败 |
|---|---|---|
| `QueryServiceConfigW` | cb ≤ **8192** | cb ≥ 8193 → `err=1783` |
| `EnumServicesStatusW` | cb ≤ 256 KB | cb ≥ 512 KB → `err=1783` |

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

### 6.3 枚举缓冲区的布局：结构体数组在前，字符串统一堆在尾部

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

### 6.4 `ENUM_SERVICE_STATUSW` 的真实成员名

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

### 6.5 服务数据的事实

- 参考机上有 **345** 个 Win32 服务，其中约 **156–158** 个在运行。
- 枚举不按状态过滤（`SERVICE_STATE_ALL`）。传 `SERVICE_ACTIVE` 只返回活动服务，
  大约一半，界面上却同样「看起来正常」—— 所以验收脚本对行数有硬断言
  （> 100），而不是只断言「有内容」。
- `QueryServiceState` 与枚举结果做过交叉验证（`--selftest` 第 5 组）。
- **启动类型必须两处都写**：`ChangeServiceConfigW(dwStartType)` +
  `ChangeServiceConfig2W(SERVICE_CONFIG_DELAYED_AUTO_START_INFO, ...)`。

## 7. 内存红线与基线

### 口径（用户 2026-10-13 拍板）

**两个指标都记录，但红线只卡私有内存（PrivateMemorySize64）。**

工作集包含从 Windows App Runtime 框架包映射进来的 DLL 页，而这些页**可跨进程共享**。
把红线卡在工作集上，等于惩罚「用了框架」，而框架依赖部署正是本项目的既定方向。
实测同一版 WinUI 3 空壳：WS=102 MB / Private=62 MB，差的那 40 MB 几乎全是
`Microsoft.UI.Xaml.dll` 等框架 DLL 的共享页。

**怎么报**：验收表里两列都给数字（工作集 / 私有内存），达标判定写在私有内存那一列，
并在表下用一句话说明差额来源。不要只报其中一个，也不要用工作集当红线。

### 红线：220 MB 私有内存（用户 2026-10-24 在 Phase 6b 实测数据上重新拍板）

2026-10-23 那条 160 MB 的理由是「**功能变多了**」：性能监视的采样管道与服务管理的
SCM 枚举管道**同时常驻** —— 切走的模块只是不往 UI 上灌，后台照跑。

**但那条线定得不对**：它是按「后台管道变多」定的，**没有量过两个页面的列表都被
实现出来要花多少**。Phase 6b 的五场景基线量到的稳态是 **201.3 MB 私有 /
251.5 MB 工作集** —— 160 已经挡不住正常状态，于是按 220 定：
实测峰值之上留约 9% 的余量，够挡住回归，又留得下测量抖动。

2026-10-23 → 2026-10-24 的两次拍板都在这条线上留了痕，**不要再往上抬**：
除非先证明某个功能确实需要更多内存。

### 合并后的实测基线（2026-10-24，Phase 6b）

`tools/common/mem-baseline.ps1`，五个场景，同一份
`build\unelevated\LingwuToolbox.exe`（清单改成 `asInvoker` 的验证副本）：

| 场景 | 时长 | 工作集 MB（起 > 终 / 峰） | 私有内存 MB（起 > 终 / 峰） | 增量 | 句柄 | 尾部 |
|---|---|---|---|---|---|---|
| idle（静止） | 28 s | 161.8 > 188.2 / 188.2 | 107.0 > 133.4 / 133.4 | +26.4 | 947 > 950 | 一次 +26 MB 阶跃 |
| **switch（两页互切 ×60）** | 122 s | 201.8 > 249.3 / 249.5 | 150.1 > 197.4 / 197.8 | +47.3 | 967 > 953 | **走平** |
| hideshow（隐藏-显示 ×10） | 8 s | 247.5 > 246.5 / 248.3 | 197.4 > 196.5 / 198.5 | −0.9 | 941 > 939 | 走平 |
| settings（设置开合 ×10） | 15 s | 252.0 > 251.5 / 252.0 | 198.3 > 201.3 / 201.3 | +3.0 | 1008 > 999 | 走平 |
| tray（托盘常驻） | 55 s | 251.5 > 251.5 / 251.5 | 201.3 > 201.3 / 201.3 | 0.0 | 999 > 996 | 走平 |

**峰值：私有内存 201.3 MB / 工作集 252.0 MB。稳态：201.3 MB / 251.5 MB。**

**没有泄漏。** 判据是「切页阶段私有内存按阶段五等分的涨幅」：
**11.3 / 34.3 / 1.9 / −0.5 / 0.1 MB** —— 后 3/5 完全走平，涨幅全在头两个
五分之一里，其中一次是 +26 MB 的一次性框架预热阶跃；句柄数 967 → 953 反而降了。
（`idle` 那 28 秒里看到的「尾部上涨」也是同一个阶跃 —— 阶跃恰好落在采样末尾，
把短阶段的斜率算成了吓人的数字。**判泄漏要看绝对增量，不是每分钟斜率。**）

**这组数据把 160 MB 那条红线推翻了。** 差别不在后台管道，而在**两个页面的
列表都被实现出来**：

| 形态 | 私有内存 | 工作集 |
|---|---|---|
| 只开性能页 | 107.0 MB | 161.8 MB |
| 只开服务页（`service/mem.ps1` 实测） | 113.8 MB | 167.4 MB |
| **两页都实现过（工具箱的正常工作状态）** | **197.4 MB** | **249.3 MB** |

347 条服务 + 约 200 条进程，两个 `ListView` 的已实现容器加起来约 84 MB。
160 MB 那条线是 2026-10-23 按「切走的模块只是不往 UI 上灌」这个假设定的，
**它是在没量过服务页 347 行实现成本的前提下定的**，
所以它挡不住现在的正常状态。

### 为什么工作集绝对不能当红线（2026-10-24 托盘阶段实测到硬证据）

托盘常驻那一段的采样里，**同一个进程、同一份私有内存，工作集自己掉了一次**：

```
t=227.2  priv=198.1  ws=248.4   h=994
t=232.2  priv=198.1  ws= 37.9   h=994   <-- 工作集掉了 210 MB
t=357.7  priv=198.0  ws= 54.3   h=988
```

那是 Windows 的 working set trimmer：应用在托盘里静置约 50 秒后，
系统把工作集里的页裁出去换回物理内存。**私有提交量一点没变**（198.1 → 198.0），
因为那些页只是从「驻留」变成了「已提交但不在内存里」。

于是同一个进程的工作集可以在 **37.9 MB ~ 248.9 MB** 之间摆动，摆动幅度
200 MB 出头，**而这个数根本不由应用决定**。拿它当红线的话，
同一个二进制在不同时刻能差出一倍，还会在系统回收内存时无端报警。
这就是「两个指标都记，但红线只卡私有内存」的道理 —— 2026-10-13 拍板时
它是道理，现在它是实测。

### 判据：阶跃 vs 泄漏

**阶跃时点飘忽 + 幅度恒定 + 之后永久走平 + 句柄数不变 ⇒ 框架一次性预热，不是泄漏。**
真泄漏会持续上升且通常伴随句柄增长。跑够长（≥180 秒）再下结论，不要看短时增长。

### 「隐藏 / 显示」窗口的内存代价（2026-10-14 四组同构建实测）

四组都是同一构建、同一脚本协议（静置 120 秒 → 5 轮循环 → 尾部 90 秒）：

| 组 | 路径 | 末端私有内存 | 相对静止 |
|---|---|---|---|
| 静止 | 全程不动 | 130.6 MB | — |
| 最小化-还原 | `ShowWindow(SW_MINIMIZE/SW_RESTORE)`，不经应用代码 | 135.8 MB | +5.2 MB |
| 真隐藏-显示（无对话框，加暂停刷新**前**） | `CloseAction=1`，点 X 直接隐藏，20 轮 | 144.4 MB | +13.8 MB |
| **真隐藏-显示（无对话框，加暂停刷新后）** | 同上，20 轮 | **139.0 MB** | **+8.4 MB** |
| 真隐藏-显示（每轮弹框） | 每次都过关闭确认框，5 轮 | 144.0 MB | +13.4 MB |

**结论（推翻了旧代码注释里的担心）**：`AppWindow.Hide()` **不比最小化贵**，
甚至略低。那条「隐藏会让列表已实现容器被回收、撞上 +26 MB 阶跃」的判断
针对的是 `Visibility = Collapsed`（改 XAML 可视性树），而 `Hide()` 不碰 XAML 树，
所以那条担心**没有成立**。窗口不可见→可见本身约付 4~5 MB，且有界、走平。

**多出来的 ~8 MB 全部来自反复弹 `ContentDialog`**（每次 `ShowAsync` 都要建
Popup 与 XamlRoot 内容呈现），与隐藏方式无关。而勾了「记住我的选择」之后
对话框不再出现，也就是「真隐藏-显示（无对话框）」那一档。

上面这些 130~144 MB 的数字是**合并前单工具**的实测值，
它们仍是判断「隐藏/显示该不该付内存」的历史依据，但不能直接当现状基线。

### 窗口隐藏时不做 UI 刷新（2026-10-14）

`Refresh()` 开头有一道 `IsWindowVisible` 闸门：窗口不可见直接返回。
理由：100 ms 的 `DispatcherQueueTimer` 照旧在跑，而 `Refresh()` 一路走到
`RebuildRows()` 的 `SetAt()`，会作废并重建列表容器 —— 窗口看不见时
一帧也用不上，却照付 CPU 与容器 churn。

数据不会过期：采样线程不受影响，`ShowAndActivate()` 里窗口一恢复可见
就补一次全量 `Refresh()`。开机自启的静默模式也走这条路。

**实测收益**：20 轮隐藏/显示的末端私有内存 144.4 → **139.0 MB**，
尾部 60 秒跨度 2.2 → 1.2 MB，即每轮开销 ~0.7 → **~0.42 MB**。

**踩坑**：判据只能用 Win32 的 `IsWindowVisible(hwnd_)` ——
XAML 那个 `Visibility` 在本工程**不存在**（见第 5 节第 8 条）。

### 设置窗口「构造一次 + 隐藏/显示」（2026-10-21，坑掉一次线性泄漏）

这一步不是洁癖，是**被实测逼出来的**：

| 实现 | 30 轮「开→关」后的私有内存 | 句柄数 |
|---|---|---|
| 每次打开都 `make<SettingsWindow>()` | 139.5 → **169.9 MB**（+30.4，**越过 145 MB 红线**） | 893 → **1272**（+379，线性不收敛） |
| 构造一次 + 隐藏/显示（现行） | 133.1 → **140.8 MB**（+7.7，曲线走平） | 868 → **942**（+74 后稳定，30 轮只动 1 个） |

**教训**：WinUI 3 的 `Window` 关闭后不可复用，所以「关掉就重新 new 一个」
看起来是唯一出路 —— 但它每轮漏一整个窗口的原生资源（句柄 + 私有内存），
**无上界**。正确做法是**别让它被销毁**（见第 5 节第 17 条）。

### 设置窗口的版式与固定尺寸（2026-10-23 用户拍板）

设置窗口固定成 **590×560 逻辑像素**，物理像素按当前显示器的缩放换算
（`GetDpiForWindow` → dpi/96）。

**为什么必须固定**：不固定的话窗口按内容自适应，实测会撑到 **1440×755 逻辑像素**
（物理 2880×1511），几乎占满整块屏，中间一大片空白。
根因是根 Grid 的第二行是 `"*"` —— 自适应时它把窗口整个撑开了。

**为什么不用 `AppWindow::RasterizationScale()`**：本版 Windows App SDK（2.5.1）的
`AppWindow` 上**没有**这个属性（它在 `Microsoft.UI.Content` 的 `ContentRoot` 上）。
`GetDpiForWindow` 是 Win32 原生接口，1607 起就有，本工程最低版本 10.0.17763 够用。

**标题栏右侧要让位系统按钮**：自绘标题栏（`ExtendsContentIntoTitleBar(true)`）时，
最小化/最大化/关闭三个系统按钮是**画在内容之上的**。自己的按钮要用
`AppWindow().TitleBar().RightInset()`（物理像素）→ 换算成逻辑像素 →
当成那个按钮的 `Margin.Right`。必须在 `ExtendsContentIntoTitleBar(true)` **之后**读。

### 窗口尺寸不是内存变量（2026-10-14 受控 A/B 实测）

主窗口默认尺寸从 1040×700 放大到工作区的 70% 之后，私有内存越过了当时的红线。
**但这不是窗口造成的** —— 同进程、等框架预热阶跃过去之后，只用 `SetWindowPos`
改窗口尺寸：2015×1192 是 130.3 MB / 905 句柄，1040×700 是 130.6 MB / 920 句柄。
差值 **+0.3 MB，在噪声内**。原因：行数据本来就在那个 `IObservableVector` 里，
视口变化只决定「哪些行容器已实现」，**不复制数据**。

**教训：别再把「窗口变大」当成内存回归的候选原因，先做同进程受控 A/B 再下结论。**

**工作集不可复现**：同一应用状态下 t=80s 的工作集在两次运行里分别是 182.1 MB 与
44.6 MB，而私有内存两次都稳在 130 出头。这正是当初把红线只卡私有内存的理由 ——
卡工作集必然误判。

**排序档之间的差异与 `SetAt` 次数无关。** 名称排序每帧也要换 74 个槽位
（新进程插入导致其后整体平移），内存却最低、连阶跃都没有；
真正的变量是**同一个槽位上是否反复换不同的行对象** —— 换行对象就要重建 DataContext
与文本测量缓存。

## 8. 生命周期：托盘 / 关闭 / 设置窗口

- **常驻托盘**：关窗口 = 隐藏到托盘，进程继续活着。
- **`CloseAction` 三态**（`core/settings.h` 的唯一定义源）：
  `kCloseAsk=0`（每次询问，默认）/ `kCloseMinimizeToTray=1`（总是隐藏到托盘）/
  `kCloseExitProgram=2`（总是退出程序）。
  设置窗口的「关闭窗口时」下拉**下标就是档位值**，读回来不用做映射。
- **设置窗口「构造一次 + 隐藏/显示」**，见第 7 节。
- **自启动**：`HKCU\...\Run` 的 `LingwuToolbox` 值。注册表是唯一事实源，
  **不落 ini**。

## 9. 设置文件契约（向后兼容，不许改）

- 路径 `%APPDATA%\LingwuToolbox\settings.ini`、段头 `[Settings]`、键名如下
  （2026-10-23 合并成工具箱时改的路径；旧路径 `%APPDATA%\LingwuSysMon\settings.ini`
  与 `%APPDATA%\LingwuServiceManager\settings.ini` 只**读**用于一次性迁移，保留不删）
- 活跃键：`MonitorAggregate` / `MonitorSortMode`(0-4) / `MonitorIntervalMs`(250-10000)
  / `ServiceSortMode`(0-5) / `ServiceFilterMode`(0-4) / `ServiceDetailExpanded`
  / `TrayHintShown` / `WindowLeft` / `WindowTop` / `WindowWidth` / `WindowHeight`
  / `CloseAction`(0-2)
- **键名带模块前缀是有原因的**：合并前两个工具各写各的 ini，两个都叫 `SortMode`，
  语义完全无关。合进同一个 ini 后若还用同名键，后写入的一方会把另一方覆盖掉 ——
  用户会发现「切到服务页排序方式被重置」。
- 采样间隔的整数除数**必须在整数侧钳 ≥1**：`(uint64_t)0.001 == 0`，
  曾致启动 6 秒必崩 `0xC0000094`

## 10. 托盘菜单契约（2026-10-23 拍板，已实跑取证）

五项，顺序固定：

```
显示工具箱
─────────
✓ 性能监视          ← 切到 MonitorPage
  服务管理          ← 切到 ServicePage
  设置…             ← SettingsWindow
─────────
  退出
```

- 「性能监视 / 服务管理」**带勾选态**，标记当前所在页。勾选态由
  `TrayCallbacks::getActiveModule()` 现场问一次，托盘模块**不持有**这份状态的副本。
- 从托盘选模块时**顺带把窗口显示出来**（否则窗口藏在托盘里时菜单动作没有可见效果）。
- 原 sysmon 菜单里的「按应用聚合」**下沉**到性能页命令栏的 ToggleSwitch
  （避免状态双写）；「重新添加托盘图标」**下沉**到设置窗口标题栏右侧
  （它是动作不是状态，托盘菜单是高频操作区，不该塞维护类动作）。

## 11. 验收脚本

### 目录布局（2026-10-23 合并后）

```
tools/
├─ common/          跨模块共享件
│  ├─ common.ps1       提权自举 + exe 解析 + 进程启停（非 UI）
│  ├─ uia.ps1          UIA 辅助件（dot-source common.ps1）
│  ├─ tray.ps1         托盘图标查找（可见区 + ^ 溢出浮窗，开了会关回去）
│  └─ mem-baseline.ps1 内存红线基线：五个场景的长跑，报告工作集/私有内存两列
├─ monitor/          性能监视侧 21 个
├─ service/          服务管理侧 4 个
└─ build/            构建工具 2 个（make-icon / install-winui-workload）
                     + run-all.ps1（整轮验收运行器）
```

| 脚本 | 覆盖 |
|---|---|
| `tools/monitor/check.ps1` | 窗口、进程列表、排序、搜索、聚合开关 |
| `tools/monitor/sort.ps1` | 三列排序（CPU / 内存 / 名称） |
| `tools/monitor/align.ps1` | 表头与数据列右边缘对齐（物理像素） |
| `tools/monitor/menu.ps1` | 命令栏菜单 |
| `tools/monitor/apply.ps1` | 设置里改的值真的作用到运行时（读 ini + 读列表） |
| `tools/monitor/dump.ps1` | UIA 树转储 |
| `tools/monitor/settings.ps1` | 设置窗口四项控件 + 改值 + ini 往返 |
| `tools/monitor/settings-reopen.ps1` | 设置窗口「开→关→**再开**」回归 + 内存阶跃 |
| `tools/monitor/mem.ps1` | 私有内存曲线（工作集/私有内存都记） |
| `tools/monitor/mem-rows.ps1` | 内存曲线与真实行数对齐 |
| `tools/monitor/mem-scroll.ps1` | 滚动是不是 +26 MB 阶跃的触发条件 |
| `tools/monitor/close-flow.ps1` | 关闭确认框全流程 T1~T9 |
| `tools/monitor/close-ab.ps1` | 真隐藏到托盘的内存 A/B |
| `tools/monitor/close-hide-dlg.ps1` | `CloseAction=1` 时隐藏不经过对话框 |
| `tools/monitor/close-mem-ab.ps1` | 静止 / 最小化-还原 / 真隐藏-显示 三组对照 |
| `tools/monitor/repro-close.ps1` | 最小复现：第三次 WM_CLOSE 为什么把进程弄没了 |
| `tools/monitor/tray-invoke.ps1` | 托盘交互（UIA `InvokePattern`，**推荐**：不需要坐标，溢出区照样能点） |
| `tools/monitor/tray-click.ps1` | 托盘交互（`SendInput` 合成点击）。本机点不动 shell → 报 **SKIP** 而不是 FAIL（见第 20 条） |
| `tools/monitor/diag.ps1` | trace 采集 |
| `tools/monitor/shot.ps1` | 截图 |
| `tools/common/mem-baseline.ps1` | **内存红线基线**：静止 / 两页互切 / 隐藏-显示 / 设置开合 / 托盘常驻 |
| `tools/service/check.ps1` | 服务列表、三列双向排序、搜索、筛选、详情面板 |
| `tools/service/create.ps1` | 创建服务的表单校验 + 真建出来 + 清理 |
| `tools/service/mem.ps1` | 服务侧干净实例内存 |
| `tools/service/admin.ps1` | 提权模型：应用是管理员、写操作直接生效、不弹提权框 |
| `tools/build/make-icon.ps1` | 生成 `resources/app.ico`（几何图形，可复现） |

### 验收脚本的输出契约

每个脚本都必须输出这三类标记，结尾再给一行汇总：

| 标记 | 含义 |
|---|---|
| `[PASS]` / `[FAIL]` | 断言结果 |
| `SKIP:` | 这一项**本机跑不了**（环境限制），既不算 pass 也不算 fail |
| `RESULT: pass=N fail=M` | 结尾汇总行，`fail>0` 时退出码为 1 |

**为什么要有这个契约**：没有它，「一切正常」和「脚本跑完但什么都没验成」
在验收板上长得一模一样 —— 都是 `ok=0 fail=0 EXIT0`。
本轮就有四个脚本是那样（`check` / `sort` / `menu` / `settings`），
它们原本只把看到的东西打印出来，从不断言，也从不返回非零。
现在补上了断言，板上才有数。

**「观察型」脚本不算失败**：`dump` / `diag` / `shot` / `repro-close` /
`mem*` / `close-ab` / `close-mem-ab` 这些是取数据和截图的，
它们按设计就没有断言，看日志和产物即可。
但**凡是有「应该成立」这种判据的，就必须有断言**。

### 脚本必须在提权 shell 里跑

**整轮怎么跑**：`tools/build/run-all.ps1` 驱动全部 24 个脚本，按标记统计
pass / fail / skip，并写一份汇总。

```powershell
# 整轮提权跑（不提权时自己重提一次，整轮只弹一次 UAC）
pwsh -NoProfile -File .\tools\build\run-all.ps1

# 非提权跑：驱动清单改成 asInvoker 的验证副本，不弹 UAC，
# 写操作脚本会报 SKIP 并写明原因
pwsh -NoProfile -File .\tools\build\run-all.ps1 -Unelevated

# 只跑一部分（-Only 是逗号串，见下面的坑）
pwsh -NoProfile -File .\tools\build\run-all.ps1 -Only 'monitor/close-flow,service/check'
```

**为什么**：UI Automation 看不到跨完整性级别的界面（451 vs 4，见第 2 节）。

**怎么做到**：`tools/common/common.ps1` 的 `Require-Elevated` 在检测到当前 shell
不是管理员时，会把脚本自己用 `-Verb RunAs` 重启一遍，并把子进程的输出与退出码
回传给原 shell。具体做法是：父进程写一个临时 wrapper + 一个 JSON（装的是
`$PSBoundParameters`，**不是 `$args`** —— `$args` 只装没绑到具名参数的参数，
`check.ps1 -Seconds 120` 那样调用会把 `-Seconds` 丢掉），提权启动 wrapper；
wrapper 用 `*>` 把子脚本的输出收进临时日志，用一个 `.code` 文件回传退出码。

**退出码为什么不走 `Start-Process -Wait`**：`-Wait` 等的是**整棵子孙进程树**，
不只是它启动的那个进程。`settings.ps1` 跑完不杀应用（它压根没这个收尾逻辑），
于是父进程会永远等下去 —— 脚本自己早就跑完并打印了结果。改成 `.code` 文件回传，
父进程就不依赖树什么时候散。

**逃生口**：`LINGWU_TOOLBOX_UNELEVATED=1` 会让脚本跳过提权（并大声警告）。
这是给「UAC 确认框没人能点」的机器用的：此时脚本驱动的是一个
**清单被改成 `asInvoker` 的验证副本**，UI 验收照常能跑，
但**所有写操作都会被 OS 拒绝**，服务侧的写操作断言不成立。
`LINGWU_TOOLBOX_EXE` 可以指定跑哪个 exe。

### UIA 验收的铁律

1. **跨进程枚举必须 `AndCondition(ControlType, ProcessId)`。** 只用 `AutomationId`
   会匹到桌面上第一个同名窗口，那通常**是别的应用**。
2. **不要让 `Reason()` 守卫 `TextChanged`。** 见第 5 节第 10 条。
3. **ListView 是虚拟化的，`ListItem` 数量不能当断言。** 列表里有 345 条，UIA 只枚举到
   已实现的那 ~37 行，而且这个数字**会随滚动增长** —— 拿它做「操作前 / 操作后」比较
   实际测的是滚动，不是筛选。**要问「列表里有多少条」就读状态栏。**
4. **ComboBox 的项只在弹出时才是 UIA 子节点。** 关着的组合框 `FindAll` 永远返回 0，
   必须先 `ExpandCollapsePattern.Expand()`。
5. **`Expander` 折叠时子节点完全不可达。** `FindFirst` 返回 null，即使对象存在、应用本身
   也能调到它。要读里面的字段先 `Expand()`。
6. **`ContentDialog` 的树是异步物化的。** 刚弹出的那一刻拿到的按钮可能是
   「正文 TextBlock 被当成按钮 + WinUI 默认的 Close」，一秒后才变成真正的按钮。
   **连采两次、一致了再用**（`Get-DialogButtons`）。

另外三条：

7. **脚本要用 `try/finally` 收尾。** 断言抛出后没有 `Stop-App`，进程会占住 exe，
   下一次 `build.bat` 报 `LNK1104 无法打开文件` —— 那个错指向链接器，而真正的原因是上一轮
   验收脚本没收尾。
8. **PowerShell 会把集合展开。** `return $AE::RootElement.FindAll(...)` 在只找到一个窗口时
   返回的是**那个元素本身**而不是集合，于是 `$w.Count` 抛「找不到属性 Count」。
   一律写 `return ,$collection`（逗号防展开）。
9. **带中文字面量的 `.ps1` 只能用 `pwsh` 跑，不能用 `powershell`（5.1）**：
   这些脚本是 UTF-8 无 BOM，而 5.1 按系统 ANSI（936）读，中文会被切碎后吞掉引号，
   报的是 `ParserError` /「表达式或语句中包含意外的标记」，
   指不到真正的原因（实测 `close-flow.ps1` 一上手就踩这个）。
   正确写法：`pwsh -NoProfile -File .\tools\monitor\close-flow.ps1`。

### 托盘、命名与「脚本卡住」（2026-10-23 Phase 6 实测新增）

10. **托盘按钮可能在「可见区」也可能在「^ 溢出浮窗」里，而且名字可能过期。**
    两条互相独立的事，验收脚本必须两条都对付上：
    - **位置**：Win11 把新托盘图标默认收进 ^ 溢出区。溢出浮窗是**另一个顶层窗口**
      （类名 `TopLevelWindowForOverflowXamlIsland`），**只有打开时它的条目才在
      UIA 树里**。应用只能靠写 `HKCU\Control Panel\NotifyIconSettings 下的 IsPromoted` 这个未公开字段求提升，而 Explorer 建那条登记项是**异步**的，
      而且不是每台机器都建（实测这台机器上它压根不建）。
      所以**只查 `Shell_TrayWnd` 会把「图标在那儿」报成「图标没了」**。
      共享件 `tools/common/tray.ps1` 的 `Get-ToolboxTrayButton` 两条路都走：
      先查可见区，找不到就**用 UIA Invoke 点开溢出浮窗**再找，**并且把浮窗关回去**
      （实测点两下 chevron 就能开关，调用方不用管桌面状态被改成什么样）。
    - **名字**：Explorer 按 `(exe 路径, uid)` 建登记项，`InitialTooltip` 只在
      **第一次**登记时写一次，之后应用再怎么改 tip 都不刷新。实测这台机器上
      合并前的 Phase 0 构建在 `dist\Release\LingwuToolbox.exe` 与
      `build\unelevated\...` 两个路径下各留了一条登记项，tip 还是
      **「Lingwu SysMon · 系统监控」**；于是 `Shell_TrayWnd` 里那个
      `SystemTray.NormalButton` 的 UIA Name 显示的是旧名，而按当前 tip 精确匹配的
      `Find-TrayButton` 一律返回 null —— 表现成「托盘图标没了」，其实图标好好地在那儿。
      **所以按产品前缀匹配（`-like '*Lingwu*'`），不要匹配精确 tip。**
    - 清理陈旧登记项：删掉 `HKCU\Control Panel\NotifyIconSettings` 下
      `ExecutablePath` 指向本仓库路径的那几条。注意删掉之后**当次运行的 Explorer
      不一定会重建**（实测如此），所以别把「删了就能重建」当成保证。

11. **改了产品命名就必须重编，否则验收全打在旧二进制上。**
    Phase 5 那次「产品命名同步」改了 `app.rc` 的 `ProductName` / AUMID **和**
    托盘 tip，但 `dist\Release\LingwuToolbox.exe` 还是 12 分钟前构建的那份。
    于是托盘相关的验收全在测**旧契约**（旧菜单项、旧 tip），
    症状是「改了托盘菜单但界面没变」。
    **判据：验收前先确认 `dist\Release\LingwuToolbox.exe` 的时间戳比源码新。**
    想确认二进制里到底有没有某个字符串，直接在 exe 里搜 UTF-16 字节 ——
    比读源码可靠，因为源码可能已经不是构建时的那份。

12. **PowerShell 函数自调用 = 无限递归，而且不报错。**
    `Wait-TrayButton` 里写 `Wait-TrayButton` 会无限递归：既不抛异常也不超时，
    脚本表现为「卡住不动」，很容易被误判成环境问题（合成点击不可信、UIA 跨进程不可见……）。
    **脚本卡住时，先 grep 一遍自己有没有函数自调用。**

13. **`pwsh -File` 传数组参数会被压成逗号串。**
    `pwsh -NoProfile -File run.ps1 -Only 'a','b'` 传进去的是**一个**字符串 `"a,b"`
    （`-File` 的尾部参数按纯脚本参数解析，不会组数组），于是 `-Only -contains` 一个都匹配不上，
    **整轮静默地跑了零个脚本**。要么在脚本里 `-split ','`，
    要么用 `pwsh -Command` 传真正的数组。

14. **残留的「手拼 exe 路径」会污染统一解析。**
    脚本里写 `if (-not $Exe) { $Exe = Join-Path $PSScriptRoot '..\dist\Release\...exe' }`
    这种行看起来是死代码，实际**有害**：它在 `Resolve-ToolboxExe` **之前**把 `$Exe`
    填成了一个（搬家后就不存在的）路径，`Resolve-ToolboxExe` 看到非空就当成显式指定，
    于是跳过 `$env:LINGWU_TOOLBOX_EXE` 兜底并抛「找不到 exe」。
    **路径解析只能有一处**（`tools/common/common.ps1` 的 `Resolve-ToolboxExe`），
    其余脚本一律 `Resolve-ToolboxExe $Exe`，不要再自己拼。
15. **`$Host` 是 PowerShell 的只读自动变量，不能拿来当参数名。**
    `function Get-thing($Host)` 会在函数体里报
    「无法覆盖变量 Host，因为它是只读变量或常量」，
    而 `$ErrorActionPreference = 'Stop'` 会让整个脚本当场退出 —— 报出来的错
    指向参数赋值，跟「托盘图标为什么找不到」毫无关系。
    踩过：`tools/common/tray.ps1` 的 `Get-ToolboxTrayButtonIn($Host)`。
    **参数名一律避开 `$Host` / `$PID` / `$PSItem` / `$Error` / `$args` 这类自动变量。**

16. **Explorer 不一定为托盘图标建 `NotifyIconSettings` 登记项。**
    `platform::TryPromoteTrayIcon` 依赖那条登记项存在才能写 `IsPromoted`。
    实测 2026-10-23：删掉本仓库路径对应的登记项之后，**Explorer 当次运行没有重建**，
    于是提升连续试 12 轮（≈4.8 秒）都找不到任何一条，图标就一直留在 ^ 溢出区。
    应用侧已经改成「定时器重试 + 写完重新 NIM_ADD 一次让 Explorer 重读」，
    但**不能依赖它一定成功** —— 所以验收脚本必须自己能在溢出区里找到图标
    （见第 10 条与 `tools/common/tray.ps1`）。

17. **不要删 Explorer 的 `NotifyIconSettings` 登记项。**
    为了清掉「tip 还是旧名」的陈旧值，2026-10-23 删掉了本仓库路径对应的两条登记项 ——
    结果比陈旧 tip 严重得多：**Explorer 在本次运行中不会为它已经认识的图标重建那条登记项**
    （实测：删掉之后，应用连续重试 12 轮都找不到任何一条）。
    于是 `IsPromoted` 那套提升手段彻底没有落点，图标就一直留在 ^ 溢出区，
    而**在溢出区里点图标不会唤回主窗口**（左键只会被浮窗吃掉），
    于是「托盘左键唤回窗口」这条验收就再也过不去了。
    **陈旧 tip 的正确解法是匹配产品前缀**（见第 10 条），不是删登记项。
    删登记项要能回滚：把 `UID` / `ExecutablePath` / `InitialTooltip` / `IsPromoted`
    原样写回去（键名是 Explorer 算出来的哈希，从删除前的探测输出里抄），
    下一次 `NIM_ADD` 时图标就会回到可见区。

18. **溢出浮窗里的托盘元素：矩形只在浮窗打开期间有效。**
    实测 2026-10-24：`TopLevelWindowForOverflowXamlIsland` 里的
    `SystemTray.NormalButton` **是有屏幕矩形的**（80×80 @ 3067,1710，
    屏幕 3840×2160）—— **前提是浮窗还开着**。浮窗一关，元素跟着浮窗一起死，
    `BoundingRectangle` 就变成 **NaN / Infinity**，`[int]$r.X` 于是抛
    「无法将值 NaN 转换为类型 System.Int32」。
    两个后果：
    - **要按坐标点它，就必须让浮窗保持打开**，点完再关。
      共享件因此提供 `Get-ToolboxTrayState -KeepOpen`
      （`Ctx` 字段带回浮窗上下文，调用方负责 `Close-ToolboxTray`）。
    - **UIA Invoke 不需要坐标**，所以 `tools/monitor/tray-invoke.ps1`
      在溢出区照样能把主窗口唤回来 —— 这是本机唯一走得通的托盘点击路径。
    - 而且**有坐标也不代表点得动**：Win11 的托盘是 XAML island 弹窗，
      实测 `SendInput` 合成右键打在上面，`#32768` 弹出菜单一个都没有。
      所以 `tools/monitor/tray-click.ps1` 的判据是「点完有没有弹出菜单」：
      弹出了就继续验菜单内容，没弹出就报 SKIP 并附证据（第 20 条）。

19. **把「找元素」改成「问在不在」之后，所有调用点必须一起改。**
    旧 API 返回 UIA 元素，新 API `Test-ToolboxTrayIconPresent` 返回 **bool**。
    只改一半的话，调用点会变成 `$tray.GetCurrentPattern(...)`，
    在 `[System.Boolean]` 上抛
    「Method invocation failed because [System.Boolean] does not contain a method named 'GetCurrentPattern'」，
    而 `$ErrorActionPreference = 'Stop'` 会让脚本**当场退出** ——
    报出来的错跟「托盘图标为什么找不到」毫无关系。
    同一个坑还有断言：旧写法 `$null -eq (Find-TrayButton)`
    在函数改成返回 bool 之后**恒为 false**，于是「托盘图标已撤销」这类断言
    永远失败，而且是**静默地**失败。
    **改动共享 API 的返回类型时，把所有调用点 grep 一遍再跑。**

20. **「环境跑不了」要报 SKIP，不能报 FAIL。**
    `tools/monitor/tray-click.ps1` 依赖 `SendInput` 合成鼠标点击，
    本机点不动 shell 的 XAML island 托盘（点了没有任何弹出菜单）。
    那是**环境限制，不是产品缺陷**，报 FAIL 会把验收板染红并误导后续判断。
    共享件 `Get-ToolboxTrayState` 把「在不在」和「有没有矩形」分成两个字段，
    脚本据此在「点了但 shell 没反应」时输出 `SKIP: ...` 并以 0 退出。
    **验收脚本的三类标记约定**（整轮运行器按它统计）：
    - `[PASS]` / `[FAIL]` —— 断言结果
    - `SKIP:` —— 这一项**本机跑不了**（环境限制），既不算 pass 也不算 fail
    - 结尾一行 `RESULT: pass=N fail=M`，`fail>0` 时退出码为 1
    **同理：某个脚本因为「本机没有管理员权限」跑不了，也要单独标出来，
    别混进 FAIL 里。**

21. **`GetCurrentPattern` 要的是 `AutomationPattern` 实例，不是类型。**
    写成 `$script:TrayIP = [System.Windows.Automation.InvokePattern]`
    再 `GetCurrentPattern($script:TrayIP)` 会抛：
    > 无法将类型「System.Windows.Automation.InvokePattern」转换为类型
    > 「System.Windows.Automation.AutomationPattern」
    > 「值「System.Windows.Automation.InvokePattern」无法转换为类型
    > 「System.Windows.Automation.AutomationPattern」

    正确写法是 **`::Pattern`**（那是 `AutomationPattern` 的静态实例）：
    ```powershell
    $script:TrayIP = [System.Windows.Automation.InvokePattern]::Pattern
    ```
    这个坑特别坑，因为它**只在第一次真的 Invoke 的时候才炸** —— 共享件里
    `Invoke-TrayChevron` / `Invoke-ToolboxTrayButton` 全都走这条路，
    于是「打开溢出浮窗」这一步直接抛异常，脚本当场退出，
    而报出来的错跟「托盘图标为什么找不到」毫无关系。
    **凡是给 `GetCurrentPattern` 传参的地方，一律 `::Pattern`。**


### `tools/common/tray.ps1` 的 API 契约

| 函数 | 返回 | 语义 |
|---|---|---|
| `Test-ToolboxTrayIconPresent` | bool | 图标在不在（可见区**或**溢出区）。自己开关浮窗 |
| `Invoke-ToolboxTrayButton` | bool | 左键语义 Invoke 图标（溢出区照样能用）。自己开关浮窗 |
| `Get-ToolboxTrayRect` | hashtable / `$null` | 屏幕矩形。浮窗关闭后元素失效 → `$null` |
| `Get-ToolboxTrayState` | object | `Present` / `Rect` / `Overflow` / `Ctx`，一次开合搞定。默认关浮窗 |
| `Get-ToolboxTrayState -KeepOpen` | object | 同上，但**保持浮窗打开**，`Ctx` 带回上下文，调用方负责 `Close-ToolboxTray` |
| `Get-ToolboxTrayButton` | object | `Button`（UIA 元素）+ `Overflow`。**浮窗保持打开，调用方必须 `Close-ToolboxTray`** |
| `Test-UiaInvokeSupported` | bool | 这个元素支不支持 `InvokePattern`（不触发它） |
| `Invoke-UiaInvoke` | bool | 对元素做 `Invoke`（不支持就 `$false`，不抛） |

**三条纪律**：
1. 要「在不在」用 `Test-ToolboxTrayIconPresent`，要「点它」用 `Invoke-ToolboxTrayButton`，
   要坐标用 `Get-ToolboxTrayState -KeepOpen`。
   **不要自己拿元素再去 `GetCurrentPattern`** —— 那正是第 19、21 条踩的坑。
2. 只有 `Get-ToolboxTrayButton` 和 `Get-ToolboxTrayState -KeepOpen` 会把浮窗留在打开状态，
   其余自己关。
3. 匹配一律用产品前缀（`-like '*Lingwu*'`），不要匹配精确 tip（见第 10 条）。

### 其它验证要求

**点标题栏 `X` 的验收一律用 `PostMessageW(hwnd, WM_CLOSE)`**，不要试图合成点击。
它与点 `X` 走同一条 `AppWindow.Closing` 路径（反过来正是第 5 节第 7 条成立的依据：
子类拦 `WM_CLOSE` 拦不住，说明它确实进了 XAML 层并抛出 `Closing`）。
HWND 从 UIA 的 `NativeWindowHandle` 取。

**托盘按钮找不到时必须重试并明确报错，不能 `if ($tray) { Invoke }` 就跳过** ——
「脚本没去点」和「点了但没反应」在输出里长得一模一样。

**本机无法用合成鼠标输入做点击取证**（实测：`mouse_event` / `SendInput`
点不动任何系统控件）。**托盘交互一律用 `tools/monitor/tray-invoke.ps1`
（UIA `InvokePattern`）。**
**不要把「模拟点击后没反应」当成 bug 证据**，要换成 UIA 断言 + 读可观测状态
（`IsIconic` / `ToggleState` / ini / 注册表 / UIA 枚举托盘按钮）。

> `tray-click.ps1` 曾经「能找到按钮坐标然后永久卡住」，那个现象曾被记成
> 「合成点击不可信」的证据。**真因是脚本自己的 bug**：`Wait-TrayButton` 的循环体里
> 调的是 `Wait-TrayButton` 自己而不是 `Find-TrayButton` —— PowerShell 的函数自调用
> 是**无限递归**，既不报错也不超时，脚本就那么挂着。
> 这个笔误从迁移前就在（原 `tools/trayclick.ps1` 同样一行），之前每次跑它都先被
> 更早的 exe 路径问题挡住，所以从没暴露过。**教训：脚本「卡住」时先怀疑自己的递归，
> 不要急着归咎于环境。**

**右键菜单文本已于 2026-10-23 live 验证**（此前是已知缺口）。
办法：给托盘模块临时加一个「把菜单弹到固定坐标」的消息钩子（本次已随对照
脚手架一并删除），再用 DPI 感知的屏幕截图把菜单那一块裁出来。
实测菜单文本与契约逐字一致，勾选标记随当前模块切换而移动。
这个办法以后还会用到，值得再写一次（截图必须先设
`SetProcessDpiAwarenessContext(PerMonitorV2)`，否则坐标错位）。

**设置窗口类改动必须跑 `tools/monitor/settings-reopen.ps1`**。
它覆盖「开 → 关 → **再开**」这条路径 —— 而「第二次打开」正是 2026-10-20
那个必现崩溃的**唯一**触发条件。既有 `settings.ps1` 只开一次设置窗口、
从不重开，所以那条路径从来没有被任何测试覆盖过，这就是那个 bug 一直没被
发现的原因。
等价路径论证：托盘菜单「设置…」与主窗口的「设置」按钮调用的是**同一个**
`MainWindow::OnOpenSettings()`，所以用主窗口按钮做自动回归即等价覆盖托盘路径的根因。

**非交互 shell 里 GUI 进程活不过单次调用**：`Start-Process` 起的 GUI 进程会在启动它的
那次 Bash/PowerShell 调用结束时被回收，所以「启动 → 操作 → 断言」必须写在同一个脚本里。

**诊断开关用标记文件**（`%LOCALAPPDATA%\LingwuToolbox\<name>.on`，存在即开），
不要用环境变量 —— 非交互 shell 里 `$env:` 传不到 `Start-Process` 起的子进程。

**删文件必须用 `rm --`（`Remove-Item` 会被安全策略拦截），且 **`rm --` 里的路径必须是
字面量，不会展开 PowerShell 变量**。

**读文件内容用 read 工具，不要用 PowerShell `Get-Content` 复核（代码页 936 会把 UTF-8
中文显示成乱码）。MSBuild 日志是 GBK，要 `[System.Text.Encoding]::GetEncoding(936)` 读。

## 12. 平台兼容

- 最低支持 **Win10 1809 (17763)**；目标 SDK `10.0.26100.0`，
  `WindowsTargetPlatformMinVersion` 锁 `10.0.17763.0`
- **Win11 专属能力一律运行时门控**，统一收敛到 `src/platform/window_decor.h`
  （自实现 `IsAtLeastBuild`，不用 `VersionHelpers.h` 的 `IsWindowsVersionAtLeast`，
  后者在未打包应用里不带 manifest 会失灵）
- 门控不过就**静默跳过**，不视为错误 —— `DwmSetWindowAttribute` 失败时不会告诉你
  「你调错了」，只会什么都不发生
- `resources/app.manifest` 里的 `supportedOS` 是未打包应用必须声明的，缺它会导致
  平台 API 行为降级；`PerMonitorV2` 与运行期声明必须一致
- **Win10 1809–1903 无实测环境**，交付时必须明示这一条

## 13. 副作用

程序会写这些位置，测试后要清理：

- `%APPDATA%\LingwuToolbox\settings.ini`
- `%LOCALAPPDATA%\LingwuToolbox\trace.log`（仅 `trace.on` 存在时）
- `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `LingwuToolbox` 值（仅自启动开启时）

**测试开机自启后必须清除注册表项**，否则会在用户机器上留下真实自启。

trace 的打开方式：环境变量 `LINGWU_TRACE` 非空，**或**存在标记文件
`%LOCALAPPDATA%\LingwuToolbox\trace.on`。
**要取证就用标记文件** —— 非交互 shell 里 `$env:` 不一定能传到 `Start-Process`
起的子进程（实测在本 agent 的 shell 里就传不过去，trace 静默不写，
极易误判成「代码没执行」）。

**trace 是窄 `vsnprintf`：打印宽字符串要用 `%ls` 不是 `%s`。**
写错的表现是服务名只显示第一个字符（`ADPSvc` 显示成 `A`），看起来像「名字被截断了」，
实际是格式化串用错了。

## 14. 需求变更

用户明确要求「不确定的必须跟我确认，不能妄自猜测，不要擅自做决定」。
以下情况先问再做：新增依赖 / 改变内存口径 / 改变统计范围 / 改交互流程 / 改默认设置。

## 15. 用户确认过的产品决策（不要擅自改）

| 决策 | 结论 |
|---|---|
| 程序形态 | 单 exe · 单窗口 · 导航切换两个模块 · 常驻托盘 |
| 权限模型 | **整个工具箱默认以管理员身份运行**（manifest `requireAdministrator`） |
| 提权入口 | **没有「以管理员身份重新启动」**（程序本来就是管理员） |
| 导航样式 | 标题栏居中的**药丸分段**，选中项整块填充强调色 |
| 关闭窗口时 | 三态下拉：每次询问 / 总是隐藏到托盘 / 总是退出程序 |
| 设置窗口版式 | 「关闭窗口时」用**下拉**；「重新添加托盘图标」在**标题栏右侧**；固定 590×560 逻辑像素 |
| 托盘菜单 | 五项，模块项**带勾选态**标记当前页 |
| 功能范围（服务） | 列表 + 只读详情 + 启停重启 + 启动类型切换 + 创建 + 删除 |
| 运行账户 | 只支持 `LocalSystem` / `LocalService` / `NetworkService`，**不支持自定义账户与密码** |
| 依赖服务 | 不支持多选，`lpDependencies` 传 `nullptr` |
| 默认视图 | 全部服务（与 `services.msc` 一致） |
| 详情面板 | **默认折叠**，不自动展开 |
| 内存红线 | **私有内存 220 MB**（工作集只记录，不卡）—— 2026-10-24 在 Phase 6b 实测基线（稳态 201.3 MB）上重新拍板 |
| 版本 | 从 `0.0.1` 起 |
| 验收脚本目录 | **子目录 + 去冗余前缀**：`tools/common/` `tools/monitor/` `tools/service/` `tools/build/` |
| 非提权 shell 跑验收 | **自动提权重跑**（`-Verb RunAs` 重启自己，输出重定向到临时日志，子进程结束后回显日志并透传退出码） |
