#include "pch.h"
#include "SettingsWindow.xaml.h"
// 与 MainWindow.xaml.cpp 同一套接线，理由见那边的长注释，简述：
//   · pch.h 必须是本编译单元的**第一个** include。放在 .xaml.h 里不够，
//     因为 .xaml.h 自己先 include 了 SettingsWindow.g.h；/Yu"pch.h" 找不到
//     指令就以 C1010「查找预编译头时遇到意外的文件结尾」失败。
//     （本项目真的踩过：SettingsWindow.xaml.cpp 起初只 include 了
//      .xaml.h，报的就是这个错。）
//   · InitializeComponent() / Connect() / GetBindingConnector() 的定义在
//     .xaml.g.hpp 里，.g.h 只有声明。必须由某一个代码后置 .cpp 引入，
//     否则编译全过、链接期一堆 LNK2019。
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif
#if __has_include("SettingsWindow.xaml.g.hpp")
#include "SettingsWindow.xaml.g.hpp"
#endif
// IWindowNative 是 MIDL 生成的 COM 接口，不在任何 cppwinrt 投影头里，
// 随 WinUI 包 include 目录分发，是官方取 HWND 的标准做法。
#include <microsoft.ui.xaml.window.h>

#include <winrt/Windows.Foundation.Collections.h>

// SortMode 枚举（SORT_MEM_DESC 等）在 core/metrics.h 里，**不在**
// core/settings.h 里。.xaml.h 只 include 了 settings.h，所以这里必须补。
// 少了它的报错很有欺骗性：`lingwu::core::monitor` 命名空间本身是好的，
// 报的却是 C2039「SORT_MEM_DESC 不是 lingwu::core::monitor 的成员」，
// 紧接着 case 标签退化成 C2051/C4065，看起来像语法写错了。
#include "core/monitor/autostart.h"
#include "core/monitor/metrics.h"
#include "platform/trace.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Media;

namespace winrt::LingwuToolbox::implementation {
// 与 MainWindow.xaml.cpp 一致：core 指 core 数据层，不是任何 WinRT 命名空间。
namespace core = lingwu::core::monitor;

namespace {

// 采样间隔的四个候选档。与 settings.cpp 的合法性收敛区间（250..10000 ms）对齐。
constexpr int kIntervalChoices[] = { 500, 1000, 2000, 5000 };

}  // namespace

SettingsWindow::SettingsWindow() {
    // 与 MainWindow 完全同一套时序：InitializeComponent 就在构造函数里跑。
    // （这里曾写过「构造期不能调 InitializeComponent」的注释，是错的 ——
    //  本项目的主窗口一直这么干，四个 XAML 文件全部验证过。留着这段说明免得
    //  下一个人再把它改回去。）
    InitializeComponent();

    ApplyBackdrop();

    // 必须在这里就挂好事件。放在这里而不是等 LoadFrom/Activate，是因为
    // WireEvents 里要碰 DragRegion()/OkButton() 等控件 —— 它们只在上面的
    // InitializeComponent 之后才真实存在。
    WireEvents();

    // 【关闭时只隐藏，不销毁】
    //
    // 原因有两条，缺一不可：
    //
    // ① WinUI 3 的 Window 在 Close() 之后**不可复用** —— 再调 Activate()
    //    会抛 hresult_error。所以「关掉就重新 new 一个」在功能上是唯一出路
    //    （2026-10-20 那个崩溃就是这么来的：异常从托盘的 LRESULT CALLBACK
    //    窗口过程逃出去 → std::terminate → abort() → 0xC0000409）。
    //
    // ② 但「每次 new 一个」这条路实测走不通 —— **线性泄漏**原生资源：
    //    每新建/关闭一轮，句柄数 +36 左右、私有内存 +1.8 MB，无上界。
    //    实测 30 轮：私有内存 139.5 → 169.9 MB（+30.4 MB，越过 145 MB 红线），
    //    句柄数 893 → 1272（+379，线性不收敛）。
    //    对照实验：同时长纯静置（完全不开设置窗口）句柄数持平
    //    （895 → 893），私有内存的 +28 MB 是一次性框架预热（AGENTS.md 已记录），
    //    与设置窗口无关 —— 所以那 +36 句柄/轮就是每轮新建窗口漏掉的。
    //
    // 所以改成「构造一次 + 隐藏/显示」：关闭时只 Hide，窗口对象一直活着，
    // 每轮零分配，泄漏从根上消失。用户看到的行为完全一样（窗口消失、
    // 任务栏不留按钮）。
    //
    // 真正关闭由主窗口退出时完成：先 AllowClose()（放行下面这个拦截），
    // 再 Window::Close()。
    //
    // 【为什么不用 AppWindow::Destroy()】实测踩过：它只销毁原生 HWND，
    // **框架的窗口计数不会归零**，消息循环照样不结束 —— 表现是「窗口全没了
    // 但进程还在」的僵留（trace 里 `MainWindow closed` 已经打过，进程仍在）。
    // 只有真正的 Window::Close() 才会把它从框架里注销掉。
    // 必须关：只要还有一个 Window 活着，XAML 框架的消息循环就不会结束。
    AppWindow().Closing([this](auto&&,
        winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
        // 主窗口已经放行（要退出了）→ 这次关闭必须成立，否则进程会僵留。
        if (allowClose_) return;
        args.Cancel(true);
        HideSelf();
    });
}

// 允许本窗口真正关闭。见 SettingsWindow.idl 里那段说明。
void SettingsWindow::AllowClose() {
    allowClose_ = true;
}

void SettingsWindow::ApplyBackdrop() {
    // 与主窗口同一套运行时选择：Win11 用 Mica，Win10 退 Acrylic。
    // 两者都在 Microsoft.UI.Xaml.Media 命名空间，且在本机之外无法编译期判断。
    if (platform::SupportsMica()) {
        SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop());
    } else {
        SystemBackdrop(Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop());
    }
}

