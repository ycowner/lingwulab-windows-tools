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
#include "core/monitor/autostart.h"
#include "platform/callback_guard.h"
#include "platform/single_instance.h"
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
namespace winrt::LingwuToolbox::implementation {
namespace {

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                          UINT_PTR /*id*/, DWORD_PTR ref) {
    // 【这个函数是窗口过程边界，异常绝不能从这里逃出去】
    // 下面三条分支都会调到 WinRT：菜单命中测试里的 list.SelectedIndex /
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
            auto* host = reinterpret_cast<MenuHost*>(ref);
            if (host && host->hitTest) {
                handled = host->hitTest(winrt::Windows::Foundation::Point{
                    static_cast<float>(pt.x), static_cast<float>(pt.y) });
            }
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
            LINGWU_TRACE("HostProc: activate message received, restoring window");
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
            LINGWU_TRACE("HostProc: TaskbarCreated, forwarding to tray");
            platform::GuardCallback("win32.HostProc.taskbarCreated", [&]{
                platform::OnTaskbarCreated();
            });
            return 0;
        }
    }

    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------- 构造
MainWindow::MainWindow() {
    LINGWU_TRACE("MainWindow ctor: step=0 enter");
    try {
        InitializeComponent();
    } catch (winrt::hresult_error const& err) {
        LINGWU_TRACE("InitializeComponent threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(err.code().value),
                         err.message().c_str());
        throw;
    }

    // 关闭确认框是常驻对话框，理由与页面里的两个完全相同：
    // 空对象上挂不进事件，也挂不进 Content。
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
        msg.Text(L"关闭窗口后程序仍在后台运行，进程采样不会停止。\n"
                 L"选择「最小化到托盘」可从系统托盘重新打开；"
                 L"选择「退出程序」将完全结束本程序。");
        msg.TextWrapping(TextWrapping::Wrap);
        closePromptPanel_.Children().Append(msg);
    }
    closeRemember_.Content(box_value(L"记住我的选择，以后不再询问"));
    closePromptPanel_.Children().Append(closeRemember_);
    // Content 只赋这一次。Title / Content 是 IInspectable，
    // 传字符串必须 box_value，直接给 L"..." 会报 C2665。
    closeExit_.Content(closePromptPanel_);

    ApplyBackdrop();
    // 自绘标题栏：内容延伸进标题栏区域，并把 DragRegion 设为可拖拽区。
    // 有了 SetTitleBar，DWM 的命中测试才会把这块区域交给 XAML，
    // 标题栏才能自己画内容（同时保留双击最大化 / 右键系统菜单）。
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(DragRegion());

    // 统一设置。两个模块共用一份 ini，内存里也只能有一份表示 ——
    // 否则 A 模块保存时会用旧副本覆盖 B 模块刚写的键。
    lingwu::core::LoadSettings(settings_);

    // 跨模块对话框闸门。两个页面共用同一把。
    gate_ = std::make_shared<DialogGate>();

    // 菜单中转结构：窗口子类是静态函数、拿不到 this，靠它传递。
    menuHost_ = std::make_shared<MenuHost>();

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
        LINGWU_TRACE("MainWindow: default rect=(%d,%d) %dx%d physical, "
                         "work area=(%d,%d) %dx%d",
                         x, y, w, h, work.X, work.Y, work.Width, work.Height);
    } catch (winrt::hresult_error const&) {
        AppWindow().Resize(winrt::Windows::Graphics::SizeInt32{ 1040, 700 });
        LINGWU_TRACE("MainWindow: DisplayArea failed, fell back to 1040x700");
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
        LINGWU_TRACE("Closing fired: quitting_=%d dialogOpen=%d closeAction=%d",
                         quitting_ ? 1 : 0,
                         (gate_ && gate_->is_open()) ? 1 : 0,
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
        // 闸门不会被弄死：ShowClosePrompt 自带 try/catch，
        // 失败时会把闸门复位（见它的 catch 分支）。
        platform::GuardCallback("window.Closing.onCloseRequested", [this]{
            OnCloseRequested();
        });
    });

    // 取 HWND 并挂窗口子类。
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

    // 页面在窗口就绪之后创建。
    //
    // 【先记下药丸的默认文字色】
    // 未选中态不另取主题画刷，而是把 Button 此刻的默认 Foreground 存下来 ——
    // 那本来就是主题给正文用的颜色（深色主题下是近白，浅色主题下是近黑）。
    // 必须在任何一次 UpdateNavPills() 之前取，否则取到的就是被改过的颜色。
    navIdleForeground_ = NavMonitorButton().Foreground();

    CreatePages();
    WireEvents();

    // 托盘放在最后：它要用到上面定好的 hwnd_（发单实例激活消息），
    // 且注册失败只该记日志不该中断启动。
    InitTray();
    LINGWU_TRACE("MainWindow ctor: done");
}

