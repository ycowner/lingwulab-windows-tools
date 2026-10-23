#pragma once

// 按官方 C++/WinRT 模板的写法，这里只引 cppwinrt 生成的 SettingsWindow.g.h。
// 它内部用 __has_include("SettingsWindow.xaml.g.h") 决定要不要把 XAML 代码后置头
// 带进来；一旦落空就退化成没有代码生成的纯壳，症状是 InitializeComponent() /
// RootGrid() 之类全部不存在，报一堆看起来不相干的编译错误。
// vcxproj 里为此把 $(IntDir)src\ui 加进了 AdditionalIncludeDirectories。
#include "SettingsWindow.g.h"

#include "pch.h"

#include "core/settings.h"
#include "platform/window_decor.h"

#include <string>

namespace winrt::LingwuToolbox::implementation {

struct SettingsWindow : SettingsWindowT<SettingsWindow> {
    SettingsWindow();

    // ---- 与主窗口的契约 ----
    // 主窗口在 Activate() 之前调用，把当前生效的值灌进来，
    // 这样设置窗口一打开就是真实状态，而不是结构体默认值。
    // 调用时机在构造函数之后，InitializeComponent 已跑完，控件都真实存在。
    void LoadFrom(bool aggregate, int sortMode, int intervalMs, bool autoStart);

    // 用户是否按了「确定」。只有 true 时主窗口才需要回读并应用。
    bool Accepted() const { return accepted_; }

    // 允许本窗口**真正关闭**（主窗口退出时调用）。
    //
    // 见 SettingsWindow.idl 里那段说明：设置窗口平时只隐藏不关闭
    // （因为关闭后的 WinUI 3 Window 不可复用，而每次新建会线性泄漏原生资源），
    // 但退出时必须让它真关闭 —— 只要还有 Window 活着，XAML 框架的消息循环
    // 就不会结束，进程会僵留在「窗口全没了但进程还在」的状态。
    // 而 AppWindow::Destroy() 不管用：它只销毁原生 HWND，框架的窗口计数
    // 不会归零，消息循环照样不结束。
    void AllowClose();

    // 关闭时由主窗口读走。aggregate / sortMode / intervalMs 是 ini 口径，
    // autoStart 反映用户的选择（注册表已在 OnOk 里写好并回读校验过）。
    bool     ResultAggregate() const { return resAggregate_; }
    int      ResultSortMode()  const { return resSortMode_; }
    int      ResultIntervalMs() const { return resIntervalMs_; }
    bool     ResultAutoStart() const { return resAutoStart_; }

private:
    void ApplyBackdrop();
    void WireEvents();
    void OnOk();
    void OnCancel();
    void ApplyAutoStart(bool wantOn);

    // 隐藏自己（而不是 Close）。
    //
    // 【为什么关闭时不销毁窗口】
    // WinUI 3 的 Window 在 Close() 之后**不可复用**（再调 Activate() 会抛
    // hresult_error），而「每次打开都新建一个窗口」实测会**线性泄漏**原生
    // 资源（每轮约 36 个句柄 + 1.8 MB 私有内存，无上界）。
    // 所以改成「构造一次 + 隐藏/显示」：关闭时只 Hide，窗口对象一直活着，
    // 每轮零分配。真正关闭交给主窗口退出时的 AllowClose() + Window::Close()。
    // 详细数据与复现见 SettingsWindow.xaml.cpp 构造函数里的长注释。
    void HideSelf();

    // 三个候选档位与 ComboBox 下标的对应关系。排序档直接用 core::SORT_* 常量，
    // 只有 0/1/2 三个值在 UI 上出现（内存降序 / CPU 降序 / 名称升序），
    // 而 core 层有五个排序档（另两个是升序档，由点表头循环切出来，不进设置）。
    static int SortModeFromIndex(int idx);
    static int IndexFromSortMode(int mode);
    static int IntervalMsFromIndex(int idx);
    static int IndexFromIntervalMs(int ms);

    bool accepted_ = false;

    // 主窗口是否已经允许本窗口真正关闭（见 AllowClose）。
    bool allowClose_ = false;

    bool resAggregate_  = true;
    int  resSortMode_   = 0;
    int  resIntervalMs_ = 1000;
    bool resAutoStart_  = false;

    // 「确定」时自启注册表写入失败的原因。UI 上把它显示出来并**阻止关闭**，
    // 否则用户会以为开关生效了，下次重启发现没自启更难排查。
    std::wstring autoStartError_;
};

}  // namespace winrt::LingwuToolbox::implementation

namespace winrt::LingwuToolbox::factory_implementation {
struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};
}  // namespace winrt::LingwuToolbox::factory_implementation
