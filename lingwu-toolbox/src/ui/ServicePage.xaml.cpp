#include "pch.h"
#include "ServicePage.xaml.h"
// XAML 生成代码必须在这里显式引入一次（官方模板也是这么做的）。
// #if __has_include 是为了容忍「第一次编译、生成物还没出来」的情况。
#if __has_include("ServicePage.g.cpp")
#include "ServicePage.g.cpp"
#endif
// InitializeComponent() / Connect() / GetBindingConnector() 的**定义**在这里，
// 不在 .g.h 里（.g.h 只有声明）。.g.hpp 里有一条
//     template struct ServicePageT<struct ServicePage>;
// 显式实例化，正是它把这些成员模板实例化成可链接的符号。
#if __has_include("ServicePage.xaml.g.hpp")
#include "ServicePage.xaml.g.hpp"
#endif
// IWindowNative 是 MIDL 生成的 **COM 接口**，不是 WinRT 类型，
// 所以不会出现在 cppwinrt 生成的任何投影头里。它随 WinUI 包的 include 目录一起分发，
// 是官方文档里取 HWND 的标准做法。
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <windowsx.h>
#include <cwctype>
#include <algorithm>

#include "core/service/scm.h"
#include "platform/single_instance.h"
#include "platform/trace.h"
#include "platform/window_decor.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
// Media 不是 Controls 的子命名空间，表头的箭头 RotateTransform 在这里，
// 漏掉它 UpdateSortIndicators 的 lambda 形参就报「意外的 RotateTransform」。
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Dispatching;
using namespace winrt::Windows::Foundation;

