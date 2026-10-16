#include "pch.h"

#include "window_decor.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace platform {
namespace {

// 这些常量在最新 SDK 里有定义，但为了在 10.0.17763 下也能编译（我们锁的是
// MinVersion），这里显式用字面量 —— 旧项目在 theme.cpp 里也踩过
// 「未导出符号只能按 ordinal 取」的坑，这里同理：不依赖 SDK 版本差异。
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwaSystemBackdropType = 38;

// DWMWINDOW_CORNER_PREFERENCE 的枚举**从 0 开始**：
//   DWMWCP_DEFAULT = 0, DWMWCP_DONOTROUND = 1, DWMWCP_ROUND = 2, DWMWCP_ROUNDSMALL = 3
// 所以这里绝不能写 TRUE（=1），那正好是「不要圆角」。
// 踩过的坑：写 TRUE 时编译正常、窗口也正常显示，只是 DwmGetWindowAttribute 读回来
// 是 1，肉眼在深色背景上几乎看不出标题栏直角，只有截图对比才发现。
constexpr int kCornerRound = 2;  // DWMWCP_ROUND

// DWMSYSTEMBACKDROP_TYPE 的枚举同样从 0 开始：
//   AUTO = 0, NONE = 1, MAINWINDOW = 2(Mica), TRANSIENTWINDOW = 3(Acrylic), TABBEDWINDOW = 4
// 注意 MAINWINDOW 的值是 2，不是 1。
constexpr int kSystemBackdropMica = 2;  // DWMSBT_MAINWINDOW

// 用 RtlGetVersion 而不是 GetVersionEx：后者在 manifest 缺 supportedOS 时会谎报 6.2。
// 也不依赖 <VersionHelpers.h> 的 IsWindows11OrGreater —— 那个宏要 NTDDI_VERSION /
// WINAPI_FAMILY 两组宏都配齐才可见，在 MinVersion=17763 的组合下并不可靠。
// 自实现一个，反而更符合「所有版本门控收敛到这一个文件」的要求。
bool IsAtLeastBuild(DWORD major, DWORD minor, DWORD build) {
    using RtlGetVersionPtr = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static const RtlGetVersionPtr s_getVersion = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<RtlGetVersionPtr>(
                           reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                     : nullptr;
    }();

    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!s_getVersion || s_getVersion(&vi) != 0) {
        return false;  // 取不到版本就当作「不支持」，走降级路径而不是冒险裸调
    }
    const DWORD v = vi.dwBuildNumber;
    if (vi.dwMajorVersion != major) {
        return vi.dwMajorVersion > major;
    }
    if (vi.dwMinorVersion != minor) {
        return vi.dwMinorVersion > minor;
    }
    return v >= build;
}

}  // namespace

bool SupportsMica() {
    // Mica 需要 Windows 11（build 22000）
    return IsAtLeastBuild(10, 0, 22000);
}

bool SupportsSystemBackdropAttribute() {
    // DWMWA_SYSTEMBACKDROP_TYPE 需要 Windows 11 22H2（build 22621）
    return IsAtLeastBuild(10, 0, 22621);
}

void ApplyWindowDecorations(HWND hwnd) {
    if (!hwnd) {
        return;
    }

    // 圆角：仅 Win11。Win10 上由 DWM 自行绘制方形标题栏，不做任何补偿 ——
    // 旧实现里那套 SetWindowRgn 圆角兜底属于要被丢弃的 UI 绕行层。
    if (SupportsMica()) {
        // 返回值故意不检查：不支持时是 E_INVALIDARG，属于「已知的降级路径」，
        // 当成错误会让干净的环境也弹错误框。
        const DWORD corner = static_cast<DWORD>(kCornerRound);
        DwmSetWindowAttribute(hwnd, kDwmwaWindowCornerPreference, &corner, sizeof(corner));
    }

    // 系统背板：仅 Win11 22H2。更低的 Win11 由 XAML 侧的 MicaBackdrop 负责，不重复设置。
    if (SupportsSystemBackdropAttribute()) {
        const int backdrop = kSystemBackdropMica;
        DwmSetWindowAttribute(hwnd, kDwmwaSystemBackdropType, &backdrop, sizeof(backdrop));
    }
}

std::wstring DescribeOsVersion() {
    // RtlGetVersion 是唯一可靠的版本取法：GetVersionEx 在 manifest 缺 supportedOS
    // 时会谎报 6.2。本项目 manifest 已声明 Win10/11，但 RtlGetVersion 没有这个坑。
    using RtlGetVersionPtr = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static const RtlGetVersionPtr s_getVersion = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<RtlGetVersionPtr>(
                           reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                     : nullptr;
    }();

    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!s_getVersion || s_getVersion(&vi) != 0) {
        return L"Windows (版本未知)";
    }

    // Windows 11 没有抬主版本号：RtlGetVersion 仍然报 10.0，只靠 build 区分。
    // 直接把 dwMajorVersion 打出来会在每一台 Win11 上都显示「Windows 10.0」。
    // 分界点与 SupportsMica() 保持一致（build 22000 = 第一个 Win11 正式版）。
    wchar_t major[8] = {};
    if (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22000) {
        wcscpy_s(major, L"11");
    } else {
        _snwprintf_s(major, _TRUNCATE, L"%lu", vi.dwMajorVersion);
    }

    return std::wstring(L"Windows ") + major + L"." +
           std::to_wstring(vi.dwMinorVersion) + L" (build " + std::to_wstring(vi.dwBuildNumber) + L")";
}

}  // namespace platform