// health.h —— 健康分（把 CPU / 内存 / 磁盘三档读数合成一个 0..100 的分）
//
// 【为什么不采温度】
// Windows 没有稳定公开的 CPU 温度接口：能拿到温度的路径要么是厂商私有驱动，
// 要么是 WMI 的 MSAcpi_ThermalZoneTemperature，而后者在绝大多数台式机与
// 笔记本上**根本不存在**（ACPI 热区不是规范要求，厂商可以完全不实现）。
// Mole 的 Windows CLI 文档也明确写了「Windows 上没有温度与风扇读数」。
// 硬塞一个「拿不到就显示 0°C」的温度项，比不做更糟 —— 0°C 会被读成「冻住了」。
// 所以健康分只用三项**到处都拿得到**的读数。
#pragma once

#include <string>

namespace lingwu {
namespace core {
namespace monitor {

struct HealthResult {
    int         score = 0;        // 0..100
    std::wstring grade;           // 优秀 / 良好 / 一般 / 较差
    bool        valid = false;
};

// cpuPercent     : 全系统 CPU 占用（任务管理器口径，0..100）
// memPercent     : 物理内存占用（0..100）
// diskFreePercent: 系统盘剩余百分比（0..100）；传负数表示「取不到」，
//                   此时权重在 CPU 与内存之间重新归一，而不是让整项作废。
//
// 权重：CPU 40% / 内存 40% / 磁盘剩余 20%。
// 磁盘权重最低是因为它变化最慢，却是最不容易自己恢复的那一项 ——
// 磁盘满了会持续拖慢一切，所以不该让它在健康分里完全缺席。
HealthResult ComputeHealth(double cpuPercent, double memPercent, double diskFreePercent);

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
