#pragma once

// 注意这里是 App.xaml.g.h（XAML 代码生成器产出的那个），不是 cppwinrt 的 App.g.h。
// ApplicationDefinition（App.xaml）不需要配 .idl —— 官方模板里也只有
// MainWindow.idl，App 的 runtimeclass 由 XAML 编译器生成。
// 所以也没有 factory_implementation::App，工厂是 cppwinrt 通过
// winrt_make_LingwuServiceManager_App() 提供的。
#include "App.xaml.g.h"

#include "pch.h"

namespace winrt::LingwuServiceManager::implementation {

// 应用对象。WinUI 3 框架在 Application::Start 时自动构造它，
// 之后所有生命周期回调都从它身上发起。
struct App : AppT<App> {
    App();

    // IApplicationOverrides：覆盖默认空实现，主窗口在这里创建。
    void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const& e);

private:
    // 必须持有强引用：XAML 框架不延长我们的生命周期，
    // 局部变量一旦析构，窗口就跟着没了。
    winrt::Microsoft::UI::Xaml::Window window{ nullptr };
};

}  // namespace winrt::LingwuServiceManager::implementation
