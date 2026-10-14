// top10.h —— 聚合（按应用名合并同名进程）+ 排序
#pragma once

#include "metrics.h"

namespace lingsysmon {
namespace core {

// 用 work_.procs 生成 work_.apps / work_.pids
//   aggregate=true  → 同名可执行文件合并成一行（任务管理器「应用」口径）
//   aggregate=false → 每个进程一行
void BuildViews(Snapshot& s, bool aggregate, int sortMode);

}  // namespace core
}  // namespace lingsysmon
