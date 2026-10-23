#include "pch.h"
#include "MonitorPage.xaml.h"
// XAML 生成代码必须在这里显式引入一次（官方模板也是这么做的）。
// #if __has_include 是为了容忍「第一次编译、生成物还没出来」的情况。
#if __has_include("MonitorPage.g.cpp")
#include "MonitorPage.g.cpp"
#endif
// InitializeComponent() / Connect() / GetBindingConnector() 的**定义**在这里，
// 不在 .g.h 里（.g.h 只有第 43-46 行的声明）。.g.hpp 第 102 行有一条
//     template struct MonitorPageT<struct MonitorPage>;
// 显式实例化，正是它把这些成员模板实例化成可链接的符号。
// C++/WinRT 的约定是由**某一个**代码后置 .cpp 引入 .g.hpp，本工程就选这里。
//
// 漏掉它的症状特别隐蔽：编译全过，链接期报一堆
//   MonitorPageT<MonitorPage>::InitializeComponent
//   MonitorPageT<MonitorPage>::Connect
//   MonitorPageT<MonitorPage>::GetBindingConnector
// 的 LNK2019/LNK2001。
#if __has_include("MonitorPage.xaml.g.hpp")
#include "MonitorPage.xaml.g.hpp"
#endif
// IWindowNative 是 MIDL 生成的 **COM 接口**，不是 WinRT 类型，
// 所以不会出现在 cppwinrt 生成的任何投影头里（实测全盘 grep 无命中）。
// 它随 WinUI 包的 include 目录一起分发，是官方文档里取 HWND 的标准做法。
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <windowsx.h>
#include <cwctype>
#include <thread>
#include "core/monitor/appnames.h"
#include "core/monitor/autostart.h"
#include "core/monitor/terminate.h"
#include "core/monitor/top10.h"
#include "platform/callback_guard.h"
#include "platform/trace.h"
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Input;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Shapes;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Dispatching;
using namespace winrt::Windows::Foundation;
namespace winrt::LingwuToolbox::implementation {
// core 这个别名只指**性能监视模块**（lingwu::core::monitor）。
// 设置持久化在合并后提到了上一层 lingwu::core（两个模块共用一份 ini），
// 所以 LoadSettings / SaveSettings 必须写全限定名 lingwu::core::，
// 写 core:: 会解析到 lingwu::core::monitor 里去 —— 那里已经没有这两个函数了。
namespace core = lingwu::core::monitor;
namespace {
// 大小写无关的子串匹配（搜索框用）
bool ContainsCI(std::wstring hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (auto& c : hay) c = static_cast<wchar_t>(::towlower(c));
    std::wstring n = needle;
    for (auto& c : n) c = static_cast<wchar_t>(::towlower(c));
    return hay.find(n) != std::wstring::npos;
}
// 行高必须与 MonitorPage.xaml 里 DataTemplate 的 <Grid Height="32"> 保持一致。
// 右键命中测试是**算术**定位：VisualTreeHelper::GetChildren 在这版投影里不存在
//（只有 GetChildrenCount / GetParent），没法遍历已实现的行容器去算包围盒，
// 所以这里用「内容坐标 y / 行高」直接推出下标。这是本文件与 XAML 之间唯一的耦合点。
constexpr double kRowHeight = 32.0;

// 列表滚动偏移的读写 helper 曾在这里（ScrollPosOf / RestoreScrollPos /
// RestoreScrollLater），已删除。
//
// 删除理由：它们唯一的存在意义是「换 ItemsSource 之后滚动位置会复位到顶部」。
// 而 RebuildRows 现在**终身只赋一次 ItemsSource**，之后一律就地增删同一个
// observable vector，滚动位置根本不会被复位，这套 Save/Later/Restore 也就没有
// 调用点了。留着它只会让人误以为还有一条会复位滚动的路径。
//
// 若将来真出现需要救滚动位置的场景（例如又改回换 ItemsSource），先把上面那段
// 「铁律：ItemsSource 只赋值一次」读一遍 —— 那条路实测会让私有内存以每次
// +26 MB 的阶跃往上跳，120 秒内突破红线。
// ── 以下函数全部只捕获值拷贝，**绝不捕获 this** ──────────────
// 结束任务的强杀最长阻塞 3 秒，期间窗口随时可能被关掉。
// 实现类没有 weak_ref 支持（它不派生自 winrt::implements<>，
// winrt::make_weak 用不了），所以唯一安全的做法是一个 this 都不碰：
// 需要的一切（DispatcherQueue / XamlRoot / 对话框 / 数据 / 存活标志）
// 都按值搬走。WinRT 对象是引用计数的，按值捕获会延长生命周期、不会悬垂。
//
// 把结果写进常驻的 notice_ 对话框（不 Show —— 由调用方在 UI 线程上 Show）。
void FillNotice(ContentDialog const& notice, XamlRoot const& root,
                core::KillResult r, int killed, const std::wstring& name) {
    notice.XamlRoot(root);
    // Title / Content 在 WinUI 3 里是 **IInspectable**（不是 hstring），
    // 投影头 Microsoft.UI.Xaml.Controls.h:4383 就是
    //     consume_..._IContentDialog<D>::Title(winrt::Windows::Foundation::IInspectable const&)
    // 直接传 L"..." 或 std::wstring 会报 C2665，必须包一层 box_value。
    if (r == core::KILL_ACCESS_DENIED) {
        // 【不再提供「以管理员身份重启」】
        // 工具箱整个进程恒定以管理员身份运行（manifest 声明 requireAdministrator），
        // 所以这里打不开的进程是**系统保护进程**（PPL / 关键进程）——
        // 提权也打不开，重启本程序更没有任何用处。
        // 原来那个入口是合并前留下的：那时程序可能以普通用户身份运行，
        // 遇到权限不足就提示用户「以管理员身份重新启动本程序」。
        // 合并后那个入口既无效又会误导用户，所以整条链删掉了
        // （RelaunchElevated 函数、notice_ 的 PrimaryButtonClick 处理器、
        //  core 里的 KILL_NEEDS_ADMIN 枚举值一并消失）。
        notice.Title(box_value(L"部分进程无法结束"));
        notice.Content(box_value(
            (L"「" + name + L"」中有进程受系统保护，即使以管理员身份运行也无法结束它。\n\n"
             L"本次已结束 " + std::to_wstring(killed) + L" 个进程，其余保持运行。").c_str()));
        notice.PrimaryButtonText(L"");
        notice.CloseButtonText(L"知道了");
    } else if (r == core::KILL_OK) {
        notice.Title(box_value(L"已结束"));
        notice.Content(box_value((L"已结束 " + std::to_wstring(killed) + L" 个进程。").c_str()));
        notice.PrimaryButtonText(L"");
        notice.CloseButtonText(L"好");
    } else {
        notice.Title(box_value(L"结束失败"));
        notice.Content(box_value(L"目标进程可能已经退出，或不具备结束它的权限。"));
        notice.PrimaryButtonText(L"");
        notice.CloseButtonText(L"好");
    }
}
void RunTerminate(std::shared_ptr<std::atomic<bool>> alive, DispatcherQueue dq,
                  ContentDialog notice, XamlRoot root,
                  std::shared_ptr<DialogGate> gate,
                  std::vector<DWORD> pids, std::wstring name) {
    std::thread([alive = std::move(alive), dq, notice, root,
                 gate = std::move(gate),
                 pids = std::move(pids), name = std::move(name)]() mutable {
        int killed = 0;
        const core::KillResult r = core::TerminateAppPids(
            pids.data(), static_cast<DWORD>(pids.size()),
            /*gracefulFirst=*/true, /*graceMs=*/3000, &killed);
        if (!alive->load()) return;          // 窗口已关，结果无处可报
        dq.TryEnqueue([alive, notice, root, gate, r, killed, name]() mutable {
            if (!alive->load()) return;
            FillNotice(notice, root, r, killed, name);
            // 互斥闸门必须在 ShowAsync **之前**拿到独占权。这里有个真实的竞态窗口：
            // confirm_ 早就关掉了，但强杀要等最多 3 秒，这 3 秒里用户可能
            // 在**另一个模块**弹了对话框（现在是同一个窗口、同一个 XamlRoot），
            // 然后本回调再弹 notice_ —— 两个对话框挤掉。
            // 拿不到独占权就干脆不弹：结果信息会留在 trace 里，
            // 而挤掉对话框的后果是「什么都没发生」，比少一条提示更糟。
            if (gate && !gate->try_acquire()) {
                LINGWU_TRACE("RunTerminate: notice skipped, a dialog is already open");
                return;
            }
            notice.ShowAsync();
        });
    }).detach();
}
}  // namespace
// ---------------------------------------------------------------- 构造
//
// 【为什么构造函里什么都不做】
// 外壳持有的是本类型的**值成员**，而 C++ 的成员构造发生在构造函数体**之前** ——
// 也就是说本页面的构造早于外壳自己的 InitializeComponent()。
// 若在这里就建 XAML 树，初始化顺序会变得不可控（谁先加载元数据、谁先拿资源）。
// 所以这里只留一个空构造，真正的初始化统一放在 Initialize()，
// 由外壳在自己 InitializeComponent() 之后、页面进入可视树之前调用。
MonitorPage::MonitorPage() {
    LINGWU_TRACE("MonitorPage ctor: enter");
}

// ---------------------------------------------------------------- 初始化
//
// 【为什么初始化不放在构造函数里】
// 本页面需要外壳注入两样东西：统一的 Settings 实例、跨模块的对话框闸门。
// 设置实例是外壳创建的（两个模块共用一份，见 MonitorPage.xaml.h 的说明），
// 所以页面构造时它还不存在 —— 构造函数里读不到设置。
//
// 【顺序不能反】
// AggregateSwitch().IsOn(aggregate_) 必须在 WireEvents **之前**调用：
// IsOn 会触发 Toggled 处理器，而那会立刻 Refresh() —— 此刻采样还没出第一帧，
// 刷新拿到的是空快照，白做一次列表容器重建。
void MonitorPage::Initialize(lingwu::core::Settings* settings,
                             std::shared_ptr<DialogGate> gate) {
    settings_ = settings;
    gate_ = std::move(gate);

    try {
        InitializeComponent();
    } catch (winrt::hresult_error const& err) {
        LINGWU_TRACE("InitializeComponent threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(err.code().value),
                         err.message().c_str());
        throw;
    }

    // 两个对话框成员声明成 `ContentDialog xxx_{ nullptr }`，那是**空对象**。
    // 必须在接任何事件之前真正构造出来：实测对空 ContentDialog 调
    // add_PrimaryButtonClick 直接以 0xC0000005 崩在 XAML 事件注册里
    // （事件日志有 APPCRASH 记录，但没有任何托管异常），
    // 症状是「构造函数中途静默死掉、窗口一个都没建出来」。
    confirm_ = ContentDialog();
    notice_  = ContentDialog();

    // 页面被摘下可视树（窗口关闭）时，后台线程的回调必须停。
    //
    // 【为什么不靠「窗口可见性」判断】
    // Unloaded 只在元素**离开可视树**时触发；切到另一个标签页只是
    // Visibility=Collapsed，元素还在树上，采样必须继续跑
    // （否则切回来时看到的是几秒前的旧数据）。
    Unloaded([this](auto&&, auto&&) {
        alive_->store(false);
        if (timer_) timer_.Stop();
        LINGWU_TRACE("MonitorPage: unloaded");
    });

    sortMode_  = settings_->sortMode;
    aggregate_ = settings_->aggregate;
    AggregateSwitch().IsOn(aggregate_);

    WireEvents();
    UpdateSortIndicators();
    StartSampling();
    LINGWU_TRACE("MonitorPage: initialized (sort=%d aggregate=%d)",
                 sortMode_, aggregate_ ? 1 : 0);
}

void MonitorPage::WireEvents() {
    // 事件全在代码后置里挂 lambda，不走 XAML 的 Click= 处理器。
    // 原因：C++/WinRT 的 XAML 事件处理器必须在 IDL 里声明成 runtimeclass 成员，
    // 每加一个按钮就要改一次 MonitorPage.idl 并触发 MIDL 重跑；
    // 而按 x:Name 取元素 + lambda 是官方 C++ 模板同样采用的做法，零额外成本。
    HeaderName().Click([this](auto&&, auto&&) { OnSortHeaderClick(kColName); });
    HeaderCpu().Click([this](auto&&, auto&&) { OnSortHeaderClick(kColCpu); });
    HeaderMem().Click([this](auto&&, auto&&) { OnSortHeaderClick(kColMem); });
    SearchBox().TextChanged([this](auto const& s, auto const& a) { OnSearchChanged(s, a); });
    // 搜索图标在代码里设，不在 XAML 里写 QueryIcon="<FontIcon .../>"。
    // 那是字符串类型的属性，属性值里再嵌一层 XML，& 与引号得转义两次；
    // 转义稍有偏差就是运行期
    //     0x802B000A  Failed to create a 'Microsoft.UI.Xaml.Controls.IconElement'
    //                from the text '<FontIcon Glyph='&#xE721;' />'
    // 编译期完全看不出来。
    {
        auto searchIcon = FontIcon();
        searchIcon.Glyph(L"\xE721");   // Segoe Fluent Icons: Search
        SearchBox().QueryIcon(searchIcon);
    }
    // Toggled 是 RoutedEventHandler，sender 是 IInspectable，不能直接 s.IsOn()。
    AggregateSwitch().Toggled([this](auto&&, auto&&) {
        aggregate_ = AggregateSwitch().IsOn();
        sampler_.SetGroupMode(aggregate_);
        Refresh();
    });
    SettingsButton().Click([this](auto&&, auto&&) { OnOpenSettings(); });
    // 行右键菜单在代码里建：Click 才能用 lambda，不必进 IDL；
    // 弹出由**外壳**的窗口子类驱动（为什么非走 Win32 子类不可，
    // 见文件上方 HitTestRow 上面的注释）。
    contextMenu_ = MenuFlyout();
    {
        auto item = MenuFlyoutItem();
        item.Text(L"结束任务");
        item.Click([this](auto&&, auto&&) { OnEndTask(); });
        contextMenu_.Items().Append(item);   // MenuFlyout 本身没有 Append，要走 Items()
    }
    ProcessList().SelectionChanged([this](auto const&, auto const& a) { OnListSelectionChanged(ProcessList(), a); });
    // 确认弹窗的处理器只挂一次：OnEndTask 只负责摆好文案和暂存目标 pid，
    // 真正开干在这个回调里 —— 因为这版 SDK 的 cppwinrt 投影头没有
    // IAsyncOperation::then，没法写 ShowAsync().then(...)。
    confirm_.PrimaryButtonClick([this](auto&&, auto&&) {
        if (pendingPids_.empty()) return;
        auto dq = DispatcherQueue::GetForCurrentThread();
        if (!dq) return;
        auto pids = std::move(pendingPids_);
        auto name = std::move(pendingName_);
        pendingPids_.clear();
        pendingName_.clear();
        RunTerminate(alive_, dq, notice_, RootGrid().XamlRoot(), gate_,
                     std::move(pids), std::move(name));
    });

    // 【结果提示框不再有主按钮】
    // 原来这里挂的是「以管理员身份重启」的处理器。合并成工具箱后整个进程
    // 恒定以管理员身份运行，那条入口既无效（受保护进程提权也打不开）
    // 又会误导用户，所以整条链删掉了：FillNotice 现在三个分支一律
    // PrimaryButtonText("") + 一个「知道了 / 好」关闭按钮。
    // 注意别把 notice_ 的 PrimaryButtonClick 处理器留成空 lambda ——
    // 那等于把「提权重启」这个入口留在代码里，只是暂时不显示。

    // ---- 对话框互斥闸门 --------------------------------------------------
    // 闸门由**外壳**创建并注入，两个页面共用同一把
    // （合并成一个窗口后，同一个 XamlRoot 上只能挂一个 ContentDialog，
    //   这条约束从「各窗口各管各的」升级成「全局唯一」）。
    // ShowAsync 是异步的，所以「开之前置位、Closed 时清位」必须成对做，
    // 少一处就会出现「对话框已经关了但闸门还锁着，再点 X 永远没反应」。
    confirm_.Closed([this](auto&&, auto&&) { if (gate_) gate_->release(); });
    notice_.Closed([this](auto&&, auto&&) { if (gate_) gate_->release(); });
}
void MonitorPage::StartSampling() {
    sampler_.SetGroupMode(aggregate_);
    sampler_.SetSortMode(sortMode_);
    sampler_.Start(settings_->intervalMs);
    // UI 侧不靠采样线程推消息（WinUI 的窗口过程不转发自定义消息，见 sampler.h），
    // 而是每 100 ms 问一次 ConsumeDirty()：置位到刷新的延迟 < 100 ms，
    // 视觉上与「消息驱动即时刷新」无差别，且没有数据竞争。
    //
    // 注意构造方式：cppwinrt 3.0.260818.1 生成的 DispatcherQueueTimer
    // （Generated Files\winrt\impl\Microsoft.UI.Dispatching.2.h:50）只有
    //   DispatcherQueueTimer(std::nullptr_t)
    //   DispatcherQueueTimer(void*, take_ownership_from_abi_t)
    // **没有默认构造函数**，写 `DispatcherQueueTimer timer;` 会报 C2512
    // 「没有合适的默认构造函数」。正解是走 DispatcherQueue::CreateTimer()。
    timer_ = DispatcherQueue::GetForCurrentThread().CreateTimer();
    timer_.Interval(std::chrono::milliseconds(100));
    // Tick 是 TypedEventHandler<DispatcherQueueTimer, IInspectable>，**两个**参数。
    // 写成单参 lambda 会在 Windows.Foundation.h:898 报 C3889「未找到匹配的调用运算符」。
    timer_.Tick([this](auto&& /*sender*/, auto&& /*args*/) { OnTimerTick(); });
    timer_.Start();
}
// ---------------------------------------------------------------- 刷新
void MonitorPage::OnTimerTick() {
    // 设置窗口的结果回收（详见 ApplyPendingSettingsResult）。
    // 必须排在 ConsumeDirty 的早退**之前** —— 采样不一定有脏数据，
    // 但用户按了「确定」之后结果要立刻被收走。
    ApplyPendingSettingsResult();
    if (!sampler_.ConsumeDirty()) return;
    Refresh();
}
void MonitorPage::Refresh() {
    // 窗口不可见时**不做 UI 刷新**。
    //
    // 【为什么】隐藏到托盘之后，那个 100 ms 的 DispatcherQueueTimer 照旧在跑，
    // 而 Refresh() 会一路走到 RebuildRows() 里的 SetAt() —— 那会作废并重建
    // 列表容器。窗口看不见时，这些工作一帧也用不上，却照付 CPU 与容器 churn，
    // 与「CPU 自耗低」这条预算直接冲突。实测隐藏/显示每轮约 0.7 MB 私有内存
    // 增量，里面就有很大一块来自这种看不见的重建。
    //
    // 【数据会不会过期】不会。采样线程完全不受影响，数据一直在采；
    // ShowAndActivate() 里窗口一恢复可见就补一次全量 Refresh()，
    // 显示出来时拿到的就是此刻的快照。
    //
    // 顺带的好处：开机自启的静默模式（窗口建了但不 Activate）也走这条路，
    // 以前它同样在后台每秒重建一个没人看的列表。
    //
    // 两种不可见都要挡住：
    //   ① 当前不是活动标签页（Visibility == Collapsed）—— 页面自己的属性；
    //   ② 整个窗口被隐藏到托盘 —— 由外壳通过 SetHostVisible 告知。
    // 两种情况下这些工作一帧也用不上，却照付 CPU 与容器 churn。
    if (Visibility() != Visibility::Visible) return;
    if (!hostVisible_) return;

    sampler_.Lock();
    snapshot_ = sampler_.View();   // 深拷贝，之后全程在锁外操作
    sampler_.Unlock();
    // 兜住并落 trace。列表填充会同步实例化行容器，XAML 侧的失败在这套
    // 依赖栈下表现为「异常被 stow 后重抛成 STATUS_STOWED_EXCEPTION」，
    // 事件日志只给得出 CoreMessagingXP.dll，完全指不到真实成因。
    // 这里失败一次还能看到 hresult 与消息，否则只表现为「列表不刷新」。
    try {
        RebuildRows();
        UpdateStatusBar();
    } catch (winrt::hresult_error const& e) {
        LINGWU_TRACE("Refresh threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    } catch (std::exception const& e) {
        LINGWU_TRACE("Refresh threw std::exception: %s", e.what());
    } catch (...) {
        LINGWU_TRACE("Refresh threw UNKNOWN non-standard exception");
    }
}
// 清掉重排节流计时，让下一帧立刻把新顺序应用到列表上。
// 0 是哨兵值：RebuildRows 里 `lastOrderApplyTick_ == 0` 一律视为「到期」。
void MonitorPage::ForceReorderNext() { lastOrderApplyTick_ = 0; }

void MonitorPage::RebuildRows() {
    // 语义：先过滤再排序。
    //
    // 把过滤后的 procs 拷进一份快照再交给 BuildViews —— 复用 core 层已验证过的
    // 聚合 + 排序实现，不在 UI 侧另写一套比较器。
    // 反过来「先排序再截断」会得到一个排序正确但被搜索词切碎的列表，
    // 上下滚动时行会整片跳变。
    core::Snapshot view = snapshot_;
    if (!filter_.empty()) {
        std::erase_if(view.procs, [&](const core::ProcInfo& p) {
            return !ContainsCI(p.name, filter_);
        });
    }
    core::BuildViews(view, aggregate_, sortMode_);
    // 本帧每一行的内容 + 稳定身份键。
    struct RowData {
        winrt::hstring name, cpuText, memText, countText;
        double   cpuNorm  = 0.0;
        uint64_t memBytes = 0;
        uint32_t procs    = 0;
        uint32_t pidStart = 0;
        uint32_t pidCount = 0;
        std::wstring key;
    };
    std::vector<RowData> next;
    next.reserve(view.apps.size());
    rowPids_.clear();
    rowPids_.reserve(view.pids.size());
    // 显示名每帧最多解析 4 个（core/appnames.h 的契约 7），首屏会在若干帧内补齐。
    // 必须在 Sampler 的锁**之外**问，这里已经解锁了。
    core::AppNamesBeginFrame();
    for (const core::AppInfo& a : view.apps) {
        ULONGLONG ct = 0;
        if (a.pidCount > 0) {
            const DWORD pid = view.pids[a.pidStart];
            for (const core::ProcInfo& p : view.procs) {
                if (p.pid == pid) { ct = p.createTime; break; }
            }
        }
        const std::wstring friendly = core::FriendlyAppName(
            a.pidCount ? view.pids[a.pidStart] : 0, ct, a.name);
        RowData d;
        d.pidStart = static_cast<uint32_t>(rowPids_.size());
        d.pidCount = a.pidCount;
        rowPids_.insert(rowPids_.end(), view.pids.begin() + a.pidStart,
                        view.pids.begin() + a.pidStart + a.pidCount);
        d.name      = to_hstring(core::Ellipsize(core::AppDisplayName(a.name, friendly), 42).c_str());
        d.cpuText   = to_hstring(core::FormatCpu(a.cpuNorm).c_str());
        d.memText   = to_hstring(core::FormatBytes(a.privWorkSet).c_str());
        d.countText = to_hstring(std::to_wstring(a.procCount).c_str());
        d.cpuNorm   = a.cpuNorm;
        d.memBytes  = a.privWorkSet;
        d.procs     = a.procCount;
        // 键用**原始**应用名而不是显示名：显示名会被 FriendlyAppName 在若干帧内
        // 慢慢补齐（前几帧往往还是进程名），拿它当身份会在名字解析完成的那一刻
        // 被误判成「旧应用消失 + 新应用出现」，白白重建一次容器。
        d.key = a.name + L'\x1f' +
                std::to_wstring(a.pidCount ? view.pids[a.pidStart] : 0);
        next.push_back(std::move(d));
    }
    visibleCount_ = next.size();
    contextRowCount_ = static_cast<int>(visibleCount_);
    // ---- 提交到列表 -----------------------------------------------------
    // 铁律：**ItemsSource 全生命周期只赋值一次**，此后只就地增删同一个向量。
    //
    // 这条铁律是两次实测换来的，两次都别再退回去：
    //
    // ①「每秒换一个新 ItemsSource」——ListView 会销毁并重建全部已实现容器，
    //    而旧容器不及时回收。私有内存 125.8 MB → 162.5 MB / 60 秒且不收敛；
    //    同一二进制关掉列表做对照，60 秒锁死在 91.4 MB、句柄恒定 858。
    //
    // ②「不每秒换，但只要 key 集合一变就换」——这一档更隐蔽，曾经骗过一轮。
    //    key 是「应用名 + 首个 pid」，而浏览器与系统后台子进程高频启停，会让
    //    一个应用组里的**最低 pid** 经常易主，于是 key 集合几乎每几秒就抖一次。
    //    PROBE_C 探针实测：60 秒内触发 **20 次**整表重建（rows 在 161↔163
    //    之间，常常只差 1 行）。每次都报废再重建 163 个容器，这正是 +26 MB
    //    阶跃的来源（180 秒观测里 t=20 与 t=164 两次阶跃，相隔 144 秒）。
    //    **差 1 行就重建 163 个容器，这个比例极端失衡**，所以下面改成就地增删。
    //
    // 另外，ItemsSource 的元素类型必须是 IInspectable，这是硬约束：
    // 实测 single_threaded_vector<IInspectable> 正常，而 <hstring> 和
    // <ProcessRow> 一律抛 E_INVALIDARG(0x80070057)；失败时事件日志只留下
    // STATUS_STOWED_EXCEPTION 0xC000027B @ CoreMessagingXP.dll，指不到成因。
    //
    // 1) 复用或新建行对象，逐个 Update（只有真变了的字段才抛通知）。
    //
    // 曾经这里有一对「退休缓冲区」（retired_ / retiredPrev_），把被挤出列表的
    // 行多留两帧，用来兜 0xc0000005 崩溃。它没能兜住：节流后 SetAt 变成每
    // 3 秒一次，绑定可以跨多个应用周期存活，两帧不够，第 4 例崩溃依然复现。
    // 真正的修法在 ProcessRow.h：GetValue 改用引擎传进来的 target 取行，
    // 一个行指针都不存，悬空路径从根上消失。已删除。
    rowPool_.clear();
    rowPool_.reserve(next.size());
    for (const RowData& d : next) {
        auto it = rowByKey_.find(d.key);
        winrt::LingwuToolbox::ProcessRow row;
        if (it != rowByKey_.end()) {
            row = it->second;
        } else {
            row = winrt::make<ProcessRow>();
        }
        row.Update(d.name, d.cpuText, d.memText, d.countText,
                   d.cpuNorm, d.memBytes, d.procs, d.pidStart, d.pidCount);
        rowPool_.push_back(row);
    }
    // 2) 只需要判断「顺序也没变」——这是唯一能完全跳过列表操作的短路条件。
    //    集合变了不再单独判定：下面的就地增删对「集合变」和「顺序变」一视同仁，
    //    差的只是尾部几次 Append/RemoveAtEnd，不再有「重建整表」这条路径。
    bool sameOrder = (currentOrder_.size() == next.size());
    if (sameOrder) {
        for (size_t i = 0; i < next.size(); ++i) {
            if (currentOrder_[i] != next[i].key) { sameOrder = false; break; }
        }
    }
    // 重排节流。行数变了（有进程启停）必须立刻应用，否则新进程会迟到；
    // 纯顺序变化则最多每 kReorderIntervalMs 应用一次。
    // 注意判据用 source_.Size()（屏幕上真实的行数）而不是 next.size()，
    // 因为节流跳过的帧里 source_ 还是旧长度，两者之差正是「还没应用的长度变化」。
    const bool lengthChanged = listBound_ && (source_.Size() != next.size());
    const ULONGLONG now = GetTickCount64();
    const bool orderDue = (lastOrderApplyTick_ == 0) ||
                          (now - lastOrderApplyTick_ >= kReorderIntervalMs);
    const bool applyOrder = !sameOrder && (lengthChanged || orderDue);
    if (!listBound_) {
        // 首帧：建向量、赋一次 ItemsSource。此后 source_ 终身不换。
        // single_threaded_observable_vector 是返回 IObservableVector<T> 的工厂
        // 函数（见 MonitorPage.xaml.h 里的说明），所以先攒 std::vector 再一次性传入。
        std::vector<winrt::Windows::Foundation::IInspectable> tmp;
        tmp.reserve(rowPool_.size());
        for (const auto& r : rowPool_) {
            tmp.push_back(r);   // ProcessRow -> IInspectable
        }
        source_ = winrt::single_threaded_observable_vector<
            winrt::Windows::Foundation::IInspectable>(std::move(tmp));
        ProcessList().ItemsSource(source_);
        listBound_ = true;
        // 首帧也记一次时间戳，否则 lastOrderApplyTick_ 一直是哨兵值 0，
        // 第二帧会被判为「到期」而立刻重排一次（那次其实是多余的）。
        lastOrderApplyTick_ = now;
        // 赋 ItemsSource 会清掉选中态，索引随之失效
        selectedPids_.clear();
        selectedName_.clear();
    } else if (applyOrder) {
        // 就地增删 + 逐槽替换。**不换 ItemsSource**，所以滚动位置天然保留，
        // 不再需要 Save/RestoreScrollLater 那一套。
        const uint32_t cur  = source_.Size();
        const uint32_t want = static_cast<uint32_t>(rowPool_.size());
        // 2a) 只调长度。尾部增减是**唯一**不可避免的结构性变更，
        //     每次一行、且只发生在真有进程启停的那一帧。
        if (want < cur) {
            for (uint32_t i = cur; i > want; --i) source_.RemoveAtEnd();
        } else if (want > cur) {
            for (uint32_t i = cur; i < want; ++i) source_.Append(rowPool_[i]);
        }
        // 2b) 再把内容不对的槽位逐位写成应有的行对象。
        //
        // 为什么用 SetAt（replace）而不是 RemoveAt + InsertAt：后者是结构性变更，
        // 每一次都会让 ItemsRepeater / ItemContainerGenerator 作废已实现的区段。
        // 稳态下每帧只有个位数行互换还扛得住，但点一次表头是近乎整体置换
        // （CPU↓ ↔ 内存↓ 几乎没几行同位置），一次循环里几百次结构变更直接把它
        // 打崩 —— 实测「箭头和排序都对，列表却只剩 1 行」，而从进程内读回
        // source_.Size() / Items().Size() 都是 **163**，数据完全正常，
        // 单纯是 UI 不再实现容器。这个假象极像「排序算错了」。
        //
        // 循环上界是 **min(cur, want)**，不是 want：刚被 Append 写对的槽位
        // [cur, want) 本来就是应有的行对象，再 SetAt 一遍是纯冗余 ——
        // 每次都多触发一次容器刷新，而那一批槽位一个都不需要动。
        // 只有 [0, min(cur, want)) 才是「可能还放着旧行、需要核对」的区间。
        const uint32_t check = cur < want ? cur : want;
        uint32_t replaced = 0;
        for (uint32_t i = 0; i < check; ++i) {
            if (currentOrder_[i] == next[i].key) continue;   // 该槽位本来就是它
            source_.SetAt(i, rowPool_[i]);
            ++replaced;
        }
        // 重排强度探针。这一行是定位「CPU↓ 排序私有内存比内存↓ 排序高 40 MB」
        // 的唯一手段：两个 +26 MB 阶跃到底由哪一帧触发、每帧替换多少个槽位，
        // 只能靠逐帧计数和内存采样曲线对齐才能看出来。
        // 开销：每帧一次 swprintf + 一次 AppendFile，只在 trace.on 存在时发生。
        LINGWU_TRACE("ListCommit: rows=%u cur=%u want=%u replaced=%u",
                         static_cast<unsigned>(rowPool_.size()), cur, want, replaced);
        lastOrderApplyTick_ = now;
        // 就地改集合不像换 ItemsSource 那样自动清选中态，但槽位内容可能已经变了
        // （原来选中的行也许被挤到别处、甚至已经退出），所以按列表**当前**的
        // 选中项重新解析一次。 SelectionChanged 此刻不会自己触发。
        OnListSelectionChanged(nullptr, nullptr);
    }
    // A：集合与顺序都没变 —— 值已在上面 Update 过，这里什么都不做。
    // 3) 刷新索引与「当前顺序」，供下一帧比对。
    //
    // rowByKey_ **每帧**都要重建：行对象按 key 复用，值也已经逐帧 Update 过。
    // currentOrder_ 只在**真的应用了顺序**的那一帧才更新 —— 它记录的是屏幕上
    // 正在显示的顺序，不是「想要」的顺序。节流跳过的帧若也更新它，下一帧比对时
    // 就会把一个从未显示过的顺序误判成已显示，于是该做的替换全被短路掉，
    // 症状是「排序箭头在变、列表顺序再也不动」。
    rowByKey_.clear();
    rowByKey_.reserve(rowPool_.size());
    for (size_t i = 0; i < rowPool_.size(); ++i) {
        rowByKey_[next[i].key] = rowPool_[i];
    }
    if (applyOrder) {
        currentOrder_.clear();
        currentOrder_.reserve(next.size());
        for (const RowData& d : next) currentOrder_.push_back(d.key);
    }
}

// ---------------------------------------------------------------- 状态栏

void MonitorPage::UpdateStatusBar() {
    if (!filter_.empty()) {
        StatusLeft().Text(to_hstring(
            (L"筛选出 " + std::to_wstring(visibleCount_) + L" 行 · 共 " +
             std::to_wstring(snapshot_.procs.size()) + L" 进程 / " +
             std::to_wstring(snapshot_.apps.size()) + L" 应用").c_str()));
    } else {
        StatusLeft().Text(to_hstring(
            (std::to_wstring(snapshot_.procs.size()) + L" 进程 · " +
             std::to_wstring(snapshot_.apps.size()) + L" 应用 · " +
             std::to_wstring(snapshot_.coreCount) + L" 逻辑核").c_str()));
    }

    // 内存口径必须显式标注：PDH 私有工作集这条路已被放弃（多占 5.2 MB），
    // 当前口径是 PrivatePageCount 私有提交字节，与任务管理器「进程」页有差异。
    std::wstring right = L"内存：专用提交 · ";
    right += L"↓ " + core::FormatSpeed(snapshot_.net.downBps);
    right += L"  ↑ " + core::FormatSpeed(snapshot_.net.upBps);
    if (!snapshot_.net.nic.empty()) right += L" · " + snapshot_.net.nic;
    StatusRight().Text(to_hstring(right.c_str()));
}

// ---------------------------------------------------------------- 表头指示器

void MonitorPage::UpdateSortIndicators() {
    // 表头数据里的 Path 是
    //     Data="M 0,5 L 9,5 L 4.5,0 Z"
    // 即一个**朝上**的三角形，所以 Angle=0 表示升序、Angle=180 表示降序。
    // 非排序列直接 Collapsed，而不是画灰色 —— Win11 的原生表格就是这样。
    auto apply = [](Path const& arrow, RotateTransform const& rot,
                    bool active, bool ascending) {
        arrow.Visibility(active ? Visibility::Visible : Visibility::Collapsed);
        if (active) {
            rot.Angle(ascending ? 0.0 : 180.0);
        }
    };

    const bool cpuActive = (sortMode_ == core::SORT_CPU_DESC || sortMode_ == core::SORT_CPU_ASC);
    const bool memActive = (sortMode_ == core::SORT_MEM_DESC || sortMode_ == core::SORT_MEM_ASC);

    // 名称列只有 A→Z 一档，是排序的「回退位」，恒定朝上。
    apply(ArrowName(), RotName(), sortMode_ == core::SORT_NAME_ASC, true);
    apply(ArrowCpu(),  RotCpu(),  cpuActive, sortMode_ == core::SORT_CPU_ASC);
    apply(ArrowMem(),  RotMem(),  memActive, sortMode_ == core::SORT_MEM_ASC);
}

// ---------------------------------------------------------------- 交互

void MonitorPage::OnSortHeaderClick(size_t column) {
    // 点击表头切换：同一列再点一次换方向，点别的列则取该列的主方向。
    if (column == kColCpu) {
        sortMode_ = (sortMode_ == core::SORT_CPU_DESC) ? core::SORT_CPU_ASC : core::SORT_CPU_DESC;
    } else if (column == kColMem) {
        sortMode_ = (sortMode_ == core::SORT_MEM_DESC) ? core::SORT_MEM_ASC : core::SORT_MEM_DESC;
    } else {
        sortMode_ = (sortMode_ == core::SORT_NAME_ASC) ? core::SORT_MEM_DESC : core::SORT_NAME_ASC;
    }

    settings_->sortMode = sortMode_;
    sampler_.SetSortMode(sortMode_);
    UpdateSortIndicators();
    // 明确的手势不该被节流拖慢：换排序后立刻重排，而不是最多等 kReorderIntervalMs 毫秒。
    ForceReorderNext();
    Refresh();
}

void MonitorPage::OnSearchChanged(AutoSuggestBox const& sender,
                                 AutoSuggestBoxTextChangedEventArgs const& args) {
    if (args.Reason() != AutoSuggestionBoxTextChangeReason::UserInput) return;
    // 注意是 .c_str() 不是 winrt::to_string() —— 后者返回的是 **std::string**，
    // 直接赋给 std::wstring 会报 C2679「二元运算符没有接受 std::string 的右操作数」。
    filter_ = sender.Text().c_str();
    // 同理：筛选条件一变，行集合立刻变，不能让新结果等节流窗口。
    ForceReorderNext();
    Refresh();
}

void MonitorPage::OnListSelectionChanged(IInspectable const& /*sender*/,
                                        SelectionChangedEventArgs const& /*args*/) {
    selectedPids_.clear();
    selectedName_.clear();
    auto sel = ProcessList().SelectedItem();
    if (!sel) return;
    const auto row = sel.try_as<winrt::LingwuToolbox::ProcessRow>();
    if (!row) return;

    // rowPids_ 与列表同批重建，但刷新与选中回调可能交错，
    // 所以这里仍然要校验下标落在范围内再取。
    const size_t end = static_cast<size_t>(row.PidStart()) + row.PidCount();
    if (end > rowPids_.size()) return;

    selectedName_ = row.Name().c_str();   // 不能用 winrt::to_string：那是窄串
    selectedPids_.assign(rowPids_.begin() + row.PidStart(), rowPids_.begin() + end);
}

void MonitorPage::OnEndTask() {
    if (selectedPids_.empty()) return;

    const std::wstring name = selectedName_.empty() ? L"该进程" : selectedName_;

    // Title / Content 是 IInspectable，必须 box_value（见 FillNotice 里的注释）。
    confirm_.XamlRoot(RootGrid().XamlRoot());
    confirm_.Title(box_value(L"结束任务"));
    confirm_.Content(box_value(
        (L"确定要结束「" + name + L"」吗？未保存的数据将丢失。").c_str()));
    confirm_.PrimaryButtonText(L"结束任务");
    confirm_.CloseButtonText(L"取消");
    // 默认落在「取消」上：结束进程是不可撤销的，误触回车代价太大。
    confirm_.DefaultButton(ContentDialogButton::Close);

    // 这版 SDK 的 cppwinrt 投影头里没有 IAsyncOperation::then，
    // 所以确认流程走 PrimaryButtonClick 事件驱动（处理器在 WireEvents 里挂一次）。
    pendingPids_ = selectedPids_;
    pendingName_ = name;
    // 闸门必须先拿到独占权，拿不到说明已经有对话框在途 —— 这时不能再弹，
    // 否则两个 ContentDialog 挤掉（症状：对话框一闪就没了，窗口也不动）。
    // 合并成单窗口之后这条约束是**跨模块**的：服务页的对话框也会占同一个闸门。
    if (gate_ && !gate_->try_acquire()) {
        LINGWU_TRACE("MonitorPage: end-task confirm skipped, a dialog is already open");
        return;
    }
    confirm_.ShowAsync();
}

// ---------------------------------------------------------------- 设置
void MonitorPage::OnOpenSettings() {
    LINGWU_TRACE("Settings clicked");
    // 【先收上一轮的结果，再开新一轮】
    // 顺序不能反：LoadFrom 会把设置窗口的 accepted_ 复位，
    // 如果先把结果复位了，上一轮的「确定」就再也读不到了。
    // 这一步也让「按完确定、100 ms 内又点开设置」这种极快操作不会丢结果。
    ApplyPendingSettingsResult();

    try {
        if (!settingsWindow_) {
            // 【只构造一次】
            //
            // 为什么不能每次都 new 一个：
            //   · WinUI 3 的 Window 在 Close() 之后**不可复用** —— 对着已关闭的
            //     窗口调 Activate() 会抛 hresult_error，而异常从托盘的
            //     LRESULT CALLBACK 窗口过程逃出去 = CRT std::terminate =
            //     0xC0000409 = 进程直接消失。2026-10-20 的崩溃就是这个。
            //   · 而「每次 new 一个」实测**线性泄漏**原生资源：每轮约 36 个
            //     句柄 + 1.8 MB 私有内存，无上界（30 轮 = +379 句柄 /
            //     +30.4 MB，直接撞穿 145 MB 红线）。详细数据见
            //     SettingsWindow.xaml.cpp 构造函数里的长注释。
            //
            // 所以设置窗口改成「构造一次 + 隐藏/显示」：关闭时只 Hide，
            // 每轮零分配。这里只在第一次真正创建。
            settingsWindow_ = winrt::make<SettingsWindow>();
        }
        // 已经开着的话，这里只把**当前**生效值重新灌进去，
        // 否则显示的是上次打开时的旧状态（用户会以为刚才的修改没保存）。
        settingsWindow_.LoadFrom(aggregate_, sortMode_, settings_->intervalMs,
                                 settings_->autoStart, settings_->closeAction);
        // 新一轮：准备接收结果（LoadFrom 已把 accepted_ 复位）。
        settingsResultConsumed_ = false;

        // 隐藏着的窗口要先 Show 再 Activate。
        // AppWindow::Show(true) = 显示并激活；Activate() 再补一次，
        // 因为部分 shell 上 Show 的激活不保证真的抢到前台。
        settingsWindow_.AppWindow().Show(true);
        settingsWindow_.Activate();
    } catch (winrt::hresult_error const& e) {
        LINGWU_TRACE("OnOpenSettings threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    } catch (std::exception const& e) {
        LINGWU_TRACE("OnOpenSettings threw std::exception: %s", e.what());
    } catch (...) {
        LINGWU_TRACE("OnOpenSettings threw UNKNOWN non-standard exception");
    }
}

// 轮询设置窗口的结果并应用。详见 MonitorPage.xaml.h 里的说明。
void MonitorPage::ApplyPendingSettingsResult() {
    if (!settingsWindow_) return;
    if (settingsResultConsumed_) return;
    // Accepted() 只有在用户按了「确定」之后才为 true；
    // 「取消」和「直接关掉」都保持 false —— 那两种情况本来就不该应用任何东西。
    if (!settingsWindow_.Accepted()) return;

    settingsResultConsumed_ = true;
    ApplySettingsResult(settingsWindow_);
}

void MonitorPage::ApplySettingsResult(winrt::LingwuToolbox::SettingsWindow const& w) {
    if (!w.Accepted()) {
        LINGWU_TRACE("Settings cancelled");
        return;   // 取消：什么都不改
    }

    settings_->aggregate  = w.ResultAggregate();
    settings_->sortMode   = w.ResultSortMode();
    settings_->intervalMs = w.ResultIntervalMs();
    settings_->autoStart  = w.ResultAutoStart();
    // 「关闭窗口时」的档位。这是**整个程序**的关闭行为（不是本页的），
    // 但设置窗口住在本页，所以值从本页取、再经共享的 settings_ 写回。
    settings_->closeAction = w.ResultCloseAction();
    // autoStart 的注册表写入已在设置窗口里做完并回读校验，这里只同步到 settings_。
    // ini 不落 AutoStart —— 注册表才是唯一事实源（见 settings.cpp 的注释）。

    lingwu::core::SaveSettings(*settings_);

    // ---- 应用到运行时 ----
    aggregate_ = settings_->aggregate;
    // 这个函数现在是由 ApplyPendingSettingsResult（100 ms 定时器轮询）调进来的，
    // 不是 XAML 事件处理器 —— 但异常一样不能放出去：这里是 UI 线程的定时器回调，
    // 抛出去会一路穿过 DispatcherQueue 的回调边界。
    // 包一层，保证「聚合档位没切过去」不会变成更难查的问题。
    try {
        AggregateSwitch().IsOn(aggregate_);
    } catch (winrt::hresult_error const& e) {
        LINGWU_TRACE("ApplySettingsResult: AggregateSwitch::IsOn threw "
                         "hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    }
    sampler_.SetGroupMode(aggregate_);

    // 排序档位变了要同步给采样线程，否则 RebuildRows 用的还是旧档。
    if (sortMode_ != settings_->sortMode) {
        sortMode_ = settings_->sortMode;
        sampler_.SetSortMode(sortMode_);
        UpdateSortIndicators();
    }

    // 采样间隔变化要重建定时器驱动的采样周期。
    // Sampler::SetInterval 已在 Phase 1 实现，直接调即可。
    sampler_.SetInterval(settings_->intervalMs);

    // 设置里改了排序档同理：用户明确选的档位要立刻生效。
    ForceReorderNext();
    Refresh();
    LINGWU_TRACE("Settings applied: aggregate=%d sort=%d interval=%d autoStart=%d",
                     aggregate_ ? 1 : 0, sortMode_, settings_->intervalMs,
                     settings_->autoStart ? 1 : 0);
}

// ---------------------------------------------------------------- 公共入口
void MonitorPage::OpenSettings() {
    OnOpenSettings();
}

void MonitorPage::Shutdown() {
    // alive_ 置 false 是告诉后台结束任务的线程「别再回调了」；
    // 采样线程有最长 3 秒的 Stop() 等待，所以这里只置标志、不在回调里做重活。
    alive_->store(false);
    if (timer_) timer_.Stop();

    // 设置窗口也要真正关闭。只要还有一个 Window 活着，XAML 框架的消息循环
    // 就不会结束，进程会变成「窗口全没了但进程还在」的僵留状态。
    if (settingsWindow_) {
        try {
            settingsWindow_.AllowClose();
            settingsWindow_.Close();
        } catch (...) {
            // 关闭流程里不能再抛。
        }
        settingsWindow_ = nullptr;
    }
    LINGWU_TRACE("MonitorPage: shutdown");
}

// 行右键菜单的命中测试。外壳的窗口子类拦到 WM_CONTEXTMENU 后回调这里。
//
// 【为什么由页面自己做】
// 命中测试要用到行高、列表位置、当前滚动偏移 —— 这些都是本页面的布局信息。
// 外壳不知道这些，硬塞给它等于把页面的布局知识复制一份到外壳里。
bool MonitorPage::HitTestRow(winrt::Windows::Foundation::Point const& clientPt) {
    if (!contextMenu_ || contextRowCount_ <= 0) return false;

    // 第一步必须把**物理像素换算成逻辑像素**，这一步漏了会同时错两处。
    //
    // WM_CONTEXTMENU 的 lParam 经 ScreenToClient 得到的是**物理**客户区像素，
    // 而下面用到的三个量全是**逻辑**像素：
    //   · TransformToVisual().TransformPoint() 的返回（XAML 坐标系是 DIP）
    //   · ScrollPresenter.VerticalOffset()
    //   · kRowHeight = 32（与 XAML 里 <Grid Height="32"> 对应）
    // 两者只在 100% 缩放下相等。本机是 200%，不换算的话：
    //   index = (物理 y − 逻辑 origin) / 32 ≈ 真实行号的 2 倍
    // 表现为「右键某一行的落点随机地命中别的行、或干脆越界不发生任何事」。
    // ShowAt 的 point 也是逻辑坐标（相对 target），直接用客户区物理坐标会把
    // 菜单弹到离光标很远的位置。
    double scale = 1.0;
    if (auto xr = RootGrid().XamlRoot()) {
        scale = xr.RasterizationScale();
    }
    if (scale <= 0.0) scale = 1.0;
    // Windows::Foundation::Point 的成员是 **float**，除法会提升成 double，
    // /WX 下必须显式收窄，否则是 C2398「需要收缩转换」。
    const winrt::Windows::Foundation::Point logical{
        static_cast<float>(clientPt.X / scale),
        static_cast<float>(clientPt.Y / scale) };

    const auto origin = ProcessList().TransformToVisual(RootGrid())
                            .TransformPoint(winrt::Windows::Foundation::Point{ 0.0, 0.0 });
    double y = logical.Y - origin.Y;
    if (y < 0.0) return false;
    // 必须叠加当前滚动偏移，否则滚下去之后命中算出来的下标会整体偏。
    double offset = 0.0;
    if (auto sv = ProcessList().try_as<IScrollView>()) {
        if (auto sp = sv.ScrollPresenter()) offset = sp.VerticalOffset();
    }
    y += offset;
    const int idx = static_cast<int>(y / kRowHeight);
    if (idx < 0 || idx >= contextRowCount_) return false;
    ProcessList().SelectedIndex(idx);
    // ShowAt 的坐标是**相对 target（列表）**的逻辑坐标。
    contextMenu_.ShowAt(ProcessList(), winrt::Windows::Foundation::Point{
                                          logical.X - origin.X, logical.Y - origin.Y });
    return true;
}

void MonitorPage::SetAggregate(bool on) {
    if (aggregate_ == on) return;
    aggregate_ = on;
    settings_->aggregate = on;
    lingwu::core::SaveSettings(*settings_);
    // IsOn 会触发 Toggled 处理器（它会自己再 Refresh 一次），
    // 是个裸的 XAML 调用 —— 异常从托盘菜单那条链过来时不能让它逃出去。
    try {
        AggregateSwitch().IsOn(on);
    } catch (winrt::hresult_error const& e) {
        LINGWU_TRACE("AggregateSwitch::IsOn threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    }
    sampler_.SetGroupMode(on);
    Refresh();
    LINGWU_TRACE("MonitorPage::SetAggregate(%d)", on ? 1 : 0);
}
}
