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
//   * 每帧最多真正解析 kResolveBudget 个未缓存进程，其余直接返回空串；
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

constexpr int kResolveBudget = 4;      // 每帧最多解析几个新进程
constexpr uint32_t kEvictEveryFrames = 240;   // 每 240 帧做一次淘汰
constexpr uint32_t kIdleFramesToDrop = 480;   // 连续 480 帧没被问到就丢掉

struct PidEntry {
    ULONGLONG   createTime = 0;
    std::wstring path;      // exe 完整路径
    std::wstring display;   // 显示名（可能为空）
    uint32_t    lastSeen = 0;
};

std::unordered_map<DWORD, PidEntry>     g_pid;
std::unordered_map<std::wstring, std::wstring> g_byPath;   // 小写路径 → 显示名
uint32_t g_frame = 0;
int      g_budget = 0;

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

void EvictIdle() {
    for (auto it = g_pid.begin(); it != g_pid.end(); ) {
        if (g_frame - it->second.lastSeen > kIdleFramesToDrop) it = g_pid.erase(it);
        else ++it;
    }
}

}  // namespace

void AppNamesBeginFrame() {
    ++g_frame;
    g_budget = kResolveBudget;
    if ((g_frame % kEvictEveryFrames) == 0) EvictIdle();
}

std::wstring FriendlyAppName(DWORD pid, ULONGLONG createTime,
                             const std::wstring& exeName) {
    if (pid == 0) return L"";

    auto it = g_pid.find(pid);
    if (it != g_pid.end()) {
        // PID 会被复用：createTime 对不上就当作新进程重新解析
        if (it->second.createTime == createTime) {
            it->second.lastSeen = g_frame;
            return it->second.display;
        }
        g_pid.erase(it);
    }

    if (g_budget <= 0) return L"";
    --g_budget;

    PidEntry e;
    e.createTime = createTime;
    e.lastSeen   = g_frame;
    e.path       = ResolveProcessPath(pid);
    if (!e.path.empty())
        e.display = DisplayForPath(e.path, exeName);

    g_pid.emplace(pid, std::move(e));
    return g_pid[pid].display;
}

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
