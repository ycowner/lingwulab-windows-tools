// selftest.cpp —— 数据层离线自检的统一编排入口
//
// 两侧各有一套断言（性能监视 8 组、服务管理 7 组），实现分别放在
// core/monitor/selftest.cpp 与 core/service/selftest.cpp，
// 各自在自己的命名空间里，互不干扰。
// 这里只做编排：按顺序跑完两侧，把失败数加起来。
//
// 【为什么要编排而不是合并成一个文件】
// 两侧的输出格式本来就不同：性能侧是宽字符表格（中文要对齐），
// 服务侧是窄字符逐条 PASS/FAIL。合成一个文件会让「改一边会不会弄坏另一边」
// 变得难以判断 —— 而这正是这个仓库最在意的那类风险。
#include "pch.h"

#include "selftest.h"

#include "core/monitor/selftest.h"
#include "core/service/selftest.h"

#include <cstdio>

namespace lingwu {
namespace core {

int RunSelfTest(int ticks) {
    std::printf("LingwuToolbox --selftest\n");

    // 性能监视侧：采样 N 个周期，每周期做硬断言。
    const int monitorFail = monitor::RunSelfTest(ticks);

    std::printf("\n");

    // 服务管理侧：枚举真实 SCM，交叉验证字段与状态。
    const int serviceFail = service::RunSelfTest(1);

    const int total = monitorFail + serviceFail;
    std::printf("\n=====================================================\n");
    std::printf("LingwuToolbox selftest: %d failure(s) "
                "(monitor %d, service %d)\n",
                total, monitorFail, serviceFail);
    std::printf("=====================================================\n");
    return total;
}

}  // namespace core
}  // namespace lingwu
