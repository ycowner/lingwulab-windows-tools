// privilege.h —— 提权状态判定与「以管理员身份重新启动」。
//
// 权限模型（已确认）：默认普通权限启动，能看列表；需要提权时弹提示，
// 由用户点「以管理员身份重新启动」。所以本工程**不**在 manifest 里声明
// requireAdministrator —— 那会让每次双击都弹 UAC，而看列表根本不需要管理员。
#pragma once

#include <string>

namespace lingsvc {
namespace core {

// 当前进程是否以**真正提升**的令牌运行。
//
// 【为什么不能用 CheckTokenMembership(nullptr, Administrators)】
// 它只问「这个令牌里有没有 Administrators 组成员」，而 UAC 会给管理员账户
// 发一个**虚拟化的、已降级**的普通令牌 —— 那个令牌里同样"有"管理员 SID，
// 于是判定返回 TRUE，但实际连一个服务的启动类型都改不了。
// 唯一可靠的读法是 TokenElevation 的 TokenIsElevated。
bool IsElevated();

// 请求以管理员权限重启自身。
//
// 【关键：这里**不**直接调用 ShellExecuteExW】
// 提权路径与单实例锁的顺序必须是：
//     1. 本进程退出、CloseHandle(mutex)
//     2. 之后才拉起新实例
// 反过来（先起新的再退旧的）会让新实例在第 2 步之前就撞上单实例锁，
// 要么它激活了一个正在消失的旧窗口然后自己退出，要么两边都以为对方该退 ——
// 症状是「点了提权重启，没有任何反应」，且没有任何报错。
// 所以这里只登记一个待执行动作，真正执行由 main.cpp 在退出路径上做。
//
// extraArgs 会被拼在命令行上（用于把当前筛选/搜索词带过去）。
void RequestRelaunchElevated(std::wstring const& extraArgs);

bool HasPendingRelaunch();

// 拉起提权实例。**只允许在进程退出路径上调用一次**
// （即 Application::Start 返回之后、main 返回之前）。
void PerformPendingRelaunch();

}  // namespace core
}  // namespace lingsvc
