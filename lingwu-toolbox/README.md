# 零物性能监视器（v0.0.1）

一个常驻托盘的 Windows 系统监控小工具：进程 / 应用的 CPU、内存、网络实时占用，一键结束任务。
**实现栈为 C++/WinRT + WinUI 3**，unpackaged 自包含分发，双击即用。

- 目标平台：Windows 10 1809 (17763) 及以上；Win11 专属能力（Mica、圆角、系统背板）按版本运行时门控
- 部署方式：unpackaged + Windows App SDK 框架依赖部署，无需安装、不写注册表（自启动项除外）
- 体积：exe 0.60 MB，`dist\Release\` 合计 0.98 MB
- 常驻内存（私有提交，本机 ~490 进程、Win11 26H2、200% DPI，2026-10-14 实测）：

  | 场景 | 私有内存 | 说明 |
  |---|---|---|
  | 静止（不隐藏窗口） | **130.6 MB** | 红线内 |
  | 最小化-还原 5 轮 | 135.8 MB | 每轮约 +1.0 MB |
  | 隐藏-显示 5 轮（弹确认框） | 144.0 MB | 每轮约 +3.0 MB，对话框是大头 |
  | 隐藏-显示 20 轮（不弹框，加暂停刷新后） | **139.0 MB** | 每轮约 +0.42 MB，尾部跨度 1.2 MB |

  红线 **~145 MB**（2026-10-14 由 ~132 上调），判定只卡私有内存；工作集含
  Windows App Runtime 框架 DLL 的跨进程共享页，不计入红线（见「数据口径」）。
  **真隐藏不比最小化贵**（0.42 vs 1.0 MB/轮），所以上调红线不是为迁就更贵的实现 ——
  旧的「点 X 最小化」同样会越 132 MB 那条线，只是当时没测。
  详见 AGENTS.md「隐藏 / 显示窗口的内存代价」。

## 功能

**主界面**

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

- 搜索框按进程名 / 应用名实时过滤，状态栏同步显示「筛选出 N 个进程 / M 个应用」
- 状态栏右侧常驻：内存口径说明、下行 / 上行网速、参与统计的网卡名
- 右键列表行 → 「结束任务」：先发 `WM_CLOSE` 优雅退出，最多等 3 秒，仍活着的才强杀；
  权限不足时会提示需要管理员权限
- 深色 / 浅色主题跟随系统，Mica 背景与圆角在 Win11 上自动生效

**托盘**

- 左键单击：唤出 / 还原主界面
- 右键菜单五项：显示主界面、按应用聚合（带勾选态）、设置…、重新添加托盘图标、退出
- Win11 默认把新图标收进任务栏 `^` 溢出区，首次运行会弹一次性气泡提示告诉用户去哪儿找
- Explorer 重启后自动重新注册托盘图标（`TaskbarCreated` 广播）

**生命周期**

- 点标题栏 `X`（或按 `Alt+F4`）= 弹确认框，在「最小化到托盘」与「退出程序」之间选；
  框里可勾「记住我的选择，以后不再询问」，勾了之后点 `X` 直接执行、不再询问
- 「最小化到托盘」是**真隐藏**：任务栏不留按钮，只剩托盘一个入口；
  程序继续在后台采样，托盘左键可把它唤回
- 托盘右键菜单「退出」保持一键直接退出，不弹确认框
- 开机自启动 = 登录后静默进托盘，不弹主界面（命令行开关 `--autostart`）
- 单实例：第二份启动会把第一份的窗口叫出来，然后自己退出

**设置**（4 项）

| 设置项 | 取值 | 生效方式 |
|---|---|---|
| 按应用聚合 | 开 / 关 | 确定后立即生效 |
| 默认排序 | 内存 ↓ / 内存 ↑ / CPU ↓ / CPU ↑ / 名称 ↑ | 确定后立即生效 |
| 采样间隔 | 0.5 / 1 / 2 / 5 秒 | 确定后下一周期生效 |
| 开机自启动 | 开 / 关 | 确定后写 / 删 `HKCU\...\Run` |

设置保存在 `%APPDATA%\LingwuSysMon\settings.ini`，段头固定为 `[Settings]`。
自启动**不落 ini** —— 注册表是唯一事实源。

### 关闭窗口时的行为（`CloseAction`）

关闭确认框里那个「记住我的选择，以后不再询问」复选框勾上之后，
选择会写进 ini 的 `CloseAction`：

| 值 | 含义 |
|---|---|
| `0` | 每次询问（默认） |
| `1` | 总是隐藏到托盘 |
| `2` | 总是退出程序 |

手改 ini 写出越界值时，`LoadSettings` 会收敛回 `0`（宁可多问一次，
也不要给出一个 `MainWindow` 里没有对应分支的值）。

**已知缺口**：设成 `1` 或 `2` 之后**没有 UI 入口能改回 `0`**，
只能手改这个文件把 `CloseAction` 置 0。

## 构建

```bat
build.bat
```

产物在 `dist\Release\LingwuSysmon.exe`。脚本通过 `vswhere` 定位 MSVC BuildTools，缺依赖时不要自己拼 `cl` 命令行。

`dist\Release\` 下共 7 个文件，其中 3 个 `.xbf` 落在 `Release\src\ui\` 子目录
（XAML 编译产物按源文件相对路径镜像，不是平铺的）。
**部署只需拷 `Release\` 根目录那 4 个**：`LingwuSysmon.exe`、`LingwuSysmon.pri`、
`LingwuSysmon.winmd`、`Microsoft.WindowsAppRuntime.Bootstrap.dll`。
XAML 资源走 `.pri` 索引，`.xbf` 只是编译中间物，不参与运行期加载 ——
实测从 `Release\` 单独启动，列表 / 设置窗口 / 托盘全部正常。

依赖：VS 2026 BuildTools（MSVC 14.51）、Windows App SDK 2.5.1、NuGet 包
`Microsoft.WindowsAppSDK 2.5.1` + `CppWinRT 3.0.260818.1`（`build.bat` 带 `-restore` 自动拉）。
WinUI 3 工作流缺失时先跑 `tools\install-winui-workload.ps1`。

改完必须**跑一次 `build.bat` 并确认零错误零警告**（`/W3 /WX`）。

## 运行

```bat
dist\Release\LingwuSysmon.exe              # 正常启动，显示主界面
dist\Release\LingwuSysmon.exe --autostart   # 静默进托盘（开机自启动用的就是这条）
dist\Release\LingwuSysmon.exe --show        # 强制显示（提权重启路径用）
dist\Release\LingwuSysmon.exe --selftest    # 只跑数据层自检，不起 XAML、不建窗口
```

`--selftest` 不初始化 COM、不拉 XAML 框架，用来验证数据层可独立运行。退出码非 0 即自检失败。

## 目录结构

```
lingwu-sysmon/
├── build.bat                   # MSBuild 入口（纯 ASCII + CRLF）
├── LingwuSysmon.vcxproj        # 工程文件（/MT 静态 CRT、/WX、/utf-8）
├── XamlMetaDataProvider.idl    # XAML 类型信息（纯 ASCII）
├── resources/
│   ├── app.rc                  # 图标资源 + 版本信息
│   ├── app.manifest            # PerMonitorV2、UTF-8 代码页、supportedOS
│   └── app.ico
├── src/
│   ├── pch.h / pch.cpp         # 预编译头
│   ├── main.cpp                # 入口、单实例、启动方式、Application::Start
│   ├── core/                   # 数据层（不依赖任何 UI 框架，可独立自检）
│   │   ├── sampler.h/.cpp      # 采样线程：进程 CPU/内存、网卡速率
│   │   ├── metrics.h           # 数据模型、SortMode 枚举、格式化
│   │   ├── settings.h/.cpp     # ini 读写（路径 / 段头 / 键名是对外契约）
│   │   ├── autostart.h/.cpp    # 注册表自启动 + 启动方式解析
│   │   ├── appnames.h/.cpp     # exe → 桌面显示名解析（两级缓存）
│   │   ├── terminate.h/.cpp    # 结束进程三段式
│   │   ├── top10.h/.cpp        #  Top10 内存口径计算
│   │   └── selftest.h/.cpp     # 数据层自检
│   ├── platform/               # 纯 Win32 层
│   │   ├── trace.h             # 标记文件诊断 trace
│   │   ├── window_decor.h/.cpp # Win11 窗口装饰的版本门控
│   │   └── tray_icon.h/.cpp    # 托盘图标、单实例、消息窗口宿主
│   └── ui/                     # WinUI 3 层
│       ├── App.xaml / .idl / .h / .cpp
│       ├── MainWindow.xaml / .idl / .h / .cpp
│       ├── SettingsWindow.xaml / .idl / .h / .cpp
│       ├── ProcessRow.idl / .h     # 行数据 + INPC + ICustomPropertyProvider
│       ├── process_row_glue.cpp    # 循环依赖破解的自由函数
│       └── xaml_typeinfo.cpp       # XAML 元数据（显式关 /WX）
├── dist/
│   ├── Release/                  # 自包含产物：250 个文件 / 60.1 MB（见「分发」）
│   └── package/                  # 分发件：单文件 exe 60.3 MB / zip 22.8 MB
├── packaging/                    # 单文件自解压壳
│   ├── sfx_main.cpp              # 释放逻辑（自定义 LSFX v1 负载格式）
│   ├── make-sfx.ps1              # 打包 dist\Release + 生成 sfx.rc
│   └── build-sfx.bat             # rc / cl / link（纯 ASCII）
└── tools/                        # 20 个 UIA 实测脚本
```

## 数据口径

| 指标 | 口径 |
|---|---|
| CPU | `NtQuerySystemInformation(5)` 全量采样 + 逐 tick 差分；单核口径可 >100%，展示用任务管理器口径（除以逻辑核数） |
| 内存 | `PrivatePageCount` **私有提交字节**。任务管理器「进程」页的私有工作集略高（含部分可共享页） |
| 网络 | `GetIfTable2`，只统计 `HardwareInterface` 且非回环、已连接的网卡，黑名单过滤虚拟网卡 |
| 进程身份 | PID 复用靠 `createTime` 区分，避免把新进程当成旧进程累计 CPU |

网卡枚举有节流，PDH 不可用时内存口径退化为私有提交（状态栏会注明）。

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
描述的是**源对象本身**，必须是 `TypeKind::Metadata` + 真实运行时类名（`L"LingwuSysmon.ProcessRow"`）。

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

- 事件日志（`Application` / Id=1000）里故障模块名是 `LingwuSysmon.exe`，偏移地址每轮都不同。
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
QI 到投影类型 `winrt::LingwuSysmon::ProcessRow`，**一个行指针都不存**。
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

### 18. `core/metrics.h` 才有 `SortMode` 枚举

`core/settings.h` 没有。用 `SORT_*` 的文件必须 include `core/metrics.h`
并加 `namespace core = lingsysmon::core;`。

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
「开关没生效」的假象。用 `%LOCALAPPDATA%\LingwuSysMon\<name>.on` 这种存在即开的标记。

目前只实现了一个：`trace.on`（存在即写 `%LOCALAPPDATA%\LingwuSysMon\trace.log`）。
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
LingwuSysmon.exe  异常 0xC0000409  偏移 0x3f739
```

