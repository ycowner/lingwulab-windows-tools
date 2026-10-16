#include "pch.h"

#include "core/settings.h"

#include "platform/trace.h"

namespace lingsvc {
namespace core {
namespace {

constexpr wchar_t kSection[] = L"Settings";

std::wstring AppDataDir() {
    wchar_t base[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::wstring();
    }
    std::wstring dir(base, n);
    dir += L"\\LingwuServiceManager";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// 读一个整数键。键不存在或解析失败时返回 false（调用方保留默认值）。
// 解析失败要能区分「没有」和「坏了」：后者说明 ini 被手改过，
// 宁可退回默认值也不要拿半个值去用。
bool ReadInt(std::wstring const& path, wchar_t const* key, int& out) {
    DWORD n = ::GetPrivateProfileIntW(kSection, key, 0, path.c_str());
    // GetPrivateProfileIntW 对「键不存在」和「值为 0」返回同样的 0，
    // 靠默认值哨兵区分：真实调用时默认值传 INT_MIN 作为不存在标记。
    if (n == INT_MIN) {
        return false;
    }
    out = static_cast<int>(n);
    return true;
}

}  // namespace

std::wstring SettingsDir() {
    return AppDataDir();
}

bool LoadSettings(Settings& s) {
    const std::wstring dir = AppDataDir();
    if (dir.empty()) {
        return false;
    }
    const std::wstring path = dir + L"\\settings.ini";

    // 全部用 INT_MIN 当「不存在」哨兵，因为 0 在本工程的每个键里都是合法值。
    int v = INT_MIN;

    if (ReadInt(path, L"SortMode", v)) {
        // 上界跟着 SortMode 枚举走。写死数字的话，加了新排序档又忘了
        // 改这里，就会让一个越界值直接 reinterpret 成非法枚举。
        if (v >= 0 && v <= static_cast<int>(SortMode::StartTypeDesc)) {
            s.sortMode = static_cast<SortMode>(v);
        } else {
            LINGSVC_TRACE("ini SortMode out of range (%d), keeping default", v);
        }
    }

    if (ReadInt(path, L"FilterMode", v)) {
        if (v >= 0 && v <= 4) {
            s.filterMode = static_cast<FilterMode>(v);
        } else {
            LINGSVC_TRACE("ini FilterMode out of range (%d), keeping default", v);
        }
    }

    if (ReadInt(path, L"DetailExpanded", v)) {
        s.detailExpanded = (v != 0);
    }

    if (ReadInt(path, L"WindowLeft", v)) {
        s.left = v;
    }
    if (ReadInt(path, L"WindowTop", v)) {
        s.top = v;
    }
    if (ReadInt(path, L"WindowWidth", v)) {
        s.width = v;
    }
    if (ReadInt(path, L"WindowHeight", v)) {
        s.height = v;
    }

    // 尺寸明显不合理（换分辨率、分辨率被改）就丢弃，让窗口走默认布局。
    // 宁可重新排版，也不要开出一个 0 宽或者比屏幕大 5 倍的窗口。
    if (s.width <= 400 || s.height <= 300) {
        s.left = INT_MIN;
        s.top = INT_MIN;
        s.width = 0;
        s.height = 0;
    }

    return true;
}

void SaveSettings(const Settings& s) {
    const std::wstring dir = AppDataDir();
    if (dir.empty()) {
        return;
    }
    const std::wstring path = dir + L"\\settings.ini";

    auto write = [&path](wchar_t const* key, int value) {
        const std::wstring text = std::to_wstring(value);
        ::WritePrivateProfileStringW(kSection, key, text.c_str(), path.c_str());
    };

    write(L"SortMode", static_cast<int>(s.sortMode));
    write(L"FilterMode", static_cast<int>(s.filterMode));
    write(L"DetailExpanded", s.detailExpanded ? 1 : 0);
    write(L"WindowLeft", s.left);
    write(L"WindowTop", s.top);
    write(L"WindowWidth", s.width);
    write(L"WindowHeight", s.height);
}

}  // namespace core
}  // namespace lingsvc
