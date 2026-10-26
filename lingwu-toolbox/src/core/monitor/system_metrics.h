// system_metrics.h —— 系统级指标采集（磁盘 / 电池 / 内存 / 主机信息）
//
// 【为什么单独一个模块，而不是塞进 sampler.cpp】
// sampler.cpp 的三条采集链路（进程 / 网络 / 视图构建）都是「每周期都要跑」的，
// 而这里的指标分两类：
//   · 易变但便宜：物理内存、磁盘活动
//   · 不变但要碰系统：磁盘剩余、电池状态、CPU 型号、OS 版本
// 后者如果每周期都查，注册表与设备 IO 的开销会被白白放大，
// 而界面上这些数字本来也看不出差别。把它们收进本模块后，
// 「多久查一次」就变成调用方 sampler.cpp 里的一处节流参数，而不是散落的魔法数。
//
// 【口径约定 —— 与状态栏的私有工作集不是一回事】
// 本模块的内存口径 = **物理内存占用**（GlobalMemoryStatusEx：总量 - 可用量）。
// 状态栏那个是「各进程私有工作集之和」。
// 两者在任务管理器里也是两个不同的数（本工具的进程表口径等价于
// 任务管理器「进程」页的「内存(专用字节)」）。指标磁贴用物理内存口径，
// 状态栏保持原口径不改 —— 两者各自标注，不混成一个数。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace lingwu {
namespace core {
namespace monitor {

// 磁盘剩余空间。volume 形如 "C:"。
struct DiskSpaceInfo {
    bool        valid = false;
    uint64_t    totalBytes = 0;
    uint64_t    freeBytes  = 0;
    std::wstring volume;
};

// 磁盘活动率。
struct DiskActivityInfo {
    bool     valid = false;
    double   percentBusy = 0.0;           // 0..100（多队列盘会 >100，这里已钳过）
    uint64_t readBytesPerSec  = 0;
    uint64_t writeBytesPerSec = 0;
};

// 电池。台式机上 present 恒为 false —— 调用方据此**不显示**这块磁贴，
// 而不是显示「0%」，因为「没有电池」和「电池没电」是两件事。
struct BatteryInfo {
    bool     present = false;
    bool     charging = false;
    int      percent = 0;                 // 0..100
    uint32_t secondsLeft = 0;             // 剩余秒数，0 = 未知
};

// 物理内存。
struct MemoryInfo {
    uint64_t totalBytes = 0;
    uint64_t availableBytes = 0;
    uint64_t pageFileTotalBytes = 0;
    uint64_t pageFileUsedBytes = 0;
    double   usedPercent = 0.0;           // 0..100
};

// 主机信息。cpuName / osName 是静态的，uptimeSeconds 每次调用刷新。
struct HostInfo {
    std::wstring cpuName;                 // "13th Gen Intel(R) Core(TM) i7-1360P"
    std::wstring osName;                  // "Windows 11.0 (build 26100)"（完整描述）
    std::wstring osShort;                 // "Windows 11"（徽章用的短名）
    uint64_t     uptimeSeconds = 0;       // 系统已运行时长
};

// 系统盘（Windows 所在盘）的剩余空间。
DiskSpaceInfo QueryDiskSpace();

// 磁盘活动率。读不出来时 valid=false —— 调用方显示「—」，不抛异常。
DiskActivityInfo QueryDiskActivity();

// 电池状态。
BatteryInfo QueryBattery();

// 物理内存。
MemoryInfo QueryMemory();

// CPU 型号 / OS 版本 / 运行时长。前两项读一次就缓存。
HostInfo QueryHostInfo();

// 秒数 → "3 天 4 小时" / "4 小时 12 分" / "12 分钟"
std::wstring FormatUptime(uint64_t seconds);

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
