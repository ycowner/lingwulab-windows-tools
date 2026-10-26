// appnames.cpp —— 桌面显示名解析（进程内两级缓存）
//
// 显式 #include <cwctype>：towlower 靠 windows.h 间接拉进来是靠不住的做法，
// 写明依赖，避免换 PCH 时静默变成 C3861。
//
// 为什么不把显示名放进 AppInfo / 在采样线程里取：
//   NtQuerySystemInformation 只给 ImageName（就是 exe 文件名），拿不到路径；
//   而 FileDescription 藏在 exe 的版本资源里，必须 OpenProcess +
//   QueryFullProcessImageNameW 再 GetFileVersionInfoW 读盘。
//   采样线程每秒跑一次、对延迟敏感，把读盘塞进去会拖慢整条采样链，
//   所以这里完全独立于 Sampler，只在绘制时按需查、查完缓存。
//
// 开销控制：
//   * 每帧最多真正解析 kPathBudget 个新进程的 exe 路径；
//     版本资源另给一份 kDisplayBudget —— 路径查询很便宜，
//     读版本资源才真的碰磁盘，两者不该共用一个额度；
//   * 版本资源按 exe 路径缓存，同一个 exe 的几十个进程只读一次盘；
//   * 长期不可见的 pid 条目惰性淘汰。
#include "pch.h"

#include "appnames.h"

#include "metrics.h"

#include <winver.h>

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "version.lib")

namespace lingwu {
namespace core {
namespace monitor {
namespace {

constexpr int kPathBudget = 16;      // 每帧最多为几个新进程解析 exe 路径
constexpr int kDisplayBudget = 4;    // 每帧最多为几个进程读版本资源
constexpr uint32_t kEvictEveryFrames = 240;   // 每 240 帧做一次淘汰
constexpr uint32_t kIdleFramesToDrop = 480;   // 连续 480 帧没被问到就丢掉

struct PidEntry {
    ULONGLONG   createTime = 0;
    std::wstring path;      // exe 完整路径
    std::wstring display;   // 显示名（可能为空）
    bool        displayResolved = false;   // display 是「还没查」还是「查过确实是空」
    uint32_t    lastSeen = 0;
};

std::unordered_map<DWORD, PidEntry>     g_pid;
std::unordered_map<std::wstring, std::wstring> g_byPath;   // 小写路径 → 显示名
uint32_t g_frame = 0;
int      g_pathBudget = 0;
int      g_displayBudget = 0;

std::wstring LowerCase(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

// 从版本资源里取一个字符串字段。取不到返回空串。
// 注意 VerQueryValueW 返回的长度含结尾的 NUL，要自己剥掉。
std::wstring ReadVersionField(const std::wstring& path, const wchar_t* field) {
    const DWORD need = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    if (need == 0) return L"";
    std::vector<BYTE> buf(need);
    if (!GetFileVersionInfoW(path.c_str(), 0, need, buf.data())) return L"";

    struct LangBlock { WORD wLanguage; WORD wCodePage; };
    LangBlock* langs = nullptr;
    UINT nLangs = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<LPVOID*>(&langs), &nLangs) ||
        !langs || nLangs < sizeof(LangBlock)) {
        return L"";
    }

    const UINT count = nLangs / static_cast<UINT>(sizeof(LangBlock));
    for (UINT i = 0; i < count; ++i) {
        wchar_t sub[128] = { 0 };
        _snwprintf_s(sub, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\%s",
                     langs[i].wLanguage, langs[i].wCodePage, field);
        LPWSTR val = nullptr;
        UINT vlen = 0;
        if (VerQueryValueW(buf.data(), sub, reinterpret_cast<LPVOID*>(&val), &vlen) &&
            val && vlen > 0) {
            std::wstring s(val, vlen);
            while (!s.empty() && (s.back() == L'\0' || s.back() == L' ')) s.pop_back();
            if (!s.empty()) return s;
        }
    }
    return L"";
}

// 按路径取显示名，命中即负缓存（取不到也记空串，不重复读盘）。
std::wstring DisplayForPath(const std::wstring& path, const std::wstring& exeName) {
    const std::wstring key = LowerCase(path);
    auto it = g_byPath.find(key);
    if (it != g_byPath.end()) return it->second;

    std::wstring d = ReadVersionField(path, L"FileDescription");
    if (d.empty()) d = ReadVersionField(path, L"ProductName");

    // 很多 exe 的 FileDescription 就是自己的名字，显示出来纯属冗余
    const std::wstring base = StripExe(exeName);
    if (!d.empty() && base.compare(d) == 0) d.clear();

    g_byPath.emplace(key, d);
    return d;
}

std::wstring ResolveProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD cch = MAX_PATH;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &cch))
        out.assign(buf, cch);
    CloseHandle(h);
    return out;
}

// 取（必要时建立）某个 pid 的缓存条目。
//
// 命中时顺手刷新 lastSeen —— 被问到才算「还活着」，
// 淘汰按 lastSeen 算，长期不出现的 pid 自然被丢掉。
//
// 路径解析失败也照样建条目（path 留空）：失败的 OpenProcess 每帧重试
// 只会白烧预算，不如记下来「这个 pid 拿不到路径」。
//
// 返回的指针在 g_pid 里是稳定的 —— unordered_map 是节点式容器，
// rehash 只失效迭代器，元素本身的地址不变。
PidEntry* EnsurePath(DWORD pid, ULONGLONG createTime) {
    auto it = g_pid.find(pid);
    if (it != g_pid.end()) {
        // PID 会被复用：createTime 对不上就当作新进程重新解析
        if (it->second.createTime == createTime) {
            it->second.lastSeen = g_frame;
            return &it->second;
        }
        g_pid.erase(it);
    }

    if (g_pathBudget <= 0) return nullptr;
    --g_pathBudget;

    PidEntry e;
    e.createTime = createTime;
    e.lastSeen   = g_frame;
    e.path       = ResolveProcessPath(pid);
    return &g_pid.emplace(pid, std::move(e)).first->second;
}

void EvictIdle() {
    for (auto it = g_pid.begin(); it != g_pid.end(); ) {
        if (g_frame - it->second.lastSeen > kIdleFramesToDrop) it = g_pid.erase(it);
        else ++it;
    }
}

}  // namespace

void AppNamesBeginFrame() {
    ++g_frame;
    g_pathBudget = kPathBudget;
    g_displayBudget = kDisplayBudget;
    if ((g_frame % kEvictEveryFrames) == 0) EvictIdle();
}

std::wstring ProcessImagePath(DWORD pid, ULONGLONG createTime) {
    if (pid == 0) return L"";
    PidEntry* e = EnsurePath(pid, createTime);
    return e ? e->path : L"";
}

std::wstring FriendlyAppName(DWORD pid, ULONGLONG createTime,
                             const std::wstring& exeName) {
    if (pid == 0) return L"";

    PidEntry* e = EnsurePath(pid, createTime);
    if (!e) return L"";
    if (e->displayResolved) return e->display;

    // 路径拿不到 → 这个进程没有可显示的名字，记下来别再问。
    if (e->path.empty()) {
        e->displayResolved = true;
        return L"";
    }

    if (g_displayBudget <= 0) return L"";
    --g_displayBudget;

    e->display = DisplayForPath(e->path, exeName);
    e->displayResolved = true;
    return e->display;
}

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
