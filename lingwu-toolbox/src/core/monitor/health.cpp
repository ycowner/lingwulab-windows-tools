// health.cpp —— 健康分算法
#include "pch.h"

#include "health.h"

namespace lingwu {
namespace core {
namespace monitor {

namespace {

// 「越小越好」的因子映射到 0..100。
// goodBelow 以下满分，badAbove 以上 0 分，中间线性。
double ScoreLowIsGood(double v, double goodBelow, double badAbove) {
    if (v <= goodBelow) return 100.0;
    if (v >= badAbove)  return 0.0;
    return 100.0 * (badAbove - v) / (badAbove - goodBelow);
}

// 「越大越好」的因子映射到 0..100。
double ScoreHighIsGood(double v, double goodAbove, double badBelow) {
    if (v >= goodAbove) return 100.0;
    if (v <= badBelow) return 0.0;
    return 100.0 * (v - badBelow) / (goodAbove - badBelow);
}

}  // namespace

HealthResult ComputeHealth(double cpuPercent, double memPercent, double diskFreePercent) {
    HealthResult out;
    if (cpuPercent < 0.0 || memPercent < 0.0) return out;

    const double cpuScore = ScoreLowIsGood(cpuPercent, 25.0, 100.0);
    const double memScore = ScoreLowIsGood(memPercent, 60.0, 95.0);

    double score = 0.0;
    if (diskFreePercent >= 0.0) {
        const double diskScore = ScoreHighIsGood(diskFreePercent, 30.0, 2.0);
        // 权重：CPU 40% / 内存 40% / 磁盘剩余 20%
        score = cpuScore * 0.4 + memScore * 0.4 + diskScore * 0.2;
    } else {
        // 磁盘剩余取不到时**不把整项作废**，而是把权重在 CPU 与内存之间归一。
        // 否则一台「GetDiskFreeSpaceEx 失败」的机器会永远显示「健康分不可用」，
        // 而它其实有 CPU 与内存两项完全正常的读数。
        score = cpuScore * 0.5 + memScore * 0.5;
    }

    if (score < 0.0)  score = 0.0;
    if (score > 100.0) score = 100.0;

    out.score = static_cast<int>(score + 0.5);
    out.grade = (out.score >= 90) ? L"优秀"
              : (out.score >= 75) ? L"良好"
              : (out.score >= 50) ? L"一般"
                                  : L"较差";
    out.valid = true;
    return out;
}

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
