# AGENTS.md —— lingwu-sysmon 开发约束

面向在本目录内改代码的 AI agent / 人类开发者。**动手前必读。**

本目录是 C++/WinRT + WinUI 3 实现，版本 **0.0.1**（本项目唯一版本）。

## 构建

```bat
build.bat
```

产物 `dist\LingwuSysmon.exe`。脚本通过 `vswhere` 定位 MSVC，缺依赖时不要自己拼 `cl` 命令行。

改动源码后必须**跑一次 `build.bat` 并确认零错误零警告**（`/W3 /WX`）。
`/MT` 静态 CRT 是硬要求，复验方式见 README 踩坑 15。

## 硬性约束

这些是用户明确提出的预算，不要在没问过的情况下突破：

- **应用本体 ≤ 8 MB**（当前 exe 0.60 MB，`dist\` 合计 0.985 MB）
- **常驻内存 ~145 MB 红线**（2026-10-14 由 ~132 上调，理由见下面「隐藏/显示的内存代价」；
  当前实测见下；口径见「内存红线」）
- **CPU 自耗低**（默认 1 秒间隔；窗口隐藏到托盘时列表 UI 完全不刷新，见下）
- **免安装绿色**：unpackaged + 框架依赖部署，不写 Program Files，除自启动项外不碰注册表

### 内存红线（用户 2026-10-13 拍板）

**两个指标都记录，但红线只卡私有内存（PrivateMemorySize64）。**

工作集包含从 Windows App Runtime 框架包映射进来的 DLL 页，而这些页**可跨进程共享**。
把红线卡在工作集上，等于惩罚「用了框架」，而框架依赖部署正是本项目的既定方向。
实测同一版 WinUI 3 空壳：WS=102 MB / Private=62 MB，差的那 40 MB 几乎全是
`Microsoft.UI.Xaml.dll` 等框架 DLL 的共享页。

**怎么报**：验收表里两列都给数字（工作集 / 私有内存），达标判定写在私有内存那一列，
并在表下用一句话说明差额来源。不要只报其中一个，也不要用工作集当红线。

### 已知内存基线

**当前构建实测**（Win11 26H2 / 200% DPI / 本机 422 进程 / 内存↓ 默认档 / 180 秒观测）：

| 排序档 | 私有提交 | 工作集 | 测量日期 |
|---|---|---|---|
| 内存 ↓（默认） | **131.3 MB** | 183.6 MB | 2026-10-14 |

下面这些数字是 **2026-10-13 那版 exe** 测的。exe 已在 10-14 09:26 重建过，
**它们已经过期，不要当成现状基线**，需要现状就重跑 `tools\mem.ps1`：

| 排序档 | 私有提交 | 工作集 | 状态 |
|---|---|---|---|
| 名称 A→Z | 96.7 MB | 150.6 MB | 10-13 旧构建，待重测 |
| 内存 ↓（默认） | 123.7 MB | 177.0 MB | 已被 10-14 的 131.3 MB 取代 |
| CPU ↓（排序键量化后） | 126.5 MB | 179.9 MB | 10-13 旧构建，待重测 |
| 关列表绑定 A/B 对照 | 91.5 MB | — | 10-13 旧构建，待重测 |

列表物化总成本约 31 MB，其中 26 MB 是一次性且有界的框架预热（文本布局与排版资源）。
**判据：阶跃时点飘忽 + 幅度恒定 + 之后永久走平 + 句柄数不变 ⇒ 框架一次性预热，不是泄漏。**
真泄漏会持续上升且通常伴随句柄增长。跑够长（≥180 秒）再下结论，不要看短时增长。

### 「隐藏 / 显示」窗口的内存代价（2026-10-14 四组同构建实测）

起因：主窗口改用 `AppWindow::Hide()` 真隐藏到托盘后，要查它到底要付多少内存。
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

**注意**：132 MB 红线本来就会被普通的最小化-还原循环越过（135.8 MB），
这是既有状况，不是本次改动引入的。真隐藏没有让这条线更糟。

**红线因此由 ~132 MB 上调到 ~145 MB**（用户 2026-10-14 拍板）。
依据就是上表：静止 130.6 MB，反复隐藏/显示 20 轮实测 139.0 MB。
真隐藏比最小化更省，所以上调红线**不是**为了迁就一个更贵的实现 ——
旧实现（点 X 最小化）同样会越线，只是当时没人测。

### 窗口隐藏时不做 UI 刷新（2026-10-14）

`Refresh()` 开头有一道 `IsWindowVisible` 闸门：窗口不可见直接返回。
理由：100 ms 的 `DispatcherQueueTimer` 照旧在跑，而 `Refresh()` 一路走到
`RebuildRows()` 的 `SetAt()`，会作废并重建列表容器 —— 窗口看不见时
一帧也用不上，却照付 CPU 与容器 churn。

数据不会过期：采样线程不受影响，`ShowAndActivate()` 里窗口一恢复可见
就补一次全量 `Refresh()`。开机自启的静默模式也走这条路（以前它同样在
后台每秒重建一个没人看的列表）。

**实测收益**：20 轮隐藏/显示的末端私有内存 144.4 → **139.0 MB**，
尾部 60 秒跨度 2.2 → 1.2 MB，即每轮开销 ~0.7 → **~0.42 MB**。

**踩坑**：判据只能用 Win32 的 `IsWindowVisible(hwnd_)` ——
XAML 那个 `Visibility` 在本工程**不存在**（见踩坑 8）。

### 设置窗口「构造一次 + 隐藏/显示」（2026-10-21，坑掉一次线性泄漏）

设置窗口改成**只构造一次**，关闭时只 `AppWindow().Hide()`（详见铁律 16 与
README 踩坑 24）。这一步不是洁癖，是**被实测逼出来的**：

| 实现 | 30 轮「开→关」后的私有内存 | 句柄数 |
|---|---|---|
| 每次打开都 `make<SettingsWindow>()` | 139.5 → **169.9 MB**（+30.4，**越过 145 MB 红线**） | 893 → **1272**（+379，线性不收敛） |
| 构造一次 + 隐藏/显示（现行） | 133.1 → **140.8 MB**（+7.7，曲线走平） | 868 → **942**（+74 后稳定，30 轮只动 1 个） |

判据用 AGENTS.md 自己那条规则：「真泄漏会持续上升且通常伴随句柄增长」。
对照实验（同样 120 秒）：**完全不开设置窗口**纯静置时句柄数 895 → 893 持平，
私有内存 103.9 → 131.9 MB（+28 MB = 一次性框架预热，见上文）——
所以那 +36 句柄/轮确实是「每轮新建窗口漏掉的」，不是应用稳态。

**教训**：WinUI 3 的 `Window` 关闭后不可复用，所以「关掉就重新 new 一个」
看起来是唯一出路 —— 但它每轮漏一整个窗口的原生资源（句柄 + 私有内存），
**无上界**。正确做法是**别让它被销毁**：`Hide` 代替 `Close`，
退出时用 `AllowClose()` + `Window::Close()` 让它真正关闭
（**`AppWindow::Destroy()` 不行** —— 它只销毁原生 HWND，框架的窗口计数不归零，
消息循环不结束，进程会僵留在「窗口没了但还在」的状态）。

### 窗口尺寸不是内存变量（2026-10-14 受控 A/B 实测）

主窗口默认尺寸从 1040×700 放大到工作区的 70%（本机 2015×1192，约 2 倍面积）之后，
私有内存 131.3 MB 越过了当时的 ~130 MB 红线。**但这不是窗口造成的** ——
同进程、等框架预热阶跃过去之后，只用 `SetWindowPos` 改窗口尺寸：

| 窗口 | 私有提交 | 句柄 |
|---|---|---|
| 2015×1192（默认，约 12 行可见） | 130.3 MB | 905 |
| 1040×700（缩回旧的，约 6 行可见） | 130.6 MB | 920 |

差值 **+0.3 MB，在噪声内**。原因：行数据本来就在那个 `IObservableVector` 里，
视口变化只决定「哪些行容器已实现」，**不复制数据**。

**教训：别再把「窗口变大」当成内存回归的候选原因，先做同进程受控 A/B 再下结论。**
（本次就先套用「多物化容器 = 多占内存」的直觉报了警，被 A/B 推翻。）

**工作集不可复现**：同一应用状态下 t=80s 的工作集在两次运行里分别是 182.1 MB 与
44.6 MB，而私有内存两次都稳在 130 出头。这正是当初把红线只卡私有内存的理由 ——
卡工作集必然误判。

**排序档之间的差异与 `SetAt` 次数无关。** 名称排序每帧也要换 74 个槽位
（新进程插入导致其后整体平移），内存却最低、连阶跃都没有；
CPU↓ 量化前 `replaced` 136~167、内存 132.4 MB。真正的变量是
**同一个槽位上是否反复换不同的行对象** —— 换行对象就要重建 DataContext
与文本测量缓存。这也是给 CPU 排序键加量化的动机（详见 README 第 19 节）。

## 平台兼容

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

## 源码约定

- 源码 UTF-8，编译必须带 `/utf-8`
- **`build.bat` 与所有 `.bat` 必须是纯 ASCII + CRLF**，混入中文会让 cmd 逐行丢字符
- **`.idl` 必须纯 ASCII**：midl 按系统 ANSI (936) 读，中文报 `C4819` 且撞 `/WX`
- **`.ps1` 不是纯 ASCII**：实测 `tools\` 下的脚本普遍带中文注释与中文字面量，
  编码是 **UTF-8 无 BOM + LF**。所以 `.ps1` 的约束是「**只能用 `pwsh` 跑**」
  （见下面「验证要求」），不是「必须纯 ASCII」。本仓库源码统一 **LF** 行尾
  （只有 `.bat` 是 CRLF）
- `.ps1` 里写带变量的中文字符串时注意 `"$i:"` 会被 PowerShell 当成驱动器限定
  变量，报「没有有效的文本终止符」，要写成 `"${i}:"`
- `/DNOMINMAX` 已在编译选项里，不要移除
- 每个 `.cpp` 第一行必须 `#include "pch.h"`（`/Yu"pch.h"` 找不到指令即 `C1010`）
- `core/metrics.h` 才有 `SortMode` 枚举，`core/settings.h` 没有。用 `SORT_*` 的文件
  必须 include `core/metrics.h` 并加 `namespace core = lingsysmon::core;`
