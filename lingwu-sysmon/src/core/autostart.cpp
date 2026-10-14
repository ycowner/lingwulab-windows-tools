// autostart.cpp —— HKCU Run 读写
#include "pch.h"

#include "autostart.h"

#include <shellapi.h>
#include <string>

namespace lingsysmon {
namespace core {
namespace {

const wchar_t* kRunKey  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunName = L"LingwuSysmon";

LSTATUS OpenRunKey(bool forWrite, HKEY& hk) {
    if (forWrite) {
        return RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr,
                               REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hk, nullptr);
    }
    return RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &hk);
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = { 0 };
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

// 从 "…\foo.exe" --autostart 中取出 exe 路径；没有引号则取到首个空格为止
std::wstring ExtractExeFromCommand(const std::wstring& cmd) {
    std::wstring s = cmd;
    while (!s.empty() && (s.front() == L' ' || s.front() == L'"')) s.erase(s.begin());
    const size_t q = s.find(L'"');
    if (q != std::wstring::npos) return s.substr(0, q);
    const size_t sp = s.find(L' ');
    if (sp != std::wstring::npos) s = s.substr(0, sp);
    return s;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

}  // namespace

StartupMode ParseStartupMode() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return STARTUP_NORMAL;
    StartupMode mode = STARTUP_NORMAL;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--autostart") == 0) mode = STARTUP_AUTOSTART;
        else if (_wcsicmp(argv[i], L"--show") == 0)     mode = STARTUP_SHOW;
    }
    LocalFree(argv);
    return mode;
}

std::wstring AutostartRegisteredCommand() {
    HKEY hk = nullptr;
    if (OpenRunKey(false, hk) != ERROR_SUCCESS) return L"";
    wchar_t buf[1024] = { 0 };
    DWORD type = 0, cb = sizeof(buf);
    std::wstring out;
    if (RegQueryValueExW(hk, kRunName, nullptr, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        out = buf;
    }
    RegCloseKey(hk);
    // 去掉尾部空白
    while (!out.empty() && (out.back() == L' ' || out.back() == L'\r' || out.back() == L'\n'))
        out.pop_back();
    return out;
}

std::wstring AutostartCommandForSelf() {
    return L"\"" + ExePath() + L"\" --autostart";
}

bool AutostartIsEnabled() {
    return !AutostartRegisteredCommand().empty();
}

bool AutostartEnable(std::wstring& err) {
    err.clear();
    HKEY hk = nullptr;
    LSTATUS st = OpenRunKey(true, hk);
    if (st != ERROR_SUCCESS) {
        err = L"无法打开注册表启动项（错误 " + std::to_wstring(st) +
              L"）。可能被安全软件或组策略阻止。";
        return false;
    }
    const std::wstring cmd = AutostartCommandForSelf();
    st = RegSetValueExW(hk, kRunName, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(cmd.c_str()),
                        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    if (st != ERROR_SUCCESS) {
        err = L"写入注册表启动项失败（错误 " + std::to_wstring(st) + L"）。";
        return false;
    }
    // 回读校验，避免「界面显示已开启、实际没写进去」
    const std::wstring back = AutostartRegisteredCommand();
    if (!SamePath(ExtractExeFromCommand(back), ExePath())) {
        err = L"自启动项写入后校验不一致，请检查是否被安全软件拦截。";
        return false;
    }
    return true;
}

bool AutostartDisable(std::wstring& err) {
    err.clear();
    HKEY hk = nullptr;
    LSTATUS st = OpenRunKey(true, hk);
    if (st != ERROR_SUCCESS) {
        err = L"无法打开注册表启动项（错误 " + std::to_wstring(st) + L"）。";
        return false;
    }
    st = RegDeleteValueW(hk, kRunName);
    RegCloseKey(hk);
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
        if (st == ERROR_ACCESS_DENIED)
            err = L"删除失败：可能被组策略或安全软件锁定，请手动清理启动项。";
        else
            err = L"删除自启动项失败（错误 " + std::to_wstring(st) + L"）。";
        return false;
    }
    return true;
}

void AutostartReconcile(bool& enabled, bool& stale) {
    const std::wstring cmd = AutostartRegisteredCommand();
    enabled = !cmd.empty();
    stale = enabled && !SamePath(ExtractExeFromCommand(cmd), ExePath());
}

}  // namespace core
}  // namespace lingsysmon
