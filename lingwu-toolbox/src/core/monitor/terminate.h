// terminate.h —— 结束进程（三段式）
#pragma once

#include <windows.h>

namespace lingwu {
namespace core {
namespace monitor {

// 进程结束结果
enum KillResult {
    KILL_OK,
    // 有进程**打不开**（OpenProcess 返回 ERROR_ACCESS_DENIED）。
    //
    // 【为什么不再叫 KILL_NEEDS_ADMIN】
    // 合并成工具箱后整个程序恒定以管理员身份运行（manifest 声明
    // requireAdministrator），于是这个结果**不再意味着「缺权限、提权就能解决」** ——
    // 管理员也打不开的进程是系统保护进程（PPL / 关键进程）。
    // 所以这里报事实（访问被拒），由 UI 决定怎么措辞，
    // 绝不在这条路径上再提供「以管理员身份重启」这种无效入口。
    KILL_ACCESS_DENIED,
    KILL_FAILED,
};

// 结束一批进程。
//
//   gracefulFirst = true 时先给所有目标进程的顶层窗口发 WM_CLOSE，
//   然后最多轮询 graceMs（100 ms 一次）等它们自行退出；仍活着的再强杀。
//   killedCount 非空时回填实际结束的数量。
//
// **不要在 UI 线程上同步调用**：gracefulFirst 分支最长会阻塞 graceMs
// （调用方给的是 3000 ms）。这期间 XAML 合成会整段卡住，窗口直接「未响应」。
// 调用方应在工作线程执行，再把结果 marshal 回 UI。
KillResult TerminateAppPids(const DWORD* pids, DWORD count, bool gracefulFirst,
                            DWORD graceMs, int* killedCount);

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
