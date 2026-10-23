// single_instance.h —— 单实例互斥 + 「把已有窗口叫出来」。
//
// 【为什么取带超时的等待，而不是「看到已存在就激活别人并退出」】
// 朴素做法（CreateMutex → ERROR_ALREADY_EXISTS 就激活别人并退出）在有
// 「退出后重启」路径时会死锁：
//     用户触发重启 → 本进程退出 → 新实例被拉起
// 新实例起来时旧进程的互斥量可能还没被系统回收（进程刚死、句柄刚关），
// 于是它看到 ERROR_ALREADY_EXISTS，激活一个正在消失的旧窗口，然后自己退出 ——
// 症状是「点了重启，什么都没发生」，且没有任何报错。
//
// 所以这里改成**带超时的等待**：
//     ERROR_ALREADY_EXISTS → WaitForSingleObject(hMutex, kWaitMs)
//         WAIT_OBJECT_0 → 旧进程确实在退出中，拿到所有权继续启动
//         WAIT_TIMEOUT  → 有别的实例真的在跑，激活它并退出
//
// 【为什么互斥量用 Global\ 前缀】
// 本程序默认以管理员身份运行（manifest 声明 requireAdministrator），
// 而 Explorer 启动的普通进程与它处于不同完整性级别。Global\ 让两边的
// 实例能被同一把锁挡住；创建失败（权限不足等）时退到 Local\，
// 宁可少挡一种情况也不要起不来。
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

// 退出前把共享内存里的 HWND 清掉，避免下一个实例往一个已销毁的窗口发消息。
void ClearPublishedHwnd();

// 读共享内存，拿到「另一个本程序实例」的主窗口 HWND。
// 找不到（或值无效）返回 nullptr。
HWND FindOtherInstanceWindow();

// 从别的进程发来的「把主窗口叫出来」消息 id（RegisterWindowMessageW 的结果）。
// 主窗口的子类过程靠它识别；0 表示还没注册。
UINT ActivateMessageId();

}  // namespace platform
