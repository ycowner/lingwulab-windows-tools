#pragma once

// 指标读数的格式化助手。状态页与托盘面板共用同一份。
//
// 【为什么要抽出来，而不是各写一份】
// 「内存 13 GB / 31 GB」「速度 4.89 KB/s」「剩 4:22」这类文案在两个窗口里
// 都要出现。两处各写一份，迟早出现「状态页写「13 GB / 31 GB」而托盘面板
// 写「13.0GB / 31GB」」这种不一致 —— 同一台机器、同一个口径，
// 两种写法会让用户以为这两个数字不是一回事。
//
// 口径也集中在这里：内存一律是**物理内存占用**（不是各进程私有提交之和），
// 速率一律按量级选 KB/s 还是 MB/s。

#include <cstdint>
#include <cstdio>
#include <string>

namespace lingwu {
namespace ui {

// 字节 → 整数 GB（磁贴上大数值用整数，避免 "13.00 GB" 这种小数尾巴）。
inline std::wstring GbText(uint64_t bytes) {
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    return std::to_wstring(static_cast<long long>(gb));
}

// 内存占用率 → 徽章。
inline std::wstring MemoryBadge(double usedPercent) {
    if (usedPercent >= 90.0) return L"紧张";
    if (usedPercent >= 75.0) return L"偏高";
    return L"正常";
}

// 运行时长 → 紧凑徽章（"8 天" / "3 小时"）。
inline std::wstring UptimeBadge(uint64_t seconds) {
    const uint64_t days = seconds / 86400;
    if (days >= 1) return std::to_wstring(days) + L" 天";
    const uint64_t hours = seconds / 3600;
    if (hours >= 1) return std::to_wstring(hours) + L" 小时";
    return std::to_wstring(seconds / 60) + L" 分钟";
}

// 电池剩余时间 → "剩 4:22"。
inline std::wstring RemainingText(uint32_t seconds) {
    wchar_t buf[32];
    swprintf_s(buf, L"剩 %u:%02u", seconds / 3600, (seconds % 3600) / 60);
    return buf;
}

// 速率 → 大数值 + 单位。
//
// 【为什么走量级而不是一律 MB/s】
// 网速常年是几百 KB/s，一律折成 MB/s 会显示成 "0.0 MB/s"，等于没有信息。
inline void SpeedParts(uint64_t bps, std::wstring& value, std::wstring& unit) {
    const double mb = static_cast<double>(bps) / (1024.0 * 1024.0);
    if (mb >= 1.0) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.1f", mb);
        value = buf;
        unit = L"MB/s";
    } else {
        value = std::to_wstring(static_cast<long long>(bps / 1024));
        unit = L"KB/s";
    }
}

// 健康分 → 一句话说明。
inline std::wstring HealthComment(int score) {
    if (score >= 90) return L"当前读数正常";
    if (score >= 75) return L"有轻微压力";
    if (score >= 60) return L"负载偏高";
    return L"负载过高";
}

}  // namespace ui
}  // namespace lingwu
