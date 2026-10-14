// settings.h —— 设置持久化（%APPDATA%\LingwuSysMon\settings.ini）
//
// 路径 / 段头 / 键名是本工程的对外契约，不要改。
#pragma once

#include <windows.h>
#include <string>

namespace lingsysmon {
namespace core {

struct Settings {
    // 数据
    bool aggregate   = true;   // 按应用聚合（false = 逐进程）
    int  sortMode    = 0;      // SortMode，默认内存降序
    int  intervalMs  = 1000;   // 采样间隔

    // 生命周期
    // autoStart 不落 ini —— 注册表才是唯一事实源，见 autostart.cpp 的 AutostartReconcile。
    bool autoStart     = false;
    bool trayHintShown = false;  // 托盘图标在 ^ 隐藏区的一次性提示已弹过

    // 点标题栏 X 时的行为：0 = 每次询问（默认）、1 = 总是隐藏到托盘、2 = 总是退出程序。
    // 取值与 MainWindow 里的 kCloseAsk / kCloseMinimizeToTray / kCloseExitProgram 一一对应。
    //
    // 已知缺口：设为 1 或 2 之后**没有 UI 入口能改回 0**，
    // 只能手改本文件把 CloseAction 置 0。见 README「关闭窗口时的行为」。
    int  closeAction   = 0;
};

bool LoadSettings(Settings& s);
void SaveSettings(const Settings& s);
std::wstring SettingsDir();

}  // namespace core
}  // namespace lingsysmon