- 需要原子读写的成员用 `volatile LONG`，**不要用 `bool`**
  （`Interlocked*` 是 4 字节操作，直接作用在 `bool` 上会越界写坏相邻成员）
- 布局一律交给 XAML + 系统 DPI，**不要写裸像素常量**（本机 200% 缩放）
- **命名空间分层**：`lingsysmon::core`（数据层，不依赖任何 UI 框架，可独立 `--selftest`）、
  `lingsysmon::platform`（纯 Win32）、`winrt::LingwuSysmon::implementation`（WinUI 3）
- 新代码注释写**为什么这么写**，不要复述代码在做什么。踩坑依据必须落在对应源码注释里

## 改动前必读的踩坑记录

README.md「实现要点与踩坑记录」列了 24 条实测踩坑。这里只列**最容易致命**的几条，
每一条都是「不报错但功能是坏的」类型：

1. **`Application::Start` 的回调里必须自己 `make<implementation::App>()`** ——
   漏了它 `/OPT:REF` 会把整个 App 类裁掉，症状是「进程活着但一个窗口都没有」。
   **判据：直接在 exe 里搜 `OnLaunched` 这几个字符。**
2. **`ItemsSource` 终身只赋一次**，之后就地增删同一个 `IObservableVector`
   （`RemoveAtEnd`/`Append` 调长度，`SetAt` 写内容）。换 ItemsSource 会让私有内存
   60 秒涨 37 MB 且不收敛。**禁用 `RemoveAt`+`InsertAt` 重排** —— 那是结构性变更，
   会让 ItemsRepeater 彻底停止实现容器（症状：数据 163 行、屏幕只剩 1 行）。
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
10. **自定义 Win32 消息在 WinUI 3 上收不到**（窗口过程由 WinUI 接管）。
   跨线程通知用「采样线程置位原子标志 + UI 侧 `DispatcherTimer` 轮询」。