// ---------------------------------------------------------------- 导航
void MainWindow::UpdateNavPills() {
    const bool monitor = (currentModule_ == Module::Monitor);

    // 强调色与反色文字都从 XAML 探针上取（见 XAML 里探针的注释）。
    const auto accent   = AccentBrushProbe().Background().as<SolidColorBrush>();
    const auto onAccent = OnAccentBrushProbe().Background().as<SolidColorBrush>();
    if (!accent || !onAccent) return;

    const auto clear = SolidColorBrush(Microsoft::UI::Colors::Transparent());

    NavMonitorPill().Background(monitor ? accent : clear);
    NavServicePill().Background(monitor ? clear : accent);
    NavMonitorButton().Foreground(monitor ? onAccent : navIdleForeground_);
    NavServiceButton().Foreground(monitor ? navIdleForeground_ : onAccent);
}

// ---------------------------------------------------------------- 页面
void MainWindow::CreatePages() {
    // 两个页面都**构造一次、常驻**，切换只改 Visibility。
    //
    // 【为什么不按需重建】
    // 性能监视页的列表遵守「ItemsSource 全生命周期只赋值一次」的铁律
    // （换 ItemsSource 会让私有内存以每次 +26 MB 的阶跃往上跳）。
    // 重建页面 = 新建一套行容器 + 重新赋 ItemsSource，
    // 那条铁律就白立了，而且每次切页都会漏一批原生资源。
    //
    // 【为什么先 Initialize 再 Append】
    // Initialize 里会调 InitializeComponent() 建出 XAML 树；
    // 挂进 PageHost 之前建好，页面第一次布局时元素就已经在位。
    monitorPage_.Initialize(&settings_, gate_);
    PageHost().Children().Append(monitorPage_);

    servicePage_.Initialize(&settings_, gate_);
    PageHost().Children().Append(servicePage_);

    ShowModule(Module::Monitor);
}

void MainWindow::ShowModule(Module m) {
    currentModule_ = m;
    const bool monitor = (m == Module::Monitor);
    monitorPage_.Visibility(monitor ? Visibility::Visible : Visibility::Collapsed);
    servicePage_.Visibility(monitor ? Visibility::Collapsed : Visibility::Visible);

    // 导航药丸的选中态同步。
    //
    // 【为什么不需要重入保护】
    // 早先用 SelectorBar 时，程序化设置 SelectedItem 会再触发一次
    // SelectionChanged，得靠 suppressNav_ 挡住「切模块 → 事件 → 再切模块」。
    // 现在药丸的选中态是自己刷颜色（UpdateNavPills），不经过任何会回调的事件，
    // 所以那面挡板就不需要了。
    UpdateNavPills();

    // 右键菜单的命中测试改指向当前活动页面。
    // 命中测试要用到行高、列表位置、滚动偏移 —— 都是页面自己的布局信息，
    // 所以由页面自己算，外壳只负责把回调指过去。
    if (menuHost_) {
        menuHost_->hitTest = [this](winrt::Windows::Foundation::Point const& pt) {
            return currentModule_ == Module::Monitor
                ? monitorPage_.HitTestRow(pt)
                : servicePage_.HitTestRow(pt);
        };
    }

    // 切回某个模块时补一次刷新：不可见期间刷新是被跳过的
    // （见 MonitorPage::Refresh 的注释），数据会停在切走那一刻。
    if (monitor) {
        monitorPage_.RefreshNow();
    } else {
        servicePage_.RefreshNow();
    }
    LINGWU_TRACE("MainWindow: switched to %s",
                 currentModule_ == Module::Monitor ? "monitor" : "service");
}

// ---------------------------------------------------------------- 背景
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

