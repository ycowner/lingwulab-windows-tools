#include "pch.h"

#include "App.xaml.h"

#include "MainWindow.xaml.h"
#include "platform/trace.h"

// 【为什么这里必须有这两行 using】
// OnLaunched 的形参类型 LaunchActivatedEventArgs 在 Microsoft.UI.Xaml 命名空间里。
// 少了 using 时，`[[maybe_unused]] LaunchActivatedEventArgs const& e` 这一行会被
// 编译器当成声明，于是报 C2761「不允许重新声明成」+ C2059「attribute specifier」——
// 错误信息完全指不到「少了个 using」。
using namespace winrt;
using namespace Microsoft::UI::Xaml;

// 整段包 try/catch：OnLaunched 是通过 COM ABI 被框架回调的，
// 异常一旦越过 ABI 边界就没有恢复路径，Release 下表现为
// 「进程活着但一个窗口都没有」，且不写事件日志、不输出 stdout。
namespace winrt::LingwuServiceManager::implementation {

App::App() {
    LINGSVC_TRACE("App::App enter");
    // XAML 对象在构造期不能调 InitializeComponent()。
}

void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e) {
    LINGSVC_TRACE("App::OnLaunched enter");
    try {
        if (!window) {
            // 写成 implementation::MainWindow 是为了避免歧义：
            // 不限定的话，名字查找会先命中同名的 WinRT 投影类
            // winrt::LingwuServiceManager::MainWindow，报「找不到接受它的构造函数」。
            window = make<implementation::MainWindow>();
            LINGSVC_TRACE("App::OnLaunched: MainWindow constructed");

            // make<>() 返回的是**投影**类型 winrt::LingwuServiceManager::MainWindow，
            // 只能赋给基类 Window 或调投影上的成员。
            // HWND 发布、窗口装饰、后台线程都在 MainWindow 自己的构造函数里做
            // （那里有 implementation 指针，回调可以直接捕获 this），这里不重复接线。

            window.Activate();
            LINGSVC_TRACE("App::OnLaunched: Activate done");
        }
    } catch (winrt::hresult_error const& err) {
        LINGSVC_TRACE("App::OnLaunched threw hresult=0x%08X msg=%ls",
                      static_cast<unsigned>(err.code().value), err.message().c_str());
    } catch (std::exception const& err) {
        LINGSVC_TRACE("App::OnLaunched threw std::exception: %s", err.what());
    }
}

}  // namespace winrt::LingwuServiceManager::implementation
