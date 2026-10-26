// system_metrics.cpp —— 系统级指标采集实现
//
// 【为什么磁盘活动走 IOCTL 而不是 PDH】
// PDH 的 "\PhysicalDisk(*)\Disk Bytes/sec" 是最省事的写法，但它要新增
// **链接依赖** pdh.lib。本仓库把「新增依赖」列为需要用户点头的事项
// （README「需求变更」一节），而 IOCTL_DISK_PERFORMANCE 只需要
// <winioctl.h> —— 纯 Windows SDK 头文件，不多一个 .lib。
//
// 【SDK 实况 —— DISK_PERFORMANCE 里没有 PerformanceCounters】
// 实测本工程的 SDK（10.0.26100）里，DISK_PERFORMANCE 只有**累计量**：
//     BytesRead / BytesWritten / ReadTime / WriteTime / IdleTime /
//     QueryTime / ReadCount / WriteCount / QueueDepth / ...
// 没有「DiskReadBytesPerSec」这种现成的速率字段，也不存在
// DISK_PERFORMANCE_COUNTERS 这个结构（它在本 SDK 里查无此物）。
// 所以速率与占用率必须**自己做差分**：保存上一次读数，用增量除以时间。
//
// 【为什么 CPU 型号用 CPUID 而不是注册表】
// 注册表那一路（HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0）
// 在本工程里有两个坑：
//   ① windows.h 早就把 winreg.h 引进去了，而那一次 WINAPI_FAMILY 还是
//      Windows App SDK 设的 APP 族 —— 注册表 API 于是整块被跳过，
//      症状是「RegGetStringW 找不到标识符」，而错误完全指不到真正的原因；
//   ② 本 SDK 的 winreg.h 里**根本没有** RegGetStringW（它不是注册表 API
//      的一部分，只是文档里常被提起的 Win32 便利函数）。
// CPUID 的 0x80000002~0x80000004 三个叶子直接给出 48 字符的品牌串，
// 零依赖、零注册表、无需提权。
#include "pch.h"

#include "system_metrics.h"

#include "platform/window_decor.h"

#include <intrin.h>
#include <winioctl.h>

#include <cstring>
#include <string>

namespace lingwu {
namespace core {
namespace monitor {

namespace {

// 物理盘句柄 + 上一次的累计计数。QueryDiskActivity 只在采样线程被调用，
// 不存在并发问题。
HANDLE        g_diskHandle = INVALID_HANDLE_VALUE;
bool          g_diskProbed  = false;
DISK_PERFORMANCE g_diskPrev{};
bool          g_diskHavePrev = false;
ULONGLONG     g_diskPrevTickMs = 0;

void EnsureDiskHandle() {
    if (g_diskProbed) return;
    g_diskProbed = true;

    // 依次试前 8 块物理盘。访问参数用 0：MSDN 的 IOCTL_DISK_PERFORMANCE 示例
    // 就是这样写的，只需要「能发控制码」的权限，不需要读写数据。
    // 打不开就永久放弃 —— 每周期重试只是把同一个失败重复几百次。
    wchar_t path[64];
    for (int i = 0; i < 8 && g_diskHandle == INVALID_HANDLE_VALUE; ++i) {
        swprintf_s(path, L"\\\\.\\PhysicalDrive%d", i);
        g_diskHandle = CreateFileW(path, 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
    }
}

// CPU 品牌串。CPUID 0x80000002..0x80000004 共 48 字节，右补空格。
std::wstring QueryCpuBrand() {
    int regs[4] = { 0, 0, 0, 0 };
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004u) return std::wstring();

    char brand[49] = { 0 };
    for (unsigned leaf = 0x80000002u; leaf <= 0x80000004u; ++leaf) {
        __cpuid(regs, static_cast<int>(leaf));
        std::memcpy(brand + (leaf - 0x80000002u) * 16u, regs, 16);
    }

    std::string s(brand);
    const size_t b = s.find_first_not_of(' ');
    if (b == std::string::npos) return std::wstring();
    const size_t e = s.find_last_not_of(' ');
    s = s.substr(b, e - b + 1);

    std::wstring w;
    w.reserve(s.size());
    for (char ch : s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    return w;
}

// 从 platform::DescribeOsVersion() 的输出里取前两段做短名：
//     "Windows 11.0 (build 26100)" → "Windows 11"
//     "Windows 10.0 (build 19045)" → "Windows 10"
// 这样版本判定逻辑仍然只有 platform/window_decor.cpp 一份，
// 这里只是对它已经算好的字符串做一次裁剪。
std::wstring ShortOsName(const std::wstring& full) {
    const size_t sp1 = full.find(L' ');
    if (sp1 == std::wstring::npos) return full;

    const size_t sp2 = full.find(L' ', sp1 + 1);
    std::wstring second = (sp2 == std::wstring::npos)
                        ? full.substr(sp1 + 1)
                        : full.substr(sp1 + 1, sp2 - sp1 - 1);

    // "11.0" → "11"
    const size_t dot = second.find(L'.');
    if (dot != std::wstring::npos && dot > 0) second = second.substr(0, dot);

    return full.substr(0, sp1 + 1) + second;
}

}  // namespace

// ---------------------------------------------------------------- 磁盘空间

DiskSpaceInfo QueryDiskSpace() {
    DiskSpaceInfo out;

    // 盘符取 Windows 所在盘，而不是写死 C: —— 装了 D 盘版系统的机器上
    // 写死 C: 就会报「设备不存在」，磁贴直接空掉。
    wchar_t sysDir[MAX_PATH] = { 0 };
    const UINT n = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (n < 3 || n >= MAX_PATH) return out;

    const std::wstring root(sysDir, 3);   // "C:\"
    ULARGE_INTEGER freeBytes{}, totalBytes{}, totalFreeBytes{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &totalBytes, &totalFreeBytes))
        return out;