// ---------------------------------------------------------------- 事件
void MainWindow::WireEvents() {
    // 导航切换。药丸是 Button，Click 是 RoutedEventHandler（委托族可用）。
    NavMonitorButton().Click([this](auto&&, auto&&) { ShowModule(Module::Monitor); });
    NavServiceButton().Click([this](auto&&, auto&&) { ShowModule(Module::Service); });

    // ---- 对话框互斥闸门 --------------------------------------------------
    // 闸门由外壳创建，两个页面与关闭确认框共用同一把。
    // 同一个 XamlRoot 同时只能挂一个 ContentDialog，而 ShowAsync 是异步的。
    // 所以「开之前置位、Closed 时清位」必须成对做，少一处就会出现
    // 「对话框已经关了但闸门还锁着，再点 X 永远没反应」。
    closeExit_.Closed([this](auto&&, auto&&) { if (gate_) gate_->release(); });

    // ---- 关闭确认框 ------------------------------------------------------
    // 这版 SDK 的 cppwinrt 投影头里没有 IAsyncOperation::then，
    // 所以关闭框的结果同样靠 PrimaryButtonClick / SecondaryButtonClick
    // 事件驱动，ShowAsync 之后没有可挂的延续。
    closeExit_.PrimaryButtonClick([this](auto&&, auto&&) {
        ApplyCloseChoice(kCloseMinimizeToTray);
    });
    closeExit_.SecondaryButtonClick([this](auto&&, auto&&) {
        ApplyCloseChoice(kCloseExitProgram);
    });

    Closed([this](auto&&, auto&&) {
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

        // 两个页面各自的子窗口也要真正关闭。
        // 只要还有一个 Window 活着，XAML 框架的消息循环就不会结束，
        // 进程会变成「窗口全没了但进程还在」的僵留状态。
        monitorPage_.Shutdown();
        servicePage_.Shutdown();

        platform::ShutdownTrayIcon();
        LINGWU_TRACE("MainWindow closed");
    });
}

// ---------------------------------------------------------------- 托盘 / 单实例
// 下面几个方法是托盘回调与窗口子类的落点，全部是「薄转发」，
// 真正的逻辑都在既有的私有成员里，不搞第二套实现。
//
// 【为什么托盘装配放在本类里，而不是 App 或 main.cpp】
// 托盘模块的回调需要操作主窗口，而本类之外能拿到的只有 make<>() 返回的
// **投影**类型 winrt::LingwuToolbox::MainWindow —— 本版 cppwinrt 没有
// get_implementation，投影上也看不到 private 成员。把装配搬进本类，
// 回调直接捕获 this，问题消失。详见 .h 里的长注释。
void MainWindow::InitTray() {
    platform::TrayCallbacks cb;
    // 全部捕获 this：这些 lambda 只可能在本窗口活着的时候被调
    // （托盘消息窗口与本窗口同线程，本窗口 Closed 时托盘已撤）。
    cb.onShowMain = [this]() { ShowAndActivate(); };
    // 模块切换：**顺带把窗口显示出来**。
    // 窗口藏在托盘里时，用户从托盘菜单点「服务管理」，
    // 如果不显示窗口就等于什么也没发生（菜单动作没有可见效果）。
    cb.onSelectModule = [this](platform::TrayModule m) {
        ShowModule((m == platform::TrayModule::Service) ? Module::Service
                                                         : Module::Monitor);
        ShowAndActivate();
    };
    cb.getActiveModule = [this]() {
        return (currentModule_ == Module::Service) ? platform::TrayModule::Service
                                                   : platform::TrayModule::Monitor;
    };
    cb.onSettings = [this]() { monitorPage_.OpenSettings(); };
    cb.onExit     = [this]() { RequestExit(); };

    if (!platform::InitTrayIcon(cb)) {
        // 典型成因：开机自启抢在 Explorer 前面启动，NIM_ADD 失败。
        // 不打扰用户，TaskbarCreated 会补注册（由 HostProc 转发进来）。
        LINGWU_TRACE("MainWindow::InitTray failed (Explorer not ready?)");
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
            LINGWU_TRACE("AppWindow::Show failed hresult=0x%08X msg=%ls, "
                             "falling back to SW_RESTORE",
                             static_cast<unsigned>(e.code().value), e.message().c_str());
            ::ShowWindow(hwnd_, SW_RESTORE);
        }
    } else if (::IsIconic(hwnd_)) {
        ::ShowWindow(hwnd_, SW_RESTORE);
    }
    ::SetForegroundWindow(hwnd_);
    // 窗口刚从隐藏/最小化回来：告诉页面「可见了」。
    // 不可见期间页面跳过了刷新（见 MonitorPage::Refresh 的注释），
    // 采样线程却一直在跑，所以这一刻补一次全量刷新，
    // 用户看到的就是此刻的快照，不会是过期内容。
    monitorPage_.SetHostVisible(true);
    servicePage_.SetHostVisible(true);
    monitorPage_.RefreshNow();
    servicePage_.RefreshNow();
    LINGWU_TRACE("MainWindow::ShowAndActivate visible=%d iconic=%d",
                     ::IsWindowVisible(hwnd_) ? 1 : 0, ::IsIconic(hwnd_) ? 1 : 0);
}

