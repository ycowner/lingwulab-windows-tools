#pragma once

#include <Windows.h>

#include <functional>

// 系统托盘图标。
//
// 【为什么用一个独立的「消息窗口」当宿主，而不是主窗口的 HWND】
// Shell_NotifyIconW 需要一个 HWND 收回调。三种选法里只有消息窗口是对的：
//   · 主窗口 HWND + SetWindowSubclass：主窗口最小化时子类照样收得到消息，
//     看似可行；但托盘图标的生命周期必须**独立于**主窗口，主窗口一旦真的关闭
//     （X 最小化只是最小化，用户从托盘菜单退出才是真关）托盘就得先撤，
//     两者的生死耦合在一起，退出路径上很容易出现「窗口没了、图标还在」的残留。
//   · 一个可见的隐藏窗口：会多一个任务栏条目 / 一个 Alt+Tab 项。
//   · HWND_MESSAGE 消息窗口：没有可见性、不进任务栏、不进 Alt+Tab，
//     生命周期完全自己控制，消息由 WinUI 的同一个消息循环分发。
//
// 【Explorer 重启必须重新 NIM_ADD】
// Explorer 一崩 / 一重启就会清空**所有**托盘图标，Shell_NotifyIcon 的登记随之
// 失效。程序还活着，但图标再也不会自己出现。所以必须接 TaskbarCreated
// （RegisterWindowMessageW(L"TaskbarCreated")）并重新 NIM_ADD 一次。
// 托盘菜单里也放一个「重新添加托盘图标」作为用户手动兜底。
//
// 【IsPromoted 是未公开字段】
// Win11 默认把新托盘图标收进任务栏右侧的 ^ 溢出区，用户看不到就等于没有图标。
// 公开 API 没有「把自己提到可见区」这个能力。唯一能碰的是 Explorer 写在
// HKCU\Control Panel\NotifyIconSettings\<哈希>\IsPromoted 的未公开 DWORD。
// 写它成本极低、认不认都不影响正确性，所以试一下，但不依赖它 ——
// 真正兜底的是首次运行的一次性提示（MaybeShowTrayHint）。
// 注意：该登记项是 Explorer 收到 NIM_ADD **之后**才建的，而且是**异步**建的，
// 所以必须先注册再提升，而且不能只试一次 —— 见 tray_icon.cpp 里
// TryPromoteTrayIcon 上面的长注释。
namespace platform {

// 托盘菜单里的模块。
//
// 托盘模块是**纯 Win32** 的，不认识「页面」这种 XAML 概念，
// 所以这里只用一个枚举表示「切到哪个模块」，具体怎么切由外壳决定。
//
// 【为什么是六个】
// 导航模型一次定死六个页签：清理 / 应用 / 优化 / 分析 / 状态 / 服务。
// 其中前四个本轮尚未实现，托盘菜单里以**置灰**的形式占位 ——
// 菜单与标题栏的导航长得一样，用户才不会以为「工具箱里没有这个功能」。
// 取值与外壳的 Module 枚举、与 settings.ini 的 LastPage 键一一对应。
enum class TrayModule {
    Clean    = 0,   // 清理（规划中）
    Apps     = 1,   // 应用（规划中）
    Optimize = 2,   // 优化（规划中）
    Analyze  = 3,   // 分析（规划中）
    Status   = 4,   // 状态
    Service  = 5,   // 服务
};

// 托盘菜单命中的动作。用回调而不是让托盘模块直接持有窗口：
// 托盘是纯 Win32 模块，不该知道 WinRT 投影类型的存在。
//
// 【菜单契约（2026-10-25 拍板，与「六个页签」的导航模型对齐）】
//   显示工具箱
//   ─────────
//   清理          ← 规划中，置灰不可点
//   应用          ← 规划中，置灰不可点
//   优化          ← 规划中，置灰不可点
//   分析          ← 规划中，置灰不可点
//   状态          ← 切到状态页
//   服务          ← 切到服务页
//   ─────────
//   设置…           ← SettingsWindow
//   ─────────
//   退出
//
// 相对上一版的**下沉**：
//   · 「按应用聚合」→ 性能监视页命令栏里本来就有 ToggleSwitch，
//     托盘再放一份就是状态双写（两处可能不一致）。
//   · 「重新添加托盘图标」→ 移到设置窗口，它是「动作」不是「状态」，
//     放在设置项列表里更合理。
struct TrayCallbacks {
    // 显示 / 激活主窗口（菜单「显示工具箱」、第二份实例的激活消息）
    std::function<void()> onShowMain;
    // 弹出 / 收起托盘指标面板（托盘**左键**，以及右键菜单里的「指标面板」项）。
    //
    // 【为什么左键不再直接唤出主窗口】
    // 用户点托盘图标时想要的是「看一眼现在的状态」，不是「把主窗口扒拉到前面」。
    // 主窗口是个 900 DIP 宽的大窗，弹出来会盖住半个屏幕；而面板只有 360×530，
    // 看完点「打开工具箱」再唤主窗口更符合意图。
    //
    // 语义是**切换**：面板已显示时再点一次托盘就收起它。
    //
    // 【为什么菜单里也要有这个入口】
    // 左键是隐藏手势，菜单才是看得见的入口 —— 两者指向同一个动作。
    std::function<void()> onShowPanel;
    // 切换到某个模块（菜单「状态」/「服务」）。
    // 实现方要负责**把窗口也显示出来** —— 否则窗口藏在托盘里时，
    // 用户点了「服务」却什么也看不到（菜单动作没有可见效果）。
    std::function<void(TrayModule)> onSelectModule;
    // 当前是哪个模块。菜单据此给当前模块打勾（MF_CHECKED），
    // 否则用户看不出窗口现在停在哪一页。
    std::function<TrayModule()> getActiveModule;
    // 打开设置窗口
    std::function<void()> onSettings;
    // 真正退出（菜单「退出」）
    std::function<void()> onExit;
};

// 创建消息窗口并注册托盘图标。
// 返回 false 通常是 Explorer 还没就绪（开机自启抢在 Explorer 前面启动）——
// 此时不该打扰用户，TaskbarCreated 会把我们叫醒补注册一次。
// 重复调用是安全的（内部先关旧的）。
bool InitTrayIcon(const TrayCallbacks& cb);

// 撤销托盘图标、销毁消息窗口。退出路径必须调，且要在窗口析构之前。
void ShutdownTrayIcon();

// Explorer 重启后重新 NIM_ADD。内部先 NIM_DELETE 再 NIM_ADD。
void RecreateTrayIcon();

// 托盘图标当前是否已成功注册。
bool TrayIconAdded();

// 托盘图标当前的屏幕矩形（**物理像素**，屏幕坐标系）。
//
// 用途：托盘指标面板要贴在托盘图标**正上方居中**，而「托盘图标在哪」
// 只有 shell 知道 —— 任务栏可能在屏幕底部、可能在右侧、可能在多屏的
// 另一块显示器上，写死坐标必然错位。
//
// 【为什么不用 GetCursorPos + 猜托盘位置】
// 托盘图标的位置是 Explorer 自己算的（主/次显示器、任务栏方向、
// 溢出区与否都会影响它），猜出来的位置和真实位置差几十像素是常态。
//
// 返回 false 表示取不到：老系统没有 Shell_NotifyIconGetRect（Windows 10
// 1803 之前）、图标还没注册成功、或者 Explorer 正忙。
// 调用方必须准备好兜底（面板会退回屏幕右下角）。
bool TrayIconRect(RECT* out);

// 首次运行的一次性提示：图标默认被收进 ^ 溢出区，告诉用户去哪儿找。
// trayHintShown 落 ini，只弹一次。非首次运行直接返回。
void MaybeShowTrayHint();

// Explorer 重启时调用：重新 NIM_ADD 补注册托盘图标。
// 由主窗口的 SetWindowSubclass 子类过程转发进来（见下方 TaskbarCreatedMessageId
// 的说明：托盘自己的消息窗口收不到广播）。
void OnTaskbarCreated();

// TaskbarCreated 的消息 id。Explorer 用它广播「我重启了，全部托盘图标已失效」。
//
// 【为什么这个 id 要在这里取，而不是让主窗口自己 RegisterWindowMessage】
// 托盘模块是唯一知道「图标需要重新注册」的一方，消息 id 与处理动作必须同源，
// 分两处注册迟早会不一致。主窗口的子类过程只需要比对 id 然后转发。
//
// 【为什么必须由主窗口来接，托盘自己的消息窗口接不到】
// TaskbarCreated 是 Explorer 用 HWND_BROADCAST 广播的，而 HWND_MESSAGE
// 消息窗口被**明确排除**在广播范围之外。托盘宿主是消息窗口，所以永远收不到；
// 只有主窗口这种真正的顶层窗口能收到，再由它转发给这里。
UINT TaskbarCreatedMessageId();

// 从别的进程发来的「把主窗口叫出来」消息 id（RegisterWindowMessageW 的结果）。
// 主窗口的子类过程靠它识别；0 表示还没注册。
//
// 【单实例相关声明在 single_instance.h】
// 互斥量、HWND 共享内存、激活已有实例这些都归 single_instance 管，
// 托盘模块不重复声明 —— 两边各有一份定义时，链接期会出现同名符号。
//
// 注意：这一段原本在文件里把 TaskbarCreatedMessageId 又声明了一遍。
// 重复声明同一个函数本身合法（所以编译一直是过的），但它是纯粹的噪音：
// 读代码的人会以为「啊，这儿有两个不同的消息 id」。

}  // namespace platform
