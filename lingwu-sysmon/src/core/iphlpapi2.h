// iphlpapi2.h —— 现代网卡枚举 API 的正确包含方式
//
// 纯 SDK 下直接用 <iphlpapi.h> 拿不到 MIB_IF_ROW2 / GetIfTable2，原因有三层：
//   1. MIB_IF_ROW2 等结构体在 netioapi.h 的 #ifdef _WS2IPDEF_ 区块内，
//      用户态不会自动包含 ws2ipdef.h，所以必须自己先包含。
//   2. netioapi.h 靠 __IPHLPAPI_H__ 区分用户态 / 内核态：只有当它是在
//      iphlpapi.h 内部被包含（该宏已定义）时才走用户态分支，否则会走内核态
//      分支引用 WDK 才有的 NTSTATUS，直接编译失败。
//      先包含 <iphlpapi.h> 再包含 <netioapi.h> 即可命中用户态分支。
//   3. winsock2.h 必须排在 windows.h 之前，否则 windows.h 自带的 winsock.h
//      会与之冲突。因此本头文件必须在包含 windows.h 的任何头文件之前引入。
//
// ── 本工程（WinUI 3 / PCH）下的实际情况 ──────────────────────────────
// 本工程的预编译头 src\pch.h 第 51 行就 #include <windows.h>，而 MSVC 的 /Yu
// 是把 pch.h **强制注入到每个源文件最开头**的。因此在 sampler.cpp 里写
//     #include "core/iphlpapi2.h"   // 想抢在 windows.h 之前
// 其实已经晚了 —— winsock.h 早就被拉进来了。
//
// 之所以仍然能编过，靠的是 vcxproj 的 PreprocessorDefinitions 里那句
// WIN32_LEAN_AND_MEAN：Windows 头里的条件是
//     #if !defined(WIN32_LEAN_AND_MEAN) || defined(WINSOCK_API_FAMILY)
//     #include <winsock.h>
//     #endif
// 即只要 LEAN_AND_MEAN 生效且没定义 WINSOCK_API_FAMILY，windows.h 就不碰
// winsock.h，winsock2.h 随后引入也不会撞车。
//
// 但这已经是「靠一个远处的编译选项兜住」，属于典型静默失效场景：哪天有人
// 为了别的目的去掉 LEAN_AND_MEAN，症状是 winsock2.h 自己抛一句
// 「must be included before windows.h」，而不是任何能指向本文件的名字。
// 所以下面加一道显式断言：一旦 winsock.h 已经进来就立刻 #error，
// 把错误定位回真正的成因。
#pragma once

#if defined(_WINSOCKAPI_) && !defined(_WINSOCK2API_)
#error "winsock.h 已被包含。检查 core/iphlpapi2.h 的包含顺序，以及 vcxproj 里 WIN32_LEAN_AND_MEAN 是否仍然生效。"
#endif

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
