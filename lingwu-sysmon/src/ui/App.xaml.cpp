#include "pch.h"

#include "App.xaml.h"

#include "MainWindow.xaml.h"

#include "core/autostart.h"
#include "platform/trace.h"
#include "platform/tray_icon.h"

#include <shellapi.h>

// autostart.h 的契约全在 lingsysmon::core 命名空间下（Settings 在 core/settings.h）。
// 本文件短，直接用别名，读起来与 core/metrics.h 那边的用法一致。
namespace core = lingsysmon::core;

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::LingwuSysmon::implementation {
namespace {

// 命令行里有没有某个开关。与 main.cpp 里那个同功能的 helper 是两份实现，
// 但**不能**合并：main.cpp 那个在 init_apartment 之前就要用（--selftest 分支），
// 而这里只能碰 WinRT 已经起来之后的世界。逻辑足够短，重复一份比
// 为了复用而引入跨层依赖更划算。
bool HasSwitch(const wchar_t* name) {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) { found = true; break; }
    }
    ::LocalFree(argv);
    return found;
}

// 本次是否应当静默启动（建窗口但不 Activate，只进托盘）。
//
// 【为什么在这里判断，而不是由 main.cpp 传进来】
// main.cpp 只有 make<>() 返回的**投影**类型 winrt::LingwuSysmon::App，
// 而本工程这个 cppwinrt（3.0.260818.1）没有 get_implementation，
// 拿不到 implementation::App，投影类型上也没有 SilentStart 这种成员
// （它不在 App.xaml 的 XAML 类定义里）。要传参就得把方法加进 winmd，
// 代价是一次 MIDL 重跑 + 把内部状态暴露成公开 WinRT API。
// 而启动方式本来就是个纯命令行判断，OnLaunched 里自己读一遍最直接。
bool ShouldStartSilent() {
    const auto mode = core::ParseStartupMode();
    if (mode != core::STARTUP_AUTOSTART) return false;
    // --show 是提权重启路径（RelaunchElevated）传的，强制显示：
    // 用户刚点了「提权重启」，不弹界面等于没有任何反馈。
    if (HasSwitch(L"--show")) return false;
    return true;
}

}  // namespace

App::App() {
    LINGSYSMON_TRACE("App::App enter");
    // XAML 对象在构造期不能调 InitializeComponent()，
    // 官方注释见 cppwinrt 仓库 nuget/README 的 InitializeComponent 小节。
}

// 框架在 Application 启动完成后回调，这里是创建主窗口的地方。
void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e) {
    const bool silent = ShouldStartSilent();
    LINGSYSMON_TRACE("App::OnLaunched enter (silent=%d)", silent ? 1 : 0);

    // 整段包 try/catch：OnLaunched 是通过 COM ABI 被框架回调的，
    // 异常一旦越过 ABI 边界就没有恢复路径，Release 下表现为
    // 「进程活着但一个窗口都没有」，且不写事件日志、不输出 stdout。
    try {
        if (!window) {
            // 写成 implementation::MainWindow 是为了避免歧义：
            // 不限定的话，名字查找会先命中同名的 WinRT 投影类
            // winrt::LingwuSysmon::MainWindow，报「找不到接受它的构造函数」。
            window = make<implementation::MainWindow>();
            LINGSYSMON_TRACE("App::OnLaunched: MainWindow constructed");

            // 注意：make<>() 返回的是**投影**类型 winrt::LingwuSysmon::MainWindow，
            // 只能赋给基类 Window 或调投影上的成员。
            // 托盘装配与 HWND 发布都在 MainWindow 自己的构造函数里做
            // （那里有 implementation 指针，回调可以直接捕获 this），
            // 这里不重复接线。

            if (!silent) {
                window.Activate();
                LINGSYSMON_TRACE("App::OnLaunched: Activate done");
                // 首次运行的一次性提示：图标默认被收进 ^ 隐藏区。
                // 只在非静默启动时弹 —— 开机自启弹这个等于每次开机骚扰一次。
                platform::MaybeShowTrayHint();
            } else {
                LINGSYSMON_TRACE("App::OnLaunched: silent start, main window not activated");
            }
        }
    } catch (winrt::hresult_error const& err) {
        LINGSYSMON_TRACE("App::OnLaunched threw hresult=0x%08X msg=%ls",
                         static_cast<unsigned>(err.code().value),
                         err.message().c_str());
    } catch (std::exception const& err) {
        LINGSYSMON_TRACE("App::OnLaunched threw std::exception: %s", err.what());
    }
}

}  // namespace winrt::LingwuSysmon::implementation