namespace winrt::LingwuToolbox::implementation {
// core 这个别名只指**服务管理模块**（lingwu::core::service）。
// 设置持久化在合并后提到了上一层 lingwu::core（两个模块共用一份 ini），
// 所以 LoadSettings / SaveSettings 必须写全限定名 lingwu::core::。
namespace core = lingwu::core::service;

namespace {

// 大小写无关的子串匹配（搜索框用）。
//
// 【这里曾经是一个静默失效的严重 bug】原实现是：
//     return ::CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE,
//                             hay.c_str(), (int)hay.size(),
//                             needle.c_str(), (int)needle.size()) == CSTR_LESS_THAN;
// CompareStringW 做的是**字典序比较**，它根本不搜索子串。返回
// CSTR_LESS_THAN 的含义只是「hay 排在 needle 前面」，
// 于是 ContainsCI(名字, "RpcSs") 对**所有字典序在 RpcSs 之后的服务**
// 都返回 true：搜 RpcSs 把 345 条缩成 251 条，看着「有反应」，
// 但筛出来的东西跟用户输入的词毫无关系 —— 这是最难发现的一类错，
// 因为它不报错、不崩溃，只是悄悄地给出错误的答案。
//
// 判据：任何「是否包含」的问题都不能用「比较」型 API 实现。
// 要子串就用 FindStringOrdinal（ordinal 折叠，行为可预期；
// 不用 CompareString 的 locale 折叠，中文服务名的行为更难解释）。
bool ContainsCI(std::wstring const& hay, std::wstring const& needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    const int at = ::FindStringOrdinal(
        0, hay.c_str(), static_cast<int>(hay.size()),
        needle.c_str(), static_cast<int>(needle.size()), TRUE);
    return at >= 0;
}

// 行高必须与 ServicePage.xaml 里 DataTemplate 的 <Grid Height="32"> 保持一致。
// 右键命中测试是**算术**定位：VisualTreeHelper::GetChildren 在这版投影里不存在
//（只有 GetChildrenCount / GetParent），没法遍历已实现的行容器去算包围盒，
// 所以这里用「内容坐标 y / 行高」直接推出下标。这是本文件与 XAML 之间唯一的耦合点。
constexpr double kRowHeight = 32.0;

}  // namespace

// ---------------------------------------------------------------- 构造
//
// 【为什么构造函里什么都不做】
// 外壳持有的是本类型的**值成员**，而 C++ 的成员构造发生在构造函数体**之前** ——
// 也就是说本页面的构造早于外壳自己的 InitializeComponent()。
// 若在这里就建 XAML 树，初始化顺序会变得不可控。
// 所以真正的初始化统一放在 Initialize()，由外壳在自己 InitializeComponent()
// 之后、页面进入可视树之前调用。
ServicePage::ServicePage() {
    LINGWU_TRACE("ServicePage ctor: enter");
}

// ---------------------------------------------------------------- 初始化
void ServicePage::Initialize(lingwu::core::Settings* settings,
                              std::shared_ptr<DialogGate> gate) {
    settings_ = settings;
    gate_ = std::move(gate);

    try {
        InitializeComponent();
    } catch (winrt::hresult_error const& err) {
        LINGWU_TRACE("InitializeComponent threw hresult=0x%08X msg=%ls",
                      static_cast<unsigned>(err.code().value), err.message().c_str());
        throw;
    }

    // 对话框成员声明成 `ContentDialog xxx_{ nullptr }`，那是**空对象**。
    // 必须在接任何事件之前真正构造出来：实测对空 ContentDialog 调
    // add_PrimaryButtonClick 直接以 0xC0000005 崩在 XAML 事件注册里。
    confirm_ = ContentDialog();
    notice_ = ContentDialog();

    // 删除确认框的面板在这里**一次性**建好，之后永不重建。
    // XAML 元素同一时刻只能有一个父节点；重复 Append 会抛异常，
    // 而异常从 AppWindow.Closing 处理器里逃出去会让 Cancel(true) 作废 ——
    // 窗口照常关闭、进程直接退出，症状是「第一次点 X 正常，第二次程序就没了」。
    deletePromptPanel_ = StackPanel();
    deletePromptPanel_.Spacing(8);
    {
        auto msg = TextBlock();
        msg.TextWrapping(TextWrapping::Wrap);
        deletePromptText_ = msg;
        deletePromptPanel_.Children().Append(msg);

        auto input = TextBox();
        input.PlaceholderText(L"输入服务名以确认");
        input.Visibility(Visibility::Collapsed);
        deletePromptInput_ = input;
        deletePromptPanel_.Children().Append(input);
    }
    // Content 只赋这一次。Title / Content 是 IInspectable，
    // 传字符串必须 box_value，直接给 L"..." 会报 C2665。
    confirm_.Content(deletePromptPanel_);

    // 统一设置层里服务侧的字段都带 Service 前缀 —— 因为同一个 ini 里
    // 性能监视侧也有一个叫 sortMode 的键，两个 SortMode 语义完全不同。
    // 枚举与 int 之间必须显式转换：ini 里存的是 int，
    // FilterMode 是 0..4、SortMode 是 0..5，转换是等值的。
    filterMode_ = static_cast<core::FilterMode>(settings_->serviceFilterMode);
    sortMode_ = static_cast<core::SortMode>(settings_->serviceSortMode);
    DetailExpander().IsExpanded(settings_->serviceDetailExpanded);

    WireEvents();
    UpdateSortIndicators();

    // 右键菜单：菜单本身在 XAML 的 Grid.Resources 里（见 ServicePage.xaml 的说明 ——
    // MenuFlyout 在 C++ 投影里没有 MenuItems()）。
    // **弹出**走外壳的 Win32 子类 WM_CONTEXTMENU，菜单**项**回到 Click；
    // 两者不冲突：Click 是 RoutedEventHandler（可用委托族），
    // 而接不上的 RightTapped / SelectionChanged 一律绕开。
    contextMenu_ =
        RootGrid().Resources().Lookup(box_value(L"RowContextMenu")).as<MenuFlyout>();

    CtxStart().Click([this](auto&&, auto&&) { OnStartClicked(); });
    CtxStop().Click([this](auto&&, auto&&) { OnStopClicked(); });
    CtxRestart().Click([this](auto&&, auto&&) { OnRestartClicked(); });
    CtxDelete().Click([this](auto&&, auto&&) { OnDeleteClicked(); });

    // 工具箱默认以管理员身份运行（manifest 声明 requireAdministrator），
    // 所以这个标志恒为 true。保留 SetElevated 调用是因为它还决定
    // 「非提权时查不到启动类型」这条降级路径要不要走。
    controller_.SetElevated(true);
    DWORD err = ERROR_SUCCESS;
    if (!controller_.Start(err)) {
        // 连服务控制管理器都打不开的话，列不出任何东西 ——
        // 开一个空窗口让人对着看没有意义，直接以错误码退出。
        LINGWU_TRACE("Controller::Start failed err=%lu", (unsigned long)err);
        winrt::check_hresult(
            winrt::hresult(HRESULT_FROM_WIN32(err ? static_cast<LONG>(err) : ERROR_ACCESS_DENIED)));
    }

    // UI 侧的 100 ms 心跳：拉后台结果 + 比对两个 ComboBox 的选中项。
    //
    // 【为什么用轮询而不是事件】
    // 本版 cppwinrt 生成的 Microsoft.UI.Xaml.Input.h 里所有 delegate 的模板
    // 构造函数都漏了 inline，Microsoft.UI.* 的事件一个都接不上。
    // 与其为此绕开 SelectionChanged 的 lambda，不如干脆用「比对 SelectedIndex」
    // 这个纯读操作 —— 它对时机的要求只有「下一帧之内」，
    // 而 100 ms 的心跳周期完全满足，用户感知不到差别。
    // 【为什么必须走 DispatcherQueue.CreateTimer()】
    // DispatcherQueueTimer 在 WinUI 3 的 C++/WinRT 投影里**没有默认构造函数**
    // （那是 UWP 的运行时可构造类型；WinUI 3 把它做成了必须由队列工厂创建）。
    // 直接写 DispatcherQueueTimer() 报 C2512「没有合适的默认构造函数」，
    // 而 UWP 时代的样例代码里全是这么写的。
    timer_ = this->DispatcherQueue().CreateTimer();
    timer_.Interval(std::chrono::milliseconds(100));
    timer_.Tick([this](auto&&, auto&&) { OnTimerTick(); });
    timer_.Start();

    // 页面被摘下可视树（窗口关闭）时，后台线程必须停。
    // Closed 才是「窗口真的没了」的正确时刻，但页面拿不到窗口的 Closed ——
    // 所以这里用 Unloaded（元素离开可视树），再由外壳的 Shutdown() 兜底。
    Unloaded([this](auto&&, auto&&) {
        if (!alive_->exchange(false)) {
            return;
        }
        LINGWU_TRACE("ServicePage: unloaded, stopping controller");
        try {
            if (timer_) timer_.Stop();
            controller_.Stop();
        } catch (std::exception const& e) {
            LINGWU_TRACE("ServicePage unload: cleanup threw %s", e.what());
        }
    });

    LINGWU_TRACE("ServicePage: initialized");
}

// ---------------------------------------------------------------- 公共入口
void ServicePage::RefreshNow() {
    // 切回本模块时补一次刷新。不可见期间心跳仍在跑（页面常驻），
    // 但列表重建在不可见时被跳过了，所以这里主动拉一次。
    ForceReorderNext();
    RebuildRows();
    UpdateStatusBar();
}

void ServicePage::Shutdown() {
    if (!alive_->exchange(false)) {
        // 已经清理过了。Closed 可能被走两遍（提权路径 Close 一次、框架再走一次）。
        return;
    }
    // 退出前把本模块的偏好落盘。
    // 原来这件事在窗口的 Closed 处理器里做，现在窗口归外壳了，
    // 但**本模块的三个键**（排序/筛选/详情展开）仍然归本页负责。
    // 窗口几何不在这里存 —— 那是外壳的资产。
    try {
        SaveServiceSettings();
    } catch (std::exception const& e) {
        LINGWU_TRACE("ServicePage::Shutdown save threw %s", e.what());
    }
    try {
        if (timer_) timer_.Stop();
        controller_.Stop();
    } catch (std::exception const& e) {
        LINGWU_TRACE("ServicePage::Shutdown cleanup threw %s", e.what());
    }
    // 「新建服务」窗口必须真正关闭：只要还有一个 Window 活着，
    // XAML 框架的消息循环就不会结束，进程会僵留在「窗口全没了但还在」。
    if (createWindow_) {
        try {
            createWindow_.Close();
        } catch (...) {
            // 关闭流程里不能再抛。
        }
        createWindow_ = nullptr;
    }
    LINGWU_TRACE("ServicePage: shutdown");
}

// 行右键菜单的命中测试。外壳的窗口子类拦到 WM_CONTEXTMENU 后回调这里。
//
// 【为什么由页面自己做】
// 命中测试要用到行高、列表位置、当前滚动偏移 —— 这些都是本页面的布局信息。
// 外壳不知道这些，硬塞给它等于把页面的布局知识复制一份到外壳里。
bool ServicePage::HitTestRow(winrt::Windows::Foundation::Point const& clientPt) {
    if (!contextMenu_ || contextRowCount_ <= 0) return false;

    // 第一步必须把**物理像素换算成逻辑像素**，这一步漏了会同时错两处。
    //
    // WM_CONTEXTMENU 的 lParam 经 ScreenToClient 得到的是**物理**客户区像素，
    // 而下面用到的三个量全是**逻辑**像素：
    //   · TransformToVisual().TransformPoint() 的返回（XAML 坐标系是 DIP）
    //   · ScrollPresenter.VerticalOffset()
    //   · kRowHeight = 32（与 XAML 里 <Grid Height="32"> 对应）
    // 两者只在 100% 缩放下相等。本机是 200%，不换算的话
    // index = (物理 y − 逻辑 origin) / 32 ≈ 真实行号的 2 倍，
    // 表现为「右键某一行的落点随机地命中别的行、或干脆越界不发生任何事」。
    double scale = 1.0;
    if (auto xr = RootGrid().XamlRoot()) {
        scale = xr.RasterizationScale();
    }
    if (scale <= 0.0) scale = 1.0;

    // Windows::Foundation::Point 的成员是 **float**，/WX 下必须显式收窄。
    const winrt::Windows::Foundation::Point logical{
        static_cast<float>(clientPt.X / scale),
        static_cast<float>(clientPt.Y / scale) };

    const auto origin = ServiceList().TransformToVisual(RootGrid())
                            .TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
    double y = logical.Y - origin.Y;
    if (y < 0.0) return false;

    // 必须叠加当前滚动偏移，否则滚下去之后命中算出来的下标会整体偏。
    double offset = 0.0;
    if (auto sv = ServiceList().try_as<IScrollView>()) {
        if (auto sp = sv.ScrollPresenter()) offset = sp.VerticalOffset();
    }
    y += offset;
    const int idx = static_cast<int>(y / kRowHeight);
    if (idx < 0 || idx >= contextRowCount_) return false;

    ServiceList().SelectedIndex(idx);
    // ShowAt 的坐标是**相对 target（列表）**的逻辑坐标。
    contextMenu_.ShowAt(ServiceList(), winrt::Windows::Foundation::Point{
                                        static_cast<float>(logical.X - origin.X),
                                        static_cast<float>(logical.Y - origin.Y) });
    return true;
}

void ServicePage::SaveServiceSettings() {
    // 【窗口位置不在这里存】
    // 合并成单窗口之后，窗口几何是**外壳**的资产（两个模块共用一个窗口，
    // 位置只有一份，写两遍会互相覆盖）。服务页只写自己模块的三个键。
    settings_->serviceSortMode = static_cast<int>(sortMode_);
    settings_->serviceFilterMode = static_cast<int>(filterMode_);
    settings_->serviceDetailExpanded = DetailExpander().IsExpanded();
    lingwu::core::SaveSettings(*settings_);
}

void ServicePage::WireEvents() {
    // 事件全在代码后置里挂 lambda，不走 XAML 的 Click= 处理器。
    // 原因：C++/WinRT 的 XAML 事件处理器必须在 IDL 里声明成 runtimeclass 成员，
    // 每加一个按钮就要改一次 MainWindow.idl 并触发 MIDL 重跑；
    // 而按 x:Name 取元素 + lambda 是官方 C++ 模板同样采用的做法。
    HeaderName().Click([this](auto&&, auto&&) { OnSortHeaderClick(kColName); });
    HeaderState().Click([this](auto&&, auto&&) { OnSortHeaderClick(kColState); });
    HeaderStartType().Click([this](auto&&, auto&&) { OnSortHeaderClick(kColStartType); });

    SearchBox().TextChanged([this](auto const& s, auto const&) { OnSearchChanged(s); });
    // 搜索图标在代码里设，不在 XAML 里写 QueryIcon="<FontIcon .../>"。
    // 那是字符串类型的属性，属性值里再嵌一层 XML，& 与引号得转义两次；
    // 转义稍有偏差就是运行期 0x802B000A（编译期完全看不出来）。
    {
        auto searchIcon = FontIcon();
        searchIcon.Glyph(L"\xE721");   // Segoe Fluent Icons: Search
        SearchBox().QueryIcon(searchIcon);
    }

    NewServiceButton().Click([this](auto&&, auto&&) { OnNewServiceClicked(); });
    RefreshButton().Click([this](auto&&, auto&&) { OnRefreshClicked(); });
    StartButton().Click([this](auto&&, auto&&) { OnStartClicked(); });
    StopButton().Click([this](auto&&, auto&&) { OnStopClicked(); });
    RestartButton().Click([this](auto&&, auto&&) { OnRestartClicked(); });
    DeleteButton().Click([this](auto&&, auto&&) { OnDeleteClicked(); });
    StartTypeApplyButton().Click([this](auto&&, auto&&) { OnStartTypeApplyClicked(); });

    ServiceList().SelectionChanged([this](auto const& s, auto const& a) {
        OnListSelectionChanged(s, a);
    });

    // 筛选与启动类型两个下拉的选项在代码里填。
    // 「选项顺序 = 枚举值顺序」这条不变量写在同一个函数里，
    // 以后加一项只改这一处。
    {
        auto items = FilterBox().Items();
        for (int i = 0; i <= 4; ++i) {
            auto item = ComboBoxItem();
            item.Content(box_value(core::FilterText(static_cast<core::FilterMode>(i))));
            items.Append(item);
        }
        FilterBox().SelectedIndex(static_cast<int>(filterMode_));
        lastFilterIndex_ = static_cast<int>(filterMode_);
    }
    {
        auto items = StartTypeBox().Items();
        for (int i = 0; i <= 3; ++i) {
            auto item = ComboBoxItem();
            item.Content(box_value(
                core::StartTypeChoiceText(static_cast<core::StartTypeChoice>(i))));
            items.Append(item);
        }
        StartTypeBox().SelectedIndex(-1);   // 由选中服务时按其当前启动类型填
    }

    // 对话框互斥闸门：开之前置位、Closed 时清位，缺一处就永久锁死。
    //
    // 闸门由**外壳**创建并注入，两个页面 + 外壳的关闭确认框共用同一把
    // （合并成一个窗口后，同一个 XamlRoot 上只能挂一个 ContentDialog，
    //   这条约束从「各窗口各管各的」升级成「全局唯一」）。
    confirm_.Closed([this](auto&&, auto&&) {
        if (gate_) gate_->release();
        // 关闭确认框时如果还挂着一个「要弹的结果提示」，
        // 在这里弹 —— 而不是在本框还没关的时候弹。
        //
        // 【为什么必须等 Closed】
        // 如果在 PrimaryButtonClick 里就弹第二个 ContentDialog，
        // 本框的 Closed 事件随后才触发，它会把闸门**清掉**，
        // 于是第二个框还开着、闸门却已经空闲 —— 别的模块这时就能再弹一个，
        // 三个框挤在一个 XamlRoot 上（症状：对话框一闪就没了、窗口也不动）。
        // 合并成单窗口之后这个窗口期是可以被用户踩到的：
        // 服务页弹着提示时切到性能页点「结束任务」即可。
        if (!pendingOutcomeTitle_.empty()) {
            auto title = std::move(pendingOutcomeTitle_);
            auto message = std::move(pendingOutcomeMessage_);
            pendingOutcomeTitle_.clear();
            pendingOutcomeMessage_.clear();
            ShowOutcome(title, message, false);
        }
    });
    notice_.Closed([this](auto&&, auto&&) { if (gate_) gate_->release(); });

    // 提权按钮的处理器只挂一次，ShowOutcome 负责决定这一次要不要显示它。
    // 删除确认的主按钮：此刻 deletePromptInput_ 里已经回读完毕。
    confirm_.PrimaryButtonClick([this](auto&&, auto&&) { OnDeleteConfirmed(); });
}

// ---------------------------------------------------------------- 心跳
void ServicePage::OnTimerTick() {
    if (!alive_->load()) {
        return;
    }
    DrainController();
}

void ServicePage::DrainController() {
    // ---- 1. 两个下拉的选中项比对（绕开接不上的 SelectionChanged）----
    const int filterIndex = FilterBox().SelectedIndex();
    if (filterIndex >= 0 && filterIndex != lastFilterIndex_) {
        lastFilterIndex_ = filterIndex;
        filterMode_ = static_cast<core::FilterMode>(filterIndex);
        SaveServiceSettings();
        // 「第三方」需要全量可执行路径，这里才付那 N 次 RPC。
        if (filterMode_ == core::FilterMode::ThirdParty && !controller_.ThirdPartyReady()) {
            controller_.RequestThirdPartyScan();
        }
        ForceReorderNext();
        RebuildRows();
    }

    const int startTypeIndex = StartTypeBox().SelectedIndex();
    if (startTypeIndex != lastStartTypeIndex_) {
        // 只是用户改了选择，**不立刻提交**。
        //
        // 【为什么不在这里直接改启动类型】那等于「用户翻了一下下拉就把
        // 服务配置改了」，而且下拉在回填当前值时也会触发一次 ——
        // 表现为「打开详情面板就莫名其妙改了服务配置」。
        // 真正的提交点在「点应用」上，见 StartTypeApplyButton。
        lastStartTypeIndex_ = startTypeIndex;
        StartTypeApplyButton().IsEnabled(startTypeIndex >= 0 && !selectedName_.empty());
    }

    // ---- 2. 新一帧枚举 ----
    std::vector<core::ServiceInfo> frame;
    if (controller_.TakeSnapshot(frame)) {
        snapshot_.swap(frame);
        totalCount_ = snapshot_.size();
        lastRefreshTick_ = ::GetTickCount64();
        RebuildRows();
    }

    // ---- 3. 操作结果 ----
    // 注意 ShowOutcome 的两个形参都是 **hstring**（不是 IInspectable），
    // 所以这里传 L"..." 与 hstring{std::wstring}；
    // box_value 的返回值是 IInspectable，喂给 hstring 形参会报 C2664。
    core::OpOutcome outcome;
    while (controller_.TakeResult(outcome)) {
        const hstring msg{ outcome.message };
        switch (outcome.result) {
            case core::OpResult::Ok:
                // 成功静默：列表下一帧就会落到真实状态，再弹框只是噪音。
                if (!outcome.message.empty()) {
                    ShowOutcome(L"完成", msg, false);
                }
                break;
            case core::OpResult::TimedOut:
                ShowOutcome(L"仍在进行中", msg, false);
                break;
            case core::OpResult::NeedsAdmin:
                ShowOutcome(L"需要管理员权限", msg, true);
                break;
            case core::OpResult::Busy:
                ShowOutcome(L"请稍候", L"上一个操作还在进行中。", false);
                break;
            default:
                ShowOutcome(L"操作失败", msg, false);
                break;
        }
    }

    // ---- 4. 详情就绪后刷新详情面板 ----
    if (!selectedName_.empty() && !detailShown_) {
        core::ServiceDetails details;
        if (controller_.DetailsReady(selectedName_, details)) {
            detailShown_ = true;
            DetailBinaryPath().Text(hstring(details.binaryPath));
            DetailAccount().Text(hstring(details.account));
            DetailDescription().Text(hstring(
                details.description.empty() ? std::wstring(L"（无）") : details.description));
            // 依赖是 \n 分隔的多行文本，XAML 侧 TextWrapping=Wrap。
            DetailDependencies().Text(hstring(
                details.dependencies.empty() ? std::wstring(L"（无）") : details.dependencies));

            core::ServiceInfo info;
            for (auto const& s : snapshot_) {
                if (s.name == selectedName_) { info = s; break; }
            }
            wchar_t status[160];
            if (info.win32ExitCode == 0) {
                ::swprintf_s(status, L"退出代码：0（无错误）");
            } else {
                ::swprintf_s(status, L"退出代码：%lu（服务专用代码 %lu）",
                             static_cast<unsigned long>(info.win32ExitCode),
                             static_cast<unsigned long>(info.serviceSpecificExitCode));
            }
            DetailStatus().Text(hstring(
                core::StateText(info.state) + L"　·　" + status));

            // 按钮可用性跟着当前状态走：已停的服务不能「停止」，
            // 运行中的不能「启动」。不给这个反馈的话用户点了会拿到
            // 一个「服务已经/尚未运行」的无意义提示。
            const bool running = (info.state == 4 /* SERVICE_RUNNING */);
            StartButton().IsEnabled(!running);
            StopButton().IsEnabled(running);
            RestartButton().IsEnabled(!info.markedForDelete);
            DeleteButton().IsEnabled(true);
        }
    }
}

// ---------------------------------------------------------------- 列表
void ServicePage::RebuildRows() {
    if (!alive_->load()) {
        return;
    }
    // 窗口隐藏到托盘时**不重建列表**：没人看得见，纯属浪费。
    // 后台枚举照常在跑（数据要保持新鲜），只是不往 UI 上灌。
    // 窗口重新可见时由外壳调 RefreshNow() 补一次。
    if (!hostVisible_) {
        return;
    }
    // 本页不是当前活动标签页时**同样不重建列表**。
    //
    // 【为什么这条不能少 —— 2026-10-25 实测】
    // 性能页一直有这道守卫（见 MonitorPage::Refresh），本页**漏了**，
    // 于是本页在被切走的标签页上仍然每秒重建 345 行的列表。
    // 本页每行有 5 个 TextBlock，345 行 = 1725 次 TextBlock.Text 更新/秒，
    // 全部发生在没人看得见的页面上。
    //
    // 实测效果（改之前 → 改之后）：
    //   改之前：切走的标签页上后台 CPU 照跑；
    //   改之后：后台 CPU 掉一半。
    // **私有内存峰值没变**（199.9 → 199.9 MB）—— 常驻内存的大头不是这里，
    // 是 ListView 已实现的行容器（见 MonitorPage.xaml 里 CacheLength 那段）。
    // 这条守卫的价值是**别在没人看的页面上烧 CPU**，不是省内存。
    //
    // 【数据会不会过期】不会。枚举线程完全不受影响，数据一直在采；
    // 切回本页时外壳的 ShowModule 先把 Visibility 置成 Visible，
    // 再调 RefreshNow() 补一次全量重建（顺序见 MainWindow::ShowModule），
    // 显示出来时拿到的就是此刻的快照。
    if (Visibility() != Visibility::Visible) {
        return;
    }

    // ---- 过滤 ----
    std::vector<core::ServiceInfo const*> visible;
    visible.reserve(snapshot_.size());
    const bool needPaths = (filterMode_ == core::FilterMode::ThirdParty);
    for (auto const& s : snapshot_) {
        if (!filter_.empty() &&
            !ContainsCI(s.name, filter_) && !ContainsCI(s.displayName, filter_)) {
            continue;
        }
        switch (filterMode_) {
            case core::FilterMode::All:
                break;
            case core::FilterMode::Running:
                if (s.state != 4 /* SERVICE_RUNNING */) continue;
                break;
            case core::FilterMode::Stopped:
                // 「已停止」= 没有任何 pending 动作。标记待删除但进程还活着的
                // 服务不算，否则它在用户眼里会凭空消失。
                if (s.state != 1 /* SERVICE_STOPPED */) continue;
                break;
            case core::FilterMode::Disabled:
                if (!s.startTypeKnown || s.startType != 2 /* SERVICE_DISABLED */) continue;
                break;
            case core::FilterMode::ThirdParty:
                // 扫描还在跑时先不过滤，扫完那一帧自然收敛。
                if (controller_.ThirdPartyReady() && !controller_.IsThirdParty(s.name)) continue;
                break;
        }
        visible.push_back(&s);
    }

    // ---- 排序 ----
    // 每一列两个方向，方向由 SortMode 自带（见 service_model.h）。
    // 次级键统一是名称 asc —— 在「状态降序」下面按名称升序排列同状态的服务，
    // 比再按状态来一次要有用得多。
    const bool descending = core::SortDescending(sortMode_);
    std::sort(visible.begin(), visible.end(),
              [this, descending](core::ServiceInfo const* a, core::ServiceInfo const* b) {
        switch (core::SortColumn(sortMode_)) {
            case 1:   // 状态
                if (a->state != b->state) {
                    return descending ? (a->state < b->state) : (a->state > b->state);
                }
                break;
            case 2: {  // 启动类型
                // 不知道启动类型的永远排最后，而不是混在「手动」里 ——
                // 非提权时有一批服务查不到，把它们塞进某一档是撒谎。
                if (a->startTypeKnown != b->startTypeKnown) return b->startTypeKnown;
                if (a->startTypeKnown && a->startType != b->startType) {
                    return descending ? (a->startType > b->startType)
                                      : (a->startType < b->startType);
                }
                break;
            }
            default:   // 名称
                break;
        }
        const int cmp = ::CompareStringW(LOCALE_INVARIANT, 0, a->name.c_str(),
                                         static_cast<int>(a->name.size()),
                                         b->name.c_str(),
                                         static_cast<int>(b->name.size()));
        if (cmp == CSTR_LESS_THAN) return !descending;
        if (cmp == CSTR_GREATER_THAN) return descending;
        return false;
    });

    const size_t want = visible.size();
    visibleCount_ = want;

    // ---- ItemsSource 终身只赋一次 ----
    if (!listBound_) {
        source_ = single_threaded_observable_vector<IInspectable>();
        ServiceList().ItemsSource(source_);
        listBound_ = true;
    }

    // ---- 键集合变了？决定是否走「调长度」这条结构性路径 ----
    //
    // 两边键集合相同 → 纯置换 → 逐槽 SetAt（不触发结构性失效）。
    // 键集合不同 → 必须增删，此时用 RemoveAtEnd / Append 调长度，
    // 它同样是结构性的，但只在真的增删行时才发生，比例远低于置换。
    //
    // 【为什么不能全程只用 SetAt】
    // 长度变了就必须调长度，Append 与 RemoveAtEnd 是 IObservableVector 上
    // 唯一能改变 Size 的办法（RemoveAtEnd 每次只去一个）。
    bool keysChanged = (want != currentOrder_.size());
    if (!keysChanged) {
        for (size_t i = 0; i < want; ++i) {
            if (visible[i]->name != currentOrder_[i]) { keysChanged = true; break; }
        }
    }

    const bool lengthChanged = (source_.Size() != want);
    const ULONGLONG now = ::GetTickCount64();
    const bool throttleExpired = (lastOrderApplyTick_ == 0) ||
                                 (now - lastOrderApplyTick_ >= kReorderIntervalMs);
    // 长度变化必须立即应用，否则新注册的服务会「迟到几秒才出现」——
    // 那是看得出来的功能缺陷。纯顺序变化才走节流。
    const bool applyOrder = lengthChanged || keysChanged || throttleExpired;

    if (!applyOrder && !needPaths) {
        // 只更新数值，不动顺序 —— 这是绝大多数帧的路径，开销只有 TextBlock.Text。
        for (size_t i = 0; i < want; ++i) {
            auto it = rowByKey_.find(currentOrder_[i]);
            if (it == rowByKey_.end()) continue;
            UpdateRow(it->second, *visible[i]);
        }
        UpdateStatusBar();
        return;
    }

    if (!throttleExpired && !lengthChanged && !keysChanged) {
        return;
    }

    // ---- 调长度 ----
    if (lengthChanged) {
        if (source_.Size() > want) {
            while (source_.Size() > want) {
                source_.RemoveAtEnd();
            }
        } else {
            while (source_.Size() < want) {
                source_.Append(nullptr);
            }
        }
    }

    // ---- 逐槽对齐 ----
    currentOrder_.clear();
    currentOrder_.reserve(want);
    for (size_t i = 0; i < want; ++i) {
        auto const& info = *visible[i];
        auto it = rowByKey_.find(info.name);
        winrt::LingwuToolbox::ServiceRow row{ nullptr };
        if (it != rowByKey_.end()) {
            row = it->second;
        } else {
            row = winrt::make<ServiceRow>();
            rowByKey_[info.name] = row;
        }
        UpdateRow(row, info);
        // 只有「这一槽要放的对象与现在不同」才 SetAt。SetAt 是 replace 语义，
        // 不会让 ItemsRepeater 作废已实现区段；而每槽都 SetAt 会白白
        // 重建同名的行容器。
        auto current = source_.GetAt(static_cast<uint32_t>(i)).try_as<
            winrt::LingwuToolbox::ServiceRow>();
        if (!current || current.Key() != row.Key()) {
            source_.SetAt(static_cast<uint32_t>(i), row);
        }
        currentOrder_.push_back(info.name);
    }

    // 已经在 source_ 里但被筛掉的行要从 rowByKey_ 里清掉，
    // 否则它会一直被强引用着（内存只增不减）。
    if (keysChanged || lengthChanged) {
        for (auto it = rowByKey_.begin(); it != rowByKey_.end();) {
            bool stillPresent = false;
            for (auto const& n : currentOrder_) {
                if (n == it->first) { stillPresent = true; break; }
            }
            if (!stillPresent) {
                it = rowByKey_.erase(it);
            } else {
                ++it;
            }
        }
    }

    contextRowCount_ = static_cast<int>(want);
    lastOrderApplyTick_ = now;
    UpdateStatusBar();
}

void ServicePage::ForceReorderNext() {
    // 节流是为「状态每两秒自然抖动」设计的；用户的明确操作不该等它。
    lastOrderApplyTick_ = 0;
}

// ---------------------------------------------------------------- 状态栏
void ServicePage::UpdateStatusBar() {
    wchar_t left[192];
    ::swprintf_s(left, L"共 %zu 个服务 · 筛选出 %zu 个", totalCount_, visibleCount_);
    StatusLeft().Text(hstring(left));

    const bool elevated = controller_.Elevated();
    std::wstring right = elevated ? L"管理员权限" : L"标准用户权限";
    if (lastRefreshTick_ != 0) {
        const ULONGLONG ago = (::GetTickCount64() - lastRefreshTick_) / 1000;
        wchar_t buf[64];
        ::swprintf_s(buf, L" · %llu 秒前刷新", static_cast<unsigned long long>(ago));
        right += buf;
    }
    StatusRight().Text(hstring(right));
}

void ServicePage::UpdateSortIndicators() {
    auto setArrow = [](FrameworkElement const& arrow, RotateTransform const& rot,
                       bool visible, double angle) {
        if (visible) {
            arrow.Visibility(Visibility::Visible);
            rot.Angle(angle);
        } else {
            arrow.Visibility(Visibility::Collapsed);
        }
    };

    // 方向来自 SortMode，列也来自 SortMode —— 两处都不要再各写一份判断，
    // 否则加新排序档时箭头一定会漏掉一列。
    const int active = static_cast<int>(core::SortColumn(sortMode_));
    const double angle = core::SortArrowAngle(sortMode_);

    setArrow(ArrowName(), RotName(), active == kColName, angle);
    setArrow(ArrowState(), RotState(), active == kColState, angle);
    setArrow(ArrowStartType(), RotStartType(), active == kColStartType, angle);
}

void ServicePage::UpdateRow(winrt::LingwuToolbox::ServiceRow const& row,
                           core::ServiceInfo const& info) {
    // 显示名与服务名相同时不重复显示 —— 服务管理里两者相同的占多数，
    // 并排显示两遍同样的文字会让名字列显得又长又挤。
    const std::wstring disp =
        (info.displayName == info.name) ? std::wstring() : info.displayName;

    const std::wstring stateText = core::StateText(info.state);

    const std::wstring startText = info.startTypeKnown
        ? core::StartTypeCellText(info.startType, info.delayedAutoStart, true)
        : std::wstring(L"—");

    const std::wstring pidText = (info.pid == 0) ? std::wstring(L"—")
                                                  : std::to_wstring(info.pid);

    // 标记待删除但进程还活着的服务，行上要看得出来 ——
    // 否则用户删完发现它还在列表里，会以为删除失败。
    const std::wstring shownState = info.markedForDelete
        ? stateText + L"（已标记删除）"
        : stateText;

    row.Update(winrt::hstring(info.name),
               winrt::hstring(disp),
               winrt::hstring(shownState),
               winrt::hstring(startText),
               winrt::hstring(pidText),
               info.state,
               info.startType);
}

// ---------------------------------------------------------------- 交互
void ServicePage::OnSortHeaderClick(size_t column) {
    // 三列各自在自己那一列的两档之间来回切。
    //
    // 【为什么名称列也参与切换】之前这里是 sortMode_ = NameAsc 写死，
    // 于是「名称」表头点下去**永远没有任何反应** —— 它看上去是按钮，
    // 视觉上也没有任何禁用暗示。一个看起来能用、点了却什么都不发生的
    // 控件，比没有这个控件更糟。services.msc 也是三列都能双向排。
    const int active = static_cast<int>(core::SortColumn(sortMode_));
    const int target = static_cast<int>(column);

    if (active != target) {
        // 换列。名称与启动类型从升序开始（升序就是字典序，符合直觉）。
        //
        // 状态列从**降序**开始，也就是「运行中的排前面」。状态是个枚举，
        // 升序 = 已停止 → 启动中 → 停止中 → 运行中，把一堆停掉的排在
        // 头一屏；几乎所有人点「状态」想看的就是运行中的那些。
        switch (target) {
            case kColState:
                sortMode_ = core::SortMode::StateDesc;
                break;
            case kColStartType:
                sortMode_ = core::SortMode::StartTypeAsc;
                break;
            default:
                sortMode_ = core::SortMode::NameAsc;
                break;
        }
    } else {
        // 同一列，再点一次翻向另一个方向。
        const core::SortMode now = (target == kColName)
            ? (core::SortDescending(sortMode_) ? core::SortMode::NameAsc
                                                : core::SortMode::NameDesc)
            : (target == kColState)
                ? (core::SortDescending(sortMode_) ? core::SortMode::StateAsc
                                                    : core::SortMode::StateDesc)
                : (core::SortDescending(sortMode_) ? core::SortMode::StartTypeAsc
                                                    : core::SortMode::StartTypeDesc);
        sortMode_ = now;
    }

    // 排序偏好立刻落盘。走 SaveServiceSettings 而不是只写 serviceSortMode，
    // 这样「详情面板展开与否」也会一并记下来（它没有别的保存时机）。
    SaveServiceSettings();
    UpdateSortIndicators();
    ForceReorderNext();
    RebuildRows();
}

void ServicePage::OnSearchChanged(AutoSuggestBox const& sender) {
    // 不按 Reason() 分支。原来这里挡掉了除 UserInput 以外的所有情况，
    // 理由是「程序性回写也会触发本事件」。但本工程**没有任何地方**
    // 回写 SearchBox().Text()（全仓库 grep 只有这一处读取），
    // 那个「自己给自己制造忙循环」的风险并不存在，
    // 而代价是：UIA 的 ValuePattern.SetValue 走的是程序性赋值，
    // 搜索框从此对自动化完全失灵 —— 唯一能验收它的通道被自己堵死了。
    // 真正的防抖在 RebuildRows 之前比对新旧文本，不依赖这里的判断。
    // hstring 不能直接当 std::wstring 的右值初始化，必须显式转换。
    const std::wstring next{ sender.Text().c_str() };
    if (next == filter_) {
        return;
    }
    filter_ = next;
    ForceReorderNext();
    RebuildRows();
}

void ServicePage::OnRefreshClicked() {
    controller_.RequestImmediateRefresh();
    StatusLeft().Text(hstring(L"正在刷新…"));
}

std::wstring ServicePage::SelectedServiceName() const {
    if (selectedName_.empty()) {
        return std::wstring();
    }
    return selectedName_;
}

void ServicePage::OnListSelectionChanged(IInspectable const& sender,
                                        SelectionChangedEventArgs const& args) {
    const int idx = ServiceList().SelectedIndex();
    if (idx < 0 || static_cast<size_t>(idx) >= currentOrder_.size()) {
        selectedName_.clear();
        StartButton().IsEnabled(false);
        StopButton().IsEnabled(false);
        RestartButton().IsEnabled(false);
        DeleteButton().IsEnabled(false);
        StartTypeApplyButton().IsEnabled(false);
        StartTypeBox().SelectedIndex(-1);
        return;
    }

    selectedName_ = currentOrder_[static_cast<size_t>(idx)];
    const bool hasSelection = !selectedName_.empty();
    StartButton().IsEnabled(hasSelection);
    StopButton().IsEnabled(hasSelection);
    RestartButton().IsEnabled(hasSelection);
    DeleteButton().IsEnabled(hasSelection);

    // 启动类型下拉回填当前值。
    //
    // 【回填动作不提交】SubmitStartType 只在「应用」按钮的处理器里被调。
    // 否则「打开详情面板」这个纯读动作就会把服务配置改掉 ——
    // 而且回填本身也会触发一次选中项变化，若那里直接提交就成了无限写循环。
    const core::ServiceInfo* info = nullptr;
    for (auto const& s : snapshot_) {
        if (s.name == selectedName_) { info = &s; break; }
    }
    if (info) {
        int choiceIndex = 0;
        if (info->startTypeKnown) {
            if (info->startType == 2 /* DISABLED */) {
                choiceIndex = 3;
            } else if (info->startType == 4 /* DEMAND */) {
                choiceIndex = 2;
            } else if (info->startType == 3 /* AUTO */) {
                choiceIndex = info->delayedAutoStart ? 1 : 0;
            }
        }
        StartTypeBox().SelectedIndex(choiceIndex);
        lastStartTypeIndex_ = choiceIndex;
        StartTypeApplyButton().IsEnabled(false);
    } else {
        StartTypeBox().SelectedIndex(-1);
        lastStartTypeIndex_ = -1;
    }

    // 详情走队列查询（每条一次 RPC），填好前清空面板并标记「尚未取到」。
    detailShown_ = false;
    DetailBinaryPath().Text(hstring(L"正在读取…"));
    DetailAccount().Text(hstring(L""));
    DetailDescription().Text(hstring(L""));
    DetailDependencies().Text(hstring(L""));
    DetailStatus().Text(hstring(L""));
    controller_.RequestDetails(selectedName_);
}

void ServicePage::OnStartClicked() {
    const std::wstring name = SelectedServiceName();
    if (name.empty()) return;
    core::Operation op;
    op.kind = core::OpKind::Start;
    op.name = name;
    if (!controller_.Enqueue(std::move(op))) {
        ShowOutcome(L"请稍候", L"上一个操作还在进行中。", false);
    }
}

void ServicePage::OnStopClicked() {
    const std::wstring name = SelectedServiceName();
    if (name.empty()) return;
    core::Operation op;
    op.kind = core::OpKind::Stop;
    op.name = name;
    if (!controller_.Enqueue(std::move(op))) {
        ShowOutcome(L"请稍候", L"上一个操作还在进行中。", false);
    }
}

void ServicePage::OnRestartClicked() {
    const std::wstring name = SelectedServiceName();
    if (name.empty()) return;
    core::Operation op;
    op.kind = core::OpKind::Restart;
    op.name = name;
    if (!controller_.Enqueue(std::move(op))) {
        ShowOutcome(L"请稍候", L"上一个操作还在进行中。", false);
    }
}

void ServicePage::OnStartTypeApplyClicked() {
    const std::wstring name = SelectedServiceName();
    const int index = StartTypeBox().SelectedIndex();
    if (name.empty() || index < 0 || index > 3) {
        return;
    }
    core::Operation op;
    op.kind = core::OpKind::SetStartType;
    op.name = name;
    op.startTypeChoice = index;
    if (!controller_.Enqueue(std::move(op))) {
        ShowOutcome(L"请稍候", L"上一个操作还在进行中。", false);
    }
}

void ServicePage::OnDeleteClicked() {
    const std::wstring name = SelectedServiceName();
    if (name.empty()) {
        return;
    }
    if ((gate_ && gate_->is_open())) {
        LINGWU_TRACE("OnDeleteClicked ignored: a dialog is already open");
        return;
    }

    // 判据要三样都拿全：状态决定能不能直接删，可执行路径决定是不是第三方。
    core::ServiceInfo info;
    bool found = false;
    for (auto const& s : snapshot_) {
        if (s.name == name) { info = s; found = true; break; }
    }
    if (!found) {
        ShowOutcome(L"操作失败", L"该服务已不在当前列表中。", false);
        return;
    }

    core::ServiceDetails details;
    const bool thirdParty = controller_.DetailsReady(name, details) &&
                            details.accessible &&
                            core::IsThirdPartyBinary(details.binaryPath);

    const bool running = (info.state != 1 /* SERVICE_STOPPED */);
    ShowDeleteConfirm(name, thirdParty, running);
}

void ServicePage::OnDeleteConfirmed() {
    if (pendingDeleteName_.empty()) {
        return;
    }
    const std::wstring name = pendingDeleteName_;
    const bool thirdParty = pendingDeleteThirdParty_;
    const bool stopFirst = pendingDeleteStopFirst_;
    pendingDeleteName_.clear();

    // 第三方服务的输入确认在这里校验。
    //
    // 【校验放在确认按钮的处理器里，而不是关掉主按钮】
    // 关掉按钮的话，用户输错了没有任何反馈 —— 按下去**什么都不发生**，
    // 而「没反应」和「点错了」在界面上长得一模一样。
    // 改成校验不过就重新弹一次并把提示写明确。
    if (thirdParty && !stopFirst) {
        const std::wstring typed{ deletePromptInput_.Text() };
        if (typed != name) {
            LINGWU_TRACE("delete confirm: typed \"%ls\" != \"%ls\", rejected",
                          typed.c_str(), name.c_str());
            // 【不直接在这里弹提示框】
            // 确认框此刻还开着，而闸门还被它占着。
            // 现在弹第二个 ContentDialog 的话，本框随后触发的 Closed
            // 会把闸门清掉，于是「第二个框还开着、闸门却空闲」。
            // 改成**登记**一条待弹提示，由 confirm_.Closed 在本框彻底关掉后弹。
            pendingOutcomeTitle_ = L"未删除";
            pendingOutcomeMessage_ =
                L"输入的服务名与要删除的服务不一致，未执行删除。";
            return;
        }
    }

    core::Operation op;
    op.kind = core::OpKind::Delete;
    op.name = name;
    op.stopFirst = stopFirst;
    if (!controller_.Enqueue(std::move(op))) {
        ShowOutcome(L"请稍候", L"上一个操作还在进行中。", false);
    }
}

void ServicePage::OnNewServiceClicked() {
    if (!createWindow_) {
        createWindow_ = winrt::make<CreateServiceWindow>();
        // Closed 只挂一次：反复 App Window 的 Closed 回调会把同一个闭包
        // 挂进事件列表多次，第二次关窗口时回调要跑两遍。
        createWindow_.Closed([this](IInspectable const&, IInspectable const&) {
            if (!alive_->load()) return;
            // 先看窗口是怎么关的：「取消」与 X 关闭都必须什么都不做。
            // 判断依据不能是「字段有没有填」—— 取消的时候字段照样是有内容的，
            // 那样用户点取消反而会凭空建出一个服务。
            if (!createWindow_.Accepted()) {
                LINGWU_TRACE("create window closed without Create; ignoring");
                return;
            }
            // 把用户填的字段读回来，逐项校验，通过了才真的去创建。
            // 空服务名 = 用户直接关了窗口而不是点确定，什么都不该做。
            const std::wstring sName{ createWindow_.ServiceNameInput() };
            if (sName.empty()) {
                return;
            }

            const int32_t startTypeChoice = createWindow_.StartTypeChoice();

            core::Operation op;
            op.kind = core::OpKind::Create;
            op.create.serviceName = sName;
            op.create.displayName = std::wstring{ createWindow_.DisplayNameInput() };
            op.create.description = std::wstring{ createWindow_.DescriptionInput() };
            op.create.binaryPath = std::wstring{ createWindow_.BinaryPathInput() };
            op.create.startType = core::StartTypeToDWord(
                static_cast<core::StartTypeChoice>(startTypeChoice));
            op.create.delayedAutoStart =
                core::ChoiceIsDelayed(static_cast<core::StartTypeChoice>(startTypeChoice));
            op.create.account = std::wstring{ createWindow_.AccountInput() };
            op.create.startImmediately = createWindow_.StartImmediatelyInput();

            if (!controller_.Enqueue(std::move(op))) {
                ShowOutcome(L"请稍候", L"上一个操作还在进行中。", false);
            }
        });
    }
    createWindow_.Activate();
}

// ---------------------------------------------------------------- 对话框
void ServicePage::ShowOutcome(hstring const& title, hstring const& message,
                             bool offerElevate) {
    (void)offerElevate;
    if (!alive_->load()) return;
    if ((gate_ && gate_->is_open())) {
        // 同一时刻只能有一个 ContentDialog。冲突时**只落 trace 不弹第二个** ——
        // 弹第二个会把已经开着的那个挤掉，用户看到的是「对话框一闪就没了」。
        LINGWU_TRACE("ShowOutcome skipped: dialog already open");
        return;
    }

    try {
        if (gate_ && !gate_->try_acquire()) return;
        notice_.XamlRoot(RootGrid().XamlRoot());
        // Title / Content 在 WinUI 3 里是 **IInspectable**（不是 hstring），
        // 直接传 L"..." 或 std::wstring 会报 C2665，必须包一层 box_value。
        notice_.Title(box_value(title));
        notice_.Content(box_value(message));
        // 【提权入口已删除】
        // 工具箱现在默认以管理员身份运行（manifest 声明 requireAdministrator），
        // 所以「以管理员身份重新启动」这个入口没有任何意义了 ——
        // 写操作应该直接生效，而不是失败后再让用户点一次。
        //
        // offerElevate 参数保留是为了不改动所有调用点，但它已经不再影响任何行为：
        // 参数名本身也在提醒「这里曾经有个提权分支」。
        notice_.PrimaryButtonText(L"知道了");
        notice_.CloseButtonText(L"");
        notice_.ShowAsync();
    } catch (winrt::hresult_error const& err) {
        LINGWU_TRACE("ShowOutcome threw hresult=0x%08X msg=%ls",
                      static_cast<unsigned>(err.code().value), err.message().c_str());
        if (gate_) gate_->release();
    }
}

void ServicePage::ShowDeleteConfirm(std::wstring const& name, bool thirdParty,
                                   bool running) {
    if (!alive_->load()) return;
    if ((gate_ && gate_->is_open())) {
        LINGWU_TRACE("ShowDeleteConfirm skipped: dialog already open");
        return;
    }

    pendingDeleteName_ = name;
    pendingDeleteThirdParty_ = thirdParty;
    pendingDeleteStopFirst_ = running;

    try {
        if (gate_ && !gate_->try_acquire()) return;
        confirm_.XamlRoot(RootGrid().XamlRoot());
        confirm_.Title(box_value(L"删除服务"));

        // 面板元素已经常驻（构造函数里建的），这里只改文字与可见性，
        // **不重新 Append** —— XAML 元素同一时刻只能有一个父节点，
        // 重复 Append 会抛异常。
        std::wstring text = L"确定要删除服务「" + name + L"」吗？\n\n";
        text += L"删除只移除注册信息，不会删除它的可执行文件，"
                L"也不会影响已安装的程序。此操作不可撤销。";

        if (running) {
            text += L"\n\n该服务当前正在运行。将先停止它，停止成功后再删除。";
        } else {
            text += L"\n\n该服务当前已停止，可以直接删除。";
        }

        if (thirdParty && !running) {
            // 第三方服务额外要求逐字输入服务名。
            //
            // 理由：服务名是 SCM 的稳定标识符，删错了没法在界面上找回来
            // （用户得自己记名字）。系统服务有 Windows 自己的保护，
            // 而第三方服务删错就是删错了。
            //
            // 服务在跑时不要求输入：那一步已经要「停止并删除」了，
            // 停不掉就不会走到删除，输入确认只是多一层无谓的摩擦。
            text += L"\n\n这是第三方服务，请输入完整服务名以确认：";
            deletePromptInput_.Visibility(Visibility::Visible);
            deletePromptInput_.Text(L"");
        } else {
            deletePromptInput_.Visibility(Visibility::Collapsed);
        }

        deletePromptText_.Text(winrt::hstring(text));
        confirm_.PrimaryButtonText(running ? L"停止并删除" : L"删除");
        confirm_.ShowAsync();
    } catch (winrt::hresult_error const& err) {
        LINGWU_TRACE("ShowDeleteConfirm threw hresult=0x%08X msg=%ls",
                      static_cast<unsigned>(err.code().value), err.message().c_str());
        if (gate_) gate_->release();
        pendingDeleteName_.clear();
    }
}

}  // namespace winrt::LingwuToolbox::implementation
