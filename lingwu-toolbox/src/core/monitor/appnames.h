// appnames.h —— 进程的桌面显示名（版本资源里的 FileDescription / ProductName）
#pragma once

#include <windows.h>
#include <string>

namespace lingwu {
namespace core {
namespace monitor {

// 取指定进程的桌面显示名。
//
// 设计约束：
//   * 只允许 UI 线程调用。内部有两级进程内缓存，不是线程安全的。
//   * **绝不能在 Sampler 的锁内调用**：取版本资源要读盘，会阻塞采样线程。
//     调用方应当先持锁把需要的 (pid, createTime, exeName) 拷出来，解锁后再问。
//   * 返回空串表示「没有可显示的名字」，调用方此时只显示 exe 名即可。
//   * 单帧最多真正解析 kResolveBudget 个未缓存进程（见 AppNamesBeginFrame），
//     首屏几百行会在若干帧内陆续补齐，不会把任何一帧拖死。
std::wstring FriendlyAppName(DWORD pid, ULONGLONG createTime,
                             const std::wstring& exeName);

// 取进程的 exe 完整路径（进程内缓存，按 pid + createTime 定位）。
//
// 空串表示取不到（权限不足 / 进程已退出 / 本帧预算用尽）。
//
// 与 FriendlyAppName **共用同一份 pid 缓存**：两者一起用时，
// 同一个进程只会 OpenProcess 一次 —— 不会因为多问一句路径就多开一次句柄。
// 路径解析的预算与显示名解析的预算是分开的两份（见 appnames.cpp），
// 因为「问路径」很便宜（一次 QueryFullProcessImageNameW），
// 而「问显示名」要读版本资源、真正碰磁盘。
std::wstring ProcessImagePath(DWORD pid, ULONGLONG createTime);

// 每帧绘制开始时调一次，重置本帧的解析预算并推进淘汰计时。
void AppNamesBeginFrame();

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
