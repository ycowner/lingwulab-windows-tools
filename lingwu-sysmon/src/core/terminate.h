// terminate.h —— 结束进程（三段式）
#pragma once

#include <windows.h>

namespace lingsysmon {
namespace core {

// 进程结束结果
enum KillResult {
    KILL_OK,
    KILL_NEEDS_ADMIN,
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

}  // namespace core
}  // namespace lingsysmon
