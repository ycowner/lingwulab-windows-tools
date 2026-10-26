#include "pch.h"
#include "TrayPanel.xaml.h"
// 与 MainWindow.xaml.cpp 同一套接线，理由见那边的长注释，简述：
//   · pch.h 必须是本编译单元的**第一个** include；
//   · InitializeComponent() / Connect() / GetBindingConnector() 的**定义**在
//     .xaml.g.hpp 里（.g.h 只有声明），必须由某一个代码后置 .cpp 引入，
//     否则编译全过、链接期一堆 LNK2019。
#if __has_include("TrayPanel.g.cpp")
#include "TrayPanel.g.cpp"
#endif
#if __has_include("TrayPanel.xaml.g.hpp")
#include "TrayPanel.xaml.g.hpp"
#endif
// IWindowNative 是 MIDL 生成的 COM 接口，不在任何 cppwinrt 投影头里，
// 随 WinUI 包 include 目录分发，是官方取 HWND 的标准做法。
#include <microsoft.ui.xaml.window.h>
// KeyRoutedEventArgs 的投影头。pch.h 里没有这一族 —— 它不在 PCH 的
// 投影清单里（那里刻意只留 WinUI 3 真正用到的几族）。
// 少了它就是「consume_...::Key 要使用将会返回 auto 的函数，
// 必须首先定义此函数」—— 报错完全指不到「少 include 一个头」。
#include <winrt/Microsoft.UI.Xaml.Input.h>
// AutomationProperties 的投影头。pch.h 里同样没有这一族 ——
// AutomationId 是**附加属性**（AutomationProperties.AutomationIdProperty），
// 不是 FrameworkElement 的成员，少了这个 include 就是
// 「AutomationIdProperty 找不到标识符」。
#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include "core/monitor/system_metrics.h"
#include "platform/tray_icon.h"
#include "platform/trace.h"
#include "platform/window_decor.h"
// 折线 / 进度条的画法，与状态页共用同一份实现。
#include "ui/spark_geometry.h"
// 指标读数的格式化，与状态页共用同一份实现。
#include "ui/metric_format.h"

#include <algorithm>
#include <cmath>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
// AutomationProperties：AutomationId 是**附加属性**，不是 FrameworkElement 的成员，
// C++ 侧只能通过 SetValue(AutomationProperties::AutomationIdProperty(), ...) 写。
using namespace Microsoft::UI::Xaml::Automation;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Input;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Shapes;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Dispatching;

namespace winrt::LingwuToolbox::implementation {
namespace core = lingwu::core::monitor;
namespace ui = lingwu::ui;

namespace {

// 面板的固定尺寸（**逻辑**像素）。
//
// 【为什么固定尺寸】
// 托盘面板是「弹出来看一眼」的东西，尺寸必须可预期 —— 每次弹出大小都不一样
// 会让用户以为程序出问题了。而且 AppWindow 的尺寸接口用的是**物理像素**，
// 所以这里存逻辑像素，落位时再按当前显示器的缩放换算（见 ApplyFixedSize）。
constexpr int kPanelWidthLogical  = 360;

// 面板高度。
//
// 【为什么是 530 而不是拍脑袋的数】
// 面板内容（健康分 / 徽章 / 2×2 磁贴 / 电池 / Top 进程 / 动作行 / 入口）
// 在 200% 缩放下量出来是 **505 客户区 DIP**，加上上下不可见边框各 7 DIP
// → 外框至少 519。原值 640 多出来的 121 DIP 全是空白 ——
// 中间那行 `*` 把剩下的空间全吸收了，Top 进程表与动作行之间空一大段。
// 530 = 519 + 11，留一点余量（不同机器的字体回退会让行高飘一点）。
constexpr int kPanelHeightLogical = 530;

// 有电池时额外要的高度（逻辑像素）。
//
// 【为什么要跟着电池变】
// 电池条在台式机上整条隐藏（Visibility=Collapsed，一个 DIP 都不占），
// 在笔记本上多出这一整条。高度写死成「有电池」的那一套，台式机上就多一截空白；
// 写死成「没电池」的那一套，笔记本上的「打开工具箱」会被挤出可视区 ——
// 而那个按钮被挤出可视区 = 用户点不到「打开工具箱」，面板就成了死路。
constexpr int kBatteryRowHeightLogical = 72;

// 面板与托盘图标之间的间隙（物理像素）。
constexpr int kTrayGapPx = 8;

// Top 进程表显示几行。
constexpr int kTopProcessRows = 5;

// 进程表的列宽（逻辑像素）。
//
// 【为什么进程表在 C++ 里建，而不是写在 XAML 里】
// 这张表是**固定五行**、每秒原地改文字。写成 XAML 要摆 15 个有名字的
// TextBlock（名称 / 内存 / CPU × 5 行），C++ 侧就得写 15 个逐个取值的分支；
// 写成代码则是一段循环，行数还能随时改。
//
// 【为什么名称列是星号列，而不是推算出来的定宽】
// 名称列的宽度早先按「360 − 外边距 13×2 = 334」推算成 228，
// 但那个 334 是按**外框**宽度算的，客户区还要再减去不可见边框 ——
// 实测比推算值小，于是三列加起来超出容器，最右边的 CPU 列被裁掉
// （数字整个看不见）。改成「名称列吃剩余宽度、另外两列定宽」之后，
// 客户区多宽都不会溢出。
constexpr double kProcMemColWidth  = 62.0;
constexpr double kProcCpuColWidth  = 44.0;
constexpr double kProcRowHeight    = 21.0;
constexpr double kProcHeaderHeight = 16.0;

// 刷新周期（毫秒）。面板是「弹出来看一眼」的东西，1 秒足够；
// 再密就是白烧电 —— 采样周期本身就是 1 秒，快过它也拿不到新数据。
constexpr int kRefreshMs = 1000;

// 面板磁贴里折线的高度（逻辑像素）。
//
// 【为什么比状态页的小】
// 状态页的磁贴高 110 DIP，折线占 24；面板的磁贴只有 ~160 宽、~78 高，
// 折线占 14 才不至于把整块挤满。**几何高度必须与宿主容器一致** ——
// Path 的 Stretch="None" 不会裁剪，几何画到容器外面就会溢出到相邻元素上。
constexpr double kPanelSparkHeight = 14.0;

// 清理屏的底色。**故意用固定的深色**而不是主题色：
// 这个屏的作用就是「把屏幕清空」，颜色要稳定，不能跟着系统主题变。
constexpr uint32_t kCleanScreenColor = 0x101014;

}  // namespace

// ---------------------------------------------------------------- 构造
TrayPanel::TrayPanel() {
    LINGWU_TRACE("TrayPanel ctor: enter");
    InitializeComponent();

    // 先取 HWND：算 DPI、定尺寸、定位都要用。
    check_hresult(this->m_inner.as<::IWindowNative>()->get_WindowHandle(&hwnd_));

    // 高度先按「没有电池」定下来。等第一次拿到数据、知道有没有电池，
    // 再决定要不要加电池条那截高度（见 ApplyPanelHeight）。
    panelHeightLogical_ = kPanelHeightLogical;
    ApplyFixedSize();

    // 去掉任务栏条目。托盘弹出的小窗不该在任务栏上留一个按钮 ——
    // 用户点开的是托盘图标，不是任务栏。
    // WS_EX_TOOLWINDOW 是低风险做法（WinUI 3 常用）。
    const LONG_PTR ex = ::GetWindowLongPtr(hwnd_, GWL_EXSTYLE);
    ::SetWindowLongPtr(hwnd_, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);

    // 内容延伸进标题栏区：面板顶部不留一条系统标题栏，
    // 系统按钮（最小化 / 最大化 / 关闭）浮在右上角。
    //
    // 【为什么保留系统按钮】
    // 完全无边框要靠 WS_POPUP + DwmSetWindowAttribute 那一套，
    // 是 Phase 1.5 的可选项；Phase 1 保留系统按钮，
    // 至少用户有个明确的「关掉它」的办法。
    //
    // 【为什么不挡内容】
    // 系统按钮浮在**右上角**，而第一行（健康分）是左对齐的，右边是空的，
    // 两者不会打架。
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(DragRegion());

    // 背景材质与主窗口一致（Win11 用 Mica，Win10 退 Acrylic）。
    if (platform::SupportsMica()) {
        SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop());
    } else {
        SystemBackdrop(Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop());
    }

