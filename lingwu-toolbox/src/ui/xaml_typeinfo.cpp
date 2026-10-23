// XAML 类型表（TypeInfos）生成代码的编译单元。
//
// 为什么需要这个文件：
//   XamlTypeInfo.g.cpp 第 207 行要引用
//       &ActivateLocalType<::winrt::LingwuToolbox::implementation::MainWindow>
//   但 ::winrt::LingwuToolbox::implementation::MainWindow **不是** XAML 编译器产出的。
//   XAML 只产出 $(IntDir)src\ui\MainWindow.xaml.g.h 里的 MainWindowT<D, I...> 模板；
//   `struct MainWindow : MainWindowT<MainWindow>` 这句声明在**你自己的**
//   src\ui\MainWindow.xaml.h 里（官方模板 MainWindow.h 第 7 行同款）。
//
//   XamlTypeInfo.g.cpp 自己的 include 链是 pch.h + winrt/LingwuToolbox.h +
//   XamlTypeInfo.xaml.g.h + XamlBindingInfo.xaml.g.hpp，没有也不可能有你的头 ——
//   它是工具生成的，不知道你的工程布局。直接编译必然报
//       error C2039: "MainWindow" 不是 "winrt::LingwuToolbox::implementation" 的成员
//
// 所以这里按 C++/WinRT 官方「包装编译单元」的做法：先 include 自己的代码后置头，
// 再 include 生成文件。顺序不能反 —— 反了就退回到 App.xaml.cpp 里那行
// `make<implementation::MainWindow>()` 能编译、链接却找不到 MainWindowT 的坑。
//
// 注意：本文件**必须**是独立编译单元。不要把 XamlTypeInfo.g.cpp 改成
// ClCompile 条目直接编译，也不要把这段 include 搬进 pch.h ——
// pch.h 在 pch.cpp 里求值时 cppwinrt 还没生成 XamlTypeInfo.g.cpp，
// __has_include 会静默落空，PCH 里就永远少了类型表。
//
// 同一个翻译单元里不要再由别的 .cpp 重复 include MainWindow.xaml.g.hpp：
// 该文件没有 #pragma once，重复包含会直接重定义。
#include "pch.h"

#include "MainWindow.xaml.h"
// 设置窗口同理：XamlTypeInfo.g.cpp 会引用
//   &ActivateLocalType<::winrt::LingwuToolbox::implementation::SettingsWindow>
// 而那个 implementation::SettingsWindow 是你在这个头里声明的，
// 生成文件的 include 链里没有它。每个 XAML 页都要在这里补一次。
#include "SettingsWindow.xaml.h"
// 服务管理页与新建服务窗口同理 —— 它们也是 XAML 页，
// XamlTypeInfo.g.cpp 会引用它们的 implementation:: 类型。
#include "ServicePage.xaml.h"
#include "CreateServiceWindow.xaml.h"

// 两个模块页同理。合并成单窗口之后它们不再是顶层窗口，
// 但仍然是 XAML 页，类型表里照样有一份。
#include "MonitorPage.xaml.h"

#include "XamlTypeInfo.g.cpp"
