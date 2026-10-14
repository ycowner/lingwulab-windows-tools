// appnames.h —— 进程的桌面显示名（版本资源里的 FileDescription / ProductName）
#pragma once

#include <windows.h>
#include <string>

namespace lingsysmon {
namespace core {

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

// 每帧绘制开始时调一次，重置本帧的解析预算并推进淘汰计时。
void AppNamesBeginFrame();

}  // namespace core
}  // namespace lingsysmon
