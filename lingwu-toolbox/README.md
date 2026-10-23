# 零物工具箱（v0.0.1）

一个常驻托盘的 Windows 系统小工具，两个模块共用一个窗口：

- **性能监视** —— 进程 / 应用的 CPU、内存、网络实时占用，一键结束任务
- **服务管理** —— Windows 服务的列表、详情、启停 / 重启、启动类型切换、创建、删除

**实现栈为 C++/WinRT + WinUI 3**，unpackaged 自包含分发，双击即用。

- 目标平台：Windows 10 1809 (17763) 及以上；Win11 专属能力（Mica、圆角、系统背板）按版本运行时门控
- 部署方式：unpackaged + Windows App SDK 框架依赖部署，无需安装、不写注册表（自启动项除外）
- 体积：exe 1.13 MB（1,182,208 字节），`dist\Release\` 合计 62 MB —— 其中 60 MB 是随包分发的
  WinUI 3 框架依赖（`Microsoft.UI.Xaml.dll` 等），不是应用代码
- **权限模型：整个程序默认以管理员身份运行**（manifest 声明 `requireAdministrator`）。
  服务管理模块要启停 / 删除服务，没有管理员权限根本列不全。
  **代价**：开机自启拉起时 Windows 会弹一次 UAC —— 这是提权模型的必然结果，不是 bug。
  UAC 弹在应用起来之前，所以用户先看到 UAC 确认框、确认后才看到托盘图标。
- 常驻内存（私有提交，本机 ~490 进程、345 个服务、Win11 26H2、200% DPI，
  2026-10-24 用 `tools/common/mem-baseline.ps1` 五个场景实测）：

  | 场景 | 私有内存 | 工作集 | 说明 |
  |---|---|---|---|
  | 只开性能页 | 107.0 MB | 161.8 MB | 单页基线 |
  | 只开服务页 | 113.8 MB | 167.4 MB | 单页基线 |
  | **两页都实现过（工具箱的正常工作状态）** | **197.4 MB** | **249.3 MB** | 稳态 |
  | 加上设置窗口开合 + 托盘常驻 | **201.3 MB** | **251.5 MB** | 全场景峰值 |

  **没有泄漏**：切页 60 轮，私有内存按阶段五等分的涨幅是
  **11.3 / 34.3 / 1.9 / −0.5 / 0.1 MB** —— 后 3/5 完全走平，
  涨幅全在头两个五分之一里（其中一次是 +26 MB 的一次性框架预热阶跃），
  句柄数 967 → 953 反而降了。

  红线判定**只卡私有内存**；工作集含 Windows App Runtime 框架 DLL 的
  跨进程共享页，不计入红线（见「数据口径」）。
  详见 AGENTS.md「内存红线与基线」。

## 功能

### 性能监视

- 列表四列：名称 / CPU / 内存 / 进程数。名称列展示为 `exe名（描述）`，如 `chrome（Google Chrome）`
- 两种口径随时切换：
  - **按应用聚合**（默认）—— 同一 exe 的多个进程合并成一行，进程数列显示组内数量
  - **逐进程** —— 每个进程一行
- 点表头排序，共 5 档，点「名称」列回退到默认排序：

  | 档位 | 表头指示 |
  |---|---|
  | 内存 ↓（高→低，默认） | 内存列箭头朝下 |
  | 内存 ↑（低→高） | 内存列箭头朝上 |
  | CPU ↓（高→低） | CPU 列箭头朝下 |
  | CPU ↑（低→高） | CPU 列箭头朝上 |
  | 名称 A→Z | 名称列箭头朝上 |

- 搜索框按进程名 / 应用名实时过滤，状态栏同步显示「N 进程 · M 应用」
- 状态栏右侧常驻：内存口径说明、下行 / 上行网速、参与统计的网卡名
- 右键列表行 → 「结束任务」：先发 `WM_CLOSE` 优雅退出，最多等 3 秒，仍活着的才强杀；
  权限不足时会提示需要管理员权限
- 深色 / 浅色主题跟随系统，Mica 背景与圆角在 Win11 上自动生效

### 服务管理

| 能力 | 说明 |
|---|---|
| 服务列表 | 默认显示全部服务（含已停止、已禁用），与服务管理器一致 |
| 搜索 | 按服务名或显示名筛选，大小写无关的**子串**匹配 |
| 状态筛选 | 全部 / 运行中 / 已停止 / 已禁用 / 第三方 |
| 排序 | 名称、状态、启动类型三列，**每列都可双向排序**，表头箭头指示方向 |
| 详情面板 | 可执行路径、运行账户、描述、依赖服务、退出代码。默认折叠，点标题展开 |
| 启动 / 停止 / 重启 | 单个服务，带等待态（不会点了没反应） |
| 启动类型切换 | 自动 / 自动（延迟启动）/ 手动 / 禁用 |
| 创建服务 | 服务名、显示名、描述、可执行路径、启动类型、运行账户、创建后立即启动 |
| 删除服务 | 分级确认：自家服务直接删；第三方服务且已停止时要求逐字输入服务名；运行中走「停止并删除」 |

**已知不支持**（有意为之，不是缺陷）：

- 自定义运行账户与密码 —— 只支持 `LocalSystem` / `LocalService` / `NetworkService` 三个内置账户。自定义账户需要用 `lpPassword` 把明文密码交给 SCM，本工具不这么做
- 依赖服务多选 —— `lpDependencies` 恒为 `nullptr`
- 驱动服务 —— 只列 Win32 服务（`SERVICE_WIN32`），不含内核驱动
- 服务恢复策略（失败后重启等）—— 只有 `services.msc` 的「恢复」标签页能做，不在本工具范围

### 托盘

- 左键单击：唤出 / 还原主界面
- 右键菜单五项：

  ```
  显示工具箱
  ─────────
  ✓ 性能监视          ← 切到性能监视页（当前页带勾选态）
    服务管理          ← 切到服务管理页
    设置…             ← 打开设置窗口
  ─────────
    退出
  ```

- Win11 默认把新图标收进任务栏 `^` 溢出区，首次运行会弹一次性气泡提示告诉用户去哪儿找
- Explorer 重启后自动重新注册托盘图标（`TaskbarCreated` 广播）

### 生命周期

- 点标题栏 `X`（或按 `Alt+F4`）= 按「关闭窗口时」的设置行事（见下）
- 「隐藏到托盘」是**真隐藏**：任务栏不留按钮，只剩托盘一个入口；
  程序继续在后台采样，托盘左键可把它唤回
- 托盘右键菜单「退出」保持一键直接退出，不弹确认框
- 开机自启动 = 登录后静默进托盘，不弹主界面（命令行开关 `--autostart`）
- 单实例：第二份启动会把第一份的窗口叫出来，然后自己退出

### 设置

| 设置项 | 取值 | 生效方式 |
|---|---|---|
| 按应用聚合 | 开 / 关 | 确定后立即生效 |
| 默认排序 | 内存 ↓ / 内存 ↑ / CPU ↓ / CPU ↑ / 名称 ↑ | 确定后立即生效 |
| 采样间隔 | 0.5 / 1 / 2 / 5 秒 | 确定后下一周期生效 |
| 开机自启动 | 开 / 关 | 确定后写 / 删 `HKCU\...\Run` |
| 关闭窗口时 | 每次询问 / 总是隐藏到托盘 / 总是退出程序 | 确定后立即生效 |

设置窗口标题栏右侧还有一个动作按钮：**重新添加托盘图标**（托盘图标被清理软件弄丢时的补救）。

设置保存在 `%APPDATA%\LingwuToolbox\settings.ini`，段头固定为 `[Settings]`。
自启动**不落 ini** —— 注册表是唯一事实源。

合并前两个工具各写各的 ini，两个都叫 `SortMode`，语义却完全无关 —— 合进同一份 ini 后
后写入的一方会把另一方覆盖掉。所以键名带模块前缀：
`MonitorAggregate` / `MonitorSortMode` / `MonitorIntervalMs` /
`ServiceSortMode` / `ServiceFilterMode` / `ServiceDetailExpanded` /
`TrayHintShown` / `WindowLeft` / `WindowTop` / `WindowWidth` / `WindowHeight` /
`CloseAction`。

旧路径 `%APPDATA%\LingwuSysMon\settings.ini` 与 `%APPDATA%\LingwuServiceManager\settings.ini`
只**读**，用于一次性迁移，保留不删。

### 关闭窗口时的行为（`CloseAction`）

| 值 | 含义 |
|---|---|
| `0` | 每次询问（默认） |
| `1` | 总是隐藏到托盘 |
| `2` | 总是退出程序 |

「每次询问」时弹确认框，框里可勾「记住我的选择，以后不再询问」，勾了之后点 `X` 直接执行、
不再询问。**三档在设置窗口的「关闭窗口时」下拉里都能选**（早期版本只能手改 ini 改回 0）。

手改 ini 写出越界值时，`LoadSettings` 会收敛回 `0`（宁可多问一次，
也不要给出一个 `MainWindow` 里没有对应分支的值）。

## 构建

```bat
build.bat
```

产物在 `dist\Release\LingwuToolbox.exe`。脚本通过 `vswhere` 定位 MSVC BuildTools，缺依赖时不要自己拼 `cl` 命令行。

`dist\Release\` 是**自包含**产物：exe + `.pri` / `.winmd` + Windows App Runtime 的框架 DLL
与语言资源（约 250 个文件 / 60 MB）。XAML 资源走 `.pri` 索引，`.xbf` 只是编译中间物，
不参与运行期加载。

依赖：VS 2026 BuildTools（MSVC 14.51）、Windows App SDK 2.5.1、NuGet 包
`Microsoft.WindowsAppSDK 2.5.1` + `CppWinRT 3.0.260818.1`（`build.bat` 带 `-restore` 自动拉）。
WinUI 3 工作流缺失时先跑 `tools\build\install-winui-workload.ps1`。

改完必须**跑一次 `build.bat` 并确认零错误零警告**（`/W3 /WX`）。

## 运行

```bat
dist\Release\LingwuToolbox.exe              # 正常启动，显示主界面
dist\Release\LingwuToolbox.exe --autostart   # 静默进托盘（开机自启动用的就是这条）
dist\Release\LingwuToolbox.exe --show        # 强制显示
dist\Release\LingwuToolbox.exe --selftest    # 只跑数据层自检，不起 XAML、不建窗口
```

`--selftest` 不初始化 COM、不拉 XAML 框架，用来验证数据层可独立运行
（性能监视 + 服务管理两套断言）。退出码非 0 即自检失败。

## 目录结构

```
lingwu-toolbox/
├── build.bat                   # MSBuild 入口（纯 ASCII + CRLF）
├── LingwuToolbox.vcxproj       # 工程文件（/MT 静态 CRT、/WX、/utf-8）
├── XamlMetaDataProvider.idl    # XAML 类型信息（纯 ASCII）
├── resources/
│   ├── app.rc                  # 图标资源 + 版本信息
│   ├── app.manifest            # PerMonitorV2、UTF-8 代码页、supportedOS、requireAdministrator
│   └── app.ico
├── src/
│   ├── pch.h / pch.cpp         # 预编译头
│   ├── main.cpp                # 入口、单实例、启动方式、Application::Start
│   ├── core/                   # 数据层（不依赖任何 UI 框架，可独立自检）
│   │   ├── monitor/            # 性能监视
│   │   │   ├── sampler.h/.cpp    # 采样线程：进程 CPU/内存、网卡速率
│   │   │   ├── metrics.h         # 数据模型、SortMode 枚举、格式化
│   │   │   ├── appnames.h/.cpp   # exe → 桌面显示名解析（两级缓存）
│   │   │   ├── terminate.h/.cpp  # 结束进程三段式
│   │   │   ├── top10.h/.cpp      # Top10 内存口径计算
│   │   │   ├── autostart.h/.cpp  # 注册表自启动 + 启动方式解析
│   │   │   └── selftest.h/.cpp   # 数据层自检
│   │   ├── service/            # 服务管理
│   │   │   ├── service_model.h   # 纯函数与数据结构（文案、判定、排序枚举）
│   │   │   ├── scm.h/.cpp        # SCM 原始调用层（本层只做 API→错误码）
│   │   │   ├── controller.h/.cpp # poll 线程 + op 线程 + 队列
│   │   │   └── selftest.h/.cpp   # 数据层自检
│   │   ├── settings.h/.cpp      # 两边共用的 ini 读写（路径 / 段头 / 键名是对外契约）
│   │   └── selftest.h/.cpp      # --selftest 的统一入口
│   ├── platform/               # 纯 Win32 层
│   │   ├── trace.h             # 标记文件诊断 trace
│   │   ├── window_decor.h/.cpp # Win11 窗口装饰的版本门控
│   │   ├── callback_guard.h    # Win32 回调边界的异常兜底
│   │   ├── single_instance.h/.cpp # 单实例（共享内存存 HWND）
│   │   └── tray_icon.h/.cpp    # 托盘图标、消息窗口宿主
│   └── ui/                     # WinUI 3 层
│       ├── App.xaml / .h / .cpp
│       ├── MainWindow.xaml / .idl / .h / .cpp   # 外壳：标题栏 + 导航 + 页面宿主
│       ├── MonitorPage.xaml / .idl / .h / .cpp   # 性能监视页
│       ├── ServicePage.xaml / .idl / .h / .cpp   # 服务管理页
│       ├── SettingsWindow.xaml / .idl / .h / .cpp
│       ├── CreateServiceWindow.xaml / .idl / .h / .cpp
│       ├── ProcessRow.idl / .h     # 行数据 + INPC + ICustomPropertyProvider
│       ├── ServiceRow.idl / .h     # 同上（服务侧）
│       ├── process_row_glue.cpp    # 循环依赖破解的自由函数
│       ├── service_row_glue.cpp
│       └── xaml_typeinfo.cpp       # XAML 元数据（显式关 /WX）
├── dist/
│   ├── Release/                  # 自包含产物：约 250 个文件 / 62 MB
│   └── package/                  # 分发件：单文件 exe 60.7 MB
├── packaging/                    # 单文件自解压壳
│   ├── sfx_main.cpp              # 释放逻辑（自定义 LSFX v1 负载格式）
│   ├── make-sfx.ps1              # 打包 dist\Release + 生成 sfx.rc
│   └── build-sfx.bat             # rc / cl / link（纯 ASCII）
├── tools/                        # 验收脚本（common / monitor / service / build）
│   ├── common/                   # 跨模块共享件
│   │   ├── common.ps1            #   提权自举 + exe 解析 + 进程启停
│   │   ├── uia.ps1               #   UIA 辅助件
│   │   ├── tray.ps1              #   托盘图标查找（可见区 + ^ 溢出浮窗，开了会关回去）
│   │   └── mem-baseline.ps1      #   内存红线基线（五个场景长跑）
│   ├── monitor/                  # 性能监视侧验收脚本
│   ├── service/                  # 服务管理侧验收脚本
│   └── build/                    # 构建工具（make-icon / install-winui-workload）
└── AGENTS.md                     # 给 AI agent 的硬约束与踩坑清单
```

## 数据口径

**性能监视**

| 指标 | 口径 |
|---|---|
| CPU | `NtQuerySystemInformation(5)` 全量采样 + 逐 tick 差分；单核口径可 >100%，展示用任务管理器口径（除以逻辑核数） |
| 内存 | `PrivatePageCount` **私有提交字节**。任务管理器「进程」页的私有工作集略高（含部分可共享页） |
| 网络 | `GetIfTable2`，只统计 `HardwareInterface` 且非回环、已连接的网卡，黑名单过滤虚拟网卡 |
| 进程身份 | PID 复用靠 `createTime` 区分，避免把新进程当成旧进程累计 CPU |

网卡枚举有节流，PDH 不可用时内存口径退化为私有提交（状态栏会注明）。

**服务管理**

| 指标 | 口径 |
|---|---|
| 服务集合 | 只列 Win32 服务（`SERVICE_WIN32`），不含内核驱动；枚举不按状态过滤（`SERVICE_STATE_ALL`） |
| 枚举方式 | `EnumServicesStatusW` 分页 + 对运行中的服务逐个 `QueryServiceStatusEx(SC_STATUS_PROCESS_INFO)` 取 PID / 标志 |
| 启动类型 | 每服务一次 `QueryServiceConfigW`，2 KB 起步、翻倍封顶 8 KB；首次约 345 次 RPC，稳态命中缓存为 0 次 |
| 权限状态 | 状态栏右侧显示「管理员权限 / 标准用户权限」。工具箱恒为管理员 |

## 实现要点与踩坑记录

这一节全是**实际卡过一轮**的记录。不要「顺手改回」这些地方，它们看起来不合理但都有实测依据。

### 1. `Application::Start` 的回调里必须自己 `make<implementation::App>()`

框架**不会**替你构造 `Application` 子类。官方把它写在 XAML 编译器生成的
`App.xaml.g.hpp` 的 `wWinMain` 里；自己提供入口点又不补这句，`implementation::App`
这个类**没有任何代码引用**，`/OPT:REF` 会把它的 vtable 和全部虚函数（含 `OnLaunched`）
连同字符串字面量一起裁掉。

症状极其隐蔽：链接照过、`Application::Start` 正常返回、消息循环正常跑、进程活着、
XAML 框架已加载，但**一个窗口都没有**，不崩溃、不写事件日志、不输出 stdout。
**判据：直接在 exe 里搜 `OnLaunched` 这几个字符**，搜不到就是被裁了。

配套：`ApplicationInitializationCallback` 是**单参数**委托。写成两参 lambda 也能过
重载解析，但第一个 `auto` 会被绑成 params，报「`OnLaunched` 不是
`ApplicationInitializationCallbackParams` 的成员」—— 错误信息完全指不到真正原因。

### 2. 经典 `{Binding}` 与 `x:Bind` 各缺一半，缺的那半都不报错

- `x:Bind` 是编译期绑定，凭 winmd 元数据判断源支不支持通知。`ProcessRow` 的
  `INotifyPropertyChanged` 只在 C++ 侧用 `ProcessRowT<ProcessRow, INotifyPropertyChanged>`
  的模板参数实现，进不了元数据（WinUI 的 NuGet 包不提供可 import 的
  `Microsoft.UI.Xaml.Data` winmd）。于是 XAML 编译器报 `WMC1506` 并把 `Mode=OneWay`
  **静默降级成一次性读取** —— 症状是状态栏在变、列表数值 9 秒纹丝不动。
- 经典 `{Binding}` 运行期订阅 INPC，纯 C++ 实现的 INPC 接得上；但它**不解析普通
  WinRT 对象的属性**，必须自己实现 `ICustomPropertyProvider` + `ICustomProperty`。
  缺了它的表现与「绑定写错路径」一模一样：行容器在、`Items().Size()` 正确、
  状态栏照常刷新，**就是一个字都不显示**。
- 实测 `{Binding}` 的路径解析最终走**元数据**（XamlTypeInfo），`GetCustomProperty`
  一次都没被调用，引擎只用 `Type()` 确认这是个自定义类型。
- `[bindable]` 属性在本版 MIDL/SDK 下被静默忽略（IDL 里加了编译通过、无警告，
  winmd 里没有 `BindableAttribute`），别在这条路上花时间。

### 3. `ICustomPropertyProvider::Type()` 写成 `Primitive` / `"String"` 会崩

那是 `ICustomProperty::Type()`（属性级）的写法。`ICustomPropertyProvider::Type()`
描述的是**源对象本身**，必须是 `TypeKind::Metadata` + 真实运行时类名（`L"LingwuToolbox.ProcessRow"`）。

写错的表现：启动数秒后进程以 `0xc000027b (STATUS_STOWED_EXCEPTION)` 崩在
`Microsoft.UI.Xaml.dll`，无托管异常、事件日志看不出成因。
**判据：同名接口上有个 `Type()`，先分清是属性级的还是 provider 级的。**

### 4. `ItemsSource` 终身只赋一次，之后就地增删同一个 `IObservableVector`

这是整个项目最贵的一条。两次实测，两次都别再退回去：

- **① 每秒换一个新 `ItemsSource`**：ListView 销毁并重建全部已实现容器，旧容器不及时回收，
  私有内存 125.8 MB → 162.5 MB / 60 秒且不收敛。关掉列表做对照，60 秒锁死在 91.4 MB。
- **② 不每秒换，但「key 集合一变就换」** —— 这一档更隐蔽，骗过一整轮。
  key 是「应用名 + 首个 pid」，而浏览器与系统后台子进程高频启停会让一个应用组里的
  **最低 pid** 经常易主，于是集合几乎每几秒抖一次。探针实测 60 秒内触发 **20 次**
  整表重建（rows 在 161↔163，常常只差 1 行），每次报废再重建 163 个容器。
  **差 1 行就重建 163 个容器，这个比例极端失衡。**

修法：`ItemsSource` 终身只赋一次，之后就地增删同一个 `IObservableVector`：
长度用 `RemoveAtEnd()` / `Append()` 调，内容用 `SetAt()` 逐槽写。滚动位置天然保留。

两个硬约束：

- 元素类型必须是 `IInspectable`。`IVector<hstring>` / `IVector<ProcessRow>` 一律抛
  `E_INVALIDARG(0x80070057)`，失败时事件日志只留下 `STATUS_STOWED_EXCEPTION @ CoreMessagingXP.dll`。
- 必须是 **`IObservableVector`**。`single_threaded_vector()` 返回普通 `IVector`，
  `RemoveAt/InsertAt/SetAt` 都**不发 `VectorChanged`**，ListView 完全看不见 ——
  表现为「排序在数据上生效了、UI 一动不动」，极易误判成「排序没实现」。
  要 observable 必须用 `single_threaded_observable_vector()`。

### 5. 重排用 `SetAt`，禁用 `RemoveAt` + `InsertAt`

`RemoveAt/InsertAt` 是**结构性**变更，会让 `ItemsRepeater` / `ItemContainerGenerator`
作废已实现区段。稳态下每帧几个交换还扛得住，但一次表头点击是近乎整体置换，
一次循环几百次结构变更会让它**彻底停止实现容器** ——
症状：「排序指示器正确、从进程内读回 `source_.Size()` 与 `Items().Size()` 都是 163，
屏幕上却只剩 1 行」。数据完全正常，纯 UI 不再实现。

修法：先按「两边是同一键集合 + 长度相同」证明是纯置换，然后对每个与目标顺序不符的
槽位 `SetAt(i, 目标行)` 一步到位。replace 语义不触发结构性失效。

### 5b. `ICustomProperty` 里存裸行指针，会在排序时随机崩（0xc0000005）

这是本项目最凶的一个 bug：实测 180 秒内崩 4 次，全部 `0xc0000005`，故障模块都是本 exe。
**它伪装成「测量脚本把进程杀了」**，前几轮一直被误判成脚本强杀，白放过了好几轮。

症状与定位：

- 事件日志（`Application` / Id=1000）里故障模块名是 `LingwuToolbox.exe`，偏移地址每轮都不同。
- 用 map + PDB 双向符号化后，4 次的崩溃点全落在
  `RowCustomProperty::GetValue`（`+0x10C` / `+0xC9` / `+0x184`），
  另一次崩在 `memcpy+0xC6`（下游：拿到垃圾 HSTRING 后复制踩空）。
- **map 文件的 `Rva+Base` 列已含首选加载基址 `0x140000000`**，
  拿裸 RVA 去比对会永远落在第一个符号之前，白折腾很久。

根因：`RowCustomProperty` 存了裸 `ProcessRow*`，注释写着「行对象比绑定活得久」——
这个假设是错的。第 N 帧 `row.Update()` 抛 `PropertyChanged`，绑定引擎**只打脏标记**，
真正的 `GetValue` 推迟到之后的 layout pass；而同一帧的 `SetAt` / `rowByKey_` 重建
把已退出进程的行引用计数清零、**当场释放**。等 layout pass 来读时指针已悬空。

试过并**否决**的两条路（别再走）：

- `get_strong()` 持强引用 → **成环**。行 → `m_changed(event)` → `delegate_array` →
  delegate 本身是强引用链（见 cppwinrt `base.h` 的 `event::add` → `make_agile_delegate`），
  COM 没有循环回收，等于永久泄漏。
- 退休缓冲区（`retired_` / `retiredPrev_` 双缓冲，多留两帧）→ 只把窗口推到两帧。
  节流后 `SetAt` 每 3 秒一次、绑定又跨多个应用周期存活，**第 4 例崩溃依然复现**。

正解：`GetValue` **改用引擎传进来的 `target` 参数取行** ——
QI 到投影类型 `winrt::LingwuToolbox::ProcessRow`，**一个行指针都不存**。
对象活着才可能被传进来，传进来就一定活着。`target` 为空就返回 null，
下一帧自愈，不崩。`MakeRowCustomProperty` 的参数随之从 `(ProcessRow*, RowProp)`
简化成 `(RowProp)`，循环依赖解除，只保留函数前向声明。

修完复测：CPU↓ 排序（此前崩过 4 次的配置）连跑 3 轮 180 秒，事件日志零崩溃。

### 6. `Microsoft.UI.*` 命名空间的委托一律接不上

cppwinrt 3.0.260818.1 生成的 `Microsoft.UI.Xaml.Input.h` 里，**所有** delegate 的模板
构造函数都漏了 `inline`（`PointerEventHandler` / `RightTappedEventHandler` /
`SelectionChangedEventHandler` …），只有 `Windows.Foundation` 那批
（`RoutedEventHandler` / `TypedEventHandler`）是好的。

直接接 → `LNK2019`；绕开 `make_delegate` → `base.h` 的 `C2027`「使用了未定义类型」。
所以右键菜单走 Win32 `SetWindowSubclass` + `WM_CONTEXTMENU` + 算术命中测试，
设置窗口的 ComboBox 值也不在选择时读，而在「确定」时一次性读回来。

配套：命中测试要按 `RasterizationScale` 把物理像素换算成逻辑像素 ——
`WM_CONTEXTMENU` 的 lParam 给物理像素，而 `TransformToVisual` / 行高全是逻辑像素。
两者只在 100% 缩放下相等，混用后行号会按缩放倍数算错。

### 7. `X` 按钮必须用 `AppWindow.Closing` 拦截，且**异常绝不能逃出去**

`SetWindowSubclass` 拦 `WM_CLOSE` **实测无效**（窗口照样关）。必须挂
`AppWindow().Closing` 并 `args.Cancel(true)`，本次关闭才不成立。
置位 `quitting_` 后放行，窗口才会真的关、`Closed` 才会跑。

**2026-10-14 新增的硬约束：`Closing` 处理器里任何异常都会让 `Cancel(true)` 作废。**
症状是「点 `X` 时窗口直接关了、进程退出」，与退出逻辑写错完全无法区分，
而 trace 里往往什么线索都没有（异常在第一条日志之前抛出）。

实测踩过一次：`ShowClosePrompt()` 第二次调用时把同一个 `CheckBox` 重新
`Append` 进一个新建的 `StackPanel`，XAML 元素同一时刻只能有一个父节点，
这一步抛异常 → `Cancel(true)` 被作废 → 窗口照关。表象是
「第一次点 `X` 正常，第二次点 `X` 程序就没了」。

两条修法，都要：
1. 异常可能发生的整段包 `try/catch`，并落 trace；
2. `LINGSYSMON_TRACE` 打在**可能抛异常的语句之前**，否则现场什么都不剩。

### 7b. `ContentDialog` 的 `Content` 元素只装配一次，绝不重复 `Append`

关闭确认框的面板在构造函数里建一次就固定，`ShowClosePrompt()` 只重置
勾选状态与按钮文案。原因见上一条：`closeRemember_` 是长期存活的成员，
每次现建面板再 `Append` 它就是在做「换一个父节点」，XAML 会抛。

同一类还有：`ContentDialog` 必须**常驻复用**，用局部变量会在 `ShowAsync()`
返回后析构；而同一个 `XamlRoot` 同时只能挂一个 `ContentDialog`，所以本程序
有 `confirm_` / `notice_` / `closeExit_` 三个常驻框 + 一个统一的
`dialogOpen_` 闸门（开之前置位、`Closed` 时清位，缺一处就永久锁死）。

### 7c. WinUI 3 的 `Window` **没有** `Visibility`，隐藏要用 `AppWindow::Hide()`

本工程实际使用的投影（`Generated Files\winrt\Microsoft.UI.Xaml.h`）里，
`IWindow` 只有**只读**的 `Visible()`，没有 `put_Visible` ——
`Visible(false)` 与 `Visibility = Collapsed` 都会直接编不过。

隐藏 / 恢复只有这一套 API：

```cpp
AppWindow().Hide();      // 隐藏顶层 HWND，不动 XAML 可视性树
AppWindow().Show(true);  // 显示并激活
```

**恢复路径必须按状态分支**：`SW_RESTORE` 只对「最小化」有效，对隐藏窗口是空操作。
所以 `ShowAndActivate()` 要先判 `IsWindowVisible` 再判 `IsIconic`；
第二份实例的激活消息（`HostProc`）也必须转发到它，否则
「程序收进托盘后再点一次 exe 图标」会毫无反应且不报错。

### 8. 托盘宿主必须用 `HWND_MESSAGE` 消息窗口，但 `TaskbarCreated` 收不到

`Shell_NotifyIconW` 需要一个 HWND 收回调。三种选法里只有消息窗口是对的：
主窗口 HWND 会把托盘生死与主窗口耦合；可见的隐藏窗口会多一个任务栏条目；
`HWND_MESSAGE` 消息窗口没有可见性、不进任务栏、不进 Alt+Tab，生命周期完全自己控制。

但 `TaskbarCreated` 是 Explorer 用 `HWND_BROADCAST` 广播的，而 `HWND_MESSAGE`
消息窗口被**明确排除**在广播范围之外 —— 托盘自己的宿主永远收不到。
必须由主窗口的子类过程接，再转发给托盘模块。Explorer 一崩 / 一重启就会清空所有
托盘图标，漏了这条路径表现为「程序活着，图标再也不出现」。

### 9. 托盘左键：按 `NIM_SETVERSION` 协商结果分流新旧通知码

Explorer 一次点击会发 4 条回调（`NIN_POPUPOPEN` / `WM_LBUTTONDOWN` /
`WM_LBUTTONUP` / `NIN_SELECT`），四类码一起接会**双跑**一遍 `ShowAndActivate`
（trace 里同一毫秒两条）。

修法：记录 `NIM_SETVERSION` 的真实协商结果到 `g_version`，shell 认了 4 只收
`NIN_SELECT` / `NIN_KEYSELECT`，没认才收 `WM_LBUTTONUP` / `WM_LBUTTONDBLCLK`。
**`g_version` 必须挂在 `platform` 命名空间而非匿名命名空间** —— `InitTrayIcon`
在匿名 namespace 外，看不到里面的变量。

### 10. 单实例定位不能用类名，要共享内存双份存 HWND

WinUI 3 的窗口类名是系统统一的 `WinUIDesktopWin32WindowClass`，机器上每个 WinUI
应用都叫这个名字，`FindWindowW` 按类名必然找错。正确做法是第一份把主窗口 HWND
写进共享内存，第二份读它。

### 11. 自定义 Win32 消息在 WinUI 3 上收不到

**不要用 `PostMessageW` 从采样线程通知 UI 刷新 —— 这条路在 WinUI 3 上是断的**：
桌面 WinUI 应用虽然也走消息泵，但窗口过程由 WinUI 接管，自定义消息最终落到
`DefWindowProc`，我们自己的 WndProc 根本收不到。

用「采样线程置位原子标志 + UI 侧 `DispatcherTimer` 短周期轮询」：跨线程通知回到
一个与 UI 框架无关的位置，也不需要把 HWND 传进数据层。

### 12. 列自绘 ListViewItem 模板会静默杀死进程

试过把 `ListViewItem` 的模板换成「只有 `ListViewItemPresenter`」的极简版以做圆角选中，
实测在列表第一次填充时抛 `STATUS_STOWED_EXCEPTION (0xC000027B)`，故障模块
`CoreMessagingXP.dll`。症状是「进程在 `ItemsSource` 赋值处静默死掉、窗口一闪就没」，
事件日志里没有任何托管异常。代价只是选中高亮回到直角块。

### 13. `ListView` 样式只改属性，不覆写 `ControlTemplate`

同理，行容器样式只设 `HorizontalContentAlignment` / `Padding` / `MinHeight` /
`UseSystemFocusVisuals`，不要动模板。

### 14. 项目侧的三个静默失败模式

- **只写不读的 Item**：某 Item 只出现在 `<Output ItemName=...>` 里，全文再无第二处引用。
  症状是生成器把文件吐出来了却没人消费。
- **孤儿 Target**：`<Target Name="X">` 定义了，`DependsOnTargets` 链里没有任何人引用。
- **被排除的条件**：条件写得像样，但那个元数据 / 属性**根本不存在**，于是条件恒为 false。
  症状与「环境缺失」完全一致。

判据：任何「官方应该有 XXX 却没有」的断言，先 `MSBuild -pp:out.xml` 导出求值后的项目，
搜 `XXX` 出现几次。出现 2 次（定义 + 一次无人消费的 `Output`）= 断链；出现 1 次 = 压根没有。

配套：排除掉某一类 Item 之后，它产出的中间文件不会自动进链接。判定某个条目会不会进
链接，看的是**消费方**（`ComputeCLOutputs`）的条件，不是生产方。

### 15. `RuntimeLibrary` 写错 `ItemDefinitionGroup` 会被静默忽略

`<RuntimeLibrary>` 是 **ClCompile** 属性。误写进 `<Link>` 组里**不报错、不警告、不生效**，
随后被默认值填成 `/MD`。症状不是「构建失败」而是**运行期随机崩**，且崩溃点落在与你的代码
毫无关系的系统 DLL 里。

判据（三个都要查）：`CL.command.1.tlog` 里 `/c` 命令行有没有 `/MD`（**但必须先 `-t:Rebuild`**，
tlog 跨构建累积）；`.map` 的 Static symbols 段出现 `msvcprt:MSVCP140.dll`；
**决定性证据**是 `dumpbin /dependents` 的导入表里没有 `VCRUNTIME140*.dll` /
`MSVCP140*.dll` / `api-ms-win-crt-*.dll`。

### 16. 每个 `.cpp` 第一行必须 `#include "pch.h"`

