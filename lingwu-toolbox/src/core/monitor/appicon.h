// appicon.h —— 进程 exe 图标提取（进程内缓存）
//
// 【为什么单独一个模块】
// 采样链路明确不碰图标：提取图标要读 exe 的资源、慢且吃 I/O，
// 而采样每秒跑一次且对延迟敏感。图标只在「表格要画图标」时才需要，
// 是纯 UI 侧的按需取数。
#pragma once

#include <windows.h>

#include <string>

namespace lingwu {
namespace core {
namespace monitor {

// 取某 exe 路径的图标（HICON），按路径缓存，一个 exe 一生只提取一次。
//
// 【为什么取大图标（32×32）而不是小图标（16×16）】
// 表格的图标位是 16 DIP。在 200% DPI 屏幕上 16 DIP = 32 物理像素，
// 正好等于大图标尺寸，缩放 1:1 不糊；
// 小图标只有 16×16，被拉到 32 物理像素就是马赛克。
//
// 【为什么按路径缓存而不是按 pid】
// 同一个 exe 常开十几个进程（浏览器是典型）。
// 图标是 exe 资源里的东西，与进程无关 —— 按路径缓存等于几十个进程共用一次提取。
//
// 【线程约束】
// 内部缓存不是线程安全的，只允许 UI 线程调用（与 appnames 的约束一致）。
//
// 返回 NULL 表示取不到（路径不对 / 该 exe 没有图标资源）。
// 调用方负责回退（画一个首字母方块），不要让 nullptr 变成崩溃。
HICON GetIconForPath(const std::wstring& exePath);

// 便捷版：先解析进程的 exe 路径（走 appnames 那份 pid 缓存），再取图标。
HICON GetAppIcon(DWORD pid, ULONGLONG createTime, const std::wstring& exeName);

// 释放所有缓存的图标句柄。进程退出前调一次即可。
void ReleaseAppIcons();

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