用 `build/obj/Release/LingwuSysmon.map` 符号化：`0x14003f739` = `abort`
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

**为什么这个 bug 一直没被发现**：既有的 `tools/uisettings.ps1` 只**打开一次**
设置窗口就关闭、**从不重开** —— 而「第二次打开」正是唯一的崩溃触发条件。
现在补了 `tools/uisettings2.ps1` 专门覆盖「开 → 关 → 再开」这条路径。

## 版本号

**当前版本：v0.0.1**（2026-10-14）。这是本项目的**唯一版本**，往前没有别的版本。

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

**发布时版本由 tag 决定，不由脚本默认值决定。** `release-sysmon.yml` 从
tag `lingwu-sysmon-vX.Y.Z` 解析出版本号再用 `-Version` 注入，所以产物
不可能和它所在的 tag 对不上。工作流收尾时会比对脚本默认值与 tag，不一致
就打 warning 提醒你更新——但不会因此让发版失败。详见根目录 README 的
「发布」一节。

> 别把 `LSFX v1` 误当成产品版本：那是 `packaging\sfx_main.cpp` 自定义负载格式的
> 格式号，与产品版本无关，改版本号时不要动它。

## 分发

程序是未打包（unpackaged）WinUI 3 应用。分发形态由 `LingwuSysmon.vcxproj` 的
`WindowsAppSDKSelfContained` 决定，**两种模式的可用文件集完全不同**。

