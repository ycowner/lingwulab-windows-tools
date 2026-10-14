// metrics.h —— 指标数据模型与格式化（header-only）
//
// 全文件包在 namespace lingsysmon::core 里：数据层不污染全局符号表，
// 避免与 WinRT 投影 / WinUI 类型产生同名歧义。
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace lingsysmon {
namespace core {

// ---------------------------------------------------------------- 数据模型

// 单个进程的采样结果
struct ProcInfo {
    DWORD     pid         = 0;
    DWORD     ppid        = 0;
    ULONGLONG createTime  = 0;
    std::wstring name;            // "chrome.exe"（聚合键）
    double    cpuSingle   = 0.0;  // 单核口径，可 >100
    double    cpuNorm     = 0.0;  // 任务管理器口径（除以逻辑核数）
    uint64_t  privWorkSet = 0;    // 私有工作集（字节）
    uint64_t  privBytes   = 0;    // 私有提交字节（PrivatePageCount）
};

// 聚合后的一行（按应用名合并，或按进程各占一行）
struct AppInfo {
    std::wstring name;
    double    cpuSingle  = 0.0;
    double    cpuNorm    = 0.0;
    uint64_t  privWorkSet = 0;
    uint32_t  procCount  = 0;
    uint32_t  pidStart   = 0;     // 在 Snapshot::pids 中的起始下标
    uint32_t  pidCount   = 0;
};

// 网络速率
struct NetInfo {
    uint64_t upBps   = 0;         // 上行 字节/秒
    uint64_t downBps = 0;         // 下行 字节/秒
    std::wstring nic;             // 参与统计的网卡名（逗号分隔）
};

// 一次采样的完整快照
struct Snapshot {
    std::vector<ProcInfo> procs;
    std::vector<AppInfo>  apps;   // 已按当前排序模式排好
    std::vector<DWORD>    pids;   // 所有 app 的 pid 扁平存放
    NetInfo  net;
    ULONGLONG tickMs     = 0;     // 本次采样时刻（GetTickCount64）
    uint32_t coreCount   = 1;
    bool     memIsPrivateWorkingSet = false;  // PDH 不可用时退化为私有提交
};

// ---------------------------------------------------------------- 排序

enum SortMode {
    SORT_MEM_DESC  = 0,   // 默认：内存降序
    SORT_MEM_ASC   = 1,
    SORT_CPU_DESC  = 2,
    SORT_CPU_ASC   = 3,
    SORT_NAME_ASC  = 4,
};

inline const wchar_t* SortModeLabel(int m) {
    switch (m) {
        case SORT_MEM_DESC: return L"内存 ↓（高→低）";
        case SORT_MEM_ASC:  return L"内存 ↑（低→高）";
        case SORT_CPU_DESC: return L"CPU ↓（高→低）";
        case SORT_CPU_ASC:  return L"CPU ↑（低→高）";
        case SORT_NAME_ASC: return L"名称 A→Z";
        default:            return L"内存 ↓（高→低）";
    }
}

// 内存排序统一用哪个字段：PDH 私有工作集优先，否则退回私有提交
inline uint64_t AppMemKey(const AppInfo& a) {
    return a.privWorkSet;
}

// CPU 排序键：把 cpuNorm 量化到 0.01% 再比。
//
// 为什么必须量化，而不是直接比裸 double（2026-10-14 实测）：
// 168 个进程里占绝大多数的 cpuNorm 落在 0.001~0.05 这个区间，彼此几乎都不相等。
// 裸比较下每一次排序都是一次**按浮点噪声的全表重排** —— ListCommit 探针实测
// CPU↓ 每次提交 replaced=136~167（几乎全表 168 个槽位）。两个后果：
//   ① UX：列表每 5 秒整表洗牌，用户根本点不中任何一行；
//   ② 内存：全表 SetAt 让 ListView 重新实现几乎所有容器，触发框架第二次
//      +26 MB 预热，私有内存 132.4 MB 超 ~130 MB 红线。
//
// 量化步长取 0.01% 不是拍脑袋：FormatCpu 的最小显示精度就是 %.2f%%，
// 即 0.01%。**两个进程 CPU 差小于 0.01% 时，界面上显示的数字完全相同** ——
// 此时还按噪声分胜负，等于排序键比用户能看到的信息更细，纯属自伤。
// 量化后它们落进同一档、走名称次级键，顺序稳定；真实的高 CPU 进程
// （差 ≥0.01%）仍按真实值排序，行为与量化前完全一致。
//
// 注意只量化 **CPU** 键。内存键是字节级整数， MEM_DESC 实测 124.4 MB 已达标，
// 且量化内存会改变默认排序行为，不动。
inline double AppCpuSortKey(double cpuNorm) {
    if (!(cpuNorm > 0.0)) return 0.0;              // 夹掉负数和 NaN
    const long long q = static_cast<long long>(cpuNorm * 100.0 + 0.5);
    return static_cast<double>(q) / 100.0;
}

// ---------------------------------------------------------------- 格式化

// 字节数 → "1.23 GB" / "456 MB" / "12.3 KB" / "0 B"
inline std::wstring FormatBytes(uint64_t b) {
    static const wchar_t* kUnits[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    wchar_t buf[64];
    if (b < 1024ull) {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(b));
        return buf;
    }
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
    swprintf_s(buf, L"%.2f %s", v, kUnits[u]);
    return buf;
}

// 字节/秒 → "1.23 MB/s"
inline std::wstring FormatSpeed(uint64_t bytesPerSec) {
    static const wchar_t* kUnits[] = { L"B/s", L"KB/s", L"MB/s", L"GB/s" };
    wchar_t buf[64];
    if (bytesPerSec < 1024ull) {
        swprintf_s(buf, L"%llu B/s", static_cast<unsigned long long>(bytesPerSec));
        return buf;
    }
    double v = static_cast<double>(bytesPerSec);
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
    swprintf_s(buf, L"%.2f %s", v, kUnits[u]);
    return buf;
}

// CPU 百分比 → 自适应精度
inline std::wstring FormatCpu(double p) {
    wchar_t buf[40];
    if (p < 0.0) p = 0.0;
    if (p < 0.005) return L"0%";
    if (p < 10.0)      swprintf_s(buf, L"%.2f%%", p);
    else if (p < 100.0) swprintf_s(buf, L"%.1f%%", p);
    else                swprintf_s(buf, L"%.0f%%", p);
    return buf;
}

// 去掉 .exe 后缀用于展示
inline std::wstring StripExe(const std::wstring& name) {
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        const std::wstring ext = name.substr(dot);
        if (_wcsicmp(ext.c_str(), L".exe") == 0) return name.substr(0, dot);
    }
    return name;
}

// 展示名 = exe 名 + 桌面显示名，形如 "idea64（IntelliJ IDEA）"。
//
// 保留 exe 名在前是有意的：聚合口径是按 exe 名合并的，同一台机器上装了
// 多份同名程序时，用户仍然靠 exe 名区分。friendly 为空（取不到版本资源，
// 或 FileDescription 就是 exe 自己）时只显示 exe 名。
inline std::wstring AppDisplayName(const std::wstring& exeName,
                                   const std::wstring& friendly) {
    const std::wstring base = StripExe(exeName);
    if (friendly.empty() || friendly == base) return base;
    return base + L"（" + friendly + L"）";
}

// 按显示宽度截断，超出部分用省略号（按字符估算，中文按 2 宽）
inline std::wstring Ellipsize(const std::wstring& s, int maxWidthUnits) {
    if (maxWidthUnits <= 1) return L"";
    int w = 0;
    size_t i = 0;
    for (; i < s.size(); ++i) {
        const wchar_t c = s[i];
        // 中日韩区段按 2 个单位宽计
        const bool wide = (c >= 0x1100 && c <= 0x115F) ||
                          (c >= 0x2E80 && c <= 0xA4CF) ||
                          (c >= 0xAC00 && c <= 0xD7A3) ||
                          (c >= 0xF900 && c <= 0xFAFF) ||
                          (c >= 0xFE30 && c <= 0xFE6F) ||
                          (c >= 0xFF00 && c <= 0xFF60) ||
                          (c >= 0xFFE0 && c <= 0xFFE6);
        const int cw = wide ? 2 : 1;
        if (w + cw > maxWidthUnits - 1) {
            return s.substr(0, i) + L"…";
        }
        w += cw;
    }
    return s;
}

}  // namespace core
}  // namespace lingsysmon
