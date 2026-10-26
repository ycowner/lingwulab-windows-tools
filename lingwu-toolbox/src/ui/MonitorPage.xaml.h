#pragma once

// 按官方 C++/WinRT 模板的写法，这里只引 cppwinrt 生成的 MonitorPage.g.h。
//
// 它内部有一段
//     #if defined(WINRT_FORCE_INCLUDE_MONITORPAGE_XAML_G_H) || __has_include("MonitorPage.xaml.g.h")
//         #include "MonitorPage.xaml.g.h"
//     #else
//         ... MonitorPageT = MonitorPage_base   // 退化成没有 XAML 代码生成的纯壳
//     #endif
// 也就是说 XAML 代码后置头是被自动带进来的；一旦 __has_include 落空，
// 编译能过但 InitializeComponent()/TitleText() 之类全都不存在，症状是莫名其妙的
// 编译错误。vcxproj 里为此把 $(IntDir)src\ui 加进了 AdditionalIncludeDirectories。
#include "MonitorPage.g.h"

#include "pch.h"

// ProcessRow 必须在这里可见：x:Bind 代码在 MonitorPage.xaml.g.cpp 里直接访问它的成员，
// 而那个 .g.cpp 是被 MonitorPage.xaml.cpp 在本头之后 include 的。
#include "ProcessRow.h"

// 设置窗口（聚合 / 排序 / 采样间隔 / 开机自启）本来就是性能监视模块的资产，
// 所以它连同「打开设置」「回收设置结果」一起留在本页面里 —— 页面因此不需要
// 任何「回调外壳」的通道，所有跨模块通信都是外壳 → 页面单向的。
#include "SettingsWindow.xaml.h"

// 跨模块共享的类型：ContentDialog 独占闸门、窗口子类用的菜单中转结构。
#include "shell_types.h"

#include "core/monitor/metrics.h"
#include "core/monitor/sampler.h"
#include "core/settings.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// single_threaded_vector 只在这个头里声明，pch.h 的 WinRT 投影列表没有它。
#include <winrt/Windows.Foundation.Collections.h>

namespace winrt::LingwuToolbox::implementation {

// 性能监视模块。外壳窗口里的一个 Page。
//
// 【它拥有什么、什么归外壳】
//   本页面：采样、列表、排序、搜索、聚合开关、结束任务、设置窗口。
//   外壳  ：窗口几何、标题栏、导航、托盘、单实例、关闭流程、对话框闸门。
//
// 【为什么设置只有一个实例】
// 合并前两个工具各写各的 ini，键名空间互不相干。合并后只剩一份
// %APPDATA%\LingwuToolbox\settings.ini，而 SaveSettings 写的是**整份**结构。
// 如果每个模块各自持有一份 Settings 副本，A 模块保存时会把 B 模块刚写的值
// 用自己的旧副本覆盖掉 —— 症状是「在服务页改了排序，切回性能页点一下，
// 服务页的排序偏好被重置」。
// 所以 Settings 实例由**外壳**创建并持有，两个页面拿的是同一个指针。
struct MonitorPage : MonitorPageT<MonitorPage> {
    MonitorPage();

    // ---- 外壳调用的入口 ------------------------------------------------
    // 由外壳在创建本页面后立刻调用一次，必须早于页面第一次读写设置。
    //
    // 为什么不放进构造函数：设置实例是**外壳**创建的（两个模块共用一份，
    // 否则 A 模块保存时会用旧副本覆盖 B 模块刚写的键），
    // 页面构造时那个指针还不存在。
    void Initialize(lingwu::core::Settings* settings, std::shared_ptr<DialogGate> gate);

    // 当前是否「按应用聚合」。托盘菜单的勾选态与切换走这两个，
    // 不另存一份状态，避免两边不一致。
    bool AggregateOn() const { return aggregate_; }
    void SetAggregate(bool on);

    // 排序档位。设置窗口回读结果时要用。
    int SortMode() const { return sortMode_; }
    // 采样间隔。设置窗口回读结果时要用。
    int IntervalMs() const { return settings_ ? settings_->intervalMs : 1000; }

