// pch.h —— WinUI 3 预编译头。
//
// 包含顺序有讲究：WinRT 投影头必须在 windows.h 之前，
// 否则会被 windows.h 里的宏污染。
#pragma once

// NOMINMAX / WIN32_LEAN_AND_MEAN 由 vcxproj 的 PreprocessorDefinitions 统一下发，
// 这里不要再 #define 一遍——重复定义会触发 C4005，而本工程开了 /WX 会直接编译失败。

#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

// hstring.h / unknwn.h 已经间接拉进 windows.h → WinBase.h，而 WinBase.h:95 对
// DESKTOP family 无条件执行 `#define GetCurrentTime() GetTickCount()`
// （没有 #ifndef 守卫，所以没法用预定义去抑制，只能 undef）。
//
// cppwinrt 生成的 Windows.UI.Xaml.Media.Animation / Microsoft.UI.Xaml.Media.Animation
// 投影头里有同名方法，撞上就报 C4002「类函数宏的调用参数过多」，而本工程开了 /WX
// 会直接编译失败。所以必须在包含任何 WinUI 投影头**之前**就 undef 掉。
#undef GetCurrentTime

#include <winrt/base.h>

// 只留 WinUI 3 真正用到的 Windows.* 投影。
//
// 这里刻意**不**包含 Windows.UI.Xaml.* / Windows.UI.Composition.* /
// Windows.UI.ViewManagement.* 这一族 UWP 头：
//   · WinUI 3 的 XAML 类型全在 Microsoft.UI.Xaml，两族混用还会触发
//     EnableUnsafeMixedMicrosoftWindowsUIXamlProjections 相关的校验；
//   · 投影头体积很大，实测把它们一并塞进 PCH 会让 .pch 膨胀到 GB 量级，
//     直接拖垮编译内存与耗时。需要 UWP 类型时在 .cpp 里按需单独包含。
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.Resources.Core.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>

#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
// ServiceRow 用 INotifyPropertyChanged 做原地更新通知，必须显式引入。
// 少了这一行就是 base.h 的 C2079「produce<D,INotifyPropertyChanged> 未定义」——
// 它以前是靠 x:Bind 的代码生成顺带把这一族头拖进来的，改成经典 {Binding} 之后
// 那条路断了。
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Navigation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include <windows.h>

// ── 服务控制管理器（SCM）──
//
// 【两件事都必须做，缺任一个的症状都不指向真正的原因】
//
// ① 把 WINAPI_FAMILY 钉回桌面族。
//    winsvc.h 的**全部内容**被这行包着：
//        #if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_SYSTEM)
//    Windows App SDK 会把 WINAPI_FAMILY 定成 WINAPI_FAMILY_APP（UWP 族），
//    而 WINAPI_PARTITION_DESKTOP 在 APP 族下求值为 0，于是**整个头被跳过** ——
//    表现是 ENUM_SERVICE_STATUS_PROCESS 的成员「不是它的成员」、
//    SERVICE_DELETE 未声明、QueryServiceConfigW「函数不接受 4 个参数」，
//    几十条错误全都指着 SCM，实际只是这一行。
//    本工程是**桌面**应用（unpackaged，不是 UWP），本就在桌面族里，
//    所以把宏显式钉回去是正确的，不是绕过什么限制。
//
// ② 显式包含 winsvc.h。
//    windows.h **不**包含它 —— 有意解耦：绝大多数应用不需要 SCM。
//
// 这两条同时写进方案与 AGENTS.md：它们是「编译输出完全指不到真正原因」的典型。
#if !defined(WINAPI_FAMILY) || WINAPI_FAMILY == WINAPI_FAMILY_APP
#undef WINAPI_FAMILY
#define WINAPI_FAMILY WINAPI_FAMILY_DESKTOP_APP
#endif
#include <winsvc.h>


// XAML 代码生成桥接。
//
// XAML 生成器产出的 XamlTypeInfo.g.cpp / XamlTypeInfo.Impl.g.cpp 会 include
// 各页面的 MainWindow.xaml.g.h，而后者里的 MainWindowT 继承自
// winrt::LingwuServiceManager::implementation::MainWindow_base —— 这个类型定义在
// cppwinrt 生成的 MainWindow.g.h 里，但**生成器不会自己去 include 它**，
// 直接编译就是 C2039「MainWindow_base 不是 implementation 的成员」。
//
// 这里借 pch.h 补上：上述生成文件自己第 9 行就 #include "pch.h"，
// 且排在其它 include 之前，既满足 C++/WinRT 要求的「unknwn.h 必须先于
// C++/WinRT 头」，又让组件头在页面 .g.h 之前可见。
//
// __has_include 在这里可靠：pch.cpp 在 BuildGenerateSources 之后才编译
// （见 vcxproj 的 BuildSteps），此时 cppwinrt 已生成 MainWindow.g.h。
// 新增 runtimeclass 时不需要改这里 —— 每个组件的 .g.h 由各自的代码后置头引入。
#if __has_include("MainWindow.g.h")
#include "MainWindow.g.h"
#endif

// 版本门控不在这里：<VersionHelpers.h> 的 IsWindows11OrGreater 依赖 NTDDI_VERSION 与
// WINAPI_FAMILY 两组宏，在 WindowsTargetPlatformMinVersion=10.0.17763 的组合下不可靠。
// 已统一到 platform/window_decor.h，用 RtlGetVersion 自实现。