    WireEvents();
    BuildProcessTable();

    // 关闭时只隐藏，不销毁。
    //
    // WinUI 3 的 Window 在 Close() 之后**不可复用**（再调 Activate() 会抛
    // hresult_error），而「每次打开都新建一个」实测会线性泄漏原生资源。
    // 所以关闭时只 Hide，窗口对象一直活着，每轮零分配。
    // 真正关闭由主窗口退出时的 Shutdown() 完成。
    AppWindow().Closing([this](auto&&,
        winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
        if (allowClose_) return;   // 主窗口已经放行（要退出了）
        args.Cancel(true);
        HidePanel();
    });
}

// ---------------------------------------------------------------- 契约
void TrayPanel::Initialize(lingwu::core::Settings* settings,
                           SnapshotFn fetch,
                           std::function<void()> openMain) {
    settings_ = settings;
    fetch_    = std::move(fetch);
    openMain_ = std::move(openMain);

    // 刷新计时器。**只在面板可见时跑** —— 面板是托盘弹出才看的，
    // 藏着的时候每秒刷一次是白烧电。
    //
    // 注意构造方式：cppwinrt 3.0.260818.1 生成的 DispatcherQueueTimer
    //   DispatcherQueueTimer(std::nullptr_t)
    //   DispatcherQueueTimer(void*, take_ownership_from_abi_t)
    // **没有默认构造函数**，写 `DispatcherQueueTimer timer;` 会报 C2512。
    // 正解是走 DispatcherQueue::CreateTimer()。
    auto dq = DispatcherQueue::GetForCurrentThread();
    if (!dq) return;
    timer_ = dq.CreateTimer();
    timer_.Interval(std::chrono::milliseconds(kRefreshMs));
    // Tick 是 TypedEventHandler<DispatcherQueueTimer, IInspectable>，**两个**参数。
    timer_.Tick([this](auto&&, auto&&) { UpdateNow(); });

    // 保持常亮的初始状态要与 ini 里的一致（可能是上次退出时留下的）。
    UpdateKeepAwakeButton();

    // 常亮续申请计时器。**不受面板可见性影响** ——
    // 「保持常亮」是跨重启的常驻开关，面板收起来也必须继续生效。
    //
    // 【为什么是 10 秒】
    // 两个用途：
    //   ① 续申请 —— SetThreadExecutionState 的请求会被系统清掉，
    //      续申请只要比系统清掉它的频率快就行，10 秒绰绰有余；
    //   ② 让设置窗口里的开关改动及时生效 —— 设置窗口与托盘面板之间
    //      没有直接的通知通道，两边共用同一份 settings 字段，
    //      由这里轮询着把它落到系统上。
    keepAwakeTimer_ = dq.CreateTimer();
    keepAwakeTimer_.Interval(std::chrono::seconds(10));
    keepAwakeTimer_.Tick([this](auto&&, auto&&) { ApplyKeepAwake(); });
    keepAwakeTimer_.Start();

    // 启动时就把持久化的状态真正应用到系统。
    // 【为什么不能只更新按钮文字】
    // 上一轮留下的「常亮已开」如果只体现在按钮文字上，而系统那边其实没申请，
    // 用户看到的与实际发生的就对不上 —— 这是「显示状态」与「真实状态」脱节。
    ApplyKeepAwake();
}