// ---------------------------------------------------------------- 档位映射
// 排序下拉的 5 项与 core::SortMode 的 5 个取值一一对应。
// 用表驱动而不是 switch：枚举一旦新增，漏一项的后果是「某个档位在设置里选不到」，
// 而那种缺陷不报错、不崩溃，只是安静地少一个功能。
constexpr int kSortChoices[] = {
    lingwu::core::monitor::SORT_MEM_DESC,
    lingwu::core::monitor::SORT_MEM_ASC,
    lingwu::core::monitor::SORT_CPU_DESC,
    lingwu::core::monitor::SORT_CPU_ASC,
    lingwu::core::monitor::SORT_NAME_ASC,
};
constexpr int kSortChoiceCount = static_cast<int>(std::size(kSortChoices));

int SettingsWindow::SortModeFromIndex(int idx) {
    if (idx < 0 || idx >= kSortChoiceCount) return lingwu::core::monitor::SORT_MEM_DESC;
    return kSortChoices[idx];
}
int SettingsWindow::IndexFromSortMode(int mode) {
    for (int i = 0; i < kSortChoiceCount; ++i) {
        if (kSortChoices[i] == mode) return i;
    }
    return 0;   // 未知值（含越界）归到默认档「内存占用 ↓」
}
int SettingsWindow::IntervalMsFromIndex(int idx) {
    if (idx < 0 || idx >= static_cast<int>(std::size(kIntervalChoices))) return 1000;
    return kIntervalChoices[idx];
}
int SettingsWindow::IndexFromIntervalMs(int ms) {
    for (int i = 0; i < static_cast<int>(std::size(kIntervalChoices)); ++i) {
        if (kIntervalChoices[i] == ms) return i;
    }
    return 1;   // 1000 ms
}

// ---------------------------------------------------------------- 装配
void SettingsWindow::LoadFrom(bool aggregate, int sortMode, int intervalMs, bool autoStart) {
    // 主窗口在 make<SettingsWindow>() 之后、Activate() 之前调用。
    // 此时构造函数已经跑完 InitializeComponent + WireEvents，控件全都真实存在，
    // 所以这里既写成员（供「确定」时兜底）也直接灌控件（保证用户看到的就是现状）。
    //
    // 【accepted_ 必须在这里复位】
    // 主窗口是**轮询** Accepted() 来回收结果的（不是靠 Closed 事件 ——
    // 窗口改成「关闭时只隐藏」之后 Closed 永远不触发）。
    // 每次打开都复位一次，才不会把上一轮的「确定」结果重复应用一遍。
    accepted_ = false;

    resAggregate_  = aggregate;
    resSortMode_   = sortMode;
    resIntervalMs_ = intervalMs;
    resAutoStart_  = autoStart;

    AggregateSwitch().IsOn(aggregate);
    AutoStartSwitch().IsOn(autoStart);
    SortCombo().SelectedIndex(IndexFromSortMode(sortMode));
    IntervalCombo().SelectedIndex(IndexFromIntervalMs(intervalMs));
    AutoStartError().Visibility(Visibility::Collapsed);
}