`/Yu"pch.h"` 找不到指令即 `C1010`。

### 17. 编码：源码 UTF-8，`build.bat` / `.idl` 必须纯 ASCII，`.ps1` 是 UTF-8 无 BOM

- `build.bat` 混入中文会让 cmd 逐行丢字符（它是**纯 ASCII + CRLF**）
- `.idl` 必须纯 ASCII：midl 按系统 ANSI (936) 读，中文报 `C4819` 且撞 `/WX`
- **`.ps1` 不是纯 ASCII** —— 实测 `tools\` 下的脚本普遍带中文注释与中文字面量，
  编码是 **UTF-8 无 BOM + LF**。这条约束只对 `.idl` / `.bat` 成立，
  对 `.ps1` 真正的要求是「**只能用 `pwsh` 跑**」（见踩坑 20 与本节下面一条）：
  Windows PowerShell 5.1 按 ANSI 936 读，会把中文切碎后吞掉引号。
  另注：`.ps1` 里的中文要写成**字符串字面量**时，注意 `"$i:"` 会被 PowerShell
  当成驱动器限定变量（`$i:` = 「i 盘上的变量」），报「没有有效的文本终止符」，
  要写成 `"${i}:"`。
- 读文件内容不要用 PowerShell `Get-Content` 复核（代码页 936 会把 UTF-8 中文显示成乱码）；
  MSBuild 日志是 GBK，要 `[System.Text.Encoding]::GetEncoding(936)` 读
- 控制台代码页会把中文吞掉，trace 落盘用 `WideCharToMultiByte(CP_UTF8)` + `WriteFile`

### 18. `core/monitor/metrics.h` 才有 `SortMode` 枚举

`core/settings.h` 没有（它只有读 ini 用的那个 `MonitorSortMode` 键）。
用 `SORT_*` 的文件必须 include `core/monitor/metrics.h`
并加 `namespace core = lingwu::core::monitor;`。

### 19. 内存阶跃不是泄漏，且真正的变量是「槽位换行」不是 `SetAt` 次数

WinUI 3 列表类界面常出现一次性 +26 MB 的**阶跃**，之后长时间完全走平。
只看短时增长会误判成泄漏。判据：跑够长（≥180 秒）看**阶跃后是否走平**、句柄数是否恒定。
**时点飘忽 + 幅度恒定 + 之后永久走平 + 句柄数不变** ⇒ 框架一次性预热
（文本布局与排版资源），不是应用缺陷。真泄漏会持续上升且通常伴随句柄增长。

归因实验（都做过，别再重复）：A/B 关列表绑定，末端 91.5 MB 且 161 秒完全不动
⇒ 阶跃 100% 来自列表物化；主动滚动到底再回顶**不能**把阶跃提前 ⇒ 与滚动 / 虚拟化无关。

**四档排序的横向对比（同一二进制、同一台机器）推翻了一个想当然的归因**：

| 排序档 | 私有内存 | +26 MB 阶跃 | `replaced`（每帧换几个槽位） |
|---|---|---|---|
| 名称 A→Z | **96.7 MB** | **无** | 74（插入平移时） |
| 内存 ↓（默认） | 123.7 MB | 有 | 高 |
| CPU ↓（量化前） | 132.4 MB | 有 | 136~167 |
| CPU ↓（量化后） | 126.5 MB | 有 | 124~167 |

注意名称排序 `replaced` 也有 74，内存却最低、连阶跃都没有 ——
**所以内存与 `SetAt` 次数无关**。`replaced` 高的主因也不是排序算法：
名称排序的键（应用名）完全稳定，`replaced` 仍达 74，
因为新进程插入字母序第 i 位会让其后所有行**整体后移一位**，
位置 i 之后全部错位。这是真实工作量，不是浪费。

真正的变量是**同一个槽位上是否反复换不同的行对象**：
名称排序下槽位 i 帧间是同一个进程；CPU↓ / 内存↓ 下数值每帧变导致名次变，
槽位频繁换行对象，每次都要重新建立 DataContext 与文本测量缓存。

**由此得出的修复：给 CPU 排序键加量化。**
`metrics.h` 的 `AppCpuSortKey()` 把 `cpuNorm` 归到 0.01% 的格子，
步长不是拍脑袋 —— `FormatCpu` 的最小显示精度就是 `%.2f%%`（0.01%），
两个进程差小于 0.01% 时**界面显示的数字完全相同**，此时还按噪声分胜负
等于排序键比用户能看到的信息更细。量化后同档走名称次级键，顺序稳定。

**踩过的坑：只量化主键等于白做。** 第一版只在 `switch` 的 CPU 分支量化，
次级键仍是裸 `cpuNorm`，实测 `replaced` 依旧 153~168。
必须主键、次级键一起量化，并补一个终极 tiebreaker（组内首个 `pidStart`）——
`std::sort` **不稳定**，比较器一旦返回 false 就意味着「两者无序」，
逐进程口径下有大量同名进程（多个 `chrome.exe`），名称这一级也分不出胜负。

内存键**不量化**：它是字节级整数，`MEM_DESC` 实测 123.7 MB 已达标，
且量化内存会改变默认排序行为。

### 20. 测试侧的两个坑

- **跨进程 UIA 枚举必须卡 ProcessId**。只用 `ControlType` 在 `RootElement` 下 `FindAll`
  再按名字 `-like` 过滤，会把**别的应用**的控件也匹配进来（曾因此误点另一个应用里
  一条标题含「取消」的条目，产生了真实副作用）。一律
  `AndCondition(ControlTypeProperty, ProcessIdProperty)`。
- **非交互 shell 里 GUI 进程活不过单次调用**：`Start-Process` 起的 GUI 进程会在启动它的
  那次调用结束时被回收，所以「启动 → 操作 → 断言」必须写在同一个脚本里。

### 21. 诊断开关用标记文件，不要用环境变量

非交互 shell 里 `$env:` 设的变量**传不到** `Start-Process` 起的子进程，会得到
「开关没生效」的假象。用 `%LOCALAPPDATA%\LingwuToolbox\<name>.on` 这种存在即开的标记。

目前只实现了一个：`trace.on`（存在即写 `%LOCALAPPDATA%\LingwuToolbox\trace.log`）。
**不要凭猜测假设还有别的标记** —— 例如 `nolist.on`（关掉列表绑定做 A/B 对照）
**没有实现**，曾经因此白跑一轮 180 秒观测。

### 22. 删除文件必须用 `rm --`

直接 `Remove-Item` 会被安全策略拦截。**`rm --` 里的路径必须是字面量，不会展开
PowerShell 变量**（踩过两次）。

### 23. `AppWindow` 的尺寸 API 用**物理像素**，XAML 用逻辑像素

`AppWindow` 系列（`Resize` / `MoveAndResize` / `DisplayArea.WorkArea`）的单位是
**物理像素**，而 XAML 布局与 `RasterizationScale` 走的是逻辑像素，两者差一个缩放系数。

原先写死 `AppWindow().Resize({1040, 700})`，在本机 200% 缩放下实际只等于
**520×350 逻辑像素**，约占屏幕 36% 宽 —— 看起来「只有三分之一屏」。
**症状不是报错，是窗口莫名偏小**，没有任何日志线索，极难自查。

现在改成按当前显示器的可用工作区算比例：

```cpp
const auto work = DisplayArea::GetFromWindowId(
    AppWindow().Id(), DisplayAreaFallback::Nearest).WorkArea();