void TrayPanel::ShowPanel() {
    // 先取一次数据 → 定高度 → 定位 → 显示。
    //
    // 【为什么必须在定位之前取数】
    // 有没有电池决定面板要多高（ApplyPanelHeight），而定位是按面板自己的
    // 高度算的。顺序反了的话，位置按 530 算而实际高度是 602，
    // 面板的下沿会压在托盘图标上。
    UpdateNow();

    PositionNearTray();
    visible_ = true;
    // Show(false) = 显示但**不激活**。
    //
    // 【为什么不抢焦点】
    // 用户点托盘图标时通常正在干别的事。把面板弹出来顺手把前台抢走，
    // 会打断他正在输入的东西。「看一眼」这件事不需要焦点。
    // 只有点「打开工具箱」才激活主窗口。
    try {
        AppWindow().Show(false);
    } catch (winrt::hresult_error const& e) {
        LINGWU_TRACE("TrayPanel: AppWindow::Show failed hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
        return;
    }
    if (timer_) timer_.Start();
    // 提到 z-order 顶部，但**不抢焦点**。
    //
    // 【为什么必须这一步 —— 这是「面板看不见」的真凶】
    // Show(false) 只「显示」窗口，**不会**把它提到 z-order 顶部，
    // 它留在被创建时的那个层级。而面板是启动时就构造好的
    // （构造一次 + 隐藏/显示，见本文件头部的注释），
    // 那个层级早就被后来被拉到前面的窗口盖住了。
    // 症状：点了托盘图标，面板**确实弹出来了**，却被前台窗口完全压住，
    // 用户看到的只是「点了没反应」—— 于是以为根本没有这个功能。
    //
    // 【为什么用 RaiseWithoutActivate 而不是直接 Show(true)】
    // Show(true) 会激活窗口、抢走键盘焦点，用户正在输入的东西会被打断。
    // 「看一眼当前指标」不需要焦点：提到最前让人看见，焦点留给原窗口。
    platform::RaiseWithoutActivate(hwnd_);

    // 面板已经可见、布局也跑过了，这一次画出来的内容是实的。
    UpdateNow();
    LINGWU_TRACE("TrayPanel: shown");
}

void TrayPanel::HidePanel() {
    if (!visible_) return;

    // 收起之前必须先退出清理屏。
    //
    // 【为什么这条是「锁输入默认开」之后最危险的一条路径】
    // 清理屏期间输入是**锁着的**（BlockInput），而退出清理屏只有两条路：
    // ESC 与点击 —— 两条都要求面板**可见**。面板一旦被收起，
    // 用户就再也按不到 ESC 了，整台机器的输入被锁死，只能杀进程。
    // 所以「收起」必须先把清理屏退掉、把输入解锁。
    if (cleanMode_) {
        ExitCleanScreen();
    }

    visible_ = false;
    if (timer_) timer_.Stop();
    try {
        AppWindow().Hide();
    } catch (winrt::hresult_error const& e) {
        LINGWU_TRACE("TrayPanel: AppWindow::Hide failed hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    }
    LINGWU_TRACE("TrayPanel: hidden");
}

void TrayPanel::Shutdown() {
    // 护栏：主窗口的退出路径与它的 Closed 兜底都会调这里，
    // 两次 Close() 会抛「窗口已经关掉了」。
    if (closed_) return;
    closed_ = true;
    allowClose_ = true;
    visible_ = false;
    if (timer_) timer_.Stop();
    if (keepAwakeTimer_) keepAwakeTimer_.Stop();
    // 退出时必须把常亮请求与输入锁都撤掉。
    //
    // 【为什么进程退出也能兜底，但还是要显式撤】
    // SetThreadExecutionState 的请求随线程结束自动失效，
    // BlockInput 的锁也随进程结束释放 —— 但那是「靠操作系统兜底」，
    // 显式撤销能让 trace 里留下「确实撤过了」的证据，
    // 而且万一以后改成「关窗口但进程还在」，就不至于把整台机器锁住。
    if (keepAwakeApplied_) {
        ::SetThreadExecutionState(ES_CONTINUOUS);
        keepAwakeApplied_ = false;
    }
    ReleaseInputLock();
    // 【为什么必须真的 Close】
    // 只要还有一个 Window 活着，XAML 框架的消息循环就不会结束，进程会变成
    // 「窗口全没了但进程还在」的僵留状态。
    // 而 AppWindow::Destroy() 不管用：它只销毁原生 HWND，框架的窗口计数
    // 不会归零，消息循环照样不结束。只有真正的 Window::Close() 才会注销它。
    Close();
}

// ---------------------------------------------------------------- 布局 / 定位
void TrayPanel::ApplyFixedSize() {
    // AppWindow 的尺寸接口用**物理像素**，XAML 用逻辑像素。
    // 换算系数取当前窗口所在显示器的缩放（用 GetDpiForWindow，不用 AppWindow：
    // 本版 Windows App SDK 的 AppWindow 上没有 RasterizationScale）。
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    const double scale = (dpi == 0) ? 1.0 : (static_cast<double>(dpi) / 96.0);
    const int w = static_cast<int>(kPanelWidthLogical * scale);
    const int h = static_cast<int>(panelHeightLogical_ * scale);
    AppWindow().Resize(winrt::Windows::Graphics::SizeInt32{ w, h });
    LINGWU_TRACE("TrayPanel: size = %dx%d logical (%dx%d physical, scale=%.2f)",
                 kPanelWidthLogical, panelHeightLogical_, w, h, scale);
}

// 面板高度 = 基准高度 + （有电池时）电池条的高度。
//
// 【为什么只在真的变了时才 resize】
// Resize 会引起一次布局重排，而 UpdateNow 每秒都要跑一次 ——
// 每秒重排一次是在白烧 CPU，也会让面板看着抖。
//
// 【为什么高度变了还要重新定位】
// 面板是「贴在托盘图标正上方」的，位置由它自己的高度算出来。
// 高度变了不重算位置，面板的下沿就会压到托盘图标上。
void TrayPanel::ApplyPanelHeight() {
    // 清理屏态下**不能改尺寸**。
    //
    // 【为什么】
    // 清理屏期间那个窗口铺的是**整个虚拟桌面**（6720×2160 那种尺寸），
    // 这时候把它 resize 回 530 DIP，清理屏会「啪」地缩成一个小窗 ——
    // 用户以为程序坏了。电池状态在这期间怎么变都不该影响那个窗口。
    if (cleanMode_) return;

    const int target = kPanelHeightLogical +
                       (batteryPresent_ ? kBatteryRowHeightLogical : 0);
    if (target == panelHeightLogical_) return;
    panelHeightLogical_ = target;
    ApplyFixedSize();
    if (visible_) PositionNearTray();
    LINGWU_TRACE("TrayPanel: panel height -> %d logical (battery=%d, known=%d)",
                 panelHeightLogical_, batteryPresent_ ? 1 : 0, batteryKnown_ ? 1 : 0);
}

void TrayPanel::PositionNearTray() {
    // 目标：贴在托盘图标**正上方居中**。
    //
    // 【坐标单位】
    // Shell_NotifyIconGetRect 返回的是**物理像素**的屏幕坐标，
    // AppWindow.MoveAndResize 用的也是物理像素 —— 两边同单位，
    // 中间不需要换算。（换算只用在尺寸上：360 DIP → 物理像素。）
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    const double scale = (dpi == 0) ? 1.0 : (static_cast<double>(dpi) / 96.0);
    const int w = static_cast<int>(kPanelWidthLogical * scale);
    // 用**当前**高度，不是基准高度 —— 电池条出现后面板更高了，
    // 拿基准高度去算位置，面板就会盖住托盘图标。
    const int h = static_cast<int>(panelHeightLogical_ * scale);

    RECT icon{};
    if (platform::TrayIconRect(&icon)) {
        const int cx = icon.left + (icon.right - icon.left) / 2;
        const int x  = cx - w / 2;
        const int y  = icon.top - h - kTrayGapPx;
        AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{ x, y, w, h });
        LINGWU_TRACE("TrayPanel: placed at (%d,%d) from tray rect (%d,%d,%d,%d)",
                         x, y, icon.left, icon.top, icon.right, icon.bottom);
        return;
    }

    // 取不到托盘图标矩形（老系统没有 Shell_NotifyIconGetRect，
    // 或者图标还没注册成功）→ 退回屏幕右下角。
    //
    // 【为什么不用 WorkArea 居中】
    // 面板是「贴着托盘弹出来」的，落到屏幕中间会让用户找不到它是从哪冒出来的。
    // 右下角是托盘面板的默认落点，至少方向是对的。
    try {
        const auto work = DisplayArea::GetFromWindowId(
            AppWindow().Id(), DisplayAreaFallback::Nearest).WorkArea();
        // RectInt32 只有 X / Y / Width / Height，没有 Right / Bottom ——
        // 右下角要自己算。
        const int x = work.X + work.Width  - w - 12;
        const int y = work.Y + work.Height - h - 12;
        AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{ x, y, w, h });
        LINGWU_TRACE("TrayPanel: tray rect unavailable, fell back to work area (%d,%d)", x, y);
    } catch (winrt::hresult_error const&) {
        LINGWU_TRACE("TrayPanel: DisplayArea failed, keeping current position");
    }
}

// ---------------------------------------------------------------- 事件
void TrayPanel::WireEvents() {
    OpenMainButton().Click([this](auto&&, auto&&) {
        // 「打开工具箱」= 唤出并激活主窗口，面板自己收起。
        if (openMain_) openMain_();
        HidePanel();
    });

    KeepAwakeButton().Click([this](auto&&, auto&&) {
        if (!settings_) return;
        settings_->keepAwake = !settings_->keepAwake;

        // 立刻把新状态应用到系统（申请或撤销）。
        ApplyKeepAwake();

        try {
            lingwu::core::SaveSettings(*settings_);
        } catch (std::exception const& e) {
            LINGWU_TRACE("TrayPanel: save keepAwake threw %s", e.what());
        }
        UpdateKeepAwakeButton();
    });

    CleanScreenButton().Click([this](auto&&, auto&&) {
        EnterCleanScreen();
    });

    // 清理屏退出：ESC 或者点一下屏幕。
    WindowRoot().KeyDown([this](auto&&, KeyRoutedEventArgs const& args) {
        if (args.Key() != Windows::System::VirtualKey::Escape) return;
        ExitCleanScreen();
        args.Handled(true);
    });
    CleanScreen().Tapped([this](auto&&, auto&&) { ExitCleanScreen(); });

    // 第一次布局跑完之后补一次绘制。
    //
    // 【为什么不能只靠计时器】
    // 折线与进度条都依赖宿主容器的 ActualWidth，而宽度在布局完成前是 0。
    // 面板弹出的那一刻布局还没跑完，第一帧画出来是空的。
    // 这里在根元素 Loaded 之后**再排一次**刷新，把时序挪到布局之后。
    WindowRoot().Loaded([this](auto&&, auto&&) {
        auto dq = DispatcherQueue::GetForCurrentThread();
        if (!dq) return;
        dq.TryEnqueue([this]() { UpdateNow(); });
    });
}

// ---------------------------------------------------------------- 刷新
void TrayPanel::UpdateNow() {
    if (!fetch_) return;
    // 取数在外壳那边（状态页的采样线程），
    // 这里只拿到一份**拷贝**去画 —— 面板不碰采样器的内部状态。
    const core::Snapshot s = fetch_();
    UpdateBadges(s);
    UpdateHealth(s);
    UpdateTiles(s);
    UpdateProcesses(s);
    // 电池条出现 / 消失会改变面板要的高度。
    //
    // 【为什么放在最后】
    // 改窗口尺寸会引起一次布局重排。放在内容更新完之后改，
    // 免得先按旧尺寸画一遍、再被重排冲掉。
    ApplyPanelHeight();
}

// ---------------------------------------------------------------- 徽章行
void TrayPanel::UpdateBadges(const core::Snapshot& s) {
    // CPU 型号：徽章格子窄，用「短名 + 省略」而不是整串型号 ——
    // 「Intel(R) Core(TM) Ultra 5 225H」这种全名在小格子里必然被截成乱码。
    BadgeCpu().Text(to_hstring(core::Ellipsize(s.host.cpuName, 18).c_str()));
    BadgeMem().Text(to_hstring(
        (ui::GbText(s.memory.totalBytes) + L" GB").c_str()));
    BadgeOs().Text(to_hstring(core::Ellipsize(s.host.osShort, 16).c_str()));
    BadgeUptime().Text(to_hstring(
        (L"已运行 " + ui::UptimeBadge(s.host.uptimeSeconds)).c_str()));
}

// ---------------------------------------------------------------- 健康分
winrt::Microsoft::UI::Xaml::Media::SolidColorBrush TrayPanel::GradeBrush() {
    // 等级色按**等级文字**选，不按分数选 ——
    // 分数到等级的映射在 core/monitor/health.cpp 里定义了一份，
    // 这里再写一份阈值，迟早会对不上。
    auto pick = [](auto const& probe) {
        return probe.Background().as<SolidColorBrush>();
    };
    if (lastGrade_ == L"优秀" || lastGrade_ == L"良好") {
        return pick(HealthGoodProbe());
    }
    if (lastGrade_ == L"一般") {
        return pick(HealthWarnProbe());
    }
    return pick(HealthBadProbe());
}

void TrayPanel::UpdateHealth(const core::Snapshot& s) {
    if (s.healthValid) {
        PanelScore().Text(to_hstring(std::to_wstring(s.healthScore).c_str()));
        PanelGrade().Text(to_hstring(s.healthGrade.c_str()));
        lastGrade_ = s.healthGrade;
    } else {
        PanelScore().Text(L"—");
        PanelGrade().Text(L"采集中");
        lastGrade_.clear();
    }
    const auto brush = GradeBrush();
    if (brush) {
        HealthRing().Stroke(brush);
        PanelGrade().Foreground(brush);
    }
}

// ---------------------------------------------------------------- 保持常亮
void TrayPanel::ApplyKeepAwake() {
    if (!settings_) return;
    const bool want = settings_->keepAwake;

    // 按钮文字跟着实际状态走。
    // 【为什么每次都刷】
    // 设置窗口里也能改这个开关，两边共用同一份 settings 字段。
    // 按钮文字是用户唯一能看到「当前是什么状态」的地方，
    // 它与系统实际状态脱节就等于骗人。
    UpdateKeepAwakeButton();

    if (want) {
        // 【为什么已经开着还要再申请一次】
        // SetThreadExecutionState 的语义是「**这次调用**请求保持唤醒」，
        // 系统在若干事件（用户活动、电源策略变化、显示器休眠）后会把它清掉。
        // 想持续保持就必须**周期性重新申请** —— 这正是本函数被计时器反复调用的原因。
        ::SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
        if (!keepAwakeApplied_) {
            keepAwakeApplied_ = true;
            LINGWU_TRACE("TrayPanel: keep awake ON");
        }
    } else {
        if (keepAwakeApplied_) {
            // ES_CONTINUOUS 单独调用 = 撤销之前的显示/系统保持请求。
            ::SetThreadExecutionState(ES_CONTINUOUS);
            keepAwakeApplied_ = false;
            LINGWU_TRACE("TrayPanel: keep awake OFF");
        }
    }
}

void TrayPanel::UpdateKeepAwakeButton() {
    // 状态写在**按钮文字**上，不改按钮底色。
    //
    // 【为什么不用底色表示开关状态】
    // 按钮里的图标是矢量 Path，笔画颜色固定（跟随主题文字色）。
    // 一旦把按钮底色换成主色，图标在浅色主题下就是「近黑图标压在绿色底上」，
    // 对比度勉强但不好看；而且深浅两套主题下两个状态的颜色对比不统一。
    // 文字写「常亮已开 / 保持常亮」是最不容易出错的表达。
    auto label = KeepAwakeButton().Content();
    // 按钮的 Content 是一个 StackPanel（图标 + 文字），文字在里面。
    if (auto panel = label.try_as<StackPanel>()) {
        if (panel.Children().Size() >= 2) {
            if (auto tb = panel.Children().GetAt(1).try_as<TextBlock>()) {
                tb.Text(settings_ && settings_->keepAwake ? L"常亮已开" : L"保持常亮");
            }
        }
    }
}

// ---------------------------------------------------------------- 清理屏
void TrayPanel::EnterCleanScreen() {
    if (cleanMode_) return;
    cleanMode_ = true;

    // 记住原来的几何，退出时还原。
    //
    // 【为什么不用 AppWindow::Bounds】
    // 这版 Windows App SDK 的 AppWindow 上没有 Bounds 属性，
    // 只有 Position() 与 Size() 两个 —— 拼起来就是同一个矩形。
    const auto pos  = AppWindow().Position();
    const auto size = AppWindow().Size();
    savedRect_ = winrt::Windows::Graphics::RectInt32{
        pos.X, pos.Y, size.Width, size.Height };
    savedStyle_ = ::GetWindowLongPtr(hwnd_, GWL_STYLE);

    // 去掉标题栏：系统按钮是画在内容**之上**的，
    // 不去掉的话清理屏右上角会浮着三个按钮，「清空屏幕」就成了半截。
    //
    // 【为什么运行期改 WS_STYLE 是安全的】
    // WS_CAPTION 只是决定「要不要画标题栏」，
    // 去掉之后窗口立刻变成无标题栏；恢复时加回去即可。
    // 这是 WinUI 3 自绘标题栏之外常见的做法。
    ::SetWindowLongPtr(hwnd_, GWL_STYLE, savedStyle_ & ~WS_CAPTION);

    // 盖住整个虚拟桌面（可能跨多显示器）。
    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

    RootGrid().Visibility(Visibility::Collapsed);
    CleanScreen().Visibility(Visibility::Visible);
    AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{ vx, vy, vw, vh });

    // 输入锁定。
    //
    // 【为什么默认开】
    // 清理屏的用途就是「挡住误触」—— 演示、投屏、录屏时手会不自觉地
    // 碰到键盘鼠标，把当前窗口切走。不锁输入的话，一半时间是在跟误触
    // 打架，这个功能等于没做。
    //
    // 【为什么放在窗口铺满之后】
    // 万一 BlockInput 失败（权限不足等），这时已经是清理屏态了，
    // 失败要立刻落 trace，并且**不改变**已经显示出来的界面。
    //
    // 【为什么要把结果写到提示行上】
    // 锁没锁上，用户看得见才算数 —— 「键盘照常能动」会让人以为程序坏了。
    // 而且这也是验收脚本判断「锁到底生效没有」的抓手：asInvoker 的
    // 测试副本锁不上，提示行会如实写出来，而不是让人猜。
    if (settings_ && settings_->cleanScreenLockInput) {
        if (::BlockInput(TRUE)) {
            inputLocked_ = true;
            LINGWU_TRACE("TrayPanel: input locked");
        } else {
            LINGWU_TRACE("TrayPanel: BlockInput(TRUE) failed; input stays unlocked");
        }
    }
    UpdateCleanScreenHint();

    // 必须**激活**：ESC 是发给有焦点的窗口的，
    // 面板弹出时是不抢焦点的（AppWindow::Show(false)），
    // 所以进入清理屏时要显式抢一次焦点，否则 ESC 收不到。
    try {
        AppWindow().Show(true);
    } catch (winrt::hresult_error const& e) {
        LINGWU_TRACE("TrayPanel: clean screen Show failed hresult=0x%08X",
                         static_cast<unsigned>(e.code().value));
    }
    LINGWU_TRACE("TrayPanel: clean screen on (virtual desktop %dx%d at %d,%d)",
                     vw, vh, vx, vy);
}