// ---------------------------------------------------------------- 关闭流程
void MainWindow::OnCloseRequested() {
    // 「记住我的选择」生效时不再询问，直接执行。
    // LoadSettings 已把越界值收敛回 0，所以这里只可能命中这三个常量之一。
    if (settings_.closeAction == kCloseMinimizeToTray) {
        LINGWU_TRACE("MainWindow: close -> remembered, minimize to tray");
        HideToTray();
        return;
    }
    if (settings_.closeAction == kCloseExitProgram) {
        LINGWU_TRACE("MainWindow: close -> remembered, exit");
        RequestExit();
        return;
    }
    // 已经有一个对话框在途。同一 XamlRoot 再挂一个会把前一个挤掉，症状是
    // 「点 X 之后对话框一闪就没了、窗口也不动」，极难定位。
    // 所以这次关闭直接忽略 —— 这条完全可恢复：把当前对话框处理掉之后
    // 再点一次 X 就会正常弹框。排队 / 强制顶替都要额外状态机，不划算。
    if (gate_ && gate_->is_open()) {
        LINGWU_TRACE("MainWindow: close ignored, a dialog is already open");
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

        // 闸门必须在 ShowAsync 之前拿到独占权，否则连点两次 X 会弹两个框。
        if (gate_ && !gate_->try_acquire()) {
            LINGWU_TRACE("MainWindow: close prompt skipped, a dialog is already open");
            return;
        }

        LINGWU_TRACE("MainWindow: close prompt about to show");
        closeExit_.ShowAsync();
        LINGWU_TRACE("MainWindow: close prompt shown");
    } catch (winrt::hresult_error const& e) {
        // 没弹出去就不能把闸门留着，否则之后点 X 永远没反应。
        if (gate_) gate_->release();
        LINGWU_TRACE("ShowClosePrompt threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    } catch (std::exception const& e) {
        if (gate_) gate_->release();
        LINGWU_TRACE("ShowClosePrompt threw std::exception: %s", e.what());
    } catch (...) {
        if (gate_) gate_->release();
        LINGWU_TRACE("ShowClosePrompt threw UNKNOWN non-standard exception");
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
        lingwu::core::SaveSettings(settings_);
        LINGWU_TRACE("MainWindow: close choice remembered action=%d", action);
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
        // 页面据此跳过刷新：看不见的时候重建列表纯属浪费。
        monitorPage_.SetHostVisible(false);
        servicePage_.SetHostVisible(false);
        LINGWU_TRACE("MainWindow: hidden to tray");
    } catch (winrt::hresult_error const& e) {
        // 兜底：隐藏失败时绝不能让窗口留在「用户以为已经关掉」的状态。
        // 退到普通最小化，任务栏按钮还在，用户点得回来。
        LINGWU_TRACE("AppWindow::Hide failed hresult=0x08X msg=%ls, "
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
    LINGWU_TRACE("MainWindow::RequestExit");
    quitting_ = true;

    // ---- 两个页面的子窗口必须先真正关闭 ----------------------------------
    //
    // 设置窗口与「新建服务」窗口改成「关闭时只隐藏、不销毁」
    // （因为 WinUI 3 的 Window 关闭后不可复用，而每次新建一个会线性泄漏
    // 原生资源），所以它们会一直活着。
    // 而只要还有一个 Window 活着，XAML 框架的消息循环就不会结束 ——
    // 进程会变成「窗口全没了但进程还在」的僵留状态（托盘图标也没了、
    // 任务管理器里还在）。
    //
    // 【为什么不能用 AppWindow::Destroy()】
    // 实测踩过：Destroy() 只销毁原生 HWND，**框架的窗口计数不会归零**，
    // 消息循环照样不结束 —— 表现就是进程永远退不掉（trace 里能看到
    // 「MainWindow closed」已经打过，但进程仍在）。
    // 只有真正的 Window::Close() 才会把它从框架里注销掉。
    monitorPage_.Shutdown();
    servicePage_.Shutdown();

    // 关窗口 → Closed 处理器跑（停采样线程、摘子类）→ XAML 框架的消息循环
    // 自然结束 → main.cpp 的 Application::Start 返回 → 撤托盘、释放互斥量。
    //
    // 这里不直接 PostQuitMessage：WinUI 3 的消息循环不属于我们，
    // 从外部捅一刀会让框架的清理代码来不及跑（托盘图标会变成幽灵图标，
    // 要等鼠标划过才消失）。让窗口正常关掉是唯一干净的退出路径。
    if (hwnd_) ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

}  // namespace winrt::LingwuToolbox::implementation
