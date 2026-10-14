// settings.cpp —— INI(key=value) 读写
//
// 路径 / 段头 / 键名是本工程的对外契约，不要改。
#include "pch.h"

#include "settings.h"

#include <string>

namespace lingsysmon {
namespace core {
namespace {

std::wstring g_iniPath;

void Trim(std::wstring& s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    while (!s.empty() && (s.back()  == L' ' || s.back()  == L'\t' ||
                          s.back()  == L'\r' || s.back()  == L'\n')) s.pop_back();
}

int ReadInt(const std::wstring& key, int def) {
    wchar_t buf[64] = { 0 };
    GetPrivateProfileStringW(L"Settings", key.c_str(), L"", buf, 64, g_iniPath.c_str());
    const std::wstring v(buf);
    if (v.empty()) return def;
    return _wtoi(v.c_str());
}

bool ReadBool(const std::wstring& key, bool def) {
    wchar_t buf[16] = { 0 };
    GetPrivateProfileStringW(L"Settings", key.c_str(), L"", buf, 16, g_iniPath.c_str());
    const std::wstring v(buf);
    if (v.empty()) return def;
    return v == L"1" || _wcsicmp(v.c_str(), L"true") == 0;
}

void WriteInt(const std::wstring& key, int v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", v);
    WritePrivateProfileStringW(L"Settings", key.c_str(), buf, g_iniPath.c_str());
}

void WriteBool(const std::wstring& key, bool v) {
    WritePrivateProfileStringW(L"Settings", key.c_str(), v ? L"1" : L"0", g_iniPath.c_str());
}

}  // namespace

std::wstring SettingsDir() {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring dir = std::wstring(buf) + L"\\LingwuSysMon";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool LoadSettings(Settings& s) {
    g_iniPath = SettingsDir() + L"\\settings.ini";

    // ---- 生效键 ----
    s.aggregate     = ReadBool(L"Aggregate",     s.aggregate);
    s.sortMode      = ReadInt (L"SortMode",      s.sortMode);
    s.intervalMs    = ReadInt (L"IntervalMs",    s.intervalMs);
    s.trayHintShown = ReadBool(L"TrayHintShown", s.trayHintShown);
    s.autoStart     = ReadBool(L"AutoStart",     s.autoStart);   // 仅参考，注册表优先
    s.closeAction   = ReadInt (L"CloseAction",   s.closeAction);

    // 合法性收敛，防止手改 ini 造成异常
    if (s.sortMode < 0 || s.sortMode > 4) s.sortMode = 0;
    if (s.intervalMs < 250 || s.intervalMs > 10000) s.intervalMs = 1000;
    // 手改成 3 或负数时退回「每次询问」：宁可多问一次，也不要给出一个
    // MainWindow 里没有对应分支的值（那会走到 default 分支静默什么都不做）。
    if (s.closeAction < 0 || s.closeAction > 2) s.closeAction = 0;
    return true;
}

void SaveSettings(const Settings& s) {
    if (g_iniPath.empty()) g_iniPath = SettingsDir() + L"\\settings.ini";

    // ---- 生效键 ----
    WriteBool(L"Aggregate",     s.aggregate);
    WriteInt (L"SortMode",      s.sortMode);
    WriteInt (L"IntervalMs",    s.intervalMs);
    WriteBool(L"TrayHintShown", s.trayHintShown);
    WriteInt (L"CloseAction",   s.closeAction);

    // AutoStart 由注册表为准，不落 ini（见 autostart.cpp 的状态自愈）
}

}  // namespace core
}  // namespace lingsysmon
