// main.cpp —— 进程入口、单实例、启动方式、托盘装配。
//
// 职责：
//   · 单实例：第二份启动不新开窗口，而是把第一份的主窗口叫出来；
//   · 启动方式：--autostart（自启拉起，静默只进托盘）/ --show / 默认；
//   · 托盘：注册 + TaskbarCreated 补注册；
//   · 退出：托盘先撤，再让 XAML 框架跑完它的清理。
//
// 【与合并前的差异】
//   · 单实例改用 platform::AcquireSingleInstance —— 它内部带 5 秒超时等待，
//     并自己做「激活已有实例」。这里不再重复实现一遍激活逻辑。
//   · --selftest 跑的是合并后的统一入口（性能监视 + 服务管理两套断言）。
#include "pch.h"

#include <shellapi.h>

#include "core/monitor/autostart.h"
#include "core/selftest.h"
#include "core/settings.h"
#include "platform/single_instance.h"
#include "platform/trace.h"
#include "platform/tray_icon.h"
#include "ui/App.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// 明确告诉 XAML 生成的入口点「不要生成 wWinMain」：
// 我们自己在 main.cpp 里提供入口。
// 这个宏只影响 build\obj\<Cfg>\src\ui\App.xaml.g.hpp 里那个自动生成的 wWinMain，
// 而本工程根本没有 include 那个文件，写上它是为了让意图不产生歧义。
#define DISABLE_XAML_GENERATED_MAIN

namespace {

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

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    LINGWU_TRACE("wWinMain enter");

    // --selftest：只跑数据层，不初始化 COM、不起 XAML、不建窗口。
    // 必须放在 init_apartment 之前，否则自检会先把整个 XAML 框架拉起来
    // （连带 60 MB 私有内存），自检想验证的「数据层可独立运行」就失去意义了。
    if (HasSwitch(L"--selftest")) {
        return lingwu::core::RunSelfTest(8);
    }

    // ---------- 旧设置迁移 ----------
    // 必须在 LoadSettings 之前做：新 ini 不存在时从两个旧工具的 ini 搬值。
    // 失败也不致命 —— 拿不到就退回默认值，程序照常能跑。
    try {
        lingwu::core::MigrateLegacySettings();
    } catch (...) {
        LINGWU_TRACE("MigrateLegacySettings threw, continuing with defaults");
    }

    // ---------- 单实例 ----------
    // 必须在任何窗口 / 托盘 / 采样启动之前。互斥量句柄要持有到进程结束，
    // 所以不能放进这个 if 的局部作用域。
    //
    // AcquireSingleInstance 内部会等旧进程退出（5 秒超时），
    // 并在「确实有另一份在跑」时自己完成激活 —— 这里不再重复激活逻辑。
    bool alreadyRunning = false;
    HANDLE instanceMutex = nullptr;
    platform::AcquireSingleInstance(instanceMutex, alreadyRunning);
    if (alreadyRunning) {
        if (instanceMutex) ::CloseHandle(instanceMutex);
        return 0;
    }

    // 桌面应用没有 CoreApplication，必须自己初始化 COM 公寓。
    // 漏掉这行的话，XAML 框架起来后所有 WinRT 调用都会失败。
    ::winrt::init_apartment(::winrt::apartment_type::single_threaded);

    // ---------- 启动方式 ----------
    // 自启拉起时静默：不 Activate 主窗口，只进托盘。
    // 具体判断在 App::OnLaunched 里做（见那里的注释：投影类型拿不到
    // implementation::App，传不了参），这里只负责落 trace 留痕。
    const lingwu::core::monitor::StartupMode mode =
        lingwu::core::monitor::ParseStartupMode();
    LINGWU_TRACE("startup mode=%d showSwitch=%d", static_cast<int>(mode),
                 HasSwitch(L"--show") ? 1 : 0);

    // unpackaged WinUI 3（C++）的运行时 bootstrap 由 NuGet 包的
    // WindowsAppRuntimeAutoInitializer.cpp 以 #pragma init_seg(lib) 静态注入，
    // 它**不接管入口**，所以 wWinMain 仍然由本文件提供。
    //
    // ── 回调里那句 make<implementation::App>() 是必须的，不能省 ──
    // XAML 编译器生成的 <IntDir>\src\ui\App.xaml.g.hpp 第 71-95 行里，
    // 官方是这么写的：
    //     int __stdcall wWinMain(...) {
    //         winrt::init_apartment(...);
    //         ::winrt::Microsoft::UI::Xaml::Application::Start([](auto&&){
    //             ::winrt::make<::winrt::LingwuToolbox::implementation::App>();
    //         });
    //     }
    // 也就是说**框架不会替你构造 Application 子类**，必须在这个回调里 make 出来。
    //
    // 漏掉它的症状极其隐蔽：Application::Start 正常返回、消息循环正常跑、
    // 进程活着、加载了 XAML 框架，但一个窗口都没有，既不崩溃也不写事件日志。
    // 更糟的是 /OPT:REF 会认为 implementation::App 这个类**没人引用**，
    // 把它的 vtable 和全部虚函数（含 OnLaunched）连同字符串字面量一起裁掉 ——
    // 链接照过，exe 里连 "OnLaunched" 四个字符都搜不到。
    //
    // 注意 App 的 runtimeclass 并不在本工程的 winmd 里（winrt\LingwuToolbox.h 里
    // 没有 App，module.g.cpp 里也没有 winrt_make_LingwuToolbox_App），
    // 所以也**不能**走激活工厂那条路，只能直接 make 实现类。
    //
    // ApplicationInitializationCallback 是**单参数**委托（签名就是
    // void(ApplicationInitializationCallbackParams)，见生成的
    // Microsoft.UI.Xaml.h 第 4842 行只传一个参）。
    // 写成两参 lambda 也能过重载解析，但第一个 auto 会被绑成 params，
    // 于是报「OnLaunched 不是 ApplicationInitializationCallbackParams 的成员」
    // ——错误信息完全指不到真正的原因上。
    //
    // 异常必须在这里兜住并落 trace：OnLaunched 是通过 COM ABI 被框架回调的，
    // 异常一旦越过 ABI 边界就没有恢复路径，Release 下表现为「进程活着但一个
    // 窗口都没有」，且不写事件日志、不输出 stdout。
    int rc = 0;
    try {
        LINGWU_TRACE("Application::Start begin");
        Application::Start([](ApplicationInitializationCallbackParams const&) noexcept {
            LINGWU_TRACE("init callback: constructing App");
            ::winrt::make<::winrt::LingwuToolbox::implementation::App>();
            LINGWU_TRACE("init callback: App constructed");
        });
        LINGWU_TRACE("Application::Start returned");
    } catch (winrt::hresult_error const& e) {
        LINGWU_TRACE("Application::Start threw hresult=0x%08X msg=%ls",
                     static_cast<unsigned>(e.code().value),
                     e.message().c_str());
        rc = static_cast<int>(e.code().value);
    } catch (std::exception const& e) {
        LINGWU_TRACE("Application::Start threw std::exception: %s", e.what());
        rc = 1;
    }

    // ---------- 退出清理 ----------
    // 托盘与共享内存的清理在 MainWindow 的 Closed 处理器里做
    // （见那里的注释：Closed 才是「窗口真的没了」的正确时刻，
    //  在这里撤会太晚，框架清理已跑完，可能留下幽灵托盘图标）。
    // 所以这里只剩互斥量要放 —— 它必须最后一个放，
    // 否则第二份实例可能在清理还没完成时就检测到「没有实例在跑」而启动起来。
    if (instanceMutex) ::CloseHandle(instanceMutex);
    return rc;
}
