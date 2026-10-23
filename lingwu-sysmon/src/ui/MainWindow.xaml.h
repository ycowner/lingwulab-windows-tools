#pragma once

// 按官方 C++/WinRT 模板的写法，这里只引 cppwinrt 生成的 MainWindow.g.h。
//
// 它内部第 52 行有一句
//     #if defined(WINRT_FORCE_INCLUDE_MAINWINDOW_XAML_G_H) || __has_include("MainWindow.xaml.g.h")
//         #include "MainWindow.xaml.g.h"
//     #else
//         ... MainWindowT = MainWindow_base   // 退化成没有 XAML 代码生成的纯壳
//     #endif
// 也就是说 XAML 代码后置头是被自动带进来的；一旦 __has_include 落空，
// 编译能过但 InitializeComponent()/TitleText() 之类全都不存在，症状是莫名其妙的
// 编译错误。vcxproj 里为此把 $(IntDir)src\ui 加进了 AdditionalIncludeDirectories。
#include "MainWindow.g.h"

#include "pch.h"

// ProcessRow 必须在这里可见：x:DataType="local:ProcessRow" 让 XAML 编译器生成的
// x:Bind 代码在 MainWindow.xaml.g.cpp 里直接访问它的成员，而那个 .g.cpp 是被
// MainWindow.xaml.cpp 在本头之后 include 的。
#include "ProcessRow.h"

// 设置窗口（Phase 3）。主窗口持有它的强引用并接收 Closed 回调，
// 所以这里要看到完整定义而不是前置声明。
#include "SettingsWindow.xaml.h"

#include "core/metrics.h"
#include "core/sampler.h"
#include "core/settings.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// single_threaded_vector 只在这个头里声明，pch.h 的 WinRT 投影列表没有它。
#include <winrt/Windows.Foundation.Collections.h>

namespace winrt::LingwuSysmon::implementation {

// 窗口子类与右键菜单之间的共享状态。
//
// 为什么要走 Win32 窗口子类而不是 XAML 事件：cppwinrt 3.0.260818.1 生成的
// Microsoft.UI.Xaml.Input.h 里，**所有** delegate 的模板构造函数都漏了 inline
// （PointerEventHandler:4214、RightTappedEventHandler:4252 …），
// 只有 Windows.Foundation.h 那批（RoutedEventHandler / TypedEventHandler）是好的。
// 结论：Microsoft.UI.* 的事件一律接不上（直接接 → LNK2019；
// 绕开 make_delegate → base.h:5376 的 C2027「使用了未定义类型」）。
// 窗口过程是静态函数、拿不到 this，靠 SetWindowSubclass 的 refData 传这里。
struct MenuHost {
    winrt::Microsoft::UI::Xaml::Controls::ListView list{ nullptr };
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout menu{ nullptr };
    winrt::Microsoft::UI::Xaml::FrameworkElement root{ nullptr };
    int rowCount = 0;   // 每次重建列表后同步，供命中测试做边界判断

    // 「把主窗口叫出来」的单实例激活消息落到这里。
    //
    // 【为什么不能像以前那样在 HostProc 里直接写 ShowWindow(SW_RESTORE)】
    // 主窗口现在可以被**真隐藏**到托盘（AppWindow.Hide()），而 SW_RESTORE
    // 只对「最小化」有效，对隐藏窗口是空操作。实测后果是：程序收进托盘后
    // 用户再启动一份（第二份实例会 PostMessage 这个激活消息），
    // 接收方什么都不会发生 —— 程序彻底打不开，且没有任何报错。
    // 所以恢复逻辑必须由 MainWindow::ShowAndActivate 独家持有，子类只转发。
    std::function<void()> activateMain;
};

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();

private:
    // ---- 启动装配 ----
    void ApplyBackdrop();
    void WireEvents();
    void StartSampling();