    // 打开设置窗口（页面自己的「设置」按钮，以及托盘菜单的「设置」都走这里）。
    void OpenSettings();

    // 整个窗口是否可见。窗口被隐藏到托盘时外壳会告知，
    // 页面据此跳过刷新（隐藏时做的重建没人看得见，纯属浪费）。
    void SetHostVisible(bool visible) { hostVisible_ = visible; }

    // 窗口从隐藏/最小化恢复时补一次全量刷新。
    void RefreshNow() { Refresh(); }

    // 退出前清理：关掉设置窗口（否则框架的消息循环不结束，进程会僵留）。
    void Shutdown();

    // 行右键菜单的命中测试。外壳的窗口子类拦到 WM_CONTEXTMENU 后回调这里。
    //
    // 【为什么由页面自己做】
    // 命中测试要用到行高、列表位置、当前滚动偏移 —— 这些都是本页面的布局信息。
    // 外壳不知道这些，硬塞给它等于把页面的布局知识复制一份到外壳里。
    // 返回 true 表示已处理（菜单已弹出）。
    bool HitTestRow(winrt::Windows::Foundation::Point const& clientPt);

    // 当前快照的一份拷贝。**托盘指标面板**用它取数 ——
    // 面板是独立窗口，拿不到本页面的内部状态，只能通过外壳转发。
    //
    // 【为什么是拷贝而不是引用】
    // 采样线程随时会写下一份快照，UI 线程读的时候不能拿到半个写完的对象。
    // 拷贝一份是最省事的同步手段（面板每秒拷一次，代价可以忽略）。
    //
    // 【为什么是 public】
    // 外壳持有的是本类的**实现类型**，可以调任意 public C++ 方法；
    // 这个方法不需要进 .idl（不构成公开 WinRT API）。
    lingwu::core::monitor::Snapshot CurrentSnapshot() const { return snapshot_; }

private:
    // ---- 启动装配 ----
    void WireEvents();
    void StartSampling();

    // ---- 数据刷新 ----
    void OnTimerTick();
    void Refresh();
    void RebuildRows();
    void UpdateStatusBar();
    // bento 磁贴区（8 块）。系统级指标，与进程表分开刷新 ——
    // 两者的内存口径不同（磁贴是物理内存占用，状态栏是各进程私有提交之和），
    // 混在一个函数里迟早会把两个口径搞混，而那种错是「数字看着合理」的错。
    void UpdateTiles();
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
    // 后两步的实现是 MonitorPage.xaml.cpp 里的匿名命名空间自由函数，
    // 它们一个 this 都不碰（理由见那里的注释），因此这里没有 RunTerminate 成员。
    void OnEndTask();

    // 打开设置窗口的实现。公开入口是 OpenSettings()。
    void OnOpenSettings();

    // 设置窗口关闭后回读并应用。 Accepted() 为 false（用户按了取消）时什么都不做。
    void ApplySettingsResult(winrt::LingwuToolbox::SettingsWindow const& w);
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

    // 表头列序
    static constexpr size_t kColName = 0;
    static constexpr size_t kColCpu  = 1;
    static constexpr size_t kColMem  = 2;

    lingwu::core::monitor::Sampler sampler_{};
    // 设置实例由外壳注入（见上面的说明），本页不自己建。
    lingwu::core::Settings* settings_ = nullptr;
    lingwu::core::monitor::Snapshot snapshot_{};

    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };

    // 两个对话框都**常驻复用**，不随调用创建。
    //
    // 原因有二：
    //   · ContentDialog 必须由持有者保活到关闭为止，用局部变量会在
    //     ShowAsync() 返回后立刻析构；
    //   · 同一个 XamlRoot 上不能同时挂两个 ContentDialog，复用天然规避。
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog confirm_{ nullptr };   // 结束任务确认
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog notice_{ nullptr };    // 结果提示

    // 跨模块对话框闸门。由外壳注入，与服务页共用同一把。
    std::shared_ptr<DialogGate> gate_{ nullptr };