void TrayPanel::ExitCleanScreen() {
    if (!cleanMode_) return;
    cleanMode_ = false;

    // 先解锁，再恢复界面。
    // 【顺序为什么重要】
    // 解锁失败的话要落 trace（用户会以为程序卡住了），
    // 但界面照样要恢复 —— 否则用户连 ESC 都按不到。
    ReleaseInputLock();

    CleanScreen().Visibility(Visibility::Collapsed);
    RootGrid().Visibility(Visibility::Visible);

    // 标题栏样式恢复。
    if (savedStyle_ != 0) {
        ::SetWindowLongPtr(hwnd_, GWL_STYLE, savedStyle_);
    }
    // 清理屏期间不能改窗口尺寸（那会让清理屏缩成小窗），
    // 所以这期间电池状态的变化是被压着的 —— 退出时补算一次高度。
    ApplyPanelHeight();
    // 回到托盘图标上方的位置（用当前缩放重算，
    // 不能直接还原 savedRect_ —— 那时的缩放可能已经变了）。
    PositionNearTray();
    LINGWU_TRACE("TrayPanel: clean screen off");
}

void TrayPanel::ReleaseInputLock() {
    if (!inputLocked_) return;
    inputLocked_ = false;
    // 【为什么解锁失败必须落 trace】
    // 解锁失败 = 整台机器的输入还锁着，用户只能杀进程。
    // 这是一个「用户以为电脑死了」的状态，静默失败等于坑人。
    if (!::BlockInput(FALSE)) {
        LINGWU_TRACE("TrayPanel: BlockInput(FALSE) failed; the system stays locked");
    }
}

