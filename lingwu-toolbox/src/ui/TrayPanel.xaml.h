#pragma once

// 按官方 C++/WinRT 模板的写法，这里只引 cppwinrt 生成的 TrayPanel.g.h。
// 它内部用 __has_include("TrayPanel.g.h") 决定要不要把 XAML 代码后置头带进来；
// 一旦落空就退化成没有代码生成的纯壳，症状是 InitializeComponent() /
// RootGrid() 之类全部不存在，报一堆看起来不相干的编译错误。
// vcxproj 里为此把 $(IntDir)src\ui 加进了 AdditionalIncludeDirectories。
#include "TrayPanel.g.h"

#include "pch.h"

#include "core/monitor/metrics.h"
#include "core/settings.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace winrt::LingwuToolbox::implementation {

// 托盘指标面板。托盘左键弹出的那个 360×530 的小窗。
//
// 【它拥有什么、什么归外壳】
//   本面板：自己的布局、自己的刷新计时器、Top 进程表。
//   外壳  ：取数（快照从状态页来）、动作（打开主窗口 / 保持常亮 / 清理屏幕）、
//           窗口几何与退出序列。
//
// 【为什么数据是「外壳喂进来」而不是面板自己去采样】
// 采样线程在状态页手里，面板再开一条采样通路就是第二条跨线程通道，
// 迟早出现「两个地方的 CPU 数不一样」。所以面板只负责画。
struct TrayPanel : TrayPanelT<TrayPanel> {
    TrayPanel();

    // ---- 与外壳的契约 --------------------------------------------------
    // 外壳注入取数与动作回调。
    //
    // 【为什么用普通 C++ 回调，而不是 WinRT 委托】
    // 外壳**按值持有**本面板的实现类型，可以直接调它的任意 C++ 方法；
    // 反过来面板要拿数据、要触发动作，就需要一个回到外壳的通道。
    // 走 WinRT 委托意味着这套东西要写进 .idl、变成公开 WinRT API ——
    // 而面板只是这个程序内部的一个窗口，不需要对外暴露任何东西。
    using SnapshotFn = std::function<lingwu::core::monitor::Snapshot()>;

    // 由外壳在构造函数之后立刻调用一次（早于第一次刷新）。
    void Initialize(lingwu::core::Settings* settings,
                    SnapshotFn fetch,
                    std::function<void()> openMain);

    // 显示 / 隐藏。显示时定位到托盘图标正上方。
    void ShowPanel();
    void HidePanel();
    bool IsVisible() const { return visible_; }

    // 退出时**真正关闭**本窗口。
    //
    // 只要还有一个 Window 活着，XAML 框架的消息循环就不会结束，进程会变成
    // 「窗口全没了但进程还在」的僵留状态。而 AppWindow::Destroy() 不管用：
    // 它只销毁原生 HWND，框架的窗口计数不会归零，消息循环照样不结束。
    // 只有真正的 Window::Close() 才会把它从框架里注销掉。
    void Shutdown();

private:
    void WireEvents();
    void ApplyFixedSize();
    // 面板高度 = 基准高度 + （有电池时）电池条的高度。
    // 【为什么不是写死的常量】
    // 电池条在台式机上整条隐藏、在笔记本上多占 72 DIP。
    // 写死一套，要么台式机上多一截空白，要么笔记本上把
    // 最下面的「打开工具箱」挤出可视区（用户就点不到了）。
    void ApplyPanelHeight();
    void PositionNearTray();
    void UpdateNow();
    void BuildProcessTable();
    void UpdateBadges(const lingwu::core::monitor::Snapshot& s);
    void UpdateHealth(const lingwu::core::monitor::Snapshot& s);
    void UpdateTiles(const lingwu::core::monitor::Snapshot& s);
    void UpdateProcesses(const lingwu::core::monitor::Snapshot& s);
    // 按等级取等级画刷（绿 / 琥珀 / 红）。
    //
    // 【为什么不标 const】
    // XAML 生成的元素访问器（HealthGoodProbe() 等）**不是 const 成员**，
    // 标了 const 就调不动它们。
    winrt::Microsoft::UI::Xaml::Media::SolidColorBrush GradeBrush();
    // 「保持常亮」按钮的外观跟着状态走。
    void UpdateKeepAwakeButton();
    // 把 settings_->keepAwake 的状态**真正应用到系统**（申请 / 撤销常亮）。
    //
    // 【为什么不能只在按钮点击时调一次】
    // SetThreadExecutionState 的语义是「**这次调用**请求保持唤醒」，
    // 系统在若干事件（用户活动、电源策略变化、显示器休眠）后会把它清掉。
    // 想持续保持就必须**周期性重新申请**。
    void ApplyKeepAwake();
    // 清理屏：进入 / 退出全屏纯色模式。
    void EnterCleanScreen();
    void ExitCleanScreen();
    // 解除输入锁定（BlockInput）。
    //
    // 【为什么必须单独抽出来】
    // BlockInput 锁住的是**整台机器**的键鼠输入，只有调用它的那个线程不受影响。
    // 任何一条退出路径（ESC / 点击 / 进程退出）都必须解锁，
    // 漏一条就等于把用户的电脑锁死了。解锁失败要落 trace。
    void ReleaseInputLock();
    // 清理屏底部那行提示：报「输入到底锁没锁」。
    //
    // 【为什么要写到界面上】
    // 锁不上只有一个原因（进程不是管理员），不写出来用户会以为程序坏了。
    void UpdateCleanScreenHint();

