// selftest.h —— 数据层离线自检
//
// 用途：不经 UI / COM / XAML 直接把 core 层跑起来，对「编译通过证明不了」的
// 那几条契约做硬断言。服务管理器最怕的静默错误是「列表看着对但全是错的」，
// 而那种错误只有拿真实的 SCM 返回值交叉验证才抓得到。
#pragma once

namespace lingsvc {
namespace core {

// 返回 0 表示全部断言通过；返回值 > 0 是失败项的条数。
// 参数 ticks 保留是为了与 lingwu-sysmon 的命令行形式一致，本工程一次跑完，
// 取值被忽略（传 1 是最省事的写法）。
int RunSelfTest(int ticks);

}  // namespace core
}  // namespace lingsvc