    // 整个窗口是否可见（隐藏到托盘时为 false）。由外壳维护。
    bool hostVisible_ = true;

    // 行右键菜单。菜单本体在代码里建（Click 才能用 lambda，不必进 IDL），
    // 弹出由外壳的窗口子类驱动（为什么非走 Win32 子类不可，
    // 见 MonitorPage.xaml.cpp 里 HitTestRow 上面的注释）。
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout contextMenu_{ nullptr };
    // 每次重建列表后同步，供命中测试做边界判断。
    int contextRowCount_ = 0;

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
    winrt::LingwuToolbox::SettingsWindow settingsWindow_{ nullptr };

    // 本轮设置窗口的结果（「确定 / 取消」）是否已经被回收过了。
    //
    // 为什么要单独一个标志：设置窗口改成「关闭时只隐藏」之后，Closed 事件
    // 永远不再触发，主窗口只能靠**轮询** settingsWindow_.Accepted() 来拿结果。
    // 而 Accepted() 在用户按过「确定」之后会一直是 true，所以必须由本页自己
    // 记一份「这一轮已经收过了」，否则每 100 ms 就会重复应用一次。
    //
    // 置位时机：每次打开设置窗口时置 false（准备接收新一轮的结果），
    // 回收成功时置 true。
    bool settingsResultConsumed_ = true;

    // 结束任务跑在后台线程上（gracefulFirst 最长阻塞 3 秒，绝不能压 UI 线程）。
    // 线程回调里绝不能碰 this：窗口可能已经销毁。回调只捕获值拷贝 + 这个
    // 共享标志，标志由窗口关闭时置 false，回调据此决定还活不活着。
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);

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
    std::vector<winrt::LingwuToolbox::ProcessRow> rowPool_{};
    std::unordered_map<std::wstring, winrt::LingwuToolbox::ProcessRow> rowByKey_{};
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
    int  sortMode_  = lingwu::core::monitor::SORT_MEM_DESC;
    bool aggregate_ = true;
    size_t visibleCount_ = 0;

    std::vector<DWORD> selectedPids_{};
    std::wstring selectedName_{};

    // 右键菜单命中的那一行的目标，在**命中当场**拷出来。
    //
    // 【为什么不能只靠 selectedPids_】
    // 「结束任务」这个菜单项属于**你右键的那一行**，而不是「此刻列表里
    // 选中的是哪一行」。可这两者并不等价：
    //   · 菜单弹出到用户点「结束任务」之间可能隔着好几秒；
    //   · 而列表每秒都在刷新重排，这段时间里被选中的那一行可能被换掉或删掉
    //     （内存排名一变动，第一行就换人了）；
    //   · 一旦 SelectedItem() 变成空，OnListSelectionChanged 会把
    //     selectedPids_ 清空，OnEndTask 于是**静默 return** ——
    //     用户那边表现为「点了结束任务，既没有确认框也没有任何提示」。
    //
    // 这个缺陷是实测到的：验收脚本连跑 8 次有 1 次复现「点了没反应」，
    // trace 里能看到 end-task ignored。
    // 修法就是让菜单项绑定一个**快照**而不是活状态。
    std::vector<DWORD> contextPids_{};
    std::wstring contextName_{};

    // 确认弹窗与「结束任务」按钮之间的一次性交接。
    // 之所以要暂存成员而不是用 lambda 捕获：这版 SDK 的 cppwinrt 投影头里
    // 没有 IAsyncOperation::then，ShowAsync() 之后没有可挂的延续，
    // 只能用常驻的 PrimaryButtonClick 处理器，所以目标必须存在成员上。
    std::vector<DWORD> pendingPids_{};
    std::wstring pendingName_{};
};

}  // namespace winrt::LingwuToolbox::implementation

namespace winrt::LingwuToolbox::factory_implementation {
struct MonitorPage : MonitorPageT<MonitorPage, implementation::MonitorPage> {};
}  // namespace winrt::LingwuToolbox::factory_implementation