    lingwu::core::Settings* settings_ = nullptr;
    SnapshotFn fetch_;
    std::function<void()> openMain_;

    // 刷新计时器。只在面板可见时跑 —— 面板是托盘弹出才看的，
    // 藏着的时候每秒刷一次是白烧电。
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };

    // 常亮续申请计时器。**与面板可见性无关**，一直是活的 ——
    // 「保持常亮」是个跨重启的常驻开关，面板收起来也必须继续生效。
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer keepAwakeTimer_{ nullptr };

    // 当前**已经向系统申请过**的常亮状态。
    // 用来判断「需要重新申请」还是「状态已经对上了」。
    bool keepAwakeApplied_ = false;

    // 清理屏期间是否真的锁住了输入。
    bool inputLocked_ = false;

    // 当前面板高度（逻辑像素）。
    // 会随「有没有电池」变化（见 ApplyPanelHeight），
    // 所以定位（PositionNearTray）也必须用它，不能用基准常量。
    int panelHeightLogical_ = 0;

    // 上一帧有没有电池。用来判断面板高度要不要跟着变。
    bool batteryPresent_ = false;

    // Top 进程表的五行控件。行数固定、每秒原地改文字，
    // 所以控件只建一次（建在 BuildProcessTable 里）。
    struct ProcLine {
        winrt::Microsoft::UI::Xaml::Controls::TextBlock name{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock mem{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock cpu{ nullptr };
    };
    std::vector<ProcLine> procLines_{};

    // 上一帧的健康分等级文字。等级色按**等级文字**选而不是按分数选 ——
    // 分数到等级的映射在 core/monitor/health.cpp 里定义了一份，
    // 这里再写一份阈值，迟早会对不上。
    std::wstring lastGrade_{};

    // 上一帧的 CPU 折线点数。**只在变化时**落一条 trace ——
    // 每秒一条会把 trace.log 淹掉，而这个值的用处恰恰是现场排障：
    // 「面板弹出来时折线是空的」到底是采样还没攒够，还是渲染坏了。
    size_t lastSparkPts_ = 0;

    // 清理屏模式。进 / 出这个模式要改窗口几何，所以得记住原来的位置。
    bool cleanMode_ = false;
    winrt::Windows::Graphics::RectInt32 savedRect_{ 0, 0, 0, 0 };
    LONG_PTR savedStyle_ = 0;

    bool visible_ = false;
    // 主窗口是否已经允许本窗口真正关闭（见 Shutdown）。
    bool allowClose_ = false;
    // 本窗口是否已经真正关闭过。Shutdown() 会被退出路径与兜底路径各调一次，
    // 第二次 Close() 会抛「窗口已经关掉了」，所以要有这面护栏。
    bool closed_ = false;
    HWND hwnd_{ nullptr };
};

}  // namespace winrt::LingwuToolbox::implementation

namespace winrt::LingwuToolbox::factory_implementation {
struct TrayPanel : TrayPanelT<TrayPanel, implementation::TrayPanel> {};
}  // namespace winrt::LingwuToolbox::factory_implementation
