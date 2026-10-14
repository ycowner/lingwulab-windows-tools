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
// 写它成本极低、认不认都不影响正确性，所以试一次，但不依赖它 ——
// 真正兜底的是首次运行的一次性提示（MaybeShowTrayHint）。
// 注意：该登记项是 Explorer 收到 NIM_ADD **之后**才建的，所以必须先注册再提升。
namespace platform {

// 托盘菜单命中的动作。用回调而不是让托盘模块直接持有窗口：
// 托盘是纯 Win32 模块，不该知道 WinRT 投影类型的存在。
struct TrayCallbacks {
    // 显示 / 激活主窗口（托盘左键、菜单「显示主界面」）
    std::function<void()> onShowMain;
    // 打开设置窗口
    std::function<void()> onSettings;
    // 真正退出（菜单「退出」）
    std::function<void()> onExit;
    // 切换「按应用聚合」。托盘模块不持有这个状态的副本，
    // 每次弹菜单前通过这个 getter 问一次当前值，避免两边不一致。
    std::function<bool()> getAggregate;
    std::function<void(bool)> setAggregate;
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
UINT ActivateMessageId();

// ---------------------------------------------------------------- 单实例
// 把主窗口 HWND 写进共享内存，供第二份启动时定位第一份的窗口。
// 必须在主窗口创建后、Activate 之前调。
void PublishMainHwnd(HWND h);

// 退出前把共享内存里的 HWND 清掉，避免下一个实例往一个已销毁的窗口发消息。
void ClearPublishedHwnd();

// 读共享内存，拿到「另一个本程序实例」的主窗口 HWND。
// 找不到（或值无效）返回 nullptr。用于单实例第二份启动时激活第一份。
HWND FindOtherInstanceWindow();

// 取单实例互斥量。返回的句柄必须由调用方持有到进程结束（持有即占用）。
// alreadyRunning = true 表示已有另一份在跑，调用方应当激活它然后退出。
HANDLE AcquireSingleInstanceMutex(bool& alreadyRunning);

}  // namespace platform
