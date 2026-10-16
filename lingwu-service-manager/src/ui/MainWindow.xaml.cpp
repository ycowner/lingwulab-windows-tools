#include "pch.h"
#include "MainWindow.xaml.h"
// XAML 生成代码必须在这里显式引入一次（官方模板也是这么做的）。
// #if __has_include 是为了容忍「第一次编译、生成物还没出来」的情况。
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
// InitializeComponent() / Connect() / GetBindingConnector() 的**定义**在这里，
// 不在 .g.h 里（.g.h 只有声明）。.g.hpp 里有一条
//     template struct MainWindowT<struct MainWindow>;
// 显式实例化，正是它把这些成员模板实例化成可链接的符号。
#if __has_include("MainWindow.xaml.g.hpp")
#include "MainWindow.xaml.g.hpp"
#endif
// IWindowNative 是 MIDL 生成的 **COM 接口**，不是 WinRT 类型，
// 所以不会出现在 cppwinrt 生成的任何投影头里。它随 WinUI 包的 include 目录一起分发，
// 是官方文档里取 HWND 的标准做法。
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <windowsx.h>
#include <cwctype>
#include <algorithm>

#include "core/privilege.h"
#include "core/scm.h"
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

namespace winrt::LingwuServiceManager::implementation {
namespace core = lingsvc::core;

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

// 行高必须与 MainWindow.xaml 里 DataTemplate 的 <Grid Height="32"> 保持一致。
// 右键命中测试是**算术**定位：VisualTreeHelper::GetChildren 在这版投影里不存在
//（只有 GetChildrenCount / GetParent），没法遍历已实现的行容器去算包围盒，
// 所以这里用「内容坐标 y / 行高」直接推出下标。这是本文件与 XAML 之间唯一的耦合点。
constexpr double kRowHeight = 32.0;

// 右键命中：由窗口子类回调。返回 true 表示已处理（且菜单已弹出）。
bool HitTestRow(MenuHost* host, Point const& clientPt) {
    if (!host || !host->list || host->rowCount <= 0) return false;

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
    if (auto xr = host->root.XamlRoot()) {
        scale = xr.RasterizationScale();
    }
    if (scale <= 0.0) scale = 1.0;

    // Windows::Foundation::Point 的成员是 **float**，/WX 下必须显式收窄。
    const Point logical{
        static_cast<float>(clientPt.X / scale),
        static_cast<float>(clientPt.Y / scale) };

    const auto origin = host->list.TransformToVisual(host->root)
                            .TransformPoint(Point{ 0.0f, 0.0f });
    double y = logical.Y - origin.Y;
    if (y < 0.0) return false;

    // 必须叠加当前滚动偏移，否则滚下去之后命中算出来的下标会整体偏。
    double offset = 0.0;
    if (auto sv = host->list.try_as<IScrollView>()) {
        if (auto sp = sv.ScrollPresenter()) offset = sp.VerticalOffset();
    }
    y += offset;
    const int idx = static_cast<int>(y / kRowHeight);
    if (idx < 0 || idx >= host->rowCount) return false;

    host->list.SelectedIndex(idx);
    // ShowAt 的坐标是**相对 target（列表）**的逻辑坐标。
    host->menu.ShowAt(host->list, Point{
                                      static_cast<float>(logical.X - origin.X),
                                      static_cast<float>(logical.Y - origin.Y) });
    return true;
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                          UINT_PTR /*id*/, DWORD_PTR ref) {
    if (msg == WM_CONTEXTMENU) {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ::ScreenToClient(hwnd, &pt);
        if (HitTestRow(reinterpret_cast<MenuHost*>(ref),
                       Point{ static_cast<float>(pt.x), static_cast<float>(pt.y) })) {
            return 0;   // 吃掉，WinUI 不要再自己弹一次
        }
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------- 构造
MainWindow::MainWindow() {
    LINGSVC_TRACE("MainWindow ctor: enter");
    try {
        InitializeComponent();
    } catch (winrt::hresult_error const& err) {
        LINGSVC_TRACE("InitializeComponent threw hresult=0x%08X msg=%ls",
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

    ApplyBackdrop();

    // 自绘标题栏：内容延伸进标题栏区域，并把 DragRegion 设为可拖拽区。
    // 有了 SetTitleBar，DWM 的命中测试才会把这块区域交给 XAML，
    // 标题栏才能自己画内容（同时保留双击最大化 / 右键系统菜单）。
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(DragRegion());

    core::LoadSettings(settings_);

    // 默认尺寸与位置按**当前显示器的可用工作区**算，不写死像素。
    //
    // AppWindow 系列的 API 全部以**物理像素**为单位，与 XAML 的逻辑像素差一个
    // RasterizationScale；写死像素在 200% 缩放下只有一半大。
    try {
        constexpr double kDefaultRatio = 0.72;
        const auto work = DisplayArea::GetFromWindowId(
            AppWindow().Id(), DisplayAreaFallback::Nearest).WorkArea();

        const bool restorable =
            settings_.width > 0 && settings_.height > 0 &&
            settings_.left >= work.X && settings_.left < work.X + work.Width &&
            settings_.top >= work.Y && settings_.top < work.Y + work.Height;

        if (restorable) {
            // 上次的位置还落在当前显示器的工作区里，就恢复它。
            // 换了显示器 / 拔了外接屏时坐标会落在工作区外，这时退回默认布局 ——
            // 否则窗口会开在屏幕外，用户以为程序没启动。
            AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{
                settings_.left, settings_.top, settings_.width, settings_.height });
            LINGSVC_TRACE("MainWindow: restored rect=(%d,%d) %dx%d",
                          settings_.left, settings_.top,
                          settings_.width, settings_.height);
        } else {
            const int w = static_cast<int>(static_cast<double>(work.Width) * kDefaultRatio);
            const int h = static_cast<int>(static_cast<double>(work.Height) * kDefaultRatio);
            const int x = work.X + (work.Width - w) / 2;
            const int y = work.Y + (work.Height - h) / 2;
            AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{ x, y, w, h });
            LINGSVC_TRACE("MainWindow: default rect=(%d,%d) %dx%d physical", x, y, w, h);
        }
    } catch (winrt::hresult_error const&) {
        AppWindow().Resize(winrt::Windows::Graphics::SizeInt32{ 1040, 700 });
        LINGSVC_TRACE("MainWindow: DisplayArea failed, fell back to 1040x700");
    }

    if (auto pres = AppWindow().Presenter().try_as<OverlappedPresenter>()) {
        pres.PreferredMinimumWidth(760);
        pres.PreferredMinimumHeight(480);
    }

    filterMode_ = settings_.filterMode;
    sortMode_ = settings_.sortMode;
    DetailExpander().IsExpanded(settings_.detailExpanded);

    WireEvents();
    UpdateSortIndicators();

    HWND hwnd{};
    winrt::check_hresult(this->m_inner.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    hwnd_ = hwnd;
    // 发布 HWND 供第二份实例查找（按类名找是找不准的，
    // WinUI 3 的窗口类名是系统统一的）。
    platform::PublishMainHwnd(hwnd);

    // 右键菜单的弹出由窗口子类接管（WM_CONTEXTMENU）。
    // SetWindowSubclass 需要 comctl32.lib。
    // 右键菜单：菜单本身在 XAML 的 Grid.Resources 里（见 MainWindow.xaml 的说明 ——
    // MenuFlyout 在 C++ 投影里没有 MenuItems()）。
    // **弹出**走 Win32 子类的 WM_CONTEXTMENU，菜单**项**回到 Click；
    // 两者不冲突：Click 是 RoutedEventHandler（可用委托族），
    // 而接不上的 RightTapped / SelectionChanged 一律绕开。
    menuHost_ = std::make_shared<MenuHost>();
    menuHost_->list = ServiceList();
    menuHost_->root = RootGrid();
    menuHost_->menu =
        RootGrid().Resources().Lookup(box_value(L"RowContextMenu")).as<MenuFlyout>();

    CtxStart().Click([this](auto&&, auto&&) { OnStartClicked(); });
    CtxStop().Click([this](auto&&, auto&&) { OnStopClicked(); });
    CtxRestart().Click([this](auto&&, auto&&) { OnRestartClicked(); });
    CtxDelete().Click([this](auto&&, auto&&) { OnDeleteClicked(); });

    ::SetWindowSubclass(hwnd, &HostProc, 1,
                         reinterpret_cast<DWORD_PTR>(menuHost_.get()));

    platform::ApplyWindowDecorations(hwnd);

    controller_.SetElevated(core::IsElevated());
    DWORD err = ERROR_SUCCESS;
    if (!controller_.Start(err)) {
        // 连服务控制管理器都打不开的话，列不出任何东西 ——
        // 开一个空窗口让人对着看没有意义，直接以错误码退出。
        LINGSVC_TRACE("Controller::Start failed err=%lu", (unsigned long)err);
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

    // Closed 才是「窗口真的没了」的正确时刻。
    // 在这里停后台线程而不是在 App 的别处：此时窗口还活着，
    // 停线程的 join 有机会在几秒内完成，进程退出得更干净。
    Closed([this](auto&&, auto&&) {
        if (!alive_->exchange(false)) {
            return;   // Closed 可能被走两遍（提权路径 Close 一次、框架再走一次）
        }
        LINGSVC_TRACE("MainWindow Closed: saving settings, stopping controller");
        SaveWindowBounds();
        try {
            if (timer_) timer_.Stop();
            controller_.Stop();
        } catch (std::exception const& e) {
            LINGSVC_TRACE("MainWindow Closed: cleanup threw %s", e.what());
        }
    });

    LINGSVC_TRACE("MainWindow ctor: done");
}

void MainWindow::SaveWindowBounds() {
    // 只在窗口可见时记录位置：最小化 / 隐藏时 GetRect 返回的是还原位置，
    // 但如果它当时已经被 DWM 挪过（Win11 的 snap 组会在重启应用后复活），
    // 记下来反而会开在错误的地方。
    if (hwnd_ && ::IsWindowVisible(hwnd_)) {
        RECT r{};
        if (::GetWindowRect(hwnd_, &r)) {
            settings_.left = r.left;
            settings_.top = r.top;
            settings_.width = r.right - r.left;
            settings_.height = r.bottom - r.top;
        }
    }
    settings_.sortMode = sortMode_;
    settings_.filterMode = filterMode_;
    settings_.detailExpanded = DetailExpander().IsExpanded();
    core::SaveSettings(settings_);
}

void MainWindow::ApplyBackdrop() {
    // MicaBackdrop 只在 Win11（build >= 22000）有效；XAML 布局本身没问题，
    // 在 Win10 上会静默失效，此时退到 Acrylic（Win10 1809 起可用）。
    if (platform::SupportsMica()) {
        SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop());
    } else {
        SystemBackdrop(Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop());
    }
}

void MainWindow::WireEvents() {
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
    confirm_.Closed([this](auto&&, auto&&) { dialogOpen_->store(false); });
    notice_.Closed([this](auto&&, auto&&) { dialogOpen_->store(false); });

    // 提权按钮的处理器只挂一次，ShowOutcome 负责决定这一次要不要显示它。
    notice_.PrimaryButtonClick([this](auto&&, auto&&) { OnElevateRequested(); });
    // 删除确认的主按钮：此刻 deletePromptInput_ 里已经回读完毕。
    confirm_.PrimaryButtonClick([this](auto&&, auto&&) { OnDeleteConfirmed(); });
}

// ---------------------------------------------------------------- 心跳
void MainWindow::OnTimerTick() {
    if (!alive_->load()) {
        return;
    }
    DrainController();
}

void MainWindow::DrainController() {
    // ---- 1. 两个下拉的选中项比对（绕开接不上的 SelectionChanged）----
    const int filterIndex = FilterBox().SelectedIndex();
    if (filterIndex >= 0 && filterIndex != lastFilterIndex_) {
        lastFilterIndex_ = filterIndex;
        filterMode_ = static_cast<core::FilterMode>(filterIndex);
        settings_.filterMode = filterMode_;
        core::SaveSettings(settings_);
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
void MainWindow::RebuildRows() {
    if (!alive_->load()) {
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
        winrt::LingwuServiceManager::ServiceRow row{ nullptr };
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
            winrt::LingwuServiceManager::ServiceRow>();
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

    menuHost_->rowCount = static_cast<int>(want);
    lastOrderApplyTick_ = now;
    UpdateStatusBar();
}

void MainWindow::ForceReorderNext() {
    // 节流是为「状态每两秒自然抖动」设计的；用户的明确操作不该等它。
    lastOrderApplyTick_ = 0;
}

// ---------------------------------------------------------------- 状态栏
void MainWindow::UpdateStatusBar() {
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

void MainWindow::UpdateSortIndicators() {
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

void MainWindow::UpdateRow(winrt::LingwuServiceManager::ServiceRow const& row,
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
void MainWindow::OnSortHeaderClick(size_t column) {
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

    settings_.sortMode = sortMode_;
    core::SaveSettings(settings_);
    UpdateSortIndicators();
    ForceReorderNext();
    RebuildRows();
}

void MainWindow::OnSearchChanged(AutoSuggestBox const& sender) {
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

void MainWindow::OnRefreshClicked() {
    controller_.RequestImmediateRefresh();
    StatusLeft().Text(hstring(L"正在刷新…"));
}

std::wstring MainWindow::SelectedServiceName() const {
    if (selectedName_.empty()) {
        return std::wstring();
    }
    return selectedName_;
}

void MainWindow::OnListSelectionChanged(IInspectable const& sender,
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

void MainWindow::OnStartClicked() {
    const std::wstring name = SelectedServiceName();
    if (name.empty()) return;
    core::Operation op;
    op.kind = core::OpKind::Start;
    op.name = name;
    if (!controller_.Enqueue(std::move(op))) {
        ShowOutcome(L"请稍候", L"上一个操作还在进行中。", false);
    }
}

void MainWindow::OnStopClicked() {
    const std::wstring name = SelectedServiceName();
    if (name.empty()) return;
    core::Operation op;
    op.kind = core::OpKind::Stop;
    op.name = name;
    if (!controller_.Enqueue(std::move(op))) {
        ShowOutcome(L"请稍候", L"上一个操作还在进行中。", false);
    }
}

void MainWindow::OnRestartClicked() {
    const std::wstring name = SelectedServiceName();
    if (name.empty()) return;
    core::Operation op;
    op.kind = core::OpKind::Restart;
    op.name = name;
    if (!controller_.Enqueue(std::move(op))) {
        ShowOutcome(L"请稍候", L"上一个操作还在进行中。", false);
    }
}

void MainWindow::OnStartTypeApplyClicked() {
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

void MainWindow::OnDeleteClicked() {
    const std::wstring name = SelectedServiceName();
    if (name.empty()) {
        return;
    }
    if (dialogOpen_->load()) {
        LINGSVC_TRACE("OnDeleteClicked ignored: a dialog is already open");
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

void MainWindow::OnDeleteConfirmed() {
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
            LINGSVC_TRACE("delete confirm: typed \"%ls\" != \"%ls\", rejected",
                          typed.c_str(), name.c_str());
            dialogOpen_->store(false);
            ShowOutcome(L"未删除",
                         L"输入的服务名与要删除的服务不一致，未执行删除。", false);
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

void MainWindow::OnNewServiceClicked() {
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
                LINGSVC_TRACE("create window closed without Create; ignoring");
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

void MainWindow::OnElevateRequested() {
    // 把当前筛选与搜索词带过去，省得提权之后重新设一遍。
    // 用 --search= / --filter= 参数而不是 ini 临时段：后者要处理
    // 「新实例读到之后删掉」的失败路径，而参数没有这个副作用。
    std::wstring args;
    if (!filter_.empty()) {
        args += L"--search=" + filter_ + L" ";
    }
    args += L"--filter=" + std::to_wstring(static_cast<int>(filterMode_));

    // 这里只**登记**待执行动作，真正 runas 由 main.cpp 在进程退出路径上做 ——
    // 必须等互斥量放掉，否则新实例会认为「还有一份在跑」然后自己退出。
    core::RequestRelaunchElevated(args);

    // 关掉主窗口 → 进程退出 → 退出路径里 runas。
    //
    // 【为什么必须让窗口真的关掉、而不是直接隐藏】
    // 窗口还在的话 XAML 消息循环不会返回，Application::Start 就不返回，
    // main.cpp 的退出段也就跑不到 —— 提权永远等不到执行。
    Close();
}

// ---------------------------------------------------------------- 对话框
void MainWindow::ShowOutcome(hstring const& title, hstring const& message,
                             bool offerElevate) {
    if (!alive_->load()) return;
    if (dialogOpen_->load()) {
        // 同一时刻只能有一个 ContentDialog。冲突时**只落 trace 不弹第二个** ——
        // 弹第二个会把已经开着的那个挤掉，用户看到的是「对话框一闪就没了」。
        LINGSVC_TRACE("ShowOutcome skipped: dialog already open");
        return;
    }

    try {
        dialogOpen_->store(true);
        notice_.XamlRoot(RootGrid().XamlRoot());
        // Title / Content 在 WinUI 3 里是 **IInspectable**（不是 hstring），
        // 直接传 L"..." 或 std::wstring 会报 C2665，必须包一层 box_value。
        notice_.Title(box_value(title));
        notice_.Content(box_value(message));
        if (offerElevate) {
            notice_.PrimaryButtonText(L"以管理员身份重新启动");
            notice_.CloseButtonText(L"取消");
        } else {
            // 不给主按钮。给一个「知道了」当 CloseButton 更自然，
            // 但 ContentDialog 没有 CloseButton 时右上角 × 照样能用。
            notice_.PrimaryButtonText(L"知道了");
            notice_.CloseButtonText(L"");
        }
        notice_.ShowAsync();
    } catch (winrt::hresult_error const& err) {
        LINGSVC_TRACE("ShowOutcome threw hresult=0x%08X msg=%ls",
                      static_cast<unsigned>(err.code().value), err.message().c_str());
        dialogOpen_->store(false);
    }
}

void MainWindow::ShowDeleteConfirm(std::wstring const& name, bool thirdParty,
                                   bool running) {
    if (!alive_->load()) return;
    if (dialogOpen_->load()) {
        LINGSVC_TRACE("ShowDeleteConfirm skipped: dialog already open");
        return;
    }

    pendingDeleteName_ = name;
    pendingDeleteThirdParty_ = thirdParty;
    pendingDeleteStopFirst_ = running;

    try {
        dialogOpen_->store(true);
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
        LINGSVC_TRACE("ShowDeleteConfirm threw hresult=0x%08X msg=%ls",
                      static_cast<unsigned>(err.code().value), err.message().c_str());
        dialogOpen_->store(false);
        pendingDeleteName_.clear();
    }
}

}  // namespace winrt::LingwuServiceManager::implementation
