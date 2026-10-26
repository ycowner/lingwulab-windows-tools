// appicon.cpp —— 进程 exe 图标提取实现
//
// 【为什么用 ExtractIconEx 而不是 SHGetFileInfo】
// SHGetFileInfo(SHGFI_ICON) 给的是「按扩展名匹配的系统文件类型图标」，
// 也就是所有 .exe 都长成同一个样子 —— 那不是用户想看到的应用图标。
// ExtractIconEx 对 EXE 文件返回的是它**自己资源里**的图标。
#include "pch.h"

#include "appicon.h"

#include "appnames.h"

// ExtractIconExW 在 shellapi.h 里，windows.h 不包含它。
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <unordered_map>

namespace lingwu {
namespace core {
namespace monitor {
namespace {

// 小写路径 → HICON。
// 值为 nullptr 表示「这个路径试过了，取不到」—— 也照样记下来：
// 失败的提取每次重试都是纯浪费，而没有图标资源的 exe 在系统里并不少见。
std::unordered_map<std::wstring, HICON> g_icons;

std::wstring LowerCase(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

}  // namespace

HICON GetIconForPath(const std::wstring& exePath) {
    if (exePath.empty()) return nullptr;

    const std::wstring key = LowerCase(exePath);
    auto it = g_icons.find(key);
    if (it != g_icons.end()) return it->second;

    HICON h = nullptr;
    // 第 0 号图标就是「最有代表性」的那一个，直接取它。
    // 传 phiconSmall = nullptr 是刻意的：小图标是 16×16，
    // 在 200% DPI 上会被拉成马赛克（原因见 appicon.h 的注释）。
    if (ExtractIconExW(exePath.c_str(), 0, &h, nullptr, 1) == 0 || !h) h = nullptr;

    g_icons.emplace(key, h);
    return h;
}

HICON GetAppIcon(DWORD pid, ULONGLONG createTime, const std::wstring& exeName) {
    (void)exeName;   // exeName 只用于取不到图标时的回退展示，由 UI 侧负责
    const std::wstring path = ProcessImagePath(pid, createTime);
    return GetIconForPath(path);
}

void ReleaseAppIcons() {
    for (auto& kv : g_icons) {
        if (kv.second) ::DestroyIcon(kv.second);
    }
    g_icons.clear();
}

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
