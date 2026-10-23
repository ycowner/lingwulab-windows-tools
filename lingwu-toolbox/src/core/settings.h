// settings.h —— 统一设置持久化（%APPDATA%\LingwuToolbox\settings.ini）
//
// 【为什么键名要加模块前缀】
// 合并前有两个工具各写各的 settings.ini，键名空间互不相干：
//     sysmon 的 SortMode  = 0..4  内存↓/内存↑/CPU↓/CPU↑/名称↑
//     svcmgr 的 SortMode  = 0..5  名称/状态/启动类型（双向）
// 两个都叫 SortMode，语义完全无关。合进同一个 ini 后若还用同名键，
// 后写入的一方会把另一方覆盖掉 —— 用户会发现「切到服务页排序方式被重置」。
// 所以改成 MonitorSortMode / ServiceSortMode。
//
// 【路径 / 段头是本工程的对外契约，不要改】
// 段头固定 [Settings]，键名见下表。
#pragma once

#include <windows.h>
#include <string>

namespace lingwu {
namespace core {

struct Settings {
    // ---- 性能监视模块 ----
    bool aggregate   = true;   // 按应用聚合（false = 逐进程）
    int  sortMode    = 0;      // MonitorSortMode，默认内存降序（0..4）
    int  intervalMs  = 1000;   // MonitorIntervalMs，采样间隔

    // ---- 服务管理模块 ----
    int  serviceSortMode  = 0;  // ServiceSortMode，0..5（名称/状态/启动类型双向）
    int  serviceFilterMode = 0; // ServiceFilterMode，0..4（全部/运行中/已停止/已禁用/第三方）
    bool serviceDetailExpanded = false; // ServiceDetailExpanded

    // ---- 窗口位置尺寸（外壳主窗口）----
    // left/top 为负表示上次退出时窗口在屏幕外
    // （换显示器/拔外接屏的常见情形），此时一律退回默认布局。
    int left = INT_MIN;
    int top = INT_MIN;
    int width = 0;
    int height = 0;

    // ---- 生命周期 ----
    // autoStart 不落 ini —— 注册表才是唯一事实源，见 core/monitor/autostart.cpp。
    bool autoStart     = false;
    bool trayHintShown = false;  // 托盘图标在 ^ 隐藏区的一次性提示已弹过

    // 点标题栏 X 时的行为：0 = 每次询问（默认）、1 = 总是隐藏到托盘、2 = 总是退出程序。
    // 取值与 MainWindow 里的 kCloseAsk / kCloseMinimizeToTray / kCloseExitProgram 一一对应。
    //
    // 已知缺口：设为 1 或 2 之后**没有 UI 入口能改回 0**，
    // 只能手改这个文件把 CloseAction 置 0。
    int  closeAction   = 0;
};

bool LoadSettings(Settings& s);
void SaveSettings(const Settings& s);
std::wstring SettingsDir();

// 从两个旧工具的 ini 迁移设置值。
//
// 合并前：%APPDATA%\LingwuSysMon\settings.ini 与
//         %APPDATA%\LingwuServiceManager\settings.ini
// 合并后：%APPDATA%\LingwuToolbox\settings.ini
//
// 【为什么需要迁移】用户已经调过间隔、排序、窗口位置，
// 换工具箱后全部退回默认会被当成「合并把设置弄丢了」。
//
// 【迁移的安全边界】
//   · 只在**新 ini 不存在**时执行一次（用新 ini 是否存在作为「迁移过」的标记）；
//   · 旧文件**保留不删** —— 迁移是加法不是搬家，删了就没法回退；
//   · 迁移只搬**本模块负责**的键：monitor 侧只读 Aggregate/SortMode/IntervalMs，
//     service 侧只读 SortMode/FilterMode/DetailExpanded/Window*，
//     绝不按新键名去旧文件里找（那会读到别的语义）。
// 返回 true 表示本次执行了迁移。
bool MigrateLegacySettings();

}  // namespace core
}  // namespace lingwu