    // ---- 数据刷新 ----
    void OnTimerTick();
    void Refresh();
    void RebuildRows();
    void UpdateStatusBar();
    void UpdateSortIndicators();
    // 用户主动改变排序/筛选后调用：清掉重排节流的计时，让下一帧立刻应用新顺序。
    // 节流是为「CPU 数值每秒自然抖动」设计的，不该让用户的明确操作也等 3 秒。
    void ForceReorderNext();

    // ---- 交互 ----
    void OnSortHeaderClick(size_t column);
    void OnSearchChanged(winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const& sender,
                         winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& args);
    // SelectionChanged 是 RoutedEventHandler，sender 是 IInspectable 而不是 ListView，
    // 所以这里直接用成员 ProcessList() 取列表。
    void OnListSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);

    // 结束任务：确认弹窗 → 工作线程强杀 → DispatcherQueue 回传结果。
    // 后两步的实现是 MainWindow.xaml.cpp 里的匿名命名空间自由函数，
    // 它们一个 this 都不碰（理由见那里的注释），因此这里没有 RunTerminate 成员。
    void OnEndTask();

    // 设置窗口（Phase 3）。独立 Window 而不是 ContentDialog：四个设置项需要
    // 纵向空间，且本窗口已经常驻复用了两个 ContentDialog（confirm_ / notice_），
    // 同一个 XamlRoot 上同时只能开一个对话框，会互相挤掉。
    void OnOpenSettings();
    // 设置窗口关闭后回读并应用。 Accepted() 为 false（用户按了取消）时什么都不做。
    void ApplySettingsResult(winrt::LingwuSysmon::SettingsWindow const& w);
    // 轮询设置窗口的结果并应用。
    //
    // 【为什么是轮询而不是事件】
    // 设置窗口改成「关闭时只隐藏、不销毁」（WinUI 3 的 Window 关闭后不可复用，
    // 而每次新建一个会线性泄漏原生资源），于是 Window.Closed 永远不再触发，
    // 原来那条「靠 Closed 回读结果」的通路断了。
    // SettingsWindow 对外只暴露 Accepted() / Result*() 这几个取值接口，
    // 没有「结果变了」的事件可用；而这个应用本来就有一个 100 ms 的
    // DispatcherQueueTimer 在轮询采样脏标志（见 OnTimerTick），
    // 在那里顺带读一次取值，开销可以忽略，延迟也只有 100 ms（用户无感）。
    void ApplyPendingSettingsResult();

    // ---- 托盘 / 生命周期（Phase 4）----------------------------------------
    // 全部由托盘模块的回调或窗口子类调用，所以都是 private。
    // 【为什么不是 public】
    // 托盘模块的回调在本类的构造函数里挂，直接捕获 this，不需要外部持有
    // implementation 指针。而外部（App.xaml.cpp）只有 make<>() 返回的**投影**
    // 类型 winrt::LingwuSysmon::MainWindow，本版 cppwinrt 又没有
    // get_implementation，投影上也看不到这些成员 —— 想从外面调就得把它们
    // 加进 MainWindow.idl，代价是一次 MIDL 重跑加暴露公开 WinRT API。
    // 让本类自己挂回调，两个问题一起消失。

    // 把窗口显示出来并拿到前台。托盘左键、菜单「显示主界面」、
    // 以及第二份实例的激活消息都走这里。
    void ShowAndActivate();
    // 装配托盘图标。构造函数末尾调一次。
    void InitTray();
    // 真正退出。只由托盘菜单「退出」与关闭确认框的「退出程序」调，
    // 置 quitting_ 再发 WM_CLOSE。
    void RequestExit();
    // 「按应用聚合」的当前值 / 设置它。托盘菜单的勾选态与切换走这两个，
    // 不另存一份状态，避免两边不一致。
    bool AggregateOn() const { return aggregate_; }
    void SetAggregate(bool on);

    // ---- 关闭流程（2026-10-14 新增）--------------------------------------
    //
    // 改之前点标题栏 X = 无条件最小化（AppWindow.Closing 里 Cancel + SW_MINIMIZE），
    // 与最小化按钮完全等价，用户想退出只能去托盘菜单。改之后点 X 弹确认框，
    // 在「隐藏到托盘」与「退出程序」之间选，并可选「记住我的选择」。

    // 关闭动作取值，与 settings.ini 的 CloseAction 键一一对应。
    // **必须与 MainWindow.xaml.cpp 里 ReadCloseAction 的钳位范围一致**：
    // 手改 ini 写出越界值时，LoadSettings 已经把它收敛回 0。
    static constexpr int kCloseAsk            = 0;   // 每次询问（默认）
    static constexpr int kCloseMinimizeToTray = 1;   // 总是隐藏到托盘
    static constexpr int kCloseExitProgram    = 2;   // 总是退出程序

    // 关闭请求的落点：按 settings_.closeAction 分流到隐藏 / 退出 / 弹确认框。
    // 由 AppWindow.Closing 处理器在 a.Cancel(true) 之后调用。
    void OnCloseRequested();
    // 摆文案并弹出关闭确认框。
    void ShowClosePrompt();
    // 记下本次选择（用户勾了「记住我的选择」才记），然后执行。
    // action 取上面的三个常量。
    void ApplyCloseChoice(int action);
    // 真隐藏到托盘：任务栏不留按钮，只剩托盘一个入口。
    void HideToTray();

    // 表头列序
    static constexpr size_t kColName = 0;
    static constexpr size_t kColCpu  = 1;
    static constexpr size_t kColMem  = 2;

    lingsysmon::core::Sampler sampler_{};
    lingsysmon::core::Settings settings_{};
    lingsysmon::core::Snapshot snapshot_{};

    // true 表示「这一次关闭是真的要退出」，由 RequestExit() 置位。
    // 调用方有两个：托盘菜单的「退出」、以及关闭确认框的「退出程序」。
    // 置位后 AppWindow.Closing 处理器放行，窗口才会真的关、Closed 才会跑。
    bool quitting_ = false;

    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };

    // 两个对话框都**常驻复用**，不随调用创建。
    //
    // 原因有二：
    //   · ContentDialog 必须由持有者保活到关闭为止，用局部变量会在
    //     ShowAsync() 返回后立刻析构；
    //   · 同一个 XamlRoot 上不能同时挂两个 ContentDialog，复用天然规避。
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog confirm_{ nullptr };   // 结束任务确认
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog notice_{ nullptr };    // 结果 / 提权提示
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog closeExit_{ nullptr }; // 关闭确认（X / Alt+F4）

    // 「记住我的选择，以后不再询问」。必须是成员而不是局部变量：
    // ShowClosePrompt 只负责把它摆进 closeExit_.Content，而勾选状态要在
    // Primary/SecondaryButtonClick 处理器里回读 —— 那两个处理器是常驻的，
    // 捕获局部变量会悬垂。CheckBox 同理必须在构造函数里真正构造，
    // 空对象上挂不进 Content（见 MainWindow.xaml.cpp 构造函数里的注释）。
    winrt::Microsoft::UI::Xaml::Controls::CheckBox closeRemember_{ nullptr };

    // 关闭确认框的说明面板。**构造函数里只建一次**，之后永不重建。
    //
    // 【为什么不能每次 ShowClosePrompt 现建一个 StackPanel 再 Append】
    // closeRemember_ 是长期存活的成员，而 XAML 元素在同一时刻只能有一个父节点。
    // 第二次弹出时再把它 Append 进一个新面板，等于把它从上一轮的面板上摘下来
    // 重新挂 —— 这一步会抛异常。而异常从 AppWindow.Closing 处理器里逃出去之后，
    // 上面那句 a.Cancel(true) 会被作废：窗口照常关闭、进程直接退出。
    // 症状是「第一次点 X 正常，第二次点 X 程序就没了」，且 trace 里什么线索都没有。
    // 面板只建一次就彻底绕开了这个再挂路径。
    winrt::Microsoft::UI::Xaml::Controls::StackPanel closePromptPanel_{ nullptr };

    // 同一个 XamlRoot 上当前是否已有对话框打开。
    //
    // 【为什么需要它 —— 三个 ContentDialog 会互相挤掉】
    // 同一个 XamlRoot 同时只能挂一个 ContentDialog，而 ContentDialog 在
    // WinUI 3 桌面版**不禁用标题栏**，所以「结束任务确认框还开着、用户又点了 X」
    // 是可达的。此时再 ShowAsync 一个 closeExit_，前两个会被挤掉，症状是
    // 「点 X 之后对话框一闪就没了，窗口也不动」，极难定位。
    // 三个对话框 ShowAsync 之前置 true、Closed 时置 false。
    // 冲突时的处理是**忽略这一次关闭并落 trace**（见 OnCloseRequested）。
    //
    // 【为什么是 shared_ptr<atomic<bool>> 而不是普通 bool】
    // notice_ 的 ShowAsync() 发生在 RunTerminate 的工作线程回调里，而那个函数
    // 被刻意写成「一个 this 都不碰」（强杀最长阻塞 3 秒，窗口随时可能已被销毁）。
    // 所以闸门要能按值传进去 —— 与 alive_ 完全同一个套路。
    std::shared_ptr<std::atomic<bool>> dialogOpen_ =
        std::make_shared<std::atomic<bool>>(false);

    // 设置窗口。**必须持有强引用**：XAML 框架不延长我们的生命周期，
    // 局部变量一旦析构窗口就跟着没了（与 App::window 同理）。
    // 开着的时候复用同一个对象：重复点「设置」不该叠出一堆窗口。
    //
    // **只构造一次**，之后靠「隐藏 / 显示」复用（详见 SettingsWindow.xaml.cpp
    // 构造函数里那段长注释）：
    //   · WinUI 3 的 Window 关闭后不可复用，复用已关闭窗口会让 Activate()
    //     抛异常，而异常穿过托盘的窗口过程 = 进程直接消失（0xC0000409）；
    //   · 而「每次打开都新建一个」实测每轮线性泄漏约 36 个句柄 + 1.8 MB
    //     私有内存，无上界，直接撞穿 145 MB 红线。
    winrt::LingwuSysmon::SettingsWindow settingsWindow_{ nullptr };

    // 本轮设置窗口的结果（「确定 / 取消」）是否已经被回收过了。
    //
    // 为什么要单独一个标志：设置窗口改成「关闭时只隐藏」之后，Closed 事件
    // 永远不再触发，主窗口只能靠**轮询** settingsWindow_.Accepted() 来拿结果。
    // 而 Accepted() 在用户按过「确定」之后会一直是 true，所以必须由主窗口
    // 自己记一份「这一轮已经收过了」，否则每 100 ms 就会重复应用一次。
    //
    // 置位时机：每次打开设置窗口时置 false（准备接收新一轮的结果），
    // 回收成功时置 true。
    bool settingsResultConsumed_ = true;

    std::shared_ptr<MenuHost> menuHost_{};
    HWND hwnd_{ nullptr };

    // 与 rowPids_ 同批重建，行里的 PidStart/PidCount 指向它。
    std::vector<DWORD> rowPids_{};

    // ---- 行池（列表容器只创建一次，靠这三者保持稳定）--------------------
    //
    // source_ 是**唯一**赋给 ListView.ItemsSource 的那个对象，全程不换。
    //
    // 【为什么绝不能换 ItemsSource —— 两次实测，两次都别再退回去】
    //
    // ① 每秒换一个新的 ItemsSource：
    //    ListView 会销毁并重建全部已实现容器，旧容器不及时回收。
    //    私有内存 125.8 MB → 162.5 MB / 60 秒且不收敛；
    //    同一二进制关掉列表做对照，60 秒锁死在 91.4 MB、句柄恒定 858。
    //
    // ② 不每秒换，但「key 集合一变就换」—— 这一档更隐蔽，骗过一整轮。
    //    key 是「应用名 + 首个 pid」，而浏览器与系统后台子进程高频启停会让
    //    一个应用组里的**最低 pid** 经常易主，于是集合几乎每几秒抖一次。
    //    PROBE_C 探针实测：60 秒内触发 **20 次**整表重建（rows 在 161↔163
    //    之间，常常只差 1 行）。每次都报废再重建 163 个容器，
    //    这正是 +26 MB 阶跃的来源（180 秒观测里 t=20 与 t=164 两次阶跃）。
    //    **差 1 行就重建 163 个容器，这个比例极端失衡。**
    //
    // 【修复后的实测口径（2026-10-13，本机 Win11 26H2 / 200% DPI）】
    //    列表开启、就地增删：180 秒末端私有内存 **122.7 MB**，工作集 176.1 MB，
    //                        句柄 864；阶跃只剩一次（t=56，+26.1 MB），
    //                        之后 124 秒完全走平。
    //    A/B 对照（关掉列表绑定，只跑数据管道）：末端 **91.5 MB**，
    //                        曲线几乎是一条直线（t=19 后 161 秒不动）。
    //    ⇒ 列表物化的总成本约 31 MB，其中 26 MB 是一次性且有界的。
    //    滚动实验（t=10s 主动滚到底再回顶）**不能**把阶跃提前，依旧落在 t=53，
    //    所以那次 +26 MB 与滚动/虚拟化无关，归因于 WinUI 3 / XAML 框架自身的
    //    惰性初始化（文本布局与排版资源），不是应用缺陷。
    //
    // 另外，ItemsSource 的元素类型必须是 IInspectable，这是硬约束：
    // 实测 single_threaded_vector<IInspectable> 正常，而 <hstring> 和
    // <ProcessRow> 一律抛 E_INVALIDARG(0x80070057)；失败时事件日志只留下
    // STATUS_STOWED_EXCEPTION 0xC000027B @ CoreMessagingXP.dll，指不到成因。
    //
    // 必须是 **IObservableVector**，不能是普通 IVector。
    // 这里踩过一个很贵的坑：原先用 single_threaded_vector()（返回 IVector），
    // 于是「集合没变、只有顺序变了」这条路径里的原地重排
    // **不会发出任何 VectorChanged 通知** —— 普通 IVector 根本没有这个事件。
    // 后果极具误导性：点表头排序时 OnSortHeaderClick 完整跑完（trace 证明
    // sortMode_ 从 0 变 2、SetSortMode/UpdateSortIndicators/Refresh 全部 ok、
    // 无异常），数值也在正常刷新（那是走 INotifyPropertyChanged 的），
    // 唯独**列表顺序一动不动**。看起来像「排序没实现」，其实只是没人通知 UI。
    // 改用 single_threaded_observable_vector() 后重排才会被 UI 看见。
    //
    // 重排本身用的是 **SetAt（replace）**，不是 RemoveAt + InsertAt。
    // 后者是结构性变更，会把 ItemsRepeater 已实现的区段全部作废；稳态下
    // 每帧几个交换还看不出来，但一次表头点击是近乎整体置换，几百次结构变更
    // 会让它彻底停止实现容器 —— 实测「排序指示器正确、source_.Size() 与
    // Items().Size() 都是 163，屏幕上却只有 1 行」。细节见 .cpp 路径 B 的注释。
    //
    // rowByKey_   稳定身份（应用名 + 首个 pid）→ 行对象
    // currentOrder_ 当前显示顺序，逐帧与新顺序比对以决定走哪条更新路径
    // listBound_  是否已经赋过一次 ItemsSource
    // 注意类型：cppwinrt 3.0.260818.1 里这两个工厂都**不是类模板**，而是函数
    //     template <typename T, typename Allocator = std::allocator<T>>
    //     IVector<T>           single_threaded_vector(std::vector<T, Allocator>&& = {});
    //     IObservableVector<T> single_threaded_observable_vector(std::vector<T, Allocator>&& = {});
    // （见 Generated Files\winrt\Windows.Foundation.Collections.h:2880 / 2892）
    // 所以这里必须用接口类型持有，写成 single_threaded_vector<...> 会得到
    // C3646「未知重写说明符」。
    winrt::Windows::Foundation::Collections::IObservableVector<
        winrt::Windows::Foundation::IInspectable> source_{ nullptr };
    std::vector<winrt::LingwuSysmon::ProcessRow> rowPool_{};
    std::unordered_map<std::wstring, winrt::LingwuSysmon::ProcessRow> rowByKey_{};
    std::vector<std::wstring> currentOrder_{};
    bool listBound_ = false;

    // ---- 重排节流（修 CPU↓ 排序私有内存 163.9 MB 超红线，2026-10-14）--------
    //
    // 【实测数据】trace 探针逐帧计数（ListCommit: rows/cur/want/replaced）：
    //   CPU↓ 排序（SortMode=2）：191 帧，平均每帧替换 **148.2** 行（共 172 行），
    //     182/191 帧替换 >100 行，**没有一帧**少于 20 行。
    //     即排序键每秒抖一次，整表就是一次近乎整体置换。
    //   内存↓ 排序（SortMode=0）：顺序长期稳定，绝大多数帧 sameOrder 短路，
    //     实测 180 秒末端私有内存 123.1 MB；CPU↓ 同样 180 秒是 163.9 MB。
    //
    // 【为什么值得节流】数值刷新和顺序应用是两条独立路径：
    //   · 数值：row.Update() → Notify() → 绑定引擎只更新 TextBlock.Text，
    //     **完全不碰列表容器**，每帧都做，用户看到的是实时数值；
    //   · 顺序：SetAt() → VectorChanged → ItemsRepeater 作废并重建该槽位的
    //     容器与绑定表达式。这一路才是 148 次/秒 的开销来源。
    // 所以把顺序应用降到每 N 毫秒一次，用户几乎无感（数值照实刷新），
    // 而容器churn 直接降到 1/N。
    //
    // 【为什么行数变化必须立即应用】进程刚启动时它的行还不在 source_ 里，
    // 只节流顺序会让新进程「迟到几秒才出现」，那是看得出来的功能缺陷。
    // 所以判据是「行数变了 或 距上次应用已过 N 毫秒」。
    //
    // 【关键不变量】currentOrder_ 记录的是**已经显示在屏幕上**的顺序，
    // 不是「想要」的顺序。节流跳过的帧绝不能更新 currentOrder_，
    // 否则下一帧比对时会把一个从未应用过的顺序当成已应用，
    // 于是真正该做的替换全部被跳过 —— 症状是列表顺序再也不更新。
    static constexpr ULONGLONG kReorderIntervalMs = 5000;
    ULONGLONG lastOrderApplyTick_ = 0;   // GetTickCount64，0 表示还没应用过

    std::wstring filter_{};
    int  sortMode_  = lingsysmon::core::SORT_MEM_DESC;
    bool aggregate_ = true;
    size_t visibleCount_ = 0;

    // 结束任务跑在后台线程上（gracefulFirst 最长阻塞 3 秒，绝不能压 UI 线程）。
    // 线程回调里绝不能碰 this：窗口可能已经销毁。回调只捕获值拷贝 + 这个
    // 共享标志，标志由 Closed 时置 false，回调据此决定还活不活着。
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);

    std::vector<DWORD> selectedPids_{};
    std::wstring selectedName_{};

    // 确认弹窗与「结束任务」按钮之间的一次性交接。
    // 之所以要暂存成员而不是用 lambda 捕获：这版 SDK 的 cppwinrt 投影头里
    // 没有 IAsyncOperation::then，ShowAsync() 之后没有可挂的延续，
    // 只能用常驻的 PrimaryButtonClick 处理器，所以目标必须存在成员上。
    std::vector<DWORD> pendingPids_{};
    std::wstring pendingName_{};
};

}  // namespace winrt::LingwuSysmon::implementation

namespace winrt::LingwuSysmon::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}  // namespace winrt::LingwuSysmon::factory_implementation