constexpr double kDefaultRatio = 0.70;          // 线性 70% = 面积 49% ≈ 一半
AppWindow().MoveAndResize(RectInt32{
    work.X + (work.Width  - w) / 2,             // 居中
    work.Y + (work.Height - h) / 2, w, h });
```

两个要点：

- **用 `WorkArea` 而不是 `OuterBounds`**：前者已扣掉任务栏，居中到它里面才是真居中；
  用整屏会被任务栏带偏，看着偏上。
- **这段必须包 try/catch**。构造函数整体没有 try/catch（只有 `InitializeComponent`
  那段有），异常一路冒到 `App::OnLaunched`，结果就是踩坑 1 的
  「进程活着但一个窗口都没有」。取不到显示区时退回旧固定尺寸。

本机实测（2880×1704 工作区 / 200% 缩放）：**2015×1192 物理像素 @ (432, 256)**，
面积占工作区 48.94%。宽度比理论值 2016 少 1px，是
`2880 × 0.70` 在 double 下算出 2015.999… 被 `static_cast<int>` 截断所致，无影响。

`OverlappedPresenter.PreferredMinimumWidth/Height`（本项目用的 720×420）
同样是物理像素单位，本机 200% 下只有 360×210 逻辑像素。
微软文档没写单位，是实测 + 二手资料交叉确认的；已知偏小，本次未改。

### 24. WinUI 3 的 `Window` 关闭后不可复用 —— 异常穿透 Win32 窗口过程 = 进程直接消失

**现象**：从**托盘右键菜单**点「设置…」，只要设置窗口**之前被关闭过一次**，
就**必现**进程退出 —— 没有对话框、没有提示、任务栏上的图标直接没了。

**现场**：事件日志（Application / EventId 1000）只有一条

```
LingwuToolbox.exe  异常 0xC0000409  偏移 0x3f739
```

用 `build/obj/Release/LingwuToolbox.map` 符号化：`0x14003f739` = `abort`
（VA `0x14003f704`）+ `0x35` —— 确证是 **CRT `std::terminate` 路径**
（`abort` → `__fastfail(FAST_FAIL_FATAL_APP_EXIT)`），**不是随机内存破坏**。

**根因**（三段，缺一不可）：

1. `Window.Close()` 之后，`Window` **对象仍然存在**（成员依然非空），
   只是 `AppWindow` 已经失效 → 再调 `Activate()` 抛 `hresult_error`。
2. 原代码用 `if (!settingsWindow_)` 判断「要不要新建」，于是第二次打开时
   走进了「复用已关闭窗口」的分支 → 对着一个坏窗口调 `Activate()`。
3. 该异常从 `onSettings` → `ShowTrayMenu` → `TrayHostProc`
   （一个 `LRESULT CALLBACK` 窗口过程）**一路逃出，整条链零 try/catch**。
   user32 / shell 层没有 C++ handler，CRT 只能 `std::terminate` → `abort()`
   → `0xC0000409`。现场全丢：trace 里也什么都没有（异常在第一条日志之前抛出）。

**修法两条，都要**：

1. **不复用已关闭的 `Window`**，而且**也不能每次打开都新建一个** ——
   后者实测**线性泄漏**原生资源：每新建/关闭一轮，句柄数 +36 左右、
   私有内存 +1.8 MB，无上界。实测 30 轮：私有内存 139.5 → 169.9 MB
   （+30.4 MB，越过 145 MB 红线），句柄数 893 → 1272（+379，线性不收敛）。
   对照实验：同时长纯静置（完全不开设置窗口）句柄数持平（895 → 893），
   私有内存的 +28 MB 是一次性框架预热（见踩坑 19），与设置窗口无关。
   ⇒ 最终采用「**构造一次 + 隐藏/显示**」：关闭时只 `AppWindow().Hide()`，
   窗口对象一直活着，每轮零分配，泄漏从根上消失。
   真正关闭放在主窗口退出时：先 `AllowClose()` 放行拦截，再 `Window::Close()`。
   **注意不能用 `AppWindow::Destroy()`** —— 实测踩过：它只销毁原生 HWND，
   **框架的窗口计数不会归零**，消息循环照样不结束，表现是「窗口全没了但
   进程还在」的僵留（trace 里 `MainWindow closed` 已经打过，进程仍在）。
   只有真正的 `Window::Close()` 才会把它从框架里注销掉。
   必须关：只要还有一个 `Window` 活着，XAML 框架的消息循环就不会结束。
2. **所有 Win32 回调边界（窗口过程 / 菜单回调）包异常兜底**，
   统一用 `platform::GuardCallback(tag, fn)`（见 `src/platform/callback_guard.h`），
   tag 要能直接指出是哪一条路径。C++ 异常从 `LRESULT CALLBACK` 逃出去
   就是「进程直接消失」，这个坑太贵，不能只靠「不写错」来防。

**连带变化**：设置窗口的关闭改成隐藏之后，`Window.Closed` 永远不再触发，
原来「靠 `Closed` 回读结果」的通路断了。主窗口改成**轮询**
`settingsWindow_.Accepted()` 回收结果（挂在那本来就在跑的 100 ms
`DispatcherQueueTimer` 上，见 `ApplyPendingSettingsResult`），
并用一个 `settingsResultConsumed_` 标志防止每 100 ms 重复应用一次。

**为什么这个 bug 一直没被发现**：既有的 `tools/monitor/settings.ps1` 只**打开一次**
设置窗口就关闭、**从不重开** —— 而「第二次打开」正是唯一的崩溃触发条件。
现在补了 `tools/monitor/settings-reopen.ps1` 专门覆盖「开 → 关 → 再开」这条路径。

### 25. 托盘图标的名字是 Explorer「首次登记时」的 tooltip，可能过期

Explorer 按 `(exe 路径, uid)` 给托盘图标建一条登记项，`InitialTooltip` **只在第一次
登记时写一次**，之后应用再怎么改 tip 都不刷新。

实测这台机器上，合并前的构建在 `dist\Release\LingwuToolbox.exe` 与
`build\unelevated\...` 两个路径下各留了一条登记项，tip 还是 **「Lingwu SysMon · 系统监控」**。
于是 `Shell_TrayWnd` 里那个 `SystemTray.NormalButton` 的 UIA Name 显示的是**旧名**，
按当前 tip 精确匹配的 `Find-TrayButton` 一律返回 null —— 表现成「托盘图标没了」，
其实图标好好地在那儿，托盘菜单也能弹。

**按产品前缀匹配（`-like '*Lingwu*'`），不要匹配精确 tip。**
清理办法：删掉 `HKCU\Control Panel\NotifyIconSettings` 下 `ExecutablePath`
指向本仓库路径的那几条登记项，下次 `NIM_ADD` 会用新 tip 重建。

（Win11 默认把图标收进 ^ 溢出区，溢出浮窗是另一个顶层窗口
`TopLevelWindowForOverflowXamlIsland`。应用可以写 `IsPromoted` 字段求提升，
但**依赖 Explorer 先建好那条登记项** —— 实测这台机器上 Explorer 压根不建，
所以图标**就是留在溢出区里**，验收脚本必须两条路都能走：
`tools/common/tray.ps1` 会先查可见区，找不到就**用 UIA Invoke 点开溢出浮窗**再找，
并且**把浮窗关回去**。）

### 26. 改了产品命名必须重编，否则验收全打在旧二进制上

Phase 5 那次「产品命名同步」改了 `app.rc` 的 `ProductName` / AUMID **和**托盘 tip，
但 `dist\Release\LingwuToolbox.exe` 还是 12 分钟前构建的那份。于是托盘相关的验收
全在测**旧契约**（旧菜单项、旧 tip），症状是「改了托盘菜单但界面没变」。

**判据：验收前先确认 `dist\Release\LingwuToolbox.exe` 的时间戳比源码新。**
想确认二进制里到底有没有某个字符串，直接在 exe 里搜 UTF-16 字节 ——
比读源码可靠，因为源码可能已经不是构建时的那份。

### 27. PowerShell 函数自调用 = 无限递归，而且不报错

`Wait-TrayButton` 的循环体里写 `Wait-TrayButton`（而不是 `Find-TrayButton`）会无限递归：
既不抛异常也不超时，脚本表现为「卡住不动」，很容易被误判成环境问题
（合成点击不可信、UIA 跨进程不可见……）。这个笔误从迁移前就在，因为每次跑它都先被
更早的 exe 路径问题挡住，所以从没暴露过。

**脚本卡住时先 grep 一遍自己有没有函数自调用，不要急着归咎于环境。**

### 28. 托盘图标可能在「可见区」也可能在「^ 溢出浮窗」里

Win11 把新托盘图标默认收进 ^ 溢出区。溢出浮窗是**另一个顶层窗口**
（`TopLevelWindowForOverflowXamlIsland`），**只有打开时它的条目才在 UIA 树里**。
应用只能靠写 `HKCU\Control Panel\NotifyIconSettings` 下的 `IsPromoted` 这个未公开字段
求提升，而 Explorer 建那条登记项是**异步**的，且不是每台机器都建（实测这台机器不建）。

所以**只查 `Shell_TrayWnd` 会把「图标在那儿」报成「图标没了」**。
共享件 `tools/common/tray.ps1` 两条路都走：先查可见区，找不到就用 UIA Invoke
点开溢出浮窗再找，**并且把浮窗关回去**（实测点两下 chevron 就能开关）。

浮窗里的图标**是有屏幕矩形的**（实测 80×80 @ 3067,1710），但那个矩形
**只在浮窗打开期间有效** —— 浮窗一关，元素跟着浮窗一起死，
`BoundingRectangle` 变成 NaN/∞，`[int]` 转换直接抛异常。
要按坐标点它，就得让浮窗保持打开，点完再关
（`Get-ToolboxTrayState -KeepOpen`）。

### 29. `GetCurrentPattern` 要的是 `AutomationPattern` 实例，不是类型

```powershell
$ip = [System.Windows.Automation.InvokePattern]              # 错：这是类型
$ip = [System.Windows.Automation.InvokePattern]::Pattern     # 对：这是实例
```

传错会抛「无法将类型 System.Windows.Automation.InvokePattern 转换为类型
System.Windows.Automation.AutomationPattern」。

这个坑特别坑，因为它**只在第一次真的 Invoke 的时候才炸** ——
共享件里「打开溢出浮窗」这一步直接抛异常，脚本当场退出，
而报出来的错跟「托盘图标为什么找不到」毫无关系。

### 30. 「环境跑不了」要报 SKIP，不能报 FAIL

`monitor/tray-click.ps1` 依赖 `SendInput` 合成鼠标点击，本机点不动 shell 的
XAML island 托盘（点了没有任何弹出菜单）。那是**环境限制，不是产品缺陷**，
报 FAIL 会把验收板染红并误导后续判断。

判据是「点完有没有弹出菜单」：弹出了就继续验菜单内容，
没弹出就输出 `SKIP: ...` 并以 0 退出。

**验收脚本的三类标记约定**（整轮运行器按它统计）：

| 标记 | 含义 |
|---|---|
| `[PASS]` / `[FAIL]` | 断言结果 |
| `SKIP:` | 这一项**本机跑不了**（环境限制），既不算 pass 也不算 fail |
| `RESULT: pass=N fail=M` | 结尾汇总行，`fail>0` 时退出码为 1 |

同理：某个脚本因为「本机没有管理员权限」跑不了，也要单独标出来，别混进 FAIL 里。

### 31. 共享 API 的返回类型一改，所有调用点都要一起改

把「找 UIA 元素」改成「问在不在」（返回 bool）之后，只改一半的话，
调用点会变成 `$tray.GetCurrentPattern(...)`，在 `[System.Boolean]` 上抛
「Method invocation failed because [System.Boolean] does not contain a method
named 'GetCurrentPattern'」，而 `$ErrorActionPreference = 'Stop'` 会让脚本当场退出。

同一个坑还有断言：旧写法 `$null -eq (Find-TrayButton)` 在函数改成返回 bool 之后
**恒为 false**，于是「托盘图标已撤销」这类断言永远失败，而且是**静默地**失败。

**改动共享 API 的返回类型时，把所有调用点 grep 一遍再跑。**

## 版本号

**当前版本：v0.0.1**（2026-10-23 合并成工具箱）。这是本项目的**唯一版本**，往前没有别的版本。

版本号出现在 4 个地方，必须一起改。前三处是**元数据可见**的：

| 位置 | 形态 | 作用 |
|---|---|---|
| `resources/app.rc` | `0.0.1.0` | exe 的 `FileVersion` / `ProductVersion`（属性对话框「详细信息」页） |
| `resources/app.manifest` | `0.0.1.0` | `assemblyIdentity` 的 version |
| `packaging/make-sfx.ps1` 的 `$Version` 参数默认值 | `0.0.1` | **本地构建**的真源：壳的 VERSIONINFO、输出文件名、buildId 都由它派生 |
| 本文档的示例命令 | `0.0.1` | 文档，改版本时记得跟着改 |

`make-sfx.ps1` 把版本当参数传给 `build-sfx.bat`（`build-sfx.bat 0.0.1`），
**`.bat` 里没有版本字面量** —— 两处各存一份是必然漂移点，会出现
「文件名是 0.0.1、内嵌版本资源还是别的号」这种只有右键看属性才发现的错位。

### 打包时指定版本

```pwsh
pwsh -NoProfile -File .\packaging\make-sfx.ps1            # 用脚本里的默认值
pwsh -NoProfile -File .\packaging\make-sfx.ps1 -Version 0.1.0
```

参数必须是三段式（`X.Y.Z`），否则脚本会立即抛出并中止——tag 写错时
不会流进壳的文件名或 `FILEVERSION`。

**发布时版本由 tag 决定，不由脚本默认值决定。** `.github/workflows/release-toolbox.yml`
从 tag `lingwu-toolbox-vX.Y.Z` 解析出版本号再用 `-Version` 注入，所以产物
不可能和它所在的 tag 对不上。工作流收尾时会比对脚本默认值与 tag，不一致
就打 warning 提醒你更新——但不会因此让发版失败。详见根目录 README 的
「发布」一节。

> 别把 `LSFX v1` 误当成产品版本：那是 `packaging\sfx_main.cpp` 自定义负载格式的
> 格式号，与产品版本无关，改版本号时不要动它。

## 分发

程序是未打包（unpackaged）WinUI 3 应用。分发形态由 `LingwuToolbox.vcxproj` 的
`WindowsAppSDKSelfContained` 决定，**两种模式的可用文件集完全不同**。

### 默认：自包含（免安装，发给别人用）

`WindowsAppSDKSelfContained = true`（`LingwuToolbox.vcxproj` 顶部，已是默认值）。

`dist\Release\` 变成约 250 个文件 / 62 MB，含全量 Windows App Runtime DLL。
目标机**不需要**预装任何东西，解压即用。

打成 zip 分发：

```
tar.exe -a -c -f dist\package\LingwuToolbox-0.0.1-win64-portable.zip -C dist Release
```

### 可选：框架依赖（小包，但要求对方先装运行时）

```
build.bat -p:WindowsAppSDKSelfContained=false
```

产物只有 ~1 MB，但**不是单个 exe**。目标机必须先装 Windows App Runtime
（`Get-AppxPackage -Name '*WindowsAppRuntime*'` 能查到 `Microsoft.WindowsAppRuntime.2` 才算数，
版本要与 AppSDK 对上：2.5.1 ↔ 2.5.1.0）。没装时的症状极具迷惑性：

| 发过去的东西 | 现象 |
|---|---|
| 只发 exe | 进程在、**一个窗口都没有**、trace 零行 |
| exe + `Microsoft.WindowsAppRuntime.Bootstrap.dll`（缺 `.pri`） | `InitializeComponent` 抛 `hresult=0x802B000A`，进程退出 |
| exe + `.pri` + `.xbf` + `.winmd`（**缺 Bootstrap.dll**） | 进程在、没有窗口、trace 零行 |

三种都不崩、不写事件日志、任务管理器里看进程还活着。所以**对外分发不要用这条路径**。

缺 Bootstrap.dll 时 trace 零行的原因：AppSDK 的 `WindowsAppRuntimeAutoInitializer.cpp`
用 `#pragma init_seg(lib)` 在 **wWinMain 之前**初始化运行时，失败就停在那里 ——
「trace 一行都没有」本身就是「死在 wWinMain 之前」的判据。

