#include "pch.h"

#include "core/privilege.h"

#include <shellapi.h>

#include "platform/trace.h"

namespace lingsvc {
namespace core {
namespace {

// 待执行的提权重启。进程内只有一个，静态局部足够。
struct Pending {
    bool requested = false;
    std::wstring args;
};
Pending& PendingRef() {
    static Pending p;
    return p;
}

}  // namespace

bool IsElevated() {
    HANDLE token = nullptr;
    // 只请求 TOKEN_QUERY。
    //
    // 【为什么不用 TOKEN_QUERY_LIMITED_INFORMATION】本机 SDK 的 um\ 头下
    // **根本没有定义**这个宏（实测 grep 零命中），写了就是 C2065。
    // 而且普通权限运行的进程本来就拿得到 TOKEN_QUERY —— 降级令牌也是完整令牌，
    // 只是默认 DACL 被改小，而读 TokenElevation 不需要额外权限。
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elev{};
    DWORD returned = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elev, sizeof(elev),
                                          &returned);
    ::CloseHandle(token);
    if (!ok) {
        LINGSVC_TRACE("GetTokenInformation(TokenElevation) failed err=%lu",
                      static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    return elev.TokenIsElevated != 0;
}

void RequestRelaunchElevated(std::wstring const& extraArgs) {
    auto& p = PendingRef();
    p.requested = true;
    p.args = extraArgs;
    LINGSVC_TRACE("elevated relaunch requested, args=\"%ls\"", p.args.c_str());
}

bool HasPendingRelaunch() {
    return PendingRef().requested;
}

void PerformPendingRelaunch() {
    auto& p = PendingRef();
    if (!p.requested) {
        return;
    }
    // 只做一次，防止退出路径被走两遍时弹两个 UAC。
    p.requested = false;

    wchar_t exe[MAX_PATH] = { 0 };
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        ::MessageBoxW(nullptr, L"无法确定本程序路径，提权重启已取消。", L"需要管理员权限",
                      MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return;
    }

    std::wstring args = L"--show";
    if (!p.args.empty()) {
        args += L" ";
        args += p.args;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    // 不设 SEE_MASK_FLAG_NO_UI：用户拒绝 UAC 时需要拿到失败返回值来弹提示，
    // 而那个返回值在有 FLAG_NO_UI 的组合下不够可靠。
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&sei)) {
        const DWORD e = ::GetLastError();
        LINGSVC_TRACE("ShellExecuteExW(runas) failed err=%lu",
                      static_cast<unsigned long>(e));
        ::MessageBoxW(nullptr, L"提权启动被取消或被系统拒绝。", L"需要管理员权限",
                      MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return;
    }
    if (sei.hProcess) {
        ::CloseHandle(sei.hProcess);
    }
    LINGSVC_TRACE("elevated instance launched, args=\"%ls\"", args.c_str());
}

}  // namespace core
}  // namespace lingsvc
