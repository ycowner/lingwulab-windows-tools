// terminate.cpp —— 结束进程的三段式实现
//
// 三段式：跳过受保护 / 自身 → WM_CLOSE 优雅关闭 → 轮询 → TerminateProcess 强杀。
// 打不开目标进程（ERROR_ACCESS_DENIED）时返回 KILL_ACCESS_DENIED ——
// 报的是事实，措辞由 UI 决定：合并成工具箱后程序恒定以管理员身份运行，
// 这个结果通常意味着「进程受系统保护」，而不是「缺权限、提权就能解决」。
#include "pch.h"

#include "terminate.h"

#include <vector>

namespace lingwu {
namespace core {
namespace monitor {
namespace {

bool ProcessAlive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;            // 打不开 → 视为已退出或无权访问
    const DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

void PostCloseToProcess(DWORD pid) {
    for (HWND w = GetTopWindow(nullptr); w; w = GetNextWindow(w, GW_HWNDNEXT)) {
        DWORD p = 0;
        GetWindowThreadProcessId(w, &p);
        if (p == pid) PostMessageW(w, WM_CLOSE, 0, 0);
    }
}

}  // namespace

KillResult TerminateAppPids(const DWORD* pids, DWORD count, bool gracefulFirst,
                            DWORD graceMs, int* killedCount) {
    int killed = 0;
    bool accessDenied = false;
    const DWORD selfPid = GetCurrentProcessId();

    std::vector<DWORD> pending;
    pending.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        const DWORD pid = pids[i];
        if (pid == 0 || pid == 4 || pid == selfPid) continue;   // 受保护 / 自身
        pending.push_back(pid);
    }
    const size_t total = pending.size();

    // ---- 1) 优雅关闭：给所有顶层窗口发 WM_CLOSE ----
    if (gracefulFirst && !pending.empty()) {
        for (DWORD pid : pending) PostCloseToProcess(pid);

        bool allGone = false;
        for (DWORD waited = 0; waited < graceMs && !allGone; waited += 100) {
            allGone = true;
            for (DWORD pid : pending) {
                if (ProcessAlive(pid)) { allGone = false; break; }
            }
            if (!allGone) Sleep(100);
        }
        if (allGone) { killed = static_cast<int>(total); pending.clear(); }
    }

    // ---- 2) 强杀残留 ----
    for (DWORD pid : pending) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) {
            if (GetLastError() == ERROR_ACCESS_DENIED) accessDenied = true;
            continue;
        }
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) { CloseHandle(h); killed++; continue; }
        if (TerminateProcess(h, 0)) killed++;
        CloseHandle(h);
    }

    if (killedCount) *killedCount = killed;
    if (accessDenied) return KILL_ACCESS_DENIED;
    return (killed > 0) ? KILL_OK : KILL_FAILED;
}

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
