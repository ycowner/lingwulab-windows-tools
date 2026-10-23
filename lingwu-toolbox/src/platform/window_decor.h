#pragma once

#include <Windows.h>

// Win10 / Win11 窗口装饰的统一入口。
//
// 【为什么所有 DWM 调用都必须走这里】
// 编译目标是最新 SDK（10.0.26100），最低支持 OS 却是 Win10 1809（17763）。
// DWMWA_WINDOW_CORNER_PREFERENCE 需要 build 22000（Win11），
// DWMWA_SYSTEMBACKDROP_TYPE 需要 build 22621（Win11 22H2）——
// 在 Win10 上直接调，DwmSetWindowAttribute 返回 E_INVALIDARG 或干脆失败，
// 但**不会**告诉你「你调错了」，只会静默地什么都不发生。
// 所以这两个属性一律在此按版本门控，门控不过就静默跳过，不视为错误。
//
// 相关：backdrop 类（Mica/Acrylic）不在这里选，因为它是 XAML 对象，
// 见 ui/MainWindow.xaml.cpp 的 ApplyBackdrop()。
namespace platform {

// 返回 true 表示当前系统支持 Mica（Win11 build >= 22000）。
bool SupportsMica();

// 返回 true 表示当前系统支持 DWM 系统背板属性（Win11 22H2，build >= 22621）。
bool SupportsSystemBackdropAttribute();

// 对给定窗口应用圆角与系统背板；不支持的项静默跳过。
// 重复调用是安全的（幂等）。
void ApplyWindowDecorations(HWND hwnd);

// 供日志/诊断显示的版本描述，例如 "Windows 11 (build 26300)"。
std::wstring DescribeOsVersion();

}  // namespace platform