11. **托盘宿主必须用 `HWND_MESSAGE` 消息窗口**，而它收不到 `HWND_BROADCAST` 广播，
   所以 `TaskbarCreated` 必须由主窗口子类接再转发。
12. **托盘左键按 `NIM_SETVERSION` 协商结果分流新旧通知码**。四类码一起接会双跑
    `ShowAndActivate`。`g_version` 必须挂在 `platform` 命名空间而非匿名命名空间。
13. **设置项必须能表达底层取值的全部范围**。排序下拉曾只列 3 项，导致 `MEM_ASC` /
    `CPU_ASC` 在设置里永远选不到，尽管它们能被主窗口表头设出来。
14. **单实例定位不能用类名**。WinUI 3 类名全系统统一
    （`WinUIDesktopWin32WindowClass`），要共享内存双份存 HWND。
15. **Win32 回调边界（窗口过程 / 菜单回调 / Shell 回调）必须包异常兜底**。
    C++ 异常从 `LRESULT CALLBACK` 逃出去，user32 / shell 层没有 C++ handler，
    CRT 只能 `std::terminate` → `abort()` → `__fastfail` → **0xC0000409**，
    进程直接消失且现场全丢（事件日志只有一条异常码，trace 里什么都没有）。
    统一用 `platform::GuardCallback(tag, fn)`（`src/platform/callback_guard.h`），
    tag 要能直接指出是哪一条路径。已覆盖：托盘 `WM_TRAY_CB`、托盘菜单五个
    菜单项、主窗口窗口子类三条分支、`AppWindow.Closing` 处理器。
