// selftest.h —— 数据层离线自检的统一入口
//
// 用途：不启动 UI，直接把数据层跑起来，把真实数据打到控制台，
// 并对「编译通过**不能**证明运行期正确」的那几条契约做硬断言。
//
// 【为什么是编排而不是合并】
// 两侧的输出格式本来就不同：性能侧是宽字符表格（中文要对齐），
// 服务侧是窄字符逐条 PASS/FAIL。合成一个文件会让「改一边会不会弄坏另一边」
// 变得难以判断 —— 而这正是这个仓库最在意的那类风险。
// 所以两侧的实现各自留在 core/monitor/selftest.cpp 与 core/service/selftest.cpp，
// 这里只负责按顺序跑完并汇总失败数。
//
// 【为什么必须在 init_apartment 之前调用】
// 自检的价值在于「数据层可独立运行」。一旦 XAML 框架被拉起来，
// 私有内存会直接多出 60 MB，自检想验证的那件事就失去意义了。
// 调用点在 main.cpp 的 --selftest 分支里，位置就在 init_apartment 之前。
#pragma once

namespace lingwu {
namespace core {

// ticks: 性能监视侧的采样周期数（每个周期 1 秒）。
// 返回 0 表示两侧断言全部通过；返回值 > 0 是失败项的总条数
// （性能侧 + 服务侧相加）。
int RunSelfTest(int ticks);

}  // namespace core
}  // namespace lingwu