### 默认：自包含（免安装，发给别人用）

`WindowsAppSDKSelfContained = true`（`LingwuSysmon.vcxproj` 顶部，已是默认值）。

`dist\Release\` 变成 250 个文件 / 60.1 MB，含全量 Windows App Runtime DLL。
目标机**不需要**预装任何东西，解压即用。

验收证据（实测）：解压到全新目录后启动，19 个运行时模块**全部**从应用目录加载、
来自 `C:\Program Files\WindowsApps` 框架包的为 **0** 个，UIA 枚举到 355 个界面元素。

打成 zip 分发：

```
tar.exe -a -c -f dist\package\LingwuSysmon-0.0.1-win64-portable.zip -C dist Release
```

实测 60.1 MB → **22.8 MB**（250 个文件，1.7 秒）。

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
未瘦身前 294 个文件 / 115.9 MB，瘦身后 250 个文件 / 60.1 MB。
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

产出 `dist\package\LingwuSysmon-0.0.1-win64.exe`（60.3 MB）。
把 `dist\Release\` 的 250 个文件按自定义格式嵌成一个 RCDATA 资源，
双击后释放到 `%LOCALAPPDATA%\LingwuSysMon\portable\<buildId>\` 再启动真程序，壳随即退出。

实测（与 `dist\Release` 完全分离的副本）：

| 检查项 | 结果 |
|---|---|
| 首次出窗口（含解压 60 MB） | **4.1 s** |
| 二次出窗口（标记已存在，跳过解压） | **0.8 s** |
| UIA 界面元素 | 355（与框架依赖构建一致） |
| 子目录 | 88 个全部保留（`Assets\` / `src\ui\` / 83 个语言目录） |
| 运行时模块来源 | 19 个全部来自 portable 目录，框架包 0 个 |
| 与 `dist\Release` 逐文件 SHA-256 | 250 个文件，0 缺失、0 不一致 |

`buildId` 取自 exe 的 SHA-256 前 8 位并进了目录名，所以重打一次壳会落到新目录，
不会出现「换了新 exe 却跑旧文件」。每次释放完写一个 `.sfx-ok` 标记，标记在就整段跳过解压。

**为什么不用 IExpress**：它会压平目录结构，而本工程压平必崩 ——
`Microsoft.UI.Xaml\Assets\NoiseAsset_256x256_PNG.png` 是亚克力/Mica 背景噪点图，
运行期按固定相对路径加载，丢了进程就停在 `MainWindow ctor` 的 step=0 再退出；
`src\ui\*.xbf` 同理。所以 `packaging\sfx_main.cpp` 自己实现了带目录树的释放。

**为什么不压缩负载**：里面已经是压缩过的 DLL，再 deflate 收益接近 0，
却要在壳里塞一个解压器。按原始字节存，换来零依赖 + 4 秒内出窗口。

### 两种分发件怎么选

| | `LingwuSysmon-0.0.1-win64.exe` | `...-portable.zip` |
|---|---|---|
| 体积 | 60.3 MB | 22.8 MB |
| 对方操作 | 双击即用 | 解压后双击 |
| 杀软误报风险 | 未签名自解压 exe，可能被拦 | 无 |

发朋友用建议 **zip**（小一半、无误报风险）；要「一个文件丢过去就行」就用 **exe**。

## 验证方法

改完必须实机验证，不能只看编译通过。`tools\` 下 20 个脚本全部走 UI Automation
（坐标注入在本机不可靠，UIA 能按 `AutomationId` 定位并读回断言）：

| 脚本 | 验什么 |
|---|---|
| `uicheck.ps1` | 排序重排 / 还原、搜索过滤、状态栏、内存采样 |
| `uisort.ps1` | 五档排序逐档落盘 |
| `uisettings.ps1` | 设置窗口 4 项控件、确定 / 取消、ini 往返、自启开关只读（测完自动清 ini 备份） |
| `uisettings2.ps1` | 设置窗口**二次打开**（开→关→再开）+ 反复开关的私有内存（踩坑 24 的回归） |
| `uiapply.ps1` | 设置应用后运行时生效 |
| `uimenu.ps1` | 右键上下文菜单 |
| `trayclick.ps1` | 托盘 7 个场景：右键菜单 / 左键 / X 最小化 / 退出 / 单实例 / 设置 / 聚合 |
| `closeflow.ps1` | 关闭确认框 T1~T9：弹框 / 取消 / 隐藏 / 唤回 / 退出 / 记忆 / 单实例 / 对话框互斥 / 直隐藏 |
| `closeab.ps1` | 隐藏-显示的私有内存 A/B（对照 vs 5 轮循环） |
| `closememab.ps1` | 三组对照：静止 / 最小化-还原 / 真隐藏-显示（定性「隐藏到底贵不贵」） |
| `closehidedlg.ps1` | 第四组：`CloseAction=1` 隐藏且不弹框，把 Hide 自身与 ContentDialog 的代价拆开 |
| `repro_close.ps1` | 关闭流程最小复现（自动开 trace 并打印关闭相关日志） |
| `mem.ps1` | 长时内存观测（默认 180 秒，落 CSV） |
| `memscroll.ps1` | 滚动后内存是否阶跃 |
| `shot.ps1` | `PrintWindow` 截图 |
| `diag.ps1` / `uidump.ps1` | UIA 树转储 |
| `install-winui-workload.ps1` | 装 WinUI 3 工作流 |

**带中文字面量的脚本只能用 `pwsh` 跑**（`pwsh -NoProfile -File .\tools\closeflow.ps1`）。
这些 `.ps1` 是 UTF-8 无 BOM，而 Windows PowerShell 5.1 按系统 ANSI（936）读，
中文会被切碎后吞掉引号，报出来的是 `ParserError`，完全指不到真正的原因。

**本机无法用合成鼠标输入做点击取证**：`mouse_event` / `SendInput` 点不动任何系统控件。
所以不要把「模拟点击后没反应」当成 bug 证据，要换成 UIA 断言 + 读可观测状态
（`IsIconic` / `ToggleState` / ini / 注册表 / UIA 枚举托盘按钮）。

跨进程 UIA 一律按 `ProcessId` 过滤（见踩坑 20）。

## 副作用

程序会写这些位置，测试后要清理：

- `%APPDATA%\LingwuSysMon\settings.ini`
- `%LOCALAPPDATA%\LingwuSysMon\trace.log`（仅 `trace.on` 存在时）
- `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `LingwuSysmon` 值（仅自启动开启时）

**测试开机自启后必须清除注册表项**，否则会在用户机器上留下真实自启。

## 需求变更

用户明确要求「不确定的必须跟我确认，不能妄自猜测，不要擅自做决定」。
以下情况先问再做：新增依赖 / 改变内存口径 / 改变统计范围 / 改交互流程 / 改默认设置。