// 清理屏底部那行提示。除「按 Esc 退出」外，还要说明**输入到底锁没锁**。
//
// 【为什么必须写在界面上】
// 锁不上只有一个原因：进程不是管理员（BlockInput 要提权）。
// 这种情况下键盘照常能用，用户会以为程序坏了 ——
// 把状态如实写出来，他才不会去怪程序。
void TrayPanel::UpdateCleanScreenHint() {
    if (inputLocked_) {
        CleanScreenHint().Text(L"按 Esc 退出清理屏幕 · 输入已锁定");
    } else if (settings_ && settings_->cleanScreenLockInput) {
        CleanScreenHint().Text(L"按 Esc 退出清理屏幕 · 输入锁定失败（需要管理员权限）");
    } else {
        CleanScreenHint().Text(L"按 Esc 退出清理屏幕");
    }
}

// ---------------------------------------------------------------- 磁贴
void TrayPanel::UpdateTiles(const core::Snapshot& s) {
    // ---- CPU ----
    const double cpuPercent = s.history.cpu.Last();
    TileCpuValue().Text(to_hstring(std::to_wstring(
        static_cast<long long>(cpuPercent + 0.5)).c_str()));
    TileCpuBadge().Text(to_hstring((std::to_wstring(s.coreCount) + L" 核").c_str()));
    TileCpuSub().Text(to_hstring(core::Ellipsize(s.host.cpuName, 24).c_str()));
    ui::SetSpark(TileCpuSparkHost(), TileCpuSpark(), s.history.cpu, 10.0, kPanelSparkHeight);
    // 折线点数的 trace。**只在点数变化时写一行** ——
    // 每秒一行会把 trace.log 淹掉，而这个值的用处恰恰是现场排障：
    // 「面板弹出来折线是空的」到底是采样还没攒够（点数 < 2），
    // 还是渲染真的坏了（点数够了但画不出来）。
    if (s.history.cpu.Size() != lastSparkPts_) {
        lastSparkPts_ = s.history.cpu.Size();
        LINGWU_TRACE("TrayPanel: cpu spark points=%zu", lastSparkPts_);
    }

    // ---- 内存（物理内存占用口径，与状态栏的「私有提交之和」不是一回事）----
    if (s.memory.totalBytes > 0) {
        TileMemValue().Text(to_hstring(std::to_wstring(
            static_cast<long long>(s.memory.usedPercent + 0.5)).c_str()));
        TileMemBadge().Text(to_hstring(ui::MemoryBadge(s.memory.usedPercent).c_str()));
        TileMemSub().Text(to_hstring(
            (ui::GbText(s.memory.totalBytes - s.memory.availableBytes) + L" GB / " +
             ui::GbText(s.memory.totalBytes) + L" GB").c_str()));
        ui::SetBar(TileMemBarHost(), TileMemFill(), s.memory.usedPercent / 100.0);
    } else {
        TileMemValue().Text(L"—");
        TileMemBadge().Text(L"");
        TileMemSub().Text(L"取不到");
    }

    // ---- 网络 ----
    {
        std::wstring value, unit;
        ui::SpeedParts(s.net.downBps, value, unit);
        TileNetValue().Text(to_hstring(value.c_str()));
        TileNetUnit().Text(to_hstring(unit.c_str()));
        TileNetBadge().Text(to_hstring(s.net.nic.empty() ? L"无网卡" : s.net.nic.c_str()));
        TileNetSub().Text(to_hstring((L"上传 " + core::FormatSpeed(s.net.upBps)).c_str()));
        ui::SetSpark(TileNetSparkHost(), TileNetSpark(), s.history.netDown, 0.0, kPanelSparkHeight);
    }

    // ---- 磁盘剩余 ----
    if (s.diskSpace.valid && s.diskSpace.totalBytes > 0) {
        const double total = static_cast<double>(s.diskSpace.totalBytes);
        const double used  = total - static_cast<double>(s.diskSpace.freeBytes);
        const double usedPct = used * 100.0 / total;
        const double freePct = 100.0 - usedPct;
        TileDiskValue().Text(to_hstring(ui::GbText(s.diskSpace.freeBytes).c_str()));
        TileDiskBadge().Text(to_hstring((ui::GbText(s.diskSpace.totalBytes) + L" GB").c_str()));
        TileDiskSub().Text(to_hstring(
            (ui::GbText(static_cast<uint64_t>(used)) + L" GB 已用 · " +
             std::to_wstring(static_cast<long long>(usedPct + 0.5)) + L"%").c_str()));
        ui::SetBar(TileDiskBarHost(), TileDiskFill(), freePct / 100.0);
    } else {
        TileDiskValue().Text(L"—");
        TileDiskBadge().Text(L"");
        TileDiskSub().Text(L"取不到");
    }

    // ---- 电池 ----
    // 无电池（present=false）时整条**隐藏**。
    // 「没有电池」和「电池没电」是两件事，显示 0% 是在撒谎。
    //
    // 【为什么要记下来】
    // 面板高度跟着电池变（见 ApplyPanelHeight），所以这里必须把
    // 「有没有电池」记成成员，供高度计算用。
    //
    // 【known 也记一份】
    // 「确实没有电池」与「状态读不到」在面板上表现相同（都不显示电池条），
    // 但它们是两回事，trace 里必须能分开，否则出问题时无从判断是哪一种。
    batteryPresent_ = s.battery.present;
    batteryKnown_   = s.battery.known;
    if (s.battery.present) {
        BatteryCard().Visibility(Visibility::Visible);
        TileBatteryValue().Text(to_hstring(std::to_wstring(s.battery.percent).c_str()));
        TileBatteryUnit().Text(L"%");
        TileBatteryBadge().Text(s.battery.charging ? L"充电中" : L"电池");
        TileBatterySub().Text(to_hstring(
            (s.battery.secondsLeft > 0
                 ? ui::RemainingText(s.battery.secondsLeft)
                 : std::wstring(L"剩余时间未知")).c_str()));
    } else {
        BatteryCard().Visibility(Visibility::Collapsed);
    }
}