### 自包含瘦身

`TrimSelfContainedPayload`（`AfterTargets="Build"`）删除本工程零引用的传递依赖：
未瘦身前 294 个文件 / 115.9 MB，瘦身后约 250 个文件 / 62 MB。
最大的两块是 `onnxruntime.dll`（20.66 MB）与 `DirectML.dll`（17.83 MB）。

⚠ 这里**不能**用官方那个 `MicrosoftWindowsAppSDKFilesExcluded` 扩展点：
item 求值正确、文件却仍在输出目录（实测）。原因是自包含产物有两条独立通道
（AppSDK payload 的 `<None CopyToOutputDirectory>`，与 NuGet RAR 的
`_CopyFilesMarkedCopyLocal`）；`onnxruntime.dll` / `DirectML.dll` 根本不在
`runtimes-framework\win-x64\` 下，而在
`microsoft.windows.ai.machinelearning\runtimes\win-x64\native\`，只走后一条。
详见 `TrimSelfContainedPayload` 上方注释。

### 目标系统要求

x64，Windows 10 1809（17763）及以上 / Windows 11。
静态 CRT（`RuntimeLibrary=MultiThreaded`），不需要装 VC++ 运行库。

### 「单个 exe」免安装包（推荐对外发这个）

```
pwsh -NoProfile -File .\packaging\make-sfx.ps1
```

产出 `dist\package\LingwuToolbox-0.0.1-win64.exe`（约 60.7 MB）。
把 `dist\Release\` 的文件按自定义格式嵌成一个 RCDATA 资源，
双击后释放到 `%LOCALAPPDATA%\LingwuToolbox\portable\<buildId>\` 再启动真程序，壳随即退出。

`buildId` 取自 exe 的 SHA-256 前 8 位并进了目录名，所以重打一次壳会落到新目录，
不会出现「换了新 exe 却跑旧文件」。每次释放完写一个 `.sfx-ok` 标记，标记在就整段跳过解压。

**为什么不用 IExpress**：它会压平目录结构，而本工程压平必崩 ——
`Microsoft.UI.Xaml\Assets\NoiseAsset_256x256_PNG.png` 是亚克力/Mica 背景噪点图，
运行期按固定相对路径加载，丢了进程就停在 `MainWindow ctor` 的 step=0 再退出；
`src\ui\*.xbf` 同理。所以 `packaging\sfx_main.cpp` 自己实现了带目录树的释放。

**为什么不压缩负载**：里面已经是压缩过的 DLL，再 deflate 收益接近 0，
却要在壳里塞一个解压器。按原始字节存，换来零依赖 + 4 秒内出窗口。

### 两种分发件怎么选

| | `LingwuToolbox-0.0.1-win64.exe` | `...-portable.zip` |
|---|---|---|
| 体积 | 约 60.7 MB | 约 22.8 MB |
| 对方操作 | 双击即用 | 解压后双击 |
| 杀软误报风险 | 未签名自解压 exe，可能被拦 | 无 |

发朋友用建议 **zip**（小一半、无误报风险）；要「一个文件丢过去就行」就用 **exe**。

## 验证方法

改完必须实机验证，不能只看编译通过。`tools\` 下 28 个脚本全部走 UI Automation
（坐标注入在本机不可靠，UIA 能按 `AutomationId` 定位并读回断言）：

| 脚本 | 验什么 |
|---|---|
| `monitor/check.ps1` | 窗口、进程列表、排序、搜索、聚合开关 |
| `monitor/sort.ps1` | 五档排序逐档落盘 |
| `monitor/align.ps1` | 表头与数据列右边缘对齐（物理像素） |
| `monitor/settings.ps1` | 设置窗口控件、确定 / 取消、ini 往返、自启开关只读（测完自动清 ini 备份） |
| `monitor/settings-reopen.ps1` | 设置窗口**二次打开**（开→关→再开）+ 反复开关的私有内存（踩坑 24 的回归） |
| `monitor/apply.ps1` | 设置应用后运行时生效 |
| `monitor/menu.ps1` | 右键上下文菜单 |
| `monitor/tray-click.ps1` | 托盘 6 个场景：右键菜单 / 左键 / X 最小化 / 退出 / 单实例 / 设置（合成点击，本机点不动 shell，会报 SKIP） |
| `monitor/tray-invoke.ps1` | 托盘交互，走 UIA `InvokePattern`（**推荐**：不需要坐标，溢出区照样能点） |
| `monitor/close-flow.ps1` | 关闭确认框 T1~T9：弹框 / 取消 / 隐藏 / 唤回 / 退出 / 记忆 / 单实例 / 对话框互斥 / 直隐藏 |
| `monitor/close-ab.ps1` | 隐藏-显示的私有内存 A/B（对照 vs 5 轮循环） |
| `monitor/close-mem-ab.ps1` | 三组对照：静止 / 最小化-还原 / 真隐藏-显示（定性「隐藏到底贵不贵」） |
| `monitor/close-hide-dlg.ps1` | 第四组：`CloseAction=1` 隐藏且不弹框，把 Hide 自身与 ContentDialog 的代价拆开 |
| `monitor/repro-close.ps1` | 关闭流程最小复现（自动开 trace 并打印关闭相关日志） |
| `monitor/mem.ps1` | 长时内存观测（默认 180 秒，落 CSV） |
| `monitor/mem-rows.ps1` | 内存曲线与真实行数对齐 |
| `monitor/mem-scroll.ps1` | 滚动后内存是否阶跃 |
| `monitor/shot.ps1` | `PrintWindow` 截图 |
| `monitor/diag.ps1` / `monitor/dump.ps1` | trace 采集 / UIA 树转储 |
| `common/mem-baseline.ps1` | **内存红线基线**：静止 / 两页互切 20 轮 / 隐藏-显示 20 轮 / 设置开合 30 轮 / 托盘常驻 180 秒，报告工作集与私有内存两列 |
| `service/check.ps1` | 服务列表、三列双向排序、搜索、筛选、详情面板 |
| `service/create.ps1` | 创建服务的表单校验 + **真建出来**（脚本结束时会删掉并确认删除成功） |
| `service/mem.ps1` | 服务侧干净实例内存 |
| `service/admin.ps1` | 提权模型：应用是管理员、写操作直接生效、不弹提权框 |
| `build/make-icon.ps1` | 生成 `resources/app.ico`（几何图形，可复现） |
| `build/install-winui-workload.ps1` | 装 WinUI 3 工作流 |

**验收脚本必须在提权 shell 里跑** —— UI Automation 看不到跨完整性级别的界面
（同一个窗口，提权客户端拿到 451 个后代，非提权客户端只拿到 4 个）。
`tools/common/common.ps1` 的 `Require-Elevated` 会自动把脚本提权重跑并回传输出与退出码。
逃生口是 `LINGWU_TOOLBOX_UNELEVATED=1`（此时驱动的是清单改成 `asInvoker` 的验证副本，
写操作会被 OS 拒绝）。详见 AGENTS.md「验收脚本」。

**带中文字面量的脚本只能用 `pwsh` 跑**（`pwsh -NoProfile -File .\tools\monitor\close-flow.ps1`）。
这些 `.ps1` 是 UTF-8 无 BOM，而 Windows PowerShell 5.1 按系统 ANSI（936）读，
中文会被切碎后吞掉引号，报出来的是 `ParserError`，完全指不到真正的原因。

**本机无法用合成鼠标输入做点击取证**：`mouse_event` / `SendInput` 点不动任何系统控件
（Win11 的托盘是 XAML island 弹窗，实测合成右键打在上面没有任何弹出菜单）。
所以不要把「模拟点击后没反应」当成 bug 证据，要换成 UIA 断言 + 读可观测状态
（`IsIconic` / `ToggleState` / ini / 注册表 / UIA 枚举托盘按钮）。
`monitor/tray-click.ps1` 因此会在「点了但 shell 没反应」时输出 `SKIP` 而不是 FAIL ——
**环境限制和产品缺陷要分开报**，运行器把 SKIP 单独统计成一列。

跨进程 UIA 一律按 `ProcessId` 过滤（见踩坑 20）。

## 副作用

程序会写这些位置，测试后要清理：

- `%APPDATA%\LingwuToolbox\settings.ini`
- `%LOCALAPPDATA%\LingwuToolbox\trace.log`（仅 `trace.on` 存在时）
- `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `LingwuToolbox` 值（仅自启动开启时）

