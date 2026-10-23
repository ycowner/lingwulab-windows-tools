#include "pch.h"
#include "MainWindow.xaml.h"
// XAML 生成代码必须在这里显式引入一次（官方模板也是这么做的）。
// #if __has_include 是为了容忍「第一次编译、生成物还没出来」的情况。
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
// InitializeComponent() / Connect() / GetBindingConnector() 的**定义**在这里，
// 不在 .g.h 里（.g.h 只有第 43-46 行的声明）。.g.hpp 第 102 行有一条
//     template struct MainWindowT<struct MainWindow>;
// 显式实例化，正是它把这些成员模板实例化成可链接的符号。
// C++/WinRT 的约定是由**某一个**代码后置 .cpp 引入 .g.hpp，本工程就选这里。
//
// 漏掉它的症状特别隐蔽：编译全过，链接期报一堆
//   MainWindowT<MainWindow>::InitializeComponent
//   MainWindowT<MainWindow>::Connect
//   MainWindowT<MainWindow>::GetBindingConnector
// 的 LNK2019/LNK2001。
#if __has_include("MainWindow.xaml.g.hpp")
#include "MainWindow.xaml.g.hpp"
#endif
// IWindowNative 是 MIDL 生成的 **COM 接口**，不是 WinRT 类型，
// 所以不会出现在 cppwinrt 生成的任何投影头里（实测全盘 grep 无命中）。
// 它随 WinUI 包的 include 目录一起分发，是官方文档里取 HWND 的标准做法。
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <cwctype>
#include <thread>
#include "core/appnames.h"
#include "core/autostart.h"
#include "core/terminate.h"
#include "core/top10.h"
#include "platform/callback_guard.h"
#include "platform/trace.h"
#include "platform/tray_icon.h"
#include "platform/window_decor.h"
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Input;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Shapes;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Dispatching;
using namespace winrt::Windows::Foundation;
namespace winrt::LingwuSysmon::implementation {
namespace core = lingsysmon::core;
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
// 行高必须与 MainWindow.xaml 里 DataTemplate 的 <Grid Height="32"> 保持一致。
// 右键命中测试是**算术**定位：VisualTreeHelper::GetChildren 在这版投影里不存在
//（只有 GetChildrenCount / GetParent），没法遍历已实现的行容器去算包围盒，
// 所以这里用「内容坐标 y / 行高」直接推出下标。这是本文件与 XAML 之间唯一的耦合点。
constexpr double kRowHeight = 32.0;
// 右键命中：由窗口子类回调。返回 true 表示已处理（且菜单已弹出）。
bool HitTestRow(MenuHost* host, winrt::Windows::Foundation::Point const& clientPt) {
    if (!host || !host->list || host->rowCount <= 0) return false;

    // 第一步必须把**物理像素换算成逻辑像素**，这一步漏了会同时错两处。
    //
    // WM_CONTEXTMENU 的 lParam 经 ScreenToClient 得到的是**物理**客户区像素，
    // 而下面用到的三个量全是**逻辑**像素：
    //   · TransformToVisual().TransformPoint() 的返回（XAML 坐标系是 DIP）
    //   · ScrollPresenter.VerticalOffset()
    //   · kRowHeight = 32（与 XAML 里 <Grid Height="32"> 对应）
    // 两者只在 100% 缩放下相等。本机是 200%，不换算的话：
    //   index = (物理 y − 逻辑 origin) / 32 ≈ 真实行号的 2 倍
    // 表现为「右键某一行的落点随机地命中别的行、或干脆越界不发生任何事」——
    // 实测同一段脚本两次运行，一次弹出了确认框、一次什么都没发生。
    // ShowAt 的 point 也是逻辑坐标（相对 target），直接用客户区物理坐标会把
    // 菜单弹到离光标很远的位置。
    double scale = 1.0;
    if (auto xr = host->root.XamlRoot()) {
        scale = xr.RasterizationScale();
    }
    if (scale <= 0.0) scale = 1.0;
    // Windows::Foundation::Point 的成员是 **float**，除法会提升成 double，
    // /WX 下必须显式收窄，否则是 C2398「需要收缩转换」。
    const winrt::Windows::Foundation::Point logical{
        static_cast<float>(clientPt.X / scale),
        static_cast<float>(clientPt.Y / scale) };

    const auto origin = host->list.TransformToVisual(host->root)
                            .TransformPoint(winrt::Windows::Foundation::Point{ 0.0, 0.0 });
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
    host->menu.ShowAt(host->list, winrt::Windows::Foundation::Point{
                                      logical.X - origin.X, logical.Y - origin.Y });
    return true;
}
LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                          UINT_PTR /*id*/, DWORD_PTR ref) {
    // 【这个函数是窗口过程边界，异常绝不能从这里逃出去】
    // 下面三条分支都会调到 WinRT：HitTestRow 里的 list.SelectedIndex /
    // MenuFlyout.ShowAt，activateMain 最终的 AppWindow().Show，
    // 以及 OnTaskbarCreated 的整套托盘重建。
    // 异常一旦穿过 LRESULT CALLBACK，user32 层没有 C++ handler，
    // CRT 只能 std::terminate → abort() → 0xC0000409，进程直接消失。
    // 所以三条分支各自包一层 GuardCallback，且**不改变消息转发语义**：
    // 兜底之后照常走原来的 return / DefSubclassProc。

    if (msg == WM_CONTEXTMENU) {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ::ScreenToClient(hwnd, &pt);
        bool handled = false;
        platform::GuardCallback("win32.HostProc.ctxmenu", [&]{
            handled = HitTestRow(reinterpret_cast<MenuHost*>(ref),
                       winrt::Windows::Foundation::Point{ static_cast<float>(pt.x),
                                                          static_cast<float>(pt.y) });
        });
        if (handled) {
            return 0;   // 吃掉，WinUI 不要再自己弹一次
        }
    }

    // ---- 单实例激活 ----
    // 第二份启动时会往这里 PostMessage。消息 id 是 RegisterWindowMessageW 拿的，
    // 每个会话可能不同，所以必须运行时比对，不能写死常量。
    //
    // 【为什么不自己 ShowWindow 了】主窗口现在可以被真隐藏到托盘
    // （AppWindow.Hide()），而 SW_RESTORE 只对最小化有效、对隐藏窗口是空操作。
    // 在这里自己做恢复的后果是「程序收进托盘后再启动一份，什么都不会发生」，
    // 而且没有任何报错。所以恢复逻辑统一收敛到 MainWindow::ShowAndActivate，
    // 子类只负责转发 —— 不搞第二套实现。
    if (const UINT am = platform::ActivateMessageId()) {
        if (msg == am) {
            LINGSYSMON_TRACE("HostProc: activate message received, restoring window");
            auto* host = reinterpret_cast<MenuHost*>(ref);
            platform::GuardCallback("win32.HostProc.activate", [&]{
                if (host && host->activateMain) host->activateMain();
            });
            return 0;
        }
    }

    // ---- Explorer 重启 ----
    // TaskbarCreated 是 Explorer 用 HWND_BROADCAST 广播的。
    // 托盘那个消息窗口（HWND_MESSAGE）**收不到广播** —— 消息窗口被明确排除在
    // 广播之外，所以只能由主窗口这个真正的顶层窗口来接，再转给托盘模块。
    // 漏掉这条的表现：Explorer 崩溃重启后，托盘图标再也不出现，
    // 而程序明明还活着，用户完全找不到入口。
    if (const UINT tc = platform::TaskbarCreatedMessageId()) {
        if (msg == tc) {
            LINGSYSMON_TRACE("HostProc: TaskbarCreated, forwarding to tray");
            platform::GuardCallback("win32.HostProc.taskbarCreated", [&]{
                platform::OnTaskbarCreated();
            });
            return 0;
        }
    }

    return ::DefSubclassProc(hwnd, msg, wp, lp);
}
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
void RelaunchElevated() {
    wchar_t exe[MAX_PATH] = { 0 };
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = L"runas";
    sei.lpFile       = exe;
    sei.lpParameters = L"--show";
    sei.nShow        = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&sei)) {
        ::MessageBoxW(nullptr, L"提权启动被取消或被系统拒绝。", L"需要管理员权限",
                      MB_OK | MB_ICONWARNING | MB_TOPMOST);
    } else if (sei.hProcess) {
        ::CloseHandle(sei.hProcess);
    }
}
// 把结果写进常驻的 notice_ 对话框（不 Show —— 由调用方在 UI 线程上 Show）。
void FillNotice(ContentDialog const& notice, XamlRoot const& root,
                core::KillResult r, int killed, const std::wstring& name) {
    notice.XamlRoot(root);
    // Title / Content 在 WinUI 3 里是 **IInspectable**（不是 hstring），
    // 投影头 Microsoft.UI.Xaml.Controls.h:4383 就是
    //     consume_..._IContentDialog<D>::Title(winrt::Windows::Foundation::IInspectable const&)
    // 直接传 L"..." 或 std::wstring 会报 C2665，必须包一层 box_value。
    if (r == core::KILL_NEEDS_ADMIN) {
        notice.Title(box_value(L"需要管理员权限"));
        notice.Content(box_value(
            (L"「" + name + L"」的部分进程受系统保护，无法结束。\n\n"
             L"是否以管理员身份重新启动本程序？").c_str()));
        notice.PrimaryButtonText(L"以管理员身份重启");
        notice.CloseButtonText(L"取消");
        notice.DefaultButton(ContentDialogButton::Primary);
    } else if (r == core::KILL_OK) {
        notice.Title(box_value(L"已结束"));
        notice.Content(box_value((L"已结束 " + std::to_wstring(killed) + L" 个进程。").c_str()));
        notice.PrimaryButtonText(L"");      // 清掉上一次留下的提权按钮
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
                  std::shared_ptr<std::atomic<bool>> dialogOpen,
                  std::vector<DWORD> pids, std::wstring name) {
    std::thread([alive = std::move(alive), dq, notice, root,
                 dialogOpen = std::move(dialogOpen),
                 pids = std::move(pids), name = std::move(name)]() mutable {
        int killed = 0;
        const core::KillResult r = core::TerminateAppPids(
            pids.data(), static_cast<DWORD>(pids.size()),
            /*gracefulFirst=*/true, /*graceMs=*/3000, &killed);
        if (!alive->load()) return;          // 窗口已关，结果无处可报
        dq.TryEnqueue([alive, notice, root, dialogOpen, r, killed, name]() mutable {
            if (!alive->load()) return;
            FillNotice(notice, root, r, killed, name);
            // 互斥闸门必须在 ShowAsync **之前**置位。这里有个真实的竞态窗口：
            // confirm_ 早就关掉了，但强杀要等最多 3 秒，这 3 秒里用户点 X
            // 会弹出 closeExit_，然后本回调再弹 notice_ —— 两个对话框挤掉。
            dialogOpen->store(true);
            notice.ShowAsync();
        });
    }).detach();
}
}  // namespace
// ---------------------------------------------------------------- 构造
MainWindow::MainWindow() {
    LINGSYSMON_TRACE("MainWindow ctor: step=0 enter");
    try {
        InitializeComponent();
    } catch (winrt::hresult_error const& err) {
        LINGSYSMON_TRACE("InitializeComponent threw hresult=0x%08X msg=%ls",
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
    // 关闭确认框是第三个常驻对话框，理由与上面两个完全相同。
    // closeRemember_ 同理：它的勾选状态要在常驻的按钮处理器里回读，
    // 所以必须活过对话框的整个生命周期，只能是成员。
    closeExit_     = ContentDialog();
    closeRemember_ = CheckBox();

    // 关闭确认框的面板在这里**一次性**建好，之后永不重建。
    //
    // 【为什么必须在这里建、而不是每次弹出时现建】closeRemember_ 是成员，
    // 而 XAML 元素同一时刻只能有一个父节点。第二次弹出时若新建一个 StackPanel
    // 再把 closeRemember_ Append 进去，等于把它从上一轮的面板上摘下来重挂，
    // 这一步会抛异常；异常逃出 AppWindow.Closing 处理器后，上面那句
    // a.Cancel(true) 会被作废 —— 窗口照常关闭、进程直接退出，
    // 症状是「第一次点 X 正常，第二次点 X 程序就没了」。
    // 只建一次就彻底绕开了这条再挂路径。
    closePromptPanel_ = StackPanel();
    closePromptPanel_.Spacing(8);
    {
        auto msg = TextBlock();
        msg.Text(L"主窗口关闭后程序仍在后台运行，进程采样不会停止。\n"
                 L"选择「最小化到托盘」可从系统托盘重新打开；"
                 L"选择「退出程序」将完全结束本程序。");
        msg.TextWrapping(TextWrapping::Wrap);
        closePromptPanel_.Children().Append(msg);
    }
    closeRemember_.Content(box_value(L"记住我的选择，以后不再询问"));
    closePromptPanel_.Children().Append(closeRemember_);
    // Content 只赋这一次。Title / Content 是 IInspectable，
    // 传字符串必须 box_value，直接给 L"..." 会报 C2665（见 FillNotice 的注释）。
    closeExit_.Content(closePromptPanel_);
    ApplyBackdrop();
    // 自绘标题栏：内容延伸进标题栏区域，并把 DragRegion 设为可拖拽区。
    // 有了 SetTitleBar，DWM 的命中测试才会把这块区域交给 XAML，
    // 标题栏才能自己画内容（同时保留双击最大化 / 右键系统菜单）。
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(DragRegion());
    // 默认尺寸与位置按**当前显示器的可用工作区**算，不写死像素。
    //
    // 【为什么必须按显示器算，而不是写死一个像素值】
    // AppWindow 系列的 API（Resize / MoveAndResize / DisplayArea.WorkArea）
    // 全部以**物理像素**为单位，与 XAML 的逻辑像素差一个 RasterizationScale。
    // 原来写死的 1040×700 在本机 200% 缩放下只等于 520×350 逻辑像素，
    // 所以"看起来只有三分之一屏" —— 不是窗口画小了，是单位用错了。
    // 比例取自显示器，换机器、换分辨率、换缩放都自动合适。
    //
    // 【0.70 的含义】线性 70%，即工作区**面积**的 49%（0.7² ≈ 一半）。
    //
    // 【为什么用 WorkArea 而不是 OuterBounds】
    // WorkArea 已扣掉任务栏，居中到它里面才是真的居中；
    // 用整屏会被那条 96px 高的任务栏带偏，看着偏上。
    //
    // 【为什么下面包 try/catch】构造函数整体没有 try/catch
    // （只有 InitializeComponent 那段有），异常一路冒到 App::OnLaunched，
    // 结果是"进程活着但一个窗口都没有"（AGENTS.md 踩坑 1）。
    // 取不到显示区时退回旧的固定尺寸，保证窗口照常出现。
    try {
        constexpr double kDefaultRatio = 0.70;
        const auto work = DisplayArea::GetFromWindowId(
            AppWindow().Id(), DisplayAreaFallback::Nearest).WorkArea();
        const int w = static_cast<int>(static_cast<double>(work.Width)  * kDefaultRatio);
        const int h = static_cast<int>(static_cast<double>(work.Height) * kDefaultRatio);
        const int x = work.X + (work.Width  - w) / 2;   // 居中
        const int y = work.Y + (work.Height - h) / 2;
        AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{ x, y, w, h });
        LINGSYSMON_TRACE("MainWindow: default rect=(%d,%d) %dx%d physical, "
                         "work area=(%d,%d) %dx%d",
                         x, y, w, h, work.X, work.Y, work.Width, work.Height);
    } catch (winrt::hresult_error const&) {
        AppWindow().Resize(winrt::Windows::Graphics::SizeInt32{ 1040, 700 });
        LINGSYSMON_TRACE("MainWindow: DisplayArea failed, fell back to 1040x700");
    }
    // 最小尺寸维持原样。720×420 是物理像素，本机 200% 下只有 360×210 逻辑像素，
    // 确实偏小，但那是既有状况，本次只改默认大小，不动它。
    if (auto pres = AppWindow().Presenter().try_as<OverlappedPresenter>()) {
        pres.PreferredMinimumWidth(720);
        pres.PreferredMinimumHeight(420);
    }
    // 点标题栏的 X（或按 Alt+F4）= 弹确认框，让用户在「隐藏到托盘」与
    // 「退出程序」之间选，而不是无条件最小化。
    //
    // 【为什么不用 WM_CLOSE / WM_DESTROY 拦】
    // WinUI 3 的窗口是框架创建的，我们自己没有窗口过程可改；能改的只有
    // SetWindowSubclass 套的那层，而 X 按钮的关闭动作在 XAML 层就已经走完
    // 了，子类里拦 WM_CLOSE 来不及（实测拦不到，窗口照样关）。
    //
    // 【为什么用 AppWindow.Closing 而不是 Window.Closed】
    // Closing 事件的 args.Cancel 能**取消**这次关闭，窗口根本不关；
    // Closed 是关完之后才通知，那时已经来不及了。
    //
    // 【为什么本次一定要先 Cancel 再弹框】
    // 不 Cancel 的话窗口当场就关了，对话框连出现的机会都没有。
    // Cancel 之后本次关闭不成立，真正要不要走由 OnCloseRequested 决定。
    //
    // 【Alt+F4 走同一条路径】Alt+F4 最终也是 WM_CLOSE，所以同样会弹这个框。
    // 这是有意保持一致的：两者都是「用户想关掉主窗口」的明确表达。
    quitting_ = false;
    AppWindow().Closing([this](auto&&, winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& a) {
        LINGSYSMON_TRACE("Closing fired: quitting_=%d dialogOpen=%d closeAction=%d",
                         quitting_ ? 1 : 0, dialogOpen_->load() ? 1 : 0,
                         settings_.closeAction);
        if (quitting_) return;   // 托盘菜单的「退出」/ 确认框的「退出程序」：放行

        // 【顺序不能反：Cancel 必须排在所有可能抛异常的语句之前】
        // 一旦 Cancel 生效，本次关闭就不成立 —— 后面即使抛异常，
        // 窗口也不会关、进程也不会退出。
        // 反过来（先做可能抛的事、后 Cancel）就复现了 AGENTS.md 铁律 7
        // 那个坑：异常逃出处理器 → Cancel 作废 → 窗口照关、进程直接退出，
        // 症状与「退出逻辑写错」完全无法区分。
        a.Cancel(true);

        // 【异常绝不能逃出这个处理器】（AGENTS.md 铁律 7 / README 踩坑 7）
        // OnCloseRequested 下面会碰 ContentDialog、AppWindow.Hide 等 WinRT 调用。
        // 这里兜住之后，即使它抛了，也只是「这次关闭没弹框」，
        // 不会变成「窗口关了、进程也没了」。
        //
        // dialogOpen_ 闸门不会被弄死：ShowClosePrompt 自带 try/catch，
        // 失败时会把闸门复位（见它的 catch 分支）。
        platform::GuardCallback("window.Closing.onCloseRequested", [this]{
            OnCloseRequested();
        });
    });
    core::LoadSettings(settings_);
    // 注册表才是自启的唯一事实源；ini 里那个键只当参考。
    bool asEnabled = false, asStale = false;
    core::AutostartReconcile(asEnabled, asStale);
    settings_.autoStart = asEnabled;
    sortMode_  = settings_.sortMode;
    aggregate_ = settings_.aggregate;
    AggregateSwitch().IsOn(aggregate_);
    WireEvents();
    UpdateSortIndicators();
    HWND hwnd{};
    winrt::check_hresult(this->m_inner.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    hwnd_ = hwnd;
    // 右键菜单的弹出由窗口子类接管（WM_CONTEXTMENU）。
    // SetWindowSubclass 需要 comctl32.lib。
    //
    // activateMain 必须在 SetWindowSubclass **之前**赋好：子类一旦挂上，
    // 第二份实例的激活消息就可能立刻到达，那时它还没有这个回调，
    // 就会走进「收到消息但什么都没做」的分支。
    menuHost_->activateMain = [this]() { ShowAndActivate(); };
    ::SetWindowSubclass(hwnd, &HostProc, 1, reinterpret_cast<DWORD_PTR>(menuHost_.get()));
    platform::ApplyWindowDecorations(hwnd);
    // 托盘放在最后：它要用到上面定好的 hwnd_（发单实例激活消息），
    // 且注册失败只该记日志不该中断启动。
    InitTray();
    StartSampling();
}
void MainWindow::ApplyBackdrop() {
    // 背景材质在编译期无法判断，只能运行时选。
    // MicaBackdrop 只在 Win11（build >= 22000）有效；XAML 布局本身没问题，
    // 在 Win10 上会静默失效，此时退到 Acrylic（Win10 1809 起可用）。
    // 注意 Mica / Acrylic 两个 Backdrop 都在 Microsoft.UI.Xaml.Media 命名空间。
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
    // 行右键菜单在代码里建：Click 才能用 lambda，不必进 IDL；弹出由窗口子类驱动
    // （为什么非走 Win32 子类不可，见文件上方 HitTestRow 上面的注释）。
    menuHost_ = std::make_shared<MenuHost>();
    menuHost_->list = ProcessList();
    menuHost_->root = RootGrid();
    menuHost_->menu = MenuFlyout();
    {
        auto item = MenuFlyoutItem();
        item.Text(L"结束任务");
        item.Click([this](auto&&, auto&&) { OnEndTask(); });
        menuHost_->menu.Items().Append(item);   // MenuFlyout 本身没有 Append，要走 Items()
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
        RunTerminate(alive_, dq, notice_, RootGrid().XamlRoot(), dialogOpen_,
                     std::move(pids), std::move(name));
    });

    // 提权按钮的处理器只挂一次，FillNotice 负责决定这一次要不要显示它。
    notice_.PrimaryButtonClick([](auto&&, auto&&) { RelaunchElevated(); });

    // ---- 对话框互斥闸门 --------------------------------------------------
    // 同一个 XamlRoot 同时只能挂一个 ContentDialog，而三个对话框的
    // ShowAsync 都是异步的。所以「开之前置位、Closed 时清位」必须成对做，
    // 少一处就会出现「对话框已经关了但闸门还锁着，再点 X 永远没反应」。
    confirm_.Closed([this](auto&&, auto&&) { dialogOpen_->store(false); });
    notice_.Closed([this](auto&&, auto&&) { dialogOpen_->store(false); });
    closeExit_.Closed([this](auto&&, auto&&) { dialogOpen_->store(false); });

    // ---- 关闭确认框 ------------------------------------------------------
    // 这版 SDK 的 cppwinrt 投影头里没有 IAsyncOperation::then，
    // 所以关闭框的结果同样靠 PrimaryButtonClick / SecondaryButtonClick
    // 事件驱动（与 confirm_ 同款），ShowAsync 之后没有可挂的延续。
    closeExit_.PrimaryButtonClick([this](auto&&, auto&&) {
        ApplyCloseChoice(kCloseMinimizeToTray);
    });
    closeExit_.SecondaryButtonClick([this](auto&&, auto&&) {
        ApplyCloseChoice(kCloseExitProgram);
    });

    Closed([this](auto&&, auto&&) {
        // alive_ 置 false 是告诉后台结束任务的线程「别再回调了」；
        // 采样线程有最长 3 秒的 Stop() 等待，所以这里只置标志、不在 Closed 里 Stop()。
        alive_->store(false);
        timer_.Stop();
        if (hwnd_) ::RemoveWindowSubclass(hwnd_, &HostProc, 1);

        // 托盘必须在这里撤掉，不能等 main.cpp。
        //
        // 两个方向都会出错：
        //   · 撤得太早（窗口还没关）：用户点了「退出」，托盘先消失，
        //     看起来像程序卡死了，其实还在跑采样线程的 Stop() 等待。
        //   · 撤得太晚（main.cpp 里 Application::Start 返回后）：框架的清理
        //     已经跑完，此时再 NIM_DELETE 可能落到一个已经失效的登记上，
        //     留下一个幽灵图标，要等鼠标划过任务栏才消失。
        // Closed 是「窗口真的没了」的时刻，正好。
        // ClearPublishedHwnd 也在这里：共享内存里的 HWND 已经指向
        // 一个即将销毁的窗口，下一个实例读到它会往死窗口发消息。
        platform::ClearPublishedHwnd();

        // 设置窗口也要真正关闭。正常退出路径上 RequestExit 已经关过，
        // 这里是兜底：只要还有一个 Window 活着，XAML 框架的消息循环就不会结束，
        // 进程会变成「窗口全没了但进程还在」的僵留状态。
        if (settingsWindow_) {
            try {
                settingsWindow_.AllowClose();
                settingsWindow_.Close();
            } catch (...) {
                // 关闭流程里不能再抛：抛出去会让下面的托盘清理做不成。
            }
            settingsWindow_ = nullptr;
        }

        platform::ShutdownTrayIcon();
        LINGSYSMON_TRACE("MainWindow closed");
    });
}
void MainWindow::StartSampling() {
    sampler_.SetGroupMode(aggregate_);
    sampler_.SetSortMode(sortMode_);
    sampler_.Start(settings_.intervalMs);
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
void MainWindow::OnTimerTick() {
    // 设置窗口的结果回收（详见 ApplyPendingSettingsResult）。
    // 必须排在 ConsumeDirty 的早退**之前** —— 采样不一定有脏数据，
    // 但用户按了「确定」之后结果要立刻被收走。
    ApplyPendingSettingsResult();
    if (!sampler_.ConsumeDirty()) return;
    Refresh();
}
void MainWindow::Refresh() {
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
    // 判据用 IsWindowVisible 而不是 XAML 的 Visibility：后者本工程不存在
    // （见 HideToTray 的注释），而且问的是「任务栏上有没有这个窗口」。
    if (hwnd_ && !::IsWindowVisible(hwnd_)) return;

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
        LINGSYSMON_TRACE("Refresh threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    } catch (std::exception const& e) {
        LINGSYSMON_TRACE("Refresh threw std::exception: %s", e.what());
    } catch (...) {
        LINGSYSMON_TRACE("Refresh threw UNKNOWN non-standard exception");
    }
}
// 清掉重排节流计时，让下一帧立刻把新顺序应用到列表上。
// 0 是哨兵值：RebuildRows 里 `lastOrderApplyTick_ == 0` 一律视为「到期」。
void MainWindow::ForceReorderNext() { lastOrderApplyTick_ = 0; }

void MainWindow::RebuildRows() {
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
    if (menuHost_) menuHost_->rowCount = static_cast<int>(visibleCount_);
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
        winrt::LingwuSysmon::ProcessRow row;
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
        // 函数（见 MainWindow.xaml.h 里的说明），所以先攒 std::vector 再一次性传入。
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
        LINGSYSMON_TRACE("ListCommit: rows=%u cur=%u want=%u replaced=%u",
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

void MainWindow::UpdateStatusBar() {
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

void MainWindow::UpdateSortIndicators() {
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

void MainWindow::OnSortHeaderClick(size_t column) {
    // 点击表头切换：同一列再点一次换方向，点别的列则取该列的主方向。
    if (column == kColCpu) {
        sortMode_ = (sortMode_ == core::SORT_CPU_DESC) ? core::SORT_CPU_ASC : core::SORT_CPU_DESC;
    } else if (column == kColMem) {
        sortMode_ = (sortMode_ == core::SORT_MEM_DESC) ? core::SORT_MEM_ASC : core::SORT_MEM_DESC;
    } else {
        sortMode_ = (sortMode_ == core::SORT_NAME_ASC) ? core::SORT_MEM_DESC : core::SORT_NAME_ASC;
    }

    settings_.sortMode = sortMode_;
    sampler_.SetSortMode(sortMode_);
    UpdateSortIndicators();
    // 明确的手势不该被节流拖慢：换排序后立刻重排，而不是最多等 kReorderIntervalMs 毫秒。
    ForceReorderNext();
    Refresh();
}

void MainWindow::OnSearchChanged(AutoSuggestBox const& sender,
                                 AutoSuggestBoxTextChangedEventArgs const& args) {
    if (args.Reason() != AutoSuggestionBoxTextChangeReason::UserInput) return;
    // 注意是 .c_str() 不是 winrt::to_string() —— 后者返回的是 **std::string**，
    // 直接赋给 std::wstring 会报 C2679「二元运算符没有接受 std::string 的右操作数」。
    filter_ = sender.Text().c_str();
    // 同理：筛选条件一变，行集合立刻变，不能让新结果等节流窗口。
    ForceReorderNext();
    Refresh();
}

void MainWindow::OnListSelectionChanged(IInspectable const& /*sender*/,
                                        SelectionChangedEventArgs const& /*args*/) {
    selectedPids_.clear();
    selectedName_.clear();
    auto sel = ProcessList().SelectedItem();
    if (!sel) return;
    const auto row = sel.try_as<winrt::LingwuSysmon::ProcessRow>();
    if (!row) return;

    // rowPids_ 与列表同批重建，但刷新与选中回调可能交错，
    // 所以这里仍然要校验下标落在范围内再取。
    const size_t end = static_cast<size_t>(row.PidStart()) + row.PidCount();
    if (end > rowPids_.size()) return;

    selectedName_ = row.Name().c_str();   // 不能用 winrt::to_string：那是窄串
    selectedPids_.assign(rowPids_.begin() + row.PidStart(), rowPids_.begin() + end);
}

void MainWindow::OnEndTask() {
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
    dialogOpen_->store(true);   // 与 Closed 处理器成对，见 WireEvents 里的注释
    confirm_.ShowAsync();
}

// ---------------------------------------------------------------- 设置
void MainWindow::OnOpenSettings() {
    LINGSYSMON_TRACE("Settings clicked");
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
        settingsWindow_.LoadFrom(aggregate_, sortMode_, settings_.intervalMs,
                                 settings_.autoStart);
        // 新一轮：准备接收结果（LoadFrom 已把 accepted_ 复位）。
        settingsResultConsumed_ = false;

        // 隐藏着的窗口要先 Show 再 Activate。
        // AppWindow::Show(true) = 显示并激活；Activate() 再补一次，
        // 因为部分 shell 上 Show 的激活不保证真的抢到前台。
        settingsWindow_.AppWindow().Show(true);
        settingsWindow_.Activate();
    } catch (winrt::hresult_error const& e) {
        LINGSYSMON_TRACE("OnOpenSettings threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    } catch (std::exception const& e) {
        LINGSYSMON_TRACE("OnOpenSettings threw std::exception: %s", e.what());
    } catch (...) {
        LINGSYSMON_TRACE("OnOpenSettings threw UNKNOWN non-standard exception");
    }
}

// 轮询设置窗口的结果并应用。详见 MainWindow.xaml.h 里的说明。
void MainWindow::ApplyPendingSettingsResult() {
    if (!settingsWindow_) return;
    if (settingsResultConsumed_) return;
    // Accepted() 只有在用户按了「确定」之后才为 true；
    // 「取消」和「直接关掉」都保持 false —— 那两种情况本来就不该应用任何东西。
    if (!settingsWindow_.Accepted()) return;

    settingsResultConsumed_ = true;
    ApplySettingsResult(settingsWindow_);
}

void MainWindow::ApplySettingsResult(winrt::LingwuSysmon::SettingsWindow const& w) {
    if (!w.Accepted()) {
        LINGSYSMON_TRACE("Settings cancelled");
        return;   // 取消：什么都不改
    }

    settings_.aggregate  = w.ResultAggregate();
    settings_.sortMode   = w.ResultSortMode();
    settings_.intervalMs = w.ResultIntervalMs();
    settings_.autoStart  = w.ResultAutoStart();
    // autoStart 的注册表写入已在设置窗口里做完并回读校验，这里只同步到 settings_。
    // ini 不落 AutoStart —— 注册表才是唯一事实源（见 settings.cpp 的注释）。

    core::SaveSettings(settings_);

    // ---- 应用到运行时 ----
    aggregate_ = settings_.aggregate;
    // 这个函数现在是由 ApplyPendingSettingsResult（100 ms 定时器轮询）调进来的，
    // 不是 XAML 事件处理器 —— 但异常一样不能放出去：这里是 UI 线程的定时器回调，
    // 抛出去会一路穿过 DispatcherQueue 的回调边界。
    // 包一层，保证「聚合档位没切过去」不会变成更难查的问题。
    try {
        AggregateSwitch().IsOn(aggregate_);
    } catch (winrt::hresult_error const& e) {
        LINGSYSMON_TRACE("ApplySettingsResult: AggregateSwitch::IsOn threw "
                         "hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    }
    sampler_.SetGroupMode(aggregate_);

    // 排序档位变了要同步给采样线程，否则 RebuildRows 用的还是旧档。
    if (sortMode_ != settings_.sortMode) {
        sortMode_ = settings_.sortMode;
        sampler_.SetSortMode(sortMode_);
        UpdateSortIndicators();
    }

    // 采样间隔变化要重建定时器驱动的采样周期。
    // Sampler::SetInterval 已在 Phase 1 实现，直接调即可。
    sampler_.SetInterval(settings_.intervalMs);

    // 设置里改了排序档同理：用户明确选的档位要立刻生效。
    ForceReorderNext();
    Refresh();
    LINGSYSMON_TRACE("Settings applied: aggregate=%d sort=%d interval=%d autoStart=%d",
                     aggregate_ ? 1 : 0, sortMode_, settings_.intervalMs,
                     settings_.autoStart ? 1 : 0);
}

// ---------------------------------------------------------------- 托盘 / 单实例
// 下面几个方法是托盘回调与窗口子类的落点，全部是「薄转发」，
// 真正的逻辑都在既有的私有成员里，不搞第二套实现。
//
// 【为什么托盘装配放在本类里，而不是 App 或 main.cpp】
// 托盘模块的回调需要操作主窗口，而本类之外能拿到的只有 make<>() 返回的
// **投影**类型 winrt::LingwuSysmon::MainWindow —— 本版 cppwinrt 没有
// get_implementation，投影上也看不到 private 成员。把装配搬进本类，
// 回调直接捕获 this，问题消失。详见 .h 里的长注释。
void MainWindow::InitTray() {
    platform::TrayCallbacks cb;
    // 全部捕获 this：这些 lambda 只可能在本窗口活着的时候被调
    // （托盘消息窗口与本窗口同线程，本窗口 Closed 时托盘已撤）。
    cb.onShowMain = [this]() { ShowAndActivate(); };
    cb.onSettings = [this]() { OnOpenSettings(); };
    cb.onExit     = [this]() { RequestExit(); };
    cb.getAggregate = [this]() { return aggregate_; };
    cb.setAggregate = [this](bool on) { SetAggregate(on); };

    if (!platform::InitTrayIcon(cb)) {
        // 典型成因：开机自启抢在 Explorer 前面启动，NIM_ADD 失败。
        // 不打扰用户，TaskbarCreated 会补注册（由 HostProc 转发进来）。
        LINGSYSMON_TRACE("MainWindow::InitTray failed (Explorer not ready?)");
    }

    // 把主窗口 HWND 写进共享内存，供第二份启动时定位本份窗口。
    // 必须在 Activate 之前：第二份可能在 Activate 之前就发消息过来。
    platform::PublishMainHwnd(hwnd_);
}

void MainWindow::ShowAndActivate() {
    if (!hwnd_) return;
    // 三种状态要分开处理，**顺序不能换**。
    //
    // 【隐藏态】主窗口现在可以被真隐藏到托盘（AppWindow.Hide()），
    // 而 SW_RESTORE 只对最小化有效、对隐藏窗口是空操作 ——
    // 不先 Show 的话，点托盘图标会「任务栏按钮闪一下、窗口不出现」。
    if (!::IsWindowVisible(hwnd_)) {
        // Show(true) = 显示并激活。注意不能省掉后面的 SetForegroundWindow：
        // AppWindow.Show 的激活在部分 shell 上不保证真的抢到前台。
        //
        // 这里用**裸 try/catch** 而不是 GuardCallback：因为需要「失败了就降级
        // 重试一次」，而 GuardCallback 的语义是「吞掉并记录」。
        // 调用方（tray.onShowMain / win32.HostProc.activate）那层仍然有
        // GuardCallback 兜底，两层不冲突。
        try {
            AppWindow().Show(true);
        } catch (winrt::hresult_error const& e) {
            LINGSYSMON_TRACE("AppWindow::Show failed hresult=0x%08X msg=%ls, "
                             "falling back to SW_RESTORE",
                             static_cast<unsigned>(e.code().value), e.message().c_str());
            ::ShowWindow(hwnd_, SW_RESTORE);
        }
    } else if (::IsIconic(hwnd_)) {
        ::ShowWindow(hwnd_, SW_RESTORE);
    }
    ::SetForegroundWindow(hwnd_);
    // 窗口刚从隐藏/最小化回来：补一次全量刷新。
    // 不可见期间 Refresh() 会被跳过（见那里的注释），采样线程却一直在跑，
    // 所以这一次拿到的是此刻的快照，用户不会看到过期内容。
    // 已经可见时调它也无害 —— 只是多跑一次刷新。
    Refresh();
    LINGSYSMON_TRACE("MainWindow::ShowAndActivate visible=%d iconic=%d",
                     ::IsWindowVisible(hwnd_) ? 1 : 0, ::IsIconic(hwnd_) ? 1 : 0);
}

// ---------------------------------------------------------------- 关闭流程
void MainWindow::OnCloseRequested() {
    // 「记住我的选择」生效时不再询问，直接执行。
    // LoadSettings 已把越界值收敛回 0，所以这里只可能命中这三个常量之一。
    if (settings_.closeAction == kCloseMinimizeToTray) {
        LINGSYSMON_TRACE("MainWindow: close -> remembered, minimize to tray");
        HideToTray();
        return;
    }
    if (settings_.closeAction == kCloseExitProgram) {
        LINGSYSMON_TRACE("MainWindow: close -> remembered, exit");
        RequestExit();
        return;
    }
    // 已经有一个对话框在途。同一 XamlRoot 再挂一个会把前一个挤掉，症状是
    // 「点 X 之后对话框一闪就没了、窗口也不动」，极难定位。
    // 所以这次关闭直接忽略 —— 这条完全可恢复：把当前对话框处理掉之后
    // 再点一次 X 就会正常弹框。排队 / 强制顶替都要额外状态机，不划算。
    if (dialogOpen_->load()) {
        LINGSYSMON_TRACE("MainWindow: close ignored, a dialog is already open");
        return;
    }
    ShowClosePrompt();
}

void MainWindow::ShowClosePrompt() {
    // 面板与复选框在构造函数里**已经建好**（见那里关于「不能重复 Append
    // 同一个 XAML 元素」的注释），这里只重置本次要用的状态并弹出。
    //
    // 【整个过程包 try —— 异常绝不能逃出 AppWindow.Closing 处理器】
    // 逃出去的异常会让上面那句 a.Cancel(true) 被作废：窗口照常关闭、
    // 进程直接退出，症状与「点了 X 程序就退」一模一样。
    // 而且 trace 只能打在**异常可能发生点之前**，否则现场什么都不剩。
    try {
        // 每次都从「不记」开始。否则上一次勾过之后，
        // 用户想临时问一次都做不到。
        closeRemember_.IsChecked(false);

        closeExit_.XamlRoot(RootGrid().XamlRoot());
        closeExit_.Title(box_value(L"关闭窗口"));
        closeExit_.PrimaryButtonText(L"最小化到托盘");
        closeExit_.SecondaryButtonText(L"退出程序");
        closeExit_.CloseButtonText(L"取消");
        // 默认落在「取消」：回车与 Esc 都只是关掉对话框，误触代价最低。
        // 「退出程序」是次按钮，所以它永远不会是默认位。
        closeExit_.DefaultButton(ContentDialogButton::Close);

        // 闸门必须在 ShowAsync 之前置位，否则连点两次 X 会弹两个框。
        dialogOpen_->store(true);

        LINGSYSMON_TRACE("MainWindow: close prompt about to show");
        closeExit_.ShowAsync();
        LINGSYSMON_TRACE("MainWindow: close prompt shown");
    } catch (winrt::hresult_error const& e) {
        // 没弹出去就不能把闸门留着，否则之后点 X 永远没反应。
        dialogOpen_->store(false);
        LINGSYSMON_TRACE("ShowClosePrompt threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    } catch (std::exception const& e) {
        dialogOpen_->store(false);
        LINGSYSMON_TRACE("ShowClosePrompt threw std::exception: %s", e.what());
    } catch (...) {
        dialogOpen_->store(false);
        LINGSYSMON_TRACE("ShowClosePrompt threw UNKNOWN non-standard exception");
    }
}

void MainWindow::ApplyCloseChoice(int action) {
    // IsChecked() 返回**可空**的 IReference<bool>，没勾时是三态 null。
    //
    // 【别用 has_value() / value() —— 本版 cppwinrt 没有这两个成员】
    // IReference<T> 提供的是 `operator std::optional<T>()`（见
    // Generated Files\winrt\impl\Windows.Foundation.1.h 的 IReference 定义），
    // 所以要转成 std::optional 再取。直接调 .Value() 在未勾选时会抛。
    const std::optional<bool> boxed = closeRemember_.IsChecked();
    const bool remember = boxed.value_or(false);
    if (remember && action != kCloseAsk) {
        settings_.closeAction = action;
        core::SaveSettings(settings_);
        LINGSYSMON_TRACE("MainWindow: close choice remembered action=%d", action);
    }
    if (action == kCloseMinimizeToTray) {
        HideToTray();
    } else if (action == kCloseExitProgram) {
        RequestExit();
    }
}

void MainWindow::HideToTray() {
    // 【为什么用 AppWindow::Hide() 而不是 Visibility = Collapsed】
    // WinUI 3 的 Window 类**根本没有** Visibility 属性：本工程实际使用的投影
    // （Generated Files\winrt\Microsoft.UI.Xaml.h）里 IWindow 只有只读的
    // Visible()、没有 put_Visible，写 Visibility 会直接编不过。
    // AppWindow.Hide() 只隐藏顶层 HWND，**不动 XAML 可视性树**。
    //
    // 注意这与旧代码注释里那条「可见性归零会让列表已实现容器被回收、
    // 撞上 +26 MB 阶跃」不是一回事：那条针对的是 XAML 树，Hide() 不碰它。
    // 但代价仍以实测为准（tools\closeab.ps1 的私有内存 A/B），不靠推断下结论。
    try {
        AppWindow().Hide();
        LINGSYSMON_TRACE("MainWindow: hidden to tray");
    } catch (winrt::hresult_error const& e) {
        // 兜底：隐藏失败时绝不能让窗口留在「用户以为已经关掉」的状态。
        // 退到普通最小化，任务栏按钮还在，用户点得回来。
        LINGSYSMON_TRACE("AppWindow::Hide failed hresult=0x%08X msg=%ls, "
                         "falling back to SW_MINIMIZE",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
        if (hwnd_) ::ShowWindow(hwnd_, SW_MINIMIZE);
    }
}

void MainWindow::RequestExit() {
    // 两个调用方：托盘菜单的「退出」、关闭确认框的「退出程序」。
    // 后者是在对话框正在消失的瞬间被调用的 —— 正因为这里用的是 PostMessage
    // 而不是同步的 Close()，消息会排到对话框收尾之后才被处理，
    // 所以不会出现「对话框还没关完、窗口已经被销毁」的半截状态。
    LINGSYSMON_TRACE("MainWindow::RequestExit");
    quitting_ = true;

    // ---- 设置窗口必须先真正关闭 --------------------------------------------
    //
    // 设置窗口改成「关闭时只隐藏、不销毁」（因为 WinUI 3 的 Window 关闭后
    // 不可复用，而每次新建一个会线性泄漏原生资源），所以它会一直活着。
    // 而只要还有一个 Window 活着，XAML 框架的消息循环就不会结束 ——
    // 进程会变成「窗口全没了但进程还在」的僵留状态（托盘图标也没了、
    // 任务管理器里还在）。
    //
    // 【为什么不能用 AppWindow::Destroy()】
    // 实测踩过：Destroy() 只销毁原生 HWND，**框架的窗口计数不会归零**，
    // 消息循环照样不结束 —— 表现就是进程永远退不掉（trace 里能看到
    // 「MainWindow closed」已经打过，但进程仍在）。
    // 只有真正的 Window::Close() 才会把它从框架里注销掉。
    //
    // 而 Window::Close() 会触发 AppWindow.Closing —— 那个处理器平时会把
    // 关闭拦下来改成隐藏，所以要先 AllowClose() 放行，再 Close()。
    if (settingsWindow_) {
        try {
            settingsWindow_.AllowClose();
            settingsWindow_.Close();
            LINGSYSMON_TRACE("MainWindow: settings window closed for shutdown");
        } catch (winrt::hresult_error const& e) {
            LINGSYSMON_TRACE("settings Close failed hresult=0x%08X msg=%ls",
                             static_cast<unsigned>(e.code().value), e.message().c_str());
        } catch (...) {
            LINGSYSMON_TRACE("settings Close threw UNKNOWN exception");
        }
    }

    // 关窗口 → Closed 处理器跑（停采样线程、摘子类）→ XAML 框架的消息循环
    // 自然结束 → main.cpp 的 Application::Start 返回 → 撤托盘、释放互斥量。
    //
    // 这里不直接 PostQuitMessage：WinUI 3 的消息循环不属于我们，
    // 从外部捅一刀会让框架的清理代码来不及跑（托盘图标会变成幽灵图标，
    // 要等鼠标划过才消失）。让窗口正常关掉是唯一干净的退出路径。
    if (hwnd_) ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void MainWindow::SetAggregate(bool on) {
    if (aggregate_ == on) return;
    aggregate_ = on;
    settings_.aggregate = on;
    core::SaveSettings(settings_);
    // IsOn 会触发 Toggled 处理器（它会自己再 Refresh 一次），
    // 是个裸的 XAML 调用 —— 异常从托盘菜单那条链过来时不能让它逃出去。
    try {
        AggregateSwitch().IsOn(on);
    } catch (winrt::hresult_error const& e) {
        LINGSYSMON_TRACE("AggregateSwitch::IsOn threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    }
    sampler_.SetGroupMode(on);
    Refresh();
    LINGSYSMON_TRACE("MainWindow::SetAggregate(%d)", on ? 1 : 0);
}
}
