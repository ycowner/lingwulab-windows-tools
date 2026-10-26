#pragma once

// 按官方 C++/WinRT 模板的写法，这里只引 cppwinrt 生成的 MainWindow.g.h。
//
// 它内部有一段
//     #if defined(WINRT_FORCE_INCLUDE_MAINWINDOW_XAML_G_H) || __has_include("MainWindow.xaml.g.h")
//         #include "MainWindow.xaml.g.h"
//     #else
//         ... MainWindowT = MainWindow_base   // 退化成没有 XAML 代码生成的纯壳
//     #endif
// 也就是说 XAML 代码后置头是被自动带进来的；一旦 __has_include 落空，
// 编译能过但 InitializeComponent() 之类全都不存在，症状是莫名其妙的
// 编译错误。vcxproj 里为此把 $(IntDir)src\ui 加进了 AdditionalIncludeDirectories。
#include "MainWindow.g.h"

#include "pch.h"

// 两个模块页面。外壳持有的是**实现类型**而不是投影类型 ——
// 这样可以直接调它们的任意 C++ 方法，不必把内部状态暴露成公开 WinRT API
// （那要改 .idl、触发 MIDL 重跑，而且每加一个交互点就要重跑一次）。
#include "MonitorPage.xaml.h"
#include "ServicePage.xaml.h"
// 托盘指标面板。它也是一个 XAML Window，但**按值持有**（不是指针）——
// 与两个页面同款：cppwinrt 的实现对象靠引用计数活着，值成员的析构
// 会正确释放引用；指针方案要么裸 new（析构语义不对），
// 要么包一层 smart_ptr（而 shared_ptr 的 delete 对 WinRT 实现对象
// 并不是推荐路径）。
#include "TrayPanel.xaml.h"

// 跨模块共享的类型：ContentDialog 独占闸门、窗口子类用的菜单中转结构。
#include "shell_types.h"

#include "core/settings.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace winrt::LingwuToolbox::implementation {

// 工具箱外壳窗口。整个程序唯一的顶层窗口。
//
// 【为什么它必须存在，而不是让两个模块各自当窗口】
// 合并前：两个工具是两个进程、两个 exe、两条发布线。
// 合并后：共用一份 exe、一条发布线、一个托盘图标、一个设置文件。
// 于是「窗口」这件事必须收敛成一个 —— 否则会出现两个托盘图标、
// 两个单实例锁、两份窗口位置设置，而用户以为是一个程序。
//
// 【它与页面的分工】
//   外壳：窗口几何、标题栏、导航、托盘、单实例、关闭流程、对话框闸门、
//         统一设置实例。
//   页面：各自模块的全部业务逻辑与 UI。
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();