**测试开机自启后必须清除注册表项**，否则会在用户机器上留下真实自启。

## 已知限制

- **Windows 10 1809–1903 没有实测环境。** Mica 与圆角的门控逻辑是按系统版本判断的，
  但这两个版本上没人跑过。1809 是最低支持版本，不是验证过的版本。
- **本机无法用合成鼠标输入取证**，所有 UI 验收都走 UI Automation 的 Invoke / Value /
  SelectionItem 模式。右键菜单的弹出路径（Win32 `WM_CONTEXTMENU` + 命中测试）
  已用「弹到固定坐标 + DPI 感知截图」的办法做过端到端取证（2026-10-23）。
- 服务列表的启动类型需要每服务一次 `QueryServiceConfigW`。首次约 345 次 RPC
  （在后台线程里，用户看到的是列表先出来、启动类型随后补齐），稳态命中缓存为 0 次。
- 删除服务后进程可能短暂残留（SCM 的「已标记删除」状态），列表里会显示「（已标记删除）」，
  完全停止后消失。
- 状态栏的「管理员权限」是常量（manifest 保证了这一点）。用 `asInvoker` 验证副本跑的时候
  它会说谎，而写操作会被 OS 拒绝 —— 别把它当成应用缺陷。
- **托盘图标可能在 Win11 的「^ 隐藏图标」区里。** Win11 把新图标默认收进溢出区，
  应用只能靠写一个未公开的注册表字段（`NotifyIconSettings\<哈希>\IsPromoted`）求提升，
  而 **Explorer 建那条登记项是异步的，而且不是每台机器都建** —— 实测这台机器就不建，
  所以提升在这台机器上就是做不到。图标在不在可见区不影响功能，托盘菜单和左键唤回都正常；
  想把它提到可见区，手动拖一次即可（Explorer 会记下，之后的实例就常驻可见区）。

## 需求变更

用户明确要求「不确定的必须跟我确认，不能妄自猜测，不要擅自做决定」。
以下情况先问再做：新增依赖 / 改变内存口径 / 改变统计范围 / 改交互流程 / 改默认设置。
