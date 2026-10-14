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

namespace winrt::LingwuSysmon::implementation {

struct SettingsWindow : SettingsWindowT<SettingsWindow> {
    SettingsWindow();

    // ---- 与主窗口的契约 ----
    // 主窗口在 Activate() 之前调用，把当前生效的值灌进来，
    // 这样设置窗口一打开就是真实状态，而不是结构体默认值。
    // 调用时机在构造函数之后，InitializeComponent 已跑完，控件都真实存在。
    void LoadFrom(bool aggregate, int sortMode, int intervalMs, bool autoStart);

    // 用户是否按了「确定」。只有 true 时主窗口才需要回读并应用。
    bool Accepted() const { return accepted_; }

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

    // 三个候选档位与 ComboBox 下标的对应关系。排序档直接用 core::SORT_* 常量，
    // 只有 0/1/2 三个值在 UI 上出现（内存降序 / CPU 降序 / 名称升序），
    // 而 core 层有五个排序档（另两个是升序档，由点表头循环切出来，不进设置）。
    static int SortModeFromIndex(int idx);
    static int IndexFromSortMode(int mode);
    static int IntervalMsFromIndex(int idx);
    static int IndexFromIntervalMs(int ms);

    bool accepted_ = false;

    bool resAggregate_  = true;
    int  resSortMode_   = 0;
    int  resIntervalMs_ = 1000;
    bool resAutoStart_  = false;

    // 「确定」时自启注册表写入失败的原因。UI 上把它显示出来并**阻止关闭**，
    // 否则用户会以为开关生效了，下次重启发现没自启更难排查。
    std::wstring autoStartError_;
};

}  // namespace winrt::LingwuSysmon::implementation

namespace winrt::LingwuSysmon::factory_implementation {
struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};
}  // namespace winrt::LingwuSysmon::factory_implementation
