// settings.h —— 设置持久化（%APPDATA%\LingwuServiceManager\settings.ini）
//
// 路径 / 段头 / 键名是本工程的对外契约，改之前先想清楚要不要迁移旧值。
#pragma once

#include "core/service_model.h"

#include <string>

namespace lingsvc {
namespace core {

struct Settings {
    SortMode  sortMode  = SortMode::NameAsc;
    FilterMode filterMode = FilterMode::All;
    bool detailExpanded = false;

    // 窗口位置与尺寸。left/top 为负表示上次退出时窗口在屏幕外
    // （换显示器/拔外接屏的常见情形），此时一律退回默认布局。
    int left = INT_MIN;
    int top = INT_MIN;
    int width = 0;
    int height = 0;
};

bool LoadSettings(Settings& s);
void SaveSettings(const Settings& s);
std::wstring SettingsDir();

}  // namespace core
}  // namespace lingsvc
