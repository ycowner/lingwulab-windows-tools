// single_instance.h —— 单实例互斥 + 「把已有窗口叫出来」。
//
// 【与 lingwu-sysmon 的一处关键差异，必须照这个实现，不要照抄那边的】
// sysmon 的做法是「CreateMutex → ERROR_ALREADY_EXISTS 就激活别人并退出」。
// 本工具有提权重启路径，那套逻辑会死锁：
//     用户点「以管理员身份重新启动」→ 本进程退出 → runas 拉起新实例
// 新实例起来时旧进程的互斥量可能还没被系统回收（进程刚死、句柄刚关），
// 于是它看到 ERROR_ALREADY_EXISTS，激活一个正在消失的旧窗口，然后自己退出 ——
// 症状是「点了提权重启，什么都没发生」，且没有任何报错。
//
// 所以本文件改成**带超时的等待**：
//     ERROR_ALREADY_EXISTS → WaitForSingleObject(hMutex, kWaitMs)
//         WAIT_OBJECT_0 → 旧进程确实在退出中，拿到所有权继续启动
//         WAIT_TIMEOUT  → 有别的实例真的在跑，激活它并退出
#pragma once

#include <windows.h>

namespace platform {

// 拿到互斥量。
//
// 返回的句柄**必须持有到进程结束**（持有即占用）。已经有一份在跑时
// alreadyRunning 为 true，此时本进程应当安静退出（激活窗口的动作
// 由本函数内部完成）。
//
// mutex 可能为 nullptr（互斥量都创建不出来时的退路），此时按
// 「没能取得独占」处理：alreadyRunning = true，宁可少一种互斥也不双开。
bool AcquireSingleInstance(HANDLE& mutex, bool& alreadyRunning);

// 把本进程主窗口的 HWND 发布出去，供第二份实例查找。
// 【为什么不能按类名找】WinUI 3 的窗口类名是系统统一的
// "WinUIDesktopWin32WindowClass"，机器上每个 WinUI 应用都叫这个名字。
void PublishMainHwnd(HWND hwnd);

}  // namespace platform