void SettingsWindow::WireEvents() {
    // 自绘标题栏：内容延伸进标题栏区域，DragRegion 设为可拖拽区。
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(DragRegion());

    // 这里只挂 Click。ComboBox 的 SelectionChanged 是
    // Microsoft.UI.Xaml.Controls.SelectionChangedEventHandler，本版 cppwinrt
    // 的该类委托模板构造函数漏 inline，直接接会 LNK2019（详见 MainWindow.xaml.h
    // 顶部长注释）。所以两个 ComboBox 的值都在「确定」被按下时一次性读回。
    OkButton().Click([this](auto&&, auto&&) { OnOk(); });
    CancelButton().Click([this](auto&&, auto&&) { OnCancel(); });

    // 灌入成员里的当前值。LoadFrom() 会在之后覆盖成真实设置，
    // 这里保证「万一没人调 LoadFrom」时控件也不是未定义状态。
    AggregateSwitch().IsOn(resAggregate_);
    AutoStartSwitch().IsOn(resAutoStart_);
    SortCombo().SelectedIndex(IndexFromSortMode(resSortMode_));
    IntervalCombo().SelectedIndex(IndexFromIntervalMs(resIntervalMs_));
    AutoStartError().Visibility(Visibility::Collapsed);

    HWND hwnd{};
    check_hresult(this->m_inner.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    // 圆角 + 系统背板。Win10 上门控不过会静默跳过，不视为错误。
    platform::ApplyWindowDecorations(hwnd);
}

// ---------------------------------------------------------------- 自启动
void SettingsWindow::ApplyAutoStart(bool wantOn) {
    autoStartError_.clear();
    std::wstring err;
    const bool ok = wantOn ? core::AutostartEnable(err) : core::AutostartDisable(err);
    if (!ok) {
        autoStartError_ = wantOn
            ? (L"开启开机自启动失败：" + err)
            : (L"关闭开机自启动失败：" + err);
    }
    // 注册表才是唯一事实源，所以回读一次再决定 UI 上的开关位置：
    // 写入失败时开关必须留在原位，不能让用户以为改成功了。
    bool enabled = false, stale = false;
    core::AutostartReconcile(enabled, stale);
    resAutoStart_ = enabled;
    AutoStartSwitch().IsOn(enabled);
}

// ---------------------------------------------------------------- 按钮
void SettingsWindow::OnOk() {
    // 先落自启（可能失败，失败要拦住不让关窗），再收其余三项。
    ApplyAutoStart(AutoStartSwitch().IsOn());
    if (!autoStartError_.empty()) {
        AutoStartError().Text(autoStartError_);
        AutoStartError().Visibility(Visibility::Visible);
        return;   // 不关窗，让用户看到原因
    }

    resAggregate_  = AggregateSwitch().IsOn();
    resSortMode_   = SortModeFromIndex(SortCombo().SelectedIndex());
    resIntervalMs_ = IntervalMsFromIndex(IntervalCombo().SelectedIndex());
    accepted_      = true;

    // 设置由主窗口统一落 ini（它持有 settings_ 并负责 SaveSettings），
    // 这里只把结果通过 Accepted() / Result*() 交回去。
    //
    // 【为什么是隐藏而不是 Close】
    // 见构造函数里那段长注释：关掉就销毁 → 不可复用 → 只能每次新建 →
    // 每轮线性泄漏原生资源。改成只隐藏，窗口对象一直活着。
    // 主窗口靠轮询 Accepted() 回收结果（见 MainWindow::ApplyPendingSettingsResult）。
    HideSelf();
}

void SettingsWindow::OnCancel() {
    accepted_ = false;
    HideSelf();
}

// 隐藏自己（而不是 Close）。见构造函数里那段长注释。
void SettingsWindow::HideSelf() {
    try {
        AppWindow().Hide();
    } catch (winrt::hresult_error const& e) {
        LINGWU_TRACE("SettingsWindow: AppWindow::Hide failed hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(e.code().value), e.message().c_str());
    }
}

}  // namespace winrt::LingwuToolbox::implementation