// ---------------------------------------------------------------- Top 进程
void TrayPanel::BuildProcessTable() {
    auto grid = ProcListGrid();
    grid.RowDefinitions().Clear();

    // 表头 + 五行数据，一共六行。
    //
    // 【为什么行高写死而不是 Auto】
    // 行的内容是「名称 / 内存 / CPU」三个 10.5 号字，
    // Auto 量出来的行高会随字体回退（DPI、字体缺失）飘动，
    // 而这张表是**每秒原地改文字**的 —— 行高一飘，整张表就会抖。
    // 写死 21 DIP 就稳了。
    for (int i = 0; i <= kTopProcessRows; ++i) {
        grid.RowDefinitions().Append(RowDefinition());
    }
    grid.RowDefinitions().GetAt(0).Height(GridLength{ kProcHeaderHeight });
    for (int i = 1; i <= kTopProcessRows; ++i) {
        grid.RowDefinitions().GetAt(i).Height(GridLength{ kProcRowHeight });
    }

    // 【AutomationId 是验收脚本的抓手】
    // 进程表是 C++ 建的，没有 x:Name 可用，所以 AutomationId 要自己写。
    // 验收脚本要靠它精确定位「某一行的 CPU 格子」，
    // 靠文字名找是找不准的 —— 面板里有好几个叫「CPU」的元素
    // （CPU 磁贴的标题、表头），只有一个靠位置才能区分。
    auto makeCell = [](const wchar_t* text, const wchar_t* id, bool right) {
        auto tb = TextBlock();
        tb.Text(text);
        tb.SetValue(AutomationProperties::AutomationIdProperty(), box_value(id));
        tb.FontSize(10.5);
        tb.VerticalAlignment(VerticalAlignment::Center);
        if (right) {
            tb.HorizontalAlignment(HorizontalAlignment::Right);
            tb.TextAlignment(TextAlignment::Right);
        } else {
            tb.TextTrimming(TextTrimming::CharacterEllipsis);
        }
        return tb;
    };

    // 每一行（表头 + 数据行）都是「名称 / 内存 / CPU」三列的 Grid。
    //
    // 【为什么表头与数据行必须用同一套列】
    // 表头是文字、数据行是数字，两边的列宽一模一样才能对齐。
    // 用同一段构造代码，两边就不可能对不上。
    //
    // 【为什么名称列是星号列】
    // 见上面 kProcMemColWidth 处的注释：定宽推算出来的名称列会把 CPU 列挤出容器。
    auto makeRowGrid = []() {
        auto g = Grid();
        g.ColumnDefinitions().Append(ColumnDefinition());
        g.ColumnDefinitions().GetAt(0).Width(GridLength(1.0, GridUnitType::Star));
        g.ColumnDefinitions().Append(ColumnDefinition());
        g.ColumnDefinitions().GetAt(1).Width(GridLength(kProcMemColWidth));
        g.ColumnDefinitions().Append(ColumnDefinition());
        g.ColumnDefinitions().GetAt(2).Width(GridLength(kProcCpuColWidth));
        return g;
    };

    // 把一个格子放进指定列。
    //
    // 【为什么必须显式给 Grid.Column】
    // Grid 的子元素默认全落在 (0,0) —— 不给列号，三列会叠在一起，
    // 看上去就是「名称压在内存上」。
    auto putInColumn = [](TextBlock const& cell, int col) {
        cell.SetValue(Grid::ColumnProperty(), box_value(col));
    };

    // 表头。
    {
        auto head = makeRowGrid();
        head.Height(kProcHeaderHeight);
        auto name = makeCell(L"名称", L"ProcHeadName", false);
        name.FontSize(9.5);
        name.Opacity(0.45);
        auto mem = makeCell(L"内存", L"ProcHeadMem", true);
        mem.FontSize(9.5);
        mem.Opacity(0.45);
        mem.Width(kProcMemColWidth);
        auto cpu = makeCell(L"CPU", L"ProcHeadCpu", true);
        cpu.FontSize(9.5);
        cpu.Opacity(0.45);
        cpu.Width(kProcCpuColWidth);
        putInColumn(name, 0);
        putInColumn(mem, 1);
        putInColumn(cpu, 2);
        head.Children().Append(name);
        head.Children().Append(mem);
        head.Children().Append(cpu);
        head.SetValue(Grid::RowProperty(), box_value(0));
        grid.Children().Append(head);
    }

    // 数据行。控件只建一次，之后每秒原地改文字。
    //
    // 分隔线用「主题文字色 @ 14%」：跟卡片底色同一个来源，
    // 深浅两套主题下都是「比背景略深一点」的淡线。
    auto sep = SolidColorBrush();
    if (const auto probe = FillProbe().Background().as<SolidColorBrush>()) {
        sep.Color(probe.Color());
    }
    sep.Opacity(0.14);

    for (int i = 0; i < kTopProcessRows; ++i) {
        auto row = Border();
        row.Height(kProcRowHeight);
        // 顶部分隔线：demo 里每一行上方都有一条极淡的线。
        row.BorderThickness(Thickness{ 0, 1, 0, 0 });
        row.BorderBrush(sep);

        auto cells = makeRowGrid();
        cells.VerticalAlignment(VerticalAlignment::Center);

        ProcLine line;
        line.name = makeCell(L"", L"ProcName", false);
        line.mem  = makeCell(L"", L"ProcMem", true);
        line.cpu  = makeCell(L"", L"ProcCpu", true);
        line.name.Opacity(0.88);
        line.mem.Width(kProcMemColWidth);
        line.cpu.Width(kProcCpuColWidth);
        // 行号写进 AutomationId：验收脚本要能精确点到「第 3 行的 CPU 格子」。
        {
            wchar_t buf[32];
            swprintf_s(buf, L"ProcName%d", i);
            line.name.SetValue(AutomationProperties::AutomationIdProperty(), box_value(buf));
            swprintf_s(buf, L"ProcMem%d", i);
            line.mem.SetValue(AutomationProperties::AutomationIdProperty(), box_value(buf));
            swprintf_s(buf, L"ProcCpu%d", i);
            line.cpu.SetValue(AutomationProperties::AutomationIdProperty(), box_value(buf));
        }
        putInColumn(line.name, 0);
        putInColumn(line.mem, 1);
        putInColumn(line.cpu, 2);
        cells.Children().Append(line.name);
        cells.Children().Append(line.mem);
        cells.Children().Append(line.cpu);
        row.Child(cells);

        // 【行号必须显式给】
        // Grid 的子元素默认全落在 (0,0) 那一格 —— 不给 Grid.Row 的话
        // 六行会**叠在一起**，看上去就是「表头压着数据、五个数据叠成一坨」。
        row.SetValue(Grid::RowProperty(), box_value(i + 1));
        grid.Children().Append(row);
        procLines_.push_back(line);
    }
}

