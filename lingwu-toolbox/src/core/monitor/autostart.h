// autostart.h —— 开机自启动（HKCU\...\Run，无需管理员权限）
#pragma once

#include <windows.h>
#include <string>

namespace lingwu {
namespace core {
namespace monitor {

enum StartupMode {
    STARTUP_NORMAL    = 0,  // 手动启动：显示主界面
    STARTUP_AUTOSTART = 1,  // 被自启拉起：静默，只进托盘
    STARTUP_SHOW      = 2,  // 手动指定 --show：显示主界面
};

// 解析命令行，得到启动方式
StartupMode ParseStartupMode();

// 自注册表里的完整命令行（找不到返回空串）
std::wstring AutostartRegisteredCommand();

// 当前 exe 应写入自注册表的命令行："<绝对路径>" --autostart
std::wstring AutostartCommandForSelf();

// 注册表里是否存在自启项
bool AutostartIsEnabled();

// 开启：写入并回读校验。失败时 err 返回原因（中文，可直接弹窗）
bool AutostartEnable(std::wstring& err);

// 关闭：删除。值不存在也算成功
bool AutostartDisable(std::wstring& err);

// 自愈：对比注册表值与自身路径。
//   stale = true  → 注册表指向的是别的路径（exe 被移动过），需提示用户重开
//   mismatch = true → 注册表状态与期望不一致，需要修正
void AutostartReconcile(bool& enabled, bool& stale);

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
