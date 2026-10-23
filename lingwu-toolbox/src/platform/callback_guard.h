#pragma once
//
// Win32 回调边界的异常兜底层。
//
// 【为什么必须有这一层】
// 托盘菜单回调、窗口子类、Shell 回调全都跑在 Win32 的窗口过程里
// （LRESULT CALLBACK）。C++ 异常一旦从窗口过程里逃出去，上面那层
// （user32 / shell）没有 C++ handler，CRT 只能走
//     std::terminate → abort() → __fastfail(FAST_FAIL_FATAL_APP_EXIT)
// 表现为 0xC0000409，进程直接没了，现场一点不剩：
//   · 事件日志只有一条 0xC0000409，没有任何异常信息；
//   · trace 里也什么都没有（异常在第一条日志之前就抛了）。
//
// 2026-10-20 的真实崩溃就是这个签名。用 build/obj/Release/LingwuSysmon.map
// 符号化：0x14003f739 = abort(0x14003f704) + 0x35 —— 确证是 CRT 的
// std::terminate 路径，不是随机内存破坏。
//
// 【为什么这个头可以 include <winrt/base.h>】
// 只用 winrt::hresult_error 这一个**异常类型**（为了把 hresult 码落进
// trace，方便下一次现场直接读出错误码），不引入任何 WinRT 投影对象。
// pch.h 第 23 行已经 include 了它，所以零额外编译成本；
// tray_icon.cpp 仍然是「不持有 WinRT 投影对象」的纯 Win32 模块。
//
// 【怎么用】
//     platform::GuardCallback("tray.onSettings", [&]{ ... });
//
// tag 要能直接指出是哪一条路径 —— 出事时 trace 里只有这一行，
// 读的人没有第二次机会去猜。
//
#include "trace.h"

#include <exception>
#include <utility>
#include <winrt/base.h>

namespace platform {

// 包住一段「可能抛异常」的回调体。
//
// 语义：
//   · 绝不把异常放出去 —— 在窗口过程边界上放出去等于让进程直接终止；
//   · 落 trace 时带上 hresult 码与消息，让下一次现场能直接读出
//     「是哪一处调用、什么错误码」，不用再靠猜；
//   · noexcept —— 保证兜底层自己不会成为新的抛出点。
template <typename Fn>
void GuardCallback(const char* tag, Fn&& fn) noexcept {
    try {
        std::forward<Fn>(fn)();
    } catch (winrt::hresult_error const& e) {
        Trace("%s: caught hresult_error 0x%08X msg=%ls", tag,
              static_cast<unsigned>(e.code().value), e.message().c_str());
    } catch (std::exception const& e) {
        Trace("%s: caught std::exception: %s", tag, e.what());
    } catch (...) {
        Trace("%s: caught UNKNOWN non-standard exception", tag);
    }
}

}  // namespace platform