void TrayPanel::UpdateProcesses(const core::Snapshot& s) {
    // Top 进程按 **CPU 降序**取前几行（跟 demo 一致：
    // 托盘面板是「现在什么在占资源」的快照，不是「谁最占内存」）。
    std::vector<const core::AppInfo*> top;
    top.reserve(s.apps.size());
    for (const auto& a : s.apps) top.push_back(&a);
    std::sort(top.begin(), top.end(),
              [](const core::AppInfo* x, const core::AppInfo* y) {
                  if (x->cpuNorm != y->cpuNorm) return x->cpuNorm > y->cpuNorm;
                  return x->name < y->name;   // 同分时按名字排，顺序才稳定
              });

    const size_t rows = std::min<size_t>(procLines_.size(), top.size());
    for (size_t i = 0; i < procLines_.size(); ++i) {
        if (i < rows) {
            const core::AppInfo& a = *top[i];
            procLines_[i].name.Text(to_hstring(core::Ellipsize(a.name, 26).c_str()));
            procLines_[i].mem.Text(to_hstring(core::FormatBytes(a.privWorkSet).c_str()));
            wchar_t cpuBuf[32];
            swprintf_s(cpuBuf, L"%.1f", a.cpuNorm);
            procLines_[i].cpu.Text(to_hstring(cpuBuf));
        } else {
            procLines_[i].name.Text(L"");
            procLines_[i].mem.Text(L"");
            procLines_[i].cpu.Text(L"");
        }
    }
}

}  // namespace winrt::LingwuToolbox::implementation