    out.valid = true;
    out.totalBytes = totalBytes.QuadPart;
    out.freeBytes  = freeBytes.QuadPart;
    out.volume     = root.substr(0, 2);  // "C:"
    return out;
}

// ---------------------------------------------------------------- 磁盘活动

DiskActivityInfo QueryDiskActivity() {
    DiskActivityInfo out;

    EnsureDiskHandle();
    if (g_diskHandle == INVALID_HANDLE_VALUE) return out;

    // 同一个结构体既作输入也作输出 —— 这是 IOCTL_DISK_PERFORMANCE 的约定。
    DISK_PERFORMANCE perf{};
    DWORD returned = 0;
    if (!DeviceIoControl(g_diskHandle, IOCTL_DISK_PERFORMANCE, &perf, sizeof(perf),
                         &perf, sizeof(perf), &returned, nullptr)) {
        return out;
    }

    const ULONGLONG now = GetTickCount64();

    // 第一次只有基线、没有差分可用 —— 这一次不产出速率（valid=false）。
    // 这是「差分法」的固有代价：头两个采样周期拿不到活动率，
    // 好在它只影响磁贴最初两三秒的显示，不是持续性问题。
    if (!g_diskHavePrev) {
        g_diskHavePrev  = true;
        g_diskPrev      = perf;
        g_diskPrevTickMs = now;
        return out;
    }

    const LONGLONG dQuery = perf.QueryTime.QuadPart    - g_diskPrev.QueryTime.QuadPart;
    const LONGLONG dIdle  = perf.IdleTime.QuadPart     - g_diskPrev.IdleTime.QuadPart;
    const LONGLONG dRead  = perf.BytesRead.QuadPart    - g_diskPrev.BytesRead.QuadPart;
    const LONGLONG dWrite = perf.BytesWritten.QuadPart - g_diskPrev.BytesWritten.QuadPart;
    const ULONGLONG dMs   = now - g_diskPrevTickMs;

    g_diskPrev       = perf;
    g_diskPrevTickMs = now;

    if (dMs == 0) return out;

    out.valid = true;

    // 忙时占比：QueryTime 是「被查询的总时长」，IdleTime 是其中的空闲时长。
    // 多队列硬盘上这两个值都可能被放大，所以结果一律钳在 0..100。
    if (dQuery > 0) {
        const double busy = static_cast<double>(dQuery - dIdle) * 100.0
                          / static_cast<double>(dQuery);
        out.percentBusy = busy < 0.0 ? 0.0 : (busy > 100.0 ? 100.0 : busy);
    }

    const double sec = static_cast<double>(dMs) / 1000.0;
    if (dRead  > 0) out.readBytesPerSec  = static_cast<uint64_t>(static_cast<double>(dRead)  / sec);
    if (dWrite > 0) out.writeBytesPerSec = static_cast<uint64_t>(static_cast<double>(dWrite) / sec);
    return out;
}

// ---------------------------------------------------------------- 电池

BatteryInfo QueryBattery() {
    BatteryInfo out;

    SYSTEM_POWER_STATUS st{};
    if (!GetSystemPowerStatus(&st)) return out;

    // BatteryFlag: 1=无电池, 2=放电中, 4=接电源充电, 8=未充满, 255=未知
    if (st.BatteryFlag == 255) return out;   // 读不到 → 当作「没有这块信息」
    if (st.BatteryFlag & 0x01) return out;    // 无电池 = 台式机，磁贴整块不显示

    out.present  = true;
    out.charging = (st.BatteryFlag & 0x04) != 0;
    out.percent  = (st.BatteryLifePercent == 255) ? 0
                                                   : static_cast<int>(st.BatteryLifePercent);
    if (st.BatteryLifeTime != static_cast<DWORD>(-1))
        out.secondsLeft = st.BatteryLifeTime;
    return out;
}

// ---------------------------------------------------------------- 内存

MemoryInfo QueryMemory() {
    MemoryInfo out;

    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (!GlobalMemoryStatusEx(&st)) return out;

    out.totalBytes     = st.ullTotalPhys;
    out.availableBytes = st.ullAvailPhys;
    out.pageFileTotalBytes = st.ullTotalPageFile;
    out.pageFileUsedBytes  = st.ullTotalPageFile > st.ullAvailPageFile
                           ? st.ullTotalPageFile - st.ullAvailPageFile
                           : 0;
    if (st.ullTotalPhys > 0) {
        out.usedPercent = static_cast<double>(st.ullTotalPhys - st.ullAvailPhys) * 100.0
                        / static_cast<double>(st.ullTotalPhys);
    }
    return out;
}

// ---------------------------------------------------------------- 主机信息

HostInfo QueryHostInfo() {
    // 静态项只读一次：CPU 型号与 OS 版本在一次进程生命周期里不会变，
    // 每秒去算一次是纯粹的浪费。
    static std::wstring cachedCpu;
    static std::wstring cachedOs;
    static std::wstring cachedOsShort;
    static bool          cached = false;

    if (!cached) {
        cached = true;
        cachedCpu     = QueryCpuBrand();
        cachedOs      = platform::DescribeOsVersion();
        cachedOsShort = ShortOsName(cachedOs);
    }

    HostInfo out;
    out.cpuName       = cachedCpu;
    out.osName        = cachedOs;
    out.osShort       = cachedOsShort;
    out.uptimeSeconds = GetTickCount64() / 1000;
    return out;
}

// ---------------------------------------------------------------- 格式化

std::wstring FormatUptime(uint64_t seconds) {
    const uint64_t days    = seconds / 86400;
    const uint64_t hours   = (seconds % 86400) / 3600;
    const uint64_t minutes = (seconds % 3600) / 60;

    wchar_t buf[64] = { 0 };
    if (days > 0) {
        swprintf_s(buf, L"%llu 天 %llu 小时",
                   static_cast<unsigned long long>(days),
                   static_cast<unsigned long long>(hours));
    } else if (hours > 0) {
        swprintf_s(buf, L"%llu 小时 %llu 分",
                   static_cast<unsigned long long>(hours),
                   static_cast<unsigned long long>(minutes));
    } else {
        swprintf_s(buf, L"%llu 分钟", static_cast<unsigned long long>(minutes));
    }
    return buf;
}

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
