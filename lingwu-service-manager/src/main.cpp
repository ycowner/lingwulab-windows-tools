// main.cpp —— 进程入口、单实例、启动方式。
//
// 本工程与 lingwu-sysmon 的三处差异：
//   1. 没有托盘、没有开机自启，所以没有 --autostart 分支；
//   2. 单实例改成「带超时等待」（理由见 platform/single_instance.h）；
//   3. 退出段多一步 PerformPendingRelaunch() —— 它**必须**在放掉互斥量之后、
//      进程真正结束之前调用，否则提权路径上新旧实例会互撞。
#include "pch.h"

#include <shellapi.h>

#include "core/privilege.h"
#include "core/selftest.h"
#include "platform/single_instance.h"
#include "platform/trace.h"
#include "ui/App.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

// 明确告诉 XAML 生成的入口点「不要生成 wWinMain」：我们自己提供入口。
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
    LINGSVC_TRACE("wWinMain enter");

    // --selftest：只跑数据层，不初始化 COM、不起 XAML、不建窗口。
    // 必须放在 init_apartment 之前，否则自检会先把整个 XAML 框架拉起来
    // （连带数十 MB 私有内存），自检想验证的「数据层可独立运行」就失去意义了。
    if (HasSwitch(L"--selftest")) {
        return lingsvc::core::RunSelfTest(1);
    }

    // ---------- 单实例 ----------
    // 必须在任何窗口 / 后台线程启动之前。互斥量句柄要持有到进程结束。
    HANDLE instanceMutex = nullptr;
    bool alreadyRunning = false;
    platform::AcquireSingleInstance(instanceMutex, alreadyRunning);
    if (alreadyRunning) {
        // ActivateExistingInstance 已在 AcquireSingleInstance 内部完成。
        if (instanceMutex) ::CloseHandle(instanceMutex);
        return 0;
    }

    // 桌面应用没有 CoreApplication，必须自己初始化 COM 公寓。
    // 漏掉这行的话，XAML 框架起来后所有 WinRT 调用都会失败。
    ::winrt::init_apartment(::winrt::apartment_type::single_threaded);

    // ── 回调里那句 make<implementation::App>() 是必须的，不能省 ──
    //
    // XAML 编译器生成的 App.xaml.g.hpp 里，官方是这么写的：
    //     Application::Start([](auto&&){ make<implementation::App>(); });
    // 也就是说**框架不会替你构造 Application 子类**，必须在这个回调里 make 出来。
    //
    // 漏掉它的症状极其隐蔽：Application::Start 正常返回、消息循环正常跑、
    // 进程活着、加载了 XAML 框架，但一个窗口都没有，既不崩溃也不写事件日志。
    // 更糟的是 /OPT:REF 会认为 implementation::App 这个类**没人引用**，
    // 把它的 vtable 和全部虚函数（含 OnLaunched）连同字符串字面量一起裁掉 ——
    // 链接照过，exe 里连 "OnLaunched" 这几个字符都搜不到。
    //
    // ApplicationInitializationCallback 是**单参数**委托。写成两参 lambda 也能过
    // 重载解析，但第一个 auto 会被绑成 params，报「OnLaunched 不是
    // ApplicationInitializationCallbackParams 的成员」——错误信息完全指不到真正原因。
    int rc = 0;
    try {
        LINGSVC_TRACE("Application::Start begin");
        Application::Start([](ApplicationInitializationCallbackParams const&) noexcept {
            LINGSVC_TRACE("init callback: constructing App");
            ::winrt::make<::winrt::LingwuServiceManager::implementation::App>();
            LINGSVC_TRACE("init callback: App constructed");
        });
        LINGSVC_TRACE("Application::Start returned");
    } catch (winrt::hresult_error const& e) {
        LINGSVC_TRACE("Application::Start threw hresult=0x%08X msg=%ls",
                      static_cast<unsigned>(e.code().value), e.message().c_str());
        rc = static_cast<int>(e.code().value);
    } catch (std::exception const& e) {
        LINGSVC_TRACE("Application::Start threw std::exception: %s", e.what());
        rc = 1;
    }

    // ---------- 退出清理 ----------
    // 互斥量必须**最先**放掉：提权重启的新实例要靠拿到它才能起来
    // （见 platform/single_instance.h 的说明）。
    if (instanceMutex) ::CloseHandle(instanceMutex);

    // 然后才拉起提权实例。顺序反过来会让新实例永远抢不到锁 ——
    // 它会认为「还有一份在跑」，激活一个正在退出的旧窗口然后自己退出。
    lingsvc::core::PerformPendingRelaunch();

    return rc;
}