16. **WinUI 3 的 `Window` 关闭后不可复用** —— `Close()` 之后对象仍在
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

## 设置文件契约（向后兼容，不许改）

- 路径 `%APPDATA%\LingwuSysMon\settings.ini`、段头 `[Settings]`、全部键名不变
- 活跃键：`Aggregate` / `SortMode`(0-4) / `IntervalMs`(250-10000) / `TrayHintShown`
  / `CloseAction`(0-2；0=每次询问 / 1=总是隐藏到托盘 / 2=总是退出程序)
- `AutoStart` **不落 ini**，注册表是唯一事实源
- 采样间隔的整数除数**必须在整数侧钳 ≥1**：`(uint64_t)0.001 == 0`，
  曾致启动 6 秒必崩 `0xC0000094`

## 托盘菜单契约

五项，顺序固定：显示主界面 / 按应用聚合（带勾选态）/ 分隔 / 设置… / 重新添加托盘图标 /
分隔 / 退出。托盘模块不持有「按应用聚合」状态的副本，每次弹菜单前通过 getter 问一次。

## 验证要求

改完必须实机验证，不能只看编译通过。`tools\` 下 20 个脚本全部走 UI Automation。

**带中文字面量的 `.ps1` 只能用 `pwsh` 跑，不能用 `powershell`（5.1）**：
这些脚本是 UTF-8 无 BOM，而 5.1 按系统 ANSI（936）读，中文会被切碎后吞掉引号，
报的是 `ParserError` /「表达式或语句中包含意外的标记」，
指不到真正的原因（实测 `closeflow.ps1` 一上手就踩这个）。
正确写法：`pwsh -NoProfile -File .\tools\closeflow.ps1`。

**点标题栏 `X` 的验收一律用 `PostMessageW(hwnd, WM_CLOSE)`**，不要试图合成点击。
它与点 `X` 走同一条 `AppWindow.Closing` 路径（反过来正是踩坑 7 成立的依据：
子类拦 `WM_CLOSE` 拦不住，说明它确实进了 XAML 层并抛出 `Closing`）。
HWND 从 UIA 的 `NativeWindowHandle` 取。覆盖见 `tools\closeflow.ps1`（T1~T9，
2026-10-14 实测 34/34 通过），内存代价见 `tools\closememab.ps1` 与 `closehidedlg.ps1`。

**托盘按钮找不到时必须重试并明确报错，不能 `if ($tray) { Invoke }` 就跳过** ——
「脚本没去点」和「点了但没反应」在输出里长得一模一样。首版就因为静默跳过，
把一次「窗口没回来」误报成疑似应用缺陷。`closehidedlg.ps1` 的 `Restore-ViaTray`
是正确写法：循环里同时检查窗口可见性与托盘按钮是否找到，失败时打印
`trayFound` 与 `invokeAttempts`。改完用 `-Cycles 20` 压一轮（实测 20/20 全过）。

**本机无法用合成鼠标输入做点击取证**（实测：`mouse_event` / `SendInput`
点不动任何系统控件）。`trayclick.ps1` 靠 `SendInput` 点托盘按钮，
脚本**能找到按钮坐标（1115,1527）然后永久卡住**，极易误判成「托盘坏了」。
所以**不要把「模拟点击后没反应」当成 bug 证据**，要换成
UIA 断言 + 读可观测状态（`IsIconic` / `ToggleState` / ini / 注册表 / UIA 枚举托盘按钮）。

**托盘交互一律用 `tools\trayinv.ps1`（UIA `InvokePattern`）**，不要用 `trayclick.ps1`。
`Shell_Tray.NormalButton` 实测支持 `InvokePattern`，且不依赖光标位置。
已通过的三条：最小化后 Invoke → 主窗口恢复（`IsIconic` 由 True 变 False）；
Invoke 不会误退出进程；托盘按钮始终在。
**右键菜单文本仍未经 live 验证** —— 弹出它必须真实点击，本机做不到；
菜单内容靠直读 `src/platform/tray_icon.cpp` 的 `ShowTrayMenu()` 核对
（五项契约见上一节）。这一条是已知的验证缺口，不要当成已验证。

**设置窗口类改动必须跑 `tools\uisettings2.ps1`**（2026-10-21 新增）。
它覆盖「开 → 关 → **再开**」这条路径 —— 而「第二次打开」正是 2026-10-20
那个必现崩溃的**唯一**触发条件。既有 `uisettings.ps1` 只开一次设置窗口、
从不重开，所以那条路径从来没有被任何测试覆盖过，这就是那个 bug 一直没被
发现的原因。
等价路径论证：托盘菜单「设置…」与主窗口的「设置」按钮调用的是**同一个**
`MainWindow::OnOpenSettings()`（前者经 `g_cb.onSettings`，后者经
`SettingsButton().Click`），所以用主窗口按钮做自动回归即等价覆盖托盘路径的根因；
托盘右键菜单本身的 live 验证仍是上面那条已知缺口，需要人工确认。
该脚本同时量「反复开关」的私有内存增量 —— 设置窗口改成「构造一次 + 隐藏/显示」
就是为了让这个增量不随轮数线性增长（详见 README 踩坑 24）。

跨进程 UIA 枚举**必须** `AndCondition(ControlTypeProperty, ProcessIdProperty)`，
绝不能只用 ControlType 在桌面根 FindAll 再按名字过滤（曾误点别的应用）。

**非交互 shell 里 GUI 进程活不过单次调用**：`Start-Process` 起的 GUI 进程会在启动它的
那次 Bash/PowerShell 调用结束时被回收，所以「启动 → 操作 → 断言」必须写在同一个脚本里。

诊断开关用**标记文件**（`%LOCALAPPDATA%\LingwuSysMon\<name>.on`，存在即开），
不要用环境变量 —— 非交互 shell 里 `$env:` 传不到 `Start-Process` 起的子进程。

删文件必须用 `rm --`（`Remove-Item` 会被安全策略拦截），且 **`rm --` 里的路径必须是
字面量，不会展开 PowerShell 变量**。

读文件内容用 read 工具，不要用 PowerShell `Get-Content` 复核（代码页 936 会把 UTF-8
中文显示成乱码）。MSBuild 日志是 GBK，要 `[System.Text.Encoding]::GetEncoding(936)` 读。

## 副作用

程序会写这些位置，测试后要清理：

- `%APPDATA%\LingwuSysMon\settings.ini`
- `%LOCALAPPDATA%\LingwuSysMon\trace.log`（仅 `trace.on` 存在时）
- `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `LingwuSysmon` 值（仅自启动开启时）

**测试开机自启后必须清除注册表项**，否则会在用户机器上留下真实自启。

## 需求变更

用户明确要求「不确定的必须跟我确认，不能妄自猜测，不要擅自做决定」。
以下情况先问再做：新增依赖 / 改变内存口径 / 改变统计范围 / 改交互流程 / 改默认设置。
