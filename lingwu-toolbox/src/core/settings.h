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

// 点标题栏 X（或 Alt+F4）时的行为。与 settings.ini 的 CloseAction 键一一对应。
//
// 【为什么定义在设置层，而不是各 UI 文件各写一份】
// 这个三态同时被三处使用：设置窗口的 UI 入口（写）、外壳的关闭拦截（读）、
// ini 的读写（存）。以前 MainWindow.xaml.h 里有 private 的一份、设置窗口
// 只能照抄字面量 0/1/2 —— 改一处漏一处的后果很隐蔽：用户选了「总是退出」，
// 设置窗口显示的却是「总是隐藏到托盘」，而行为确实是退出。
// 设置层是唯一事实源，别的地方引用这里的常量。
constexpr int kCloseAsk            = 0;   // 每次询问（默认）
constexpr int kCloseMinimizeToTray = 1;   // 总是隐藏到托盘
constexpr int kCloseExitProgram    = 2;   // 总是退出程序

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

    // ---- 导航（外壳层）----
    // 上次停留的页签，取值与外壳的 Module 枚举一一对应（0..5）。
    //
    // 【为什么收敛只管到 0..5，不管「哪个页签能用」】
    // 「哪些页签已实现」是导航层的知识，settings 是纯数据层，不该知道。
    // 所以这里只做范围收敛；外壳拿到一个尚未实现的页签时自行退回「状态」。
    //
    // 【什么时候写回 ini】
    // 只在退出时写一次（由外壳的退出路径负责），不是每次切页都写 ——
    // 切页是高频动作，而 ini 写入是同步磁盘 IO。
    int lastPage = 4;

    // ---- 托盘指标面板 ----
    // 面板里「Top 进程」显示几行。0 = 不显示这一段。
    int trayPanelProcessCount = 5;

    // ---- 动作开关状态（跨重启保持）----
    //
    // 「保持常亮」= SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED |
    // ES_SYSTEM_REQUIRED)。它是**动作**而不是「状态」，但用户按下去之后
    // 期望它一直是开的，所以要落 ini。
    bool keepAwake = false;

    // 「清理屏幕」是否锁定输入（BlockInput）。
    //
    // 【为什么默认开】
    // 清理屏的用途就是「挡住误触」—— 演示、投屏、录屏时手会不自觉地
    // 碰到键盘鼠标把当前窗口切走。不锁输入的话，清理屏一半的时间
    // 是在跟误触打架，那这个功能等于没做。
    //
    // 【为什么仍然留成可关的开关】
    // BlockInput 会把整台机器的键鼠输入锁住（只有调用它的那个线程不受影响），
    // 对某些场景（需要边清理屏边操作别的程序）反而是麻烦。
    // 「默认开 + 可随时关掉」比「默认关 + 每次都要记得去开」更符合这个功能的本意。
    //
    // 【退出路径只有一条】
    // 锁上之后唯一的出口是 ESC（见 TrayPanel 的 EnterCleanScreen / ExitCleanScreen），
    // 进程被杀也会由系统释放锁。这两条路径都必须可靠，否则等于把用户的电脑锁死。
    bool cleanScreenLockInput = true;

    // ---- 生命周期 ----
    // autoStart 不落 ini —— 注册表才是唯一事实源，见 core/monitor/autostart.cpp。
    bool autoStart     = false;
    bool trayHintShown = false;  // 托盘图标在 ^ 隐藏区的一次性提示已弹过

    // 点标题栏 X 时的行为：0 = 每次询问（默认）、1 = 总是隐藏到托盘、2 = 总是退出程序。
    // 取值常量见本文件上方的 kCloseAsk / kCloseMinimizeToTray / kCloseExitProgram。
    //
    // 以前这里没有任何 UI 入口能改这个值（设为 1 或 2 之后只能手改 ini 改回 0），
    // Phase 3 起由设置窗口的「关闭窗口时」三态控件写入。
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
