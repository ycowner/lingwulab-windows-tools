// selftest.h —— 性能监视模块的数据层离线自检
//
// 用途：不经 UI 直接把数据层跑起来，把每个采样周期的真实数据打到控制台，
// 并对「移植后最容易悄悄坏掉」的几条契约做硬断言。
//
// 为什么需要它：数据层的定时器与除法链路，编译通过**不能**证明运行期正确 ——
// 「启动 6 秒必崩 0xC0000094」就是编译 100% 通过、运行才炸的例子。
#pragma once

namespace lingwu {
namespace core {
namespace monitor {

// ticks: 采样周期数（每个周期 1 秒）。返回 0 表示全部断言通过，
// 返回值 > 0 是失败项的条数。
int RunSelfTest(int ticks);

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
