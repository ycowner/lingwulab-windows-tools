// shell_types.h —— 外壳（MainWindow）与页面（MonitorPage / ServicePage）之间的共享类型
//
// 【为什么单独一个头】
// 合并前两个工具各是一个独立顶层窗口，各管各的对话框与右键菜单。
// 合并成一个窗口之后，两个模块共用**同一个 XamlRoot**，
// 于是有两件事从「各管各的」变成「全局唯一」，必须由外壳统一仲裁：
//
//   ① ContentDialog 的独占权：同一个 XamlRoot 同时只能挂一个 ContentDialog。
//      合并前性能侧有 2 个对话框、服务侧有 2 个，各自一个窗口，互不打架；
//      合并后 4 个对话框共用一个 XamlRoot，谁先 Show 谁占位。
//      → DialogGate。
//
//   ② 右键菜单的命中测试：WinUI 3 的 Microsoft.UI.* 事件一律接不上
//      （cppwinrt 3.0.260818.1 生成的委托模板构造函数漏了 inline），
//      所以行右键菜单靠 Win32 窗口子类拦 WM_CONTEXTMENU 算术定位。
//      窗口子类是**外壳**的（HWND 属于外壳窗口），而列表属于**页面**。
//      → MenuHost 里的 hitTest 回调，由外壳在切换页面时改指向当前页面。
//
// 【为什么页面不反过来持有外壳指针】
// 页面需要外壳的地方只有一处：「设置」按钮。
// 但设置窗口（聚合 / 排序 / 采样间隔 / 开机自启）本来就是性能监视模块的资产，
// 所以它连同「打开设置」「回收设置结果」一起**留在 MonitorPage 里**，
// 页面根本不需要回调外壳。剩下的一切通信都是「外壳 → 页面」单向的，
// 外壳持有页面实现指针，直接调方法即可，不需要任何反向通道。
#pragma once

#include <atomic>
#include <functional>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

namespace winrt::LingwuToolbox::implementation {

// ContentDialog 的独占闸门。
//
// 【为什么必须有】
// 同一个 XamlRoot 同时只能挂一个 ContentDialog。后挂的会把先挂的挤掉，
// 症状是「点 X 之后对话框一闪就没了、窗口也不动」，极难定位。
// 合并成单窗口之后，这个约束从「每个窗口内部唯一」升级成「全局唯一」，
// 所以闸门必须由外壳创建、两个页面共用同一把。
//
// 【为什么是 try_acquire 而不是先 load 再 store】
// 「先判断空闲、再置位」两步之间有窗口期：两个模块几乎同时触发时，
// 都能看到「空闲」，然后都去 Show —— 照样挤掉。
// compare_exchange 是原子的判断+置位，不留窗口期。
class DialogGate {
public:
    // 申请独占。拿不到返回 false，说明已经有对话框在途，调用方应当放弃这次弹出。
    bool try_acquire() noexcept {
        bool expected = false;
        return m_open.compare_exchange_strong(expected, true);
    }

    // 释放。对话框 Closed 时必须成对调用，少一处就会永久锁死。
    void release() noexcept { m_open.store(false); }

    // 当前是否有对话框在途。
    bool is_open() const noexcept { return m_open.load(); }

private:
    std::atomic<bool> m_open{ false };
};

// 窗口子类用的中转结构。
//
// 窗口子类过程是**静态函数**，拿不到 this，只能靠 SetWindowSubclass 的
// refData 传一个稳定地址过来。而「当前是哪个页面」会随导航切换变化，
// 所以这里不能直接存页面指针，改存一个**回调**：
// 外壳在切换页面时把 hitTest 改指向当前活动页面。
struct MenuHost {
    // 当前活动页面的右键命中测试。
    // 传入客户区**物理**像素坐标，返回 true 表示已处理（菜单已弹出）。
    //
    // 【为什么坐标要由页面自己换算】
    // 命中测试要用到 XAML 坐标（DIP）、滚动偏移与行高 —— 这些都是页面自己的
    // 布局信息。外壳不知道当前页面的行高、列表位置、滚动偏移，
    // 硬塞给它只会把页面的布局知识复制一份到外壳里，改一处漏一处。
    std::function<bool(winrt::Windows::Foundation::Point const&)> hitTest;

    // 「把主窗口叫出来」的单实例激活消息。
    // 恢复逻辑由外壳独家持有（MainWindow::ShowAndActivate），
    // 子类只转发 —— 不搞第二套实现。
    std::function<void()> activateMain;
};

}  // namespace winrt::LingwuToolbox::implementation
