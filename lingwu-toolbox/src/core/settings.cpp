// settings.cpp —— 统一 INI(key=value) 读写
//
// 路径 / 段头 / 键名是本工程的对外契约，不要改。
//
// 【键名为什么加模块前缀】
// 合并前两个工具各有自己的 settings.ini，键名空间互不相干：
//     sysmon 的 SortMode = 0..4  内存↓/内存↑/CPU↓/CPU↑/名称↑
//     svcmgr 的 SortMode = 0..5  名称/状态/启动类型（双向）
// 两个都叫 SortMode，语义完全无关。合进同一个 ini 后若还用同名键，
// 后写入的一方会把另一方覆盖掉。
#include "pch.h"

#include "settings.h"

#include "platform/trace.h"

#include <string>

namespace lingwu {
namespace core {
namespace {

constexpr wchar_t kSection[] = L"Settings";

// 旧工具的 ini 路径（迁移来源）。
constexpr wchar_t kLegacyMonitorDir[] = L"LingwuSysMon";
constexpr wchar_t kLegacyServiceDir[] = L"LingwuServiceManager";

std::wstring g_iniPath;

std::wstring AppDataRoot() {
    wchar_t base[MAX_PATH] = { 0 };
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::wstring();
    }
    return std::wstring(base, n);
}

void EnsureDir(const std::wstring& dir) {
    if (!dir.empty()) {
        ::CreateDirectoryW(dir.c_str(), nullptr);
    }
}

// 读一个整数键。键不存在或解析失败时**保持 out 原值不变**
// （调用方传进来的就是默认值，或者上一轮读到的值）。
//
// 【为什么用 INT_MIN 当「不存在」哨兵】
// 本工程的每个键里 0 都是合法值（sortMode=0 是「内存降序」这个默认档），
// 所以不能用「读出来是 0」来判定「键不存在」—— 那样会把合法的 0 误判成缺失，
// 于是默认值（恰好也是 0 时）掩盖了「用户真的选了 0」与「键根本没写」的区别。
//
// 【为什么返回 void，而不是「找到了吗」】
// 合并前这一层的签名是 ReadInt(key, def) -> 值，
// 调用点是 `s.sortMode = ReadInt(L"SortMode", s.sortMode);`。
// 合并时为了支持「从两个旧 ini 迁移」，签名改成了 (path, key, out&) -> 找到了吗，
// 但**调用点没跟着改** —— 于是 `s.sortMode = ReadInt(...)` 把
// 「找到了吗」这个 bool 当成了字段值：所有设置项在读回来之后
// 统统变成 0 或 1（再被下面的收敛逻辑拉回默认值）。
// 症状是「改了设置不生效」「每次启动都回到默认」，
// 而自检里的 SaveSettings → LoadSettings 往返断言会当场报 FAIL。
// 现在返回 void：没有返回值可被误用，out 参数是唯一的取值途径。
void ReadInt(const std::wstring& path, const std::wstring& key, int& out) {
    const int v = ::GetPrivateProfileIntW(kSection, key.c_str(), INT_MIN, path.c_str());
    if (v == INT_MIN) {
        return;
    }
    out = v;
}

void ReadBool(const std::wstring& path, const std::wstring& key, bool& out) {
    wchar_t buf[16] = { 0 };
    ::GetPrivateProfileStringW(kSection, key.c_str(), L"", buf, 16, path.c_str());
    const std::wstring v(buf);
    if (v.empty()) return;
    out = (v == L"1" || _wcsicmp(v.c_str(), L"true") == 0);
}

void WriteInt(const std::wstring& path, const std::wstring& key, int v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", v);
    ::WritePrivateProfileStringW(kSection, key.c_str(), buf, path.c_str());
}

void WriteBool(const std::wstring& path, const std::wstring& key, bool v) {
    ::WritePrivateProfileStringW(kSection, key.c_str(), v ? L"1" : L"0", path.c_str());
}

}  // namespace

std::wstring SettingsDir() {
    const std::wstring root = AppDataRoot();
    if (root.empty()) {
        return L".";
    }
    std::wstring dir = root + L"\\LingwuToolbox";
    EnsureDir(dir);
    return dir;
}

bool LoadSettings(Settings& s) {
    g_iniPath = SettingsDir() + L"\\settings.ini";

    // ---- 性能监视模块 ----
    // 注意：**不要**写成 `s.aggregate = ReadBool(...)` ——
    // ReadBool/ReadInt 现在返回 void，取值只能通过 out 参数拿。
    ReadBool(g_iniPath, L"MonitorAggregate", s.aggregate);
    ReadInt (g_iniPath, L"MonitorSortMode",  s.sortMode);
    ReadInt (g_iniPath, L"MonitorIntervalMs", s.intervalMs);

    // ---- 服务管理模块 ----
    ReadInt (g_iniPath, L"ServiceSortMode",  s.serviceSortMode);
    ReadInt (g_iniPath, L"ServiceFilterMode", s.serviceFilterMode);
    ReadBool(g_iniPath, L"ServiceDetailExpanded", s.serviceDetailExpanded);

    // ---- 窗口位置尺寸 ----
    ReadInt(g_iniPath, L"WindowLeft",   s.left);
    ReadInt(g_iniPath, L"WindowTop",    s.top);
    ReadInt(g_iniPath, L"WindowWidth",  s.width);
    ReadInt(g_iniPath, L"WindowHeight", s.height);

    // 尺寸明显不合理（换分辨率、分辨率被改）就丢弃，让窗口走默认布局。
    // 宁可重新排版，也不要开出一个 0 宽或者比屏幕大 5 倍的窗口。
    if (s.width <= 400 || s.height <= 300) {
        s.left = INT_MIN;
        s.top = INT_MIN;
        s.width = 0;
        s.height = 0;
    }

    // ---- 生命周期 ----
    ReadBool(g_iniPath, L"TrayHintShown", s.trayHintShown);
    ReadInt (g_iniPath, L"CloseAction",   s.closeAction);

    // 合法性收敛，防止手改 ini 造成异常
    if (s.sortMode < 0 || s.sortMode > 4) s.sortMode = 0;
    if (s.intervalMs < 250 || s.intervalMs > 10000) s.intervalMs = 1000;
    if (s.serviceSortMode < 0 || s.serviceSortMode > 5) s.serviceSortMode = 0;
    if (s.serviceFilterMode < 0 || s.serviceFilterMode > 4) s.serviceFilterMode = 0;
    // 手改成 3 或负数时退回「每次询问」：宁可多问一次，也不要给出一个
    // MainWindow 里没有对应分支的值（那会走到 default 分支静默什么都不做）。
    if (s.closeAction < 0 || s.closeAction > 2) s.closeAction = 0;

    return true;
}

void SaveSettings(const Settings& s) {
    if (g_iniPath.empty()) g_iniPath = SettingsDir() + L"\\settings.ini";

    // ---- 性能监视模块 ----
    WriteBool(g_iniPath, L"MonitorAggregate",   s.aggregate);
    WriteInt (g_iniPath, L"MonitorSortMode",    s.sortMode);
    WriteInt (g_iniPath, L"MonitorIntervalMs",  s.intervalMs);

    // ---- 服务管理模块 ----
    WriteInt (g_iniPath, L"ServiceSortMode",       s.serviceSortMode);
    WriteInt (g_iniPath, L"ServiceFilterMode",     s.serviceFilterMode);
    WriteBool(g_iniPath, L"ServiceDetailExpanded", s.serviceDetailExpanded);

    // ---- 窗口位置尺寸 ----
    WriteInt(g_iniPath, L"WindowLeft",   s.left);
    WriteInt(g_iniPath, L"WindowTop",    s.top);
    WriteInt(g_iniPath, L"WindowWidth",  s.width);
    WriteInt(g_iniPath, L"WindowHeight", s.height);

    // ---- 生命周期 ----
    WriteBool(g_iniPath, L"TrayHintShown", s.trayHintShown);
    WriteInt (g_iniPath, L"CloseAction",  s.closeAction);

    // AutoStart 由注册表为准，不落 ini（见 core/monitor/autostart.cpp 的状态自愈）
}

bool MigrateLegacySettings() {
    const std::wstring root = AppDataRoot();
    if (root.empty()) return false;

    const std::wstring newPath = SettingsDir() + L"\\settings.ini";

    // 【只在新 ini 不存在时迁移一次】
    // 用「新 ini 是否存在」当标记，是因为不额外往 ini 里写一个 Migrated 键 ——
    // 多写一个键就多一份契约，而 ini 文件的存在性本身就是最可靠的标记。
    if (::GetFileAttributesW(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    const std::wstring legacyMonitor  = root + L"\\" + kLegacyMonitorDir  + L"\\settings.ini";
    const std::wstring legacyService = root + L"\\" + kLegacyServiceDir + L"\\settings.ini";

    const bool hasMonitor =
        ::GetFileAttributesW(legacyMonitor.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool hasService =
        ::GetFileAttributesW(legacyService.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!hasMonitor && !hasService) {
        return false;
    }

    Settings s;

    // ---- 从旧 sysmon ini 搬性能监视的键 ----
    // 注意读的是**旧键名**（Aggregate / SortMode / IntervalMs），
    // 不是新键名 —— 新键名在旧文件里根本不存在，按新名去读会全部落空，
    // 迁移就变成了「把所有设置重置成默认值」。
    if (hasMonitor) {
        ReadBool(legacyMonitor, L"Aggregate",  s.aggregate);
        ReadInt (legacyMonitor, L"SortMode",   s.sortMode);
        ReadInt (legacyMonitor, L"IntervalMs", s.intervalMs);
        ReadBool(legacyMonitor, L"TrayHintShown", s.trayHintShown);
        ReadInt (legacyMonitor, L"CloseAction",  s.closeAction);
    }

    // ---- 从旧 svcmgr ini 搬服务管理的键与窗口位置 ----
    if (hasService) {
        ReadInt (legacyService, L"SortMode",       s.serviceSortMode);
        ReadInt (legacyService, L"FilterMode",     s.serviceFilterMode);
        ReadBool(legacyService, L"DetailExpanded", s.serviceDetailExpanded);
        ReadInt(legacyService, L"WindowLeft",   s.left);
        ReadInt(legacyService, L"WindowTop",    s.top);
        ReadInt(legacyService, L"WindowWidth",  s.width);
        ReadInt(legacyService, L"WindowHeight", s.height);
    }

    // 收敛：旧文件可能被手改过，值不合法时不能带进新文件。
    if (s.sortMode < 0 || s.sortMode > 4) s.sortMode = 0;
    if (s.intervalMs < 250 || s.intervalMs > 10000) s.intervalMs = 1000;
    if (s.serviceSortMode < 0 || s.serviceSortMode > 5) s.serviceSortMode = 0;
    if (s.serviceFilterMode < 0 || s.serviceFilterMode > 4) s.serviceFilterMode = 0;
    if (s.closeAction < 0 || s.closeAction > 2) s.closeAction = 0;
    if (s.width <= 400 || s.height <= 300) {
        s.left = INT_MIN;
        s.top = INT_MIN;
        s.width = 0;
        s.height = 0;
    }

    SaveSettings(s);
    LINGWU_TRACE("settings: migrated legacy ini (monitor=%d service=%d) -> %ls",
                 hasMonitor ? 1 : 0, hasService ? 1 : 0, newPath.c_str());
    // 旧文件保留不删：迁移是加法不是搬家，删了就没法回退。
    return true;
}

}  // namespace core
}  // namespace lingwu