private:
    // ---- 导航 ----
    // 当前显示的模块。
    //
    // 六个页签在导航模型里一次定死：清理 / 应用 / 优化 / 分析 / 状态 / 服务。
    // 其中前四个本轮尚未实现，是「规划中」的禁用态占位 ——
    // 枚举里先占好位置，将来实现了直接接上，不用改导航结构。
    //
    // 取值与 settings.ini 的 LastPage 键一一对应（0..5），
    // 所以 Settings::lastPage 的默认值 4 就是「状态」。
    //
    // 【这个枚举必须排在 EnsurePage 前面】
    // EnsurePage 的形参用的是它 —— C++ 里类型要先声明后使用。
    enum class Module { Clean = 0, Apps = 1, Optimize = 2, Analyze = 3,
                       Status = 4, Service = 5 };

    // ---- 启动装配 ----
    void ApplyBackdrop();
    void WireEvents();
    // 确保某个页签对应的页面已经建好并挂进 PageHost。
    //
    // 【为什么是惰性构造】
    // 六个页签在导航模型里一次定死，但页面本体**首次进入时才建**：
    // 服务页要建一整套服务列表（拉 SCM、读注册表），用户这一轮根本
    // 没打开过的话，那份内存与初始化开销都是白付。
    // 页面一旦建好就**永不重建** —— 性能监视页的列表遵守
    // 「ItemsSource 全生命周期只赋值一次」的铁律，重建页面等于重新赋一次。
    void EnsurePage(Module m);
    // 装配托盘图标。构造函数末尾调一次。
    void InitTray();

    // 切换模块。只改页面的 Visibility，不重建任何东西。
    void ShowModule(Module m);
    Module currentModule_ = Module::Status;

    // 页签主体是否已经建好（惰性构造的产物）。
    // 所有对页面的调用都要先看这两个标志 —— 未建好的页面里
    // settings_ 是 nullptr，ServicePage::Shutdown() 会拿它去落盘，直接崩。
    bool monitorPageReady_ = false;
    bool servicePageReady_ = false;

    // 导航页签的选中态。
    //
    // 【为什么颜色要现算】
    // 选中态的填充色是**该页自己的主色**（决策轴 A：每页独立主色），
    // 而主色是 StaticResource —— 只在 XAML 解析期生效。
    // C++ 侧要按当前页签挑出对应的那一个，所以 XAML 里放了六个 0×0 的
    // 隐藏探针把画刷解析出来，这里从它们身上读。
    //
    // 至于「未选中」态的文字色：不另取主题画刷，而是把 Button 构造完
    // 之后的默认 Foreground 存下来 —— 那本来就是主题给正文用的那个颜色。
    void UpdateNavTabs();

    // ---- 生命周期 ----
    // 把窗口显示出来并拿到前台。托盘左键、菜单「显示主界面」、
    // 以及第二份实例的激活消息都走这里。
    void ShowAndActivate();
    // 托盘左键：弹出 / 收起托盘指标面板。
    //
    // 语义是**切换**（面板已显示时再点一次托盘就收起它），
    // 而不是「每次都弹出来」。防双击由托盘模块那侧做（500 ms 防抖）。
    void ToggleTrayPanel();
    // 真隐藏到托盘：任务栏不留按钮，只剩托盘一个入口。
    void HideToTray();
    // 真正退出。只由托盘菜单「退出」与关闭确认框的「退出程序」调，
    // 置 quitting_ 再发 WM_CLOSE。
    void RequestExit();
    // 关闭请求的落点：按 settings_.closeAction 分流到隐藏 / 退出 / 弹确认框。
    // 由 AppWindow.Closing 处理器在 a.Cancel(true) 之后调用。
    void OnCloseRequested();
    // 摆文案并弹出关闭确认框。
    void ShowClosePrompt();
    // 记下本次选择（用户勾了「记住我的选择」才记），然后执行。
    // action 取下面的三个常量。
    void ApplyCloseChoice(int action);

    // 关闭动作取值，与 settings.ini 的 CloseAction 键一一对应。
    //
    // 【定义在 core/settings.h，这里只是别名】
    // 这三态被三处使用：设置窗口的 UI 入口（写）、这里的关闭拦截（读）、
    // ini 读写（存）。以前定义只在本头里（private），设置窗口只能照抄字面量
    // 0/1/2 —— 改一处漏一处的后果很隐蔽：用户选了「总是退出」，设置窗口却显示
    // 「总是隐藏到托盘」，而实际行为是退出。设置层是唯一事实源。
    //
    // **必须与 settings.cpp 里 ReadCloseAction 的钳位范围一致**：
    // 手改 ini 写出越界值时，LoadSettings 已经把它收敛回 0。
    static constexpr int kCloseAsk            = lingwu::core::kCloseAsk;
    static constexpr int kCloseMinimizeToTray = lingwu::core::kCloseMinimizeToTray;
    static constexpr int kCloseExitProgram    = lingwu::core::kCloseExitProgram;

    // ---- 托盘 / 单实例 --------------------------------------------------
    // 全部由托盘模块的回调或窗口子类调用，所以都是 private。
    // 【为什么不是 public】
    // 托盘模块的回调在本类的构造函数里挂，直接捕获 this，不需要外部持有
    // implementation 指针。而外部（App.xaml.cpp）只有 make<>() 返回的**投影**
    // 类型 winrt::LingwuToolbox::MainWindow，本版 cppwinrt 又没有
    // get_implementation，投影上也看不到这些成员 —— 想从外面调就得把它们
    // 加进 MainWindow.idl，代价是一次 MIDL 重跑加暴露公开 WinRT API。
    // 让本类自己挂回调，两个问题一起消失。

    // 「按应用聚合」的当前值 / 设置它。
    //
    // 【2026-10-23 起这里不再有转发】
    // 这两个转发原本只服务于托盘菜单里的「按应用聚合」勾选项。
    // 新契约把那一项下沉到性能监视页的 ToggleSwitch（避免状态双写），
    // 于是外壳这两个转发成了死代码，删掉。
    // 聚合开关的实际状态住在性能监视页里（monitorPage_.AggregateOn()）。

    // true 表示「这一次关闭是真的要退出」，由 RequestExit() 置位。
    // 调用方有两个：托盘菜单的「退出」、以及关闭确认框的「退出程序」。
    // 置位后 AppWindow.Closing 处理器放行，窗口才会真的关、Closed 才会跑。
    bool quitting_ = false;

    // 统一的设置实例。两个模块共用一份 ini，所以也只能有一份内存表示 ——
    // 否则 A 模块保存时会用旧副本覆盖 B 模块刚写的键
    // （症状：「在服务页改了排序，切回性能页点一下，服务页的偏好被重置」）。
    lingwu::core::Settings settings_{};

    // 跨模块对话框闸门。合并成单窗口后，同一个 XamlRoot 上只能挂一个
    // ContentDialog，这条约束从「各窗口各管各的」升级成「全局唯一」，
    // 所以闸门由外壳创建、两个页面共用同一把。
    std::shared_ptr<DialogGate> gate_{ nullptr };

    // 窗口子类（WM_CONTEXTMENU）用的中转结构。hitTest 由外壳在切换模块时
    // 改指向当前活动页面。
    std::shared_ptr<MenuHost> menuHost_{ nullptr };

    // 两个模块页面。常驻，只切 Visibility。
    //
    // 按**值**持有（不是指针）：cppwinrt 的实现对象靠引用计数活着，
    // 值成员的析构会正确释放引用；而指针方案要么得裸 new（析构语义不对），
    // 要么得包一层 smart_ptr（而 std::shared_ptr 的 delete 对 WinRT
    // 实现对象并不是推荐路径）。
    //
    // 页面的构造函数是空的，真正的初始化在 Initialize() 里 ——
    // 成员构造早于外壳自己的构造函数体，顺序必须由外壳掌控。
    MonitorPage monitorPage_{};
    ServicePage servicePage_{};

    // 托盘指标面板。托盘左键弹出的那个 360×530 的小窗。
    //
    // 【为什么它也是按值持有、且和两个页面一样是成员】
    // 它必须是一个**独立顶层窗口**（WinUI 3 的 Window 没有 Visibility，
    // 主窗口里的 Page 没法脱离主窗口单独显示），而只要还有一个 Window
    // 活着，XAML 框架的消息循环就不会结束 —— 所以退出序列必须连带关掉它。
    //
    // 【为什么构造一次就常驻，而不是每次点托盘都新建】
    // WinUI 3 的 Window 关闭后不可复用；「关掉就 new 一个」实测会线性泄漏
    // 原生资源（每轮约 36 个句柄 + 1.8 MB 私有内存，无上界）。
    // 所以是「构造一次 + 隐藏/显示」，每轮零分配。
    TrayPanel trayPanel_{};

    // 关闭确认框。**构造函数里只建一次**，之后永不重建。
    //
    // 为什么不能每次 ShowClosePrompt 现建一个 StackPanel 再 Append：
    // closeRemember_ 是长期存活的成员，而 XAML 元素在同一时刻只能有一个父节点。
    // 第二次弹出时再把它 Append 进一个新面板，等于把它从上一轮的面板上摘下来
    // 重新挂 —— 这一步会抛异常。而异常从 AppWindow.Closing 处理器里逃出去之后，
    // 上面那句 a.Cancel(true) 会被作废：窗口照常关闭、进程直接退出。
    // 症状是「第一次点 X 正常，第二次点 X 程序就没了」，且 trace 里什么线索都没有。
    // 面板只建一次就彻底绕开了这个再挂路径。
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog closeExit_{ nullptr };
    // 「记住我的选择，以后不再询问」。必须是成员而不是局部变量：
    // ShowClosePrompt 只负责把它摆进 closeExit_.Content，而勾选状态要在
    // Primary/SecondaryButtonClick 处理器里回读 —— 那两个处理器是常驻的，
    // 捕获局部变量会悬垂。CheckBox 同理必须在构造函数里真正构造，
    // 空对象上挂不进 Content（见 MainWindow.xaml.cpp 构造函数里的注释）。
    winrt::Microsoft::UI::Xaml::Controls::CheckBox closeRemember_{ nullptr };
    winrt::Microsoft::UI::Xaml::Controls::StackPanel closePromptPanel_{ nullptr };

    // 导航药丸的默认（未选中）文字色。构造完 Button 之后立刻记下来，
    // 之后切换模块时用它把未选中的那颗药丸还原回普通文字色。
    winrt::Microsoft::UI::Xaml::Media::Brush navIdleForeground_{ nullptr };

    HWND hwnd_{ nullptr };
};

}  // namespace winrt::LingwuToolbox::implementation

namespace winrt::LingwuToolbox::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}  // namespace winrt::LingwuToolbox::factory_implementation
