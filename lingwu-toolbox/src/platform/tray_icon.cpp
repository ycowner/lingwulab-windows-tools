#include "pch.h"

#include "tray_icon.h"

#include "callback_guard.h"
#include "core/settings.h"
#include "single_instance.h"
#include "trace.h"

#include <shellapi.h>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")

// 本文件全部是纯 Win32 代码，不持有任何 WinRT 投影对象。
// 与 WinUI 世界的唯一接口是 InitTrayIcon 传进来的那组 std::function。
//
// 唯一的例外：callback_guard.h 里的 winrt::hresult_error。那是**异常类型**
// 而不是投影对象，用它把错误码落进 trace —— 托盘链路上的回调最终会碰
// WinRT，抛出来的异常必须在这个边界上被兜住，绝不能穿过窗口过程
// （穿过去 = CRT std::terminate = 0xC0000409 = 进程直接消失）。
// 详见 callback_guard.h 顶部的长注释。

namespace platform {

// Shell_NotifyIconW 协商到的通知码版本。0 = 旧码（WM_LBUTTON*），
// NOTIFYICON_VERSION_4 = 新码（NIN_*）。
//
// 【为什么它是 platform 命名空间的变量，而不是匿名命名空间的】
// InitTrayIcon 要写它、TrayHostProc 要读它，而 TrayHostProc 在匿名
// namespace 里、InitTrayIcon 在外面。放在匿名命名空间里 InitTrayIcon
// 看不到，直接编译不过。详见 TrayHostProc 上方的长注释。
UINT g_version = 0;

namespace {

constexpr UINT    kTrayUid     = 1;
constexpr UINT    WM_TRAY_CB   = WM_APP + 0x2F;
constexpr wchar_t kWndClass[]  = L"LingwuToolbox_TrayHost";

// 提升到可见区的重试用定时器。
// id 1 是 NIM_ADD 失败后的重试（见 TrayHostProc），id 2 是「IsPromoted 写入」
// 的重试 —— 两者原因完全不同，不要混用同一个 id。
constexpr UINT    kPromoteTimerId  = 2;
constexpr UINT    kPromoteRetryMs  = 400;
constexpr int     kPromoteMaxTries = 12;   // 约 4.8 秒

// NOTIFYICON_VERSION_4 起 Explorer 用的通知码。旧码也一并接：
// 任何一套漏掉都会在某版 Windows 上表现成「托盘图标点了没反应」。
// 这两个宏在 shellapi.h 里已有定义，这里不重复定义（重复会 C2059）。

NOTIFYICONDATAW g_nid{};
bool            g_added     = false;
HWND            g_host      = nullptr;
ATOM            g_atom      = 0;
HICON           g_icon      = nullptr;    // 自己 LoadImage 出来的，退出要 DestroyIcon
bool            g_iconOwned = false;
TrayCallbacks   g_cb;

// 提升重试已经试了多少次。
int             g_promoteTries = 0;

// 提升到可见区成没成。
// g_promoted       -- 已经提到可见区了（不用再提示用户去 ^ 里找）
// g_promotionSettled -- 提升这件事已经有结论了（成功或已放弃）
//                    没结论之前不能弹「图标在 ^ 里」的提示 —— 提升成功的机器上
//                    图标就在可见区，那时候弹这个提示是错的。
bool            g_promoted        = false;
bool            g_promotionSettled = false;

// ---------------------------------------------------------------- 自启动提示
// trayHintShown 在 ini 里，所以这里直接读写 core::Settings。
// 托盘模块与主界面共用同一份设置文件，没有第二份真相。
void MaybeShowTrayHintImpl() {
    if (!g_added) return;
    // 提升成功了，图标就在可见区，不用提示用户去 ^ 里翻。
    if (g_promoted) return;
    lingwu::core::Settings s;
    if (!lingwu::core::LoadSettings(s)) return;
    if (s.trayHintShown) return;

    s.trayHintShown = true;
    lingwu::core::SaveSettings(s);

    // 不用 MessageBoxW：它会弹一个**模态**的 Win32 对话框，出现在托盘初始化
    // 路径上会把 XAML 的启动时序搅乱（开机自启时主窗口还没 Activate）。
    // 改成非模态的提示气泡，挂在托盘图标上，10 秒自动消失。
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, L"托盘图标在哪儿", _TRUNCATE);
    wcsncpy_s(nid.szInfo,
              L"图标默认被 Windows 收进了「隐藏图标」区（任务栏右侧的 ^ 里）。\n"
              L"点开 ^ 把零物工具箱拖到可见区域即可。",
              _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------- IsPromoted
bool SelfPath(wchar_t* buf, int cch) {
    const DWORD n = GetModuleFileNameW(nullptr, buf, cch);
    return n > 0 && n < static_cast<DWORD>(cch);
}

bool SubKeyMatchesSelf(const wchar_t* subKey, const wchar_t* self) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    wchar_t path[MAX_PATH] = {0};
    DWORD type = 0, cb = sizeof(path);
    const LSTATUS r = RegQueryValueExW(k, L"ExecutablePath", nullptr, &type,
                                       reinterpret_cast<BYTE*>(path), &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    path[MAX_PATH - 1] = L'\0';
    return _wcsicmp(path, self) == 0;
}

// 写 IsPromoted=1 把所有属于本程序的登记项提常驻。认不认都不影响正确性。
//
// 返回值：
//   0 = 一条属于本程序的登记项都还没找到（Explorer 还没建） —— 调用方要重试
//   1 = 找到了，而且已经是 IsPromoted=1 —— 不用动，也不用让 Explorer 重读
//   2 = 找到了并写进了 IsPromoted=1 —— 需要让 Explorer 重新读一次才会生效
//
// 【为什么必须重试 —— 这是实测踩到的真缺陷】
//   Explorer 建 NotifyIconSettings\<哈希> 这条登记项是**异步**的：
//   NIM_ADD 返回时它还不存在，要过一小会儿才建好。
//   所以「NIM_ADD 之后立刻调一次本函数」在**首次运行**的机器上必然一条都找不到
//   （那台机器本来就没有这条登记项），静默什么也没做 ——
//   结果图标被留在 Win11 的 ^ 隐藏图标区，用户在任务栏上根本看不到。
//   实测：注册表里没有对应登记项时，函数返回「找到 0 条」，
//   托盘图标只出现在 `TopLevelWindowForOverflowXamlIsland` 那个溢出浮窗里，
//   可见区 `Shell_TrayWnd` 里一个也没有。
//   而机器上留着一条旧登记项时，同样的代码就能把图标提到可见区 ——
//   这个「有没有旧登记项」的差异，正好说明问题出在时序而不是逻辑。
//
//   所以这里返回 0 时不能放弃，要由定时器接着试（见 kPromoteTimerId）。
int TryPromoteTrayIcon() {
    wchar_t self[MAX_PATH] = {0};
    if (!SelfPath(self, MAX_PATH)) return 0;

    const wchar_t* rootPath = L"Control Panel\\NotifyIconSettings";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, rootPath, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return 0;

    int found = 0;
    bool wrote = false;
    for (DWORD i = 0;; ++i) {
        wchar_t name[128] = {0};
        DWORD cbName = ARRAYSIZE(name);
        if (RegEnumKeyExW(root, i, name, &cbName, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS)
            break;   // ERROR_NO_MORE_ITEMS

        wchar_t sub[256] = {0};
        if (swprintf_s(sub, ARRAYSIZE(sub), L"%s\\%s", rootPath, name) < 0) continue;
        if (!SubKeyMatchesSelf(sub, self)) continue;
        found++;

        // 已经是 1 就别再写 —— 写了也算「需要 Explorer 重读」的话，
        // 会白白多一次 NIM_DELETE + NIM_ADD 的图标闪烁。
        // RegQueryValueExW 的签名是 (hKey, lpValueName, dwReserved, lpType,
        // lpData, lpcbData) —— 六个参数，子键名在 hKey 里，不再单独传。
        DWORD promoted = 0;
        DWORD cbValue = sizeof(promoted);
        DWORD type = 0;
        const LSTATUS got = RegQueryValueExW(
            HKEY_CURRENT_USER, L"IsPromoted", 0, &type,
            reinterpret_cast<BYTE*>(&promoted), &cbValue);
        if (got == ERROR_SUCCESS && type == REG_DWORD && promoted == 1) {
            continue;
        }

        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            const DWORD one = 1;
            RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&one), sizeof(one));
            RegCloseKey(k);
            wrote = true;
        }
    }
    RegCloseKey(root);

    if (found == 0) return 0;
    return wrote ? 2 : 1;
}

// 让 Explorer 重新读一次 IsPromoted。
//
// 【为什么写完还得再 NIM_ADD 一次】
//   Explorer 是在**图标被加入的那一刻**读 IsPromoted 的。
//   之后我们再把那个值改成 1，Explorer 不知道，它照旧把图标摆在溢出区。
//   所以必须先撤（NIM_DELETE）再加（NIM_ADD），让它重来一遍。
void RefreshTrayIconAfterPromote() {
    if (!g_host || !IsWindow(g_host)) return;

    NOTIFYICONDATAW del = g_nid;
    del.uFlags = 0;
    Shell_NotifyIconW(NIM_DELETE, &del);
    g_added = false;

    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_added = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);
    if (g_added) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    } else {
        // 【为什么这里必须兜住】
        // 上面是先撤再加，所以万一这一次的 NIM_ADD 失败，图标就**真的没了** ——
        // 而它本来是有的。绝不能让一次「提升」把图标搞丢。
        // 所以失败时起一个重试定时器，走的是 NIM_ADD 失败的老路径
        // （id 1 的处理器会调 RecreateTrayIcon 重新注册）。
        LINGWU_TRACE("tray: refresh after promotion failed err=%lu, scheduling a retry",
                     GetLastError());
        SetTimer(g_host, 1, 1500, nullptr);
    }
    LINGWU_TRACE("tray: refreshed after promotion, added=%d", g_added ? 1 : 0);
}

// ---------------------------------------------------------------- 菜单
constexpr UINT IDM_TRAY_MAIN     = 0x1001;
constexpr UINT IDM_TRAY_SETTINGS = 0x1002;
constexpr UINT IDM_TRAY_EXIT     = 0x1003;
constexpr UINT IDM_TRAY_MONITOR  = 0x1010;
constexpr UINT IDM_TRAY_SERVICE  = 0x1011;

void ShowTrayMenu(HWND owner) {
    HMENU m = CreatePopupMenu();
    if (!m) return;

    // RAII：菜单句柄一定要还回去。
    // 原来 DestroyMenu 在函数末尾，中途任何一处抛异常都会把它漏掉。
    // （有了 GuardCallback 之后异常已经不会逃出，但这是双保险。）
    struct MenuGuard {
        explicit MenuGuard(HMENU h_) : h(h_) {}
        ~MenuGuard() { if (h) ::DestroyMenu(h); }
        HMENU h;
    };
    MenuGuard menuGuard{ m };

    // 当前模块 —— 用来给「性能监视 / 服务管理」打勾。
    // 托盘模块不持有这个状态的副本，每次弹菜单前问一次，避免两边不一致。
    const TrayModule active =
        g_cb.getActiveModule ? g_cb.getActiveModule() : TrayModule::Monitor;

    // 菜单契约见 tray_icon.h 的 TrayCallbacks 注释。
    AppendMenuW(m, MF_STRING, IDM_TRAY_MAIN, L"显示工具箱");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (active == TrayModule::Monitor ? MF_CHECKED : 0),
                IDM_TRAY_MONITOR, L"性能监视");
    AppendMenuW(m, MF_STRING | (active == TrayModule::Service ? MF_CHECKED : 0),
                IDM_TRAY_SERVICE, L"服务管理");
    AppendMenuW(m, MF_STRING, IDM_TRAY_SETTINGS, L"设置…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"退出");

    POINT pt{};
    GetCursorPos(&pt);
    // 必须 SetForegroundWindow，否则菜单弹完点空白处不消失（经典 Win32 坑）。
    SetForegroundWindow(owner);
    int cmd = 0;
    // TrackPopupMenu 本身是纯 Win32、不会抛 C++ 异常，但这里仍然包一层，
    // 保持「菜单这条边界上的一切都在兜底层里」的一致性。
    platform::GuardCallback("tray.trackPopup", [&]{
        cmd = static_cast<int>(TrackPopupMenu(
            m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr));
    });
    // TrackPopupMenu 配合 TPM_RETURNCMD 返回后要补一条 WM_NULL，
    // 否则偶尔出现菜单已消失但按键消息被吃掉的现象。
    PostMessageW(owner, WM_NULL, 0, 0);

    // 五个菜单项各自兜一层，tag 直接指明是哪一项出的问题 ——
    // 出事时 trace 里只有这一行，读的人没有第二次机会去猜。
    //
    // 【这里为什么必须有兜底】g_cb.onXxx 最终会调到 MainWindow 的成员函数，
    // 那些函数里会碰 WinRT（AppWindow.Show / Window.Activate / XAML 属性设值）。
    // 任何一个抛 hresult_error，异常就会一路往上穿过本函数、穿过
    // TrayHostProc 这个 LRESULT CALLBACK 窗口过程 —— user32 层没有 C++ handler，
    // CRT 只能 std::terminate → abort() → 0xC0000409，进程直接没了。
    // 2026-10-20 的真实崩溃就是这么来的。
    switch (cmd) {
        case IDM_TRAY_MAIN:
            platform::GuardCallback("tray.onShowMain", [&]{
                if (g_cb.onShowMain) g_cb.onShowMain();
            });
            break;
        case IDM_TRAY_MONITOR:
            platform::GuardCallback("tray.onSelectModule.monitor", [&]{
                if (g_cb.onSelectModule) g_cb.onSelectModule(TrayModule::Monitor);
            });
            break;
        case IDM_TRAY_SERVICE:
            platform::GuardCallback("tray.onSelectModule.service", [&]{
                if (g_cb.onSelectModule) g_cb.onSelectModule(TrayModule::Service);
            });
            break;
        case IDM_TRAY_SETTINGS:
            platform::GuardCallback("tray.onSettings", [&]{
                if (g_cb.onSettings) g_cb.onSettings();
            });
            break;
        case IDM_TRAY_EXIT:
            platform::GuardCallback("tray.onExit", [&]{
                if (g_cb.onExit) g_cb.onExit();
            });
            break;
        default:
            break;
    }
    // DestroyMenu 由 menuGuard 负责，这里不再调。
}

// ---------------------------------------------------------------- 消息窗口
//
// 【为什么左键要按「协商到的版本」分流，而不是把新旧通知码一起接】
// NOTIFYICON_VERSION_4 起，Explorer 对左键不再发 WM_LBUTTONUP，改发
// NIN_SELECT（= WM_USER + 0）。但 NIM_SETVERSION 是「尽力而为」：
// 老版本 shell 会**静默**忽略它继续发旧码，两种码在某些 shell 上甚至会
// 同时到达。四类码一起接，实测表现为一次点击把 ShowAndActivate 跑两遍
// （trace 里同一毫秒出现两条）。虽然幂等无害，但托盘左键是最高频入口，
// 双跑会放大成「窗口抖动」，且掩盖真正的问题。
//
// 所以：记录 NIM_SETVERSION 的真实协商结果，只有 shell 认了 4 才收 NIN_*，
// 没认（或协商失败）才收旧的 WM_LBUTTON*。右键两版都发 WM_CONTEXTMENU，
// 不受影响。
//
// 注意定义位置：必须挂在 platform 命名空间而非匿名命名空间 ——
// InitTrayIcon 在匿名 namespace 之外，看不到匿名命名空间里的变量。
LRESULT CALLBACK TrayHostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            // NIM_ADD 失败（Explorer 未就绪）时的重试。只重试几次，
            // 无限重试会在 Explorer 长时间起不来的机器上变成后台忙等。
            if (wp == 1) {
                KillTimer(hwnd, 1);
                if (!g_added) RecreateTrayIcon();
                return 0;
            }
            // 「把图标提到可见区」的重试。Explorer 建那条登记项是异步的，
            // 第一次找不到是正常的，所以要一直试到它出现为止 ——
            // 见 TryPromoteTrayIcon 上面的长注释。
            if (wp == kPromoteTimerId) {
                const int r = TryPromoteTrayIcon();
                if (r != 0) {
                    // r == 2 表示我们刚写进去 IsPromoted=1，
                    // Explorer 只在图标被加入的那一刻读这个值，所以要重新加一次。
                    if (r == 2) RefreshTrayIconAfterPromote();
                    g_promoted = true;
                    g_promotionSettled = true;
                    KillTimer(hwnd, kPromoteTimerId);
                    LINGWU_TRACE("tray: promoted to the visible tray");
                    return 0;
                }
                if (++g_promoteTries >= kPromoteMaxTries) {
                    KillTimer(hwnd, kPromoteTimerId);
                    g_promotionSettled = true;
                    LINGWU_TRACE("tray: promote gave up after %d tries; the icon stays in the overflow",
                                 g_promoteTries);
                    // 提升没成功，图标多半留在 ^ 溢出区 —— 这时候才提示用户去哪儿找。
                    MaybeShowTrayHintImpl();
                }
                return 0;
            }
            break;

        case WM_TRAY_CB: {
            const UINT code = LOWORD(lp);
            LINGWU_TRACE("tray: callback code=0x%04X version=%u", code, g_version);
            // 整段通知码处理都在兜底层里。
            //
            // 【为什么这里必须有兜底】下面会调到 g_cb.onXxx，而那些回调最终会碰
            // WinRT。C++ 异常从本窗口过程（LRESULT CALLBACK）逃出去之后，
            // user32 层没有 C++ handler，CRT 只能 std::terminate → abort() →
            // 0xC0000409，进程直接消失且现场全丢。详见 callback_guard.h。
            //
            // 注意：原来的 return 语句在这个 lambda 里会**从 lambda 返回**，
            // 不是从窗口过程返回 —— 写错会静默改变消息语义。所以这里改用
            // if / else if 结构，窗口过程本身照常 return 0。
            platform::GuardCallback("tray.cb", [&]{
                // 右键：两版 shell 都发 WM_RBUTTONUP / WM_CONTEXTMENU。
                // 两个都接是因为有的 shell 只发其中一个（只发 WM_RBUTTONUP 时
                // 不收它就没有菜单，只发 WM_CONTEXTMENU 时同理）。
                if (code == WM_RBUTTONUP || code == WM_CONTEXTMENU) {
                    ShowTrayMenu(hwnd);
                } else if (g_version >= NOTIFYICON_VERSION_4) {
                    // 左键 / 回车：把主窗口叫出来。按协商到的版本分流，
                    // 见函数上方注释。
                    if (code == NIN_SELECT || code == NIN_KEYSELECT) {
                        if (g_cb.onShowMain) g_cb.onShowMain();
                    }
                } else {
                    if (code == WM_LBUTTONUP || code == WM_LBUTTONDBLCLK) {
                        if (g_cb.onShowMain) g_cb.onShowMain();
                    }
                }
            });
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool EnsureHostWindow() {
    if (g_host && IsWindow(g_host)) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = TrayHostProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    if (!g_atom) {
        g_atom = RegisterClassExW(&wc);
        if (!g_atom) {
            LINGWU_TRACE("tray: RegisterClassExW failed err=%lu", GetLastError());
            return false;
        }
    }

    // HWND_MESSAGE：消息窗口。没有可见性、不进任务栏、不进 Alt+Tab。
    g_host = CreateWindowExW(0, kWndClass, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_host) {
        LINGWU_TRACE("tray: CreateWindowExW(HWND_MESSAGE) failed err=%lu", GetLastError());
        return false;
    }
    return true;
}

HICON LoadTrayIcon() {
    if (g_icon && g_iconOwned) return g_icon;

    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);
    // 资源 ID 固定为 1，见 resources/app.rc。
    // 绝不能带 LR_LOADFROMFILE：带了它 LoadImageW 就把参数当图片文件解析，
    // 不会去遍历 PE 的 RT_GROUP_ICON，对 exe 路径永远返回 NULL。
    HICON h = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                            MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy, 0));
    if (!h) {
        // 资源缺失时退回系统图标。LoadIconW 返回的是**共享**图标，
        // 绝不能 DestroyIcon，所以 owned 必须留 false。
        h = LoadIconW(nullptr, IDI_APPLICATION);
        g_iconOwned = false;
    } else {
        g_iconOwned = true;
    }
    g_icon = h;
    return g_icon;
}

}  // namespace

// ---------------------------------------------------------------- 公开 API
bool InitTrayIcon(const TrayCallbacks& cb) {
    g_cb = cb;
    if (!EnsureHostWindow()) return false;

    // 重复 Init 先撤旧的，否则 NIM_ADD 同一 uid 会静默失败。
    if (g_added) {
        NOTIFYICONDATAW del = g_nid;
        del.uFlags = 0;
        Shell_NotifyIconW(NIM_DELETE, &del);
        g_added = false;
    }

    g_nid             = {};
    g_nid.cbSize      = sizeof(g_nid);
    g_nid.hWnd        = g_host;
    g_nid.uID         = kTrayUid;
    g_nid.uFlags      = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_CB;
    g_nid.hIcon       = LoadTrayIcon();
    wcsncpy_s(g_nid.szTip, L"Lingwu Toolbox · 零物工具箱", _TRUNCATE);

    g_added = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);
    if (g_added) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        const BOOL ok = Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
        // NIM_SETVERSION 的返回值是「shell 认不认 4.0」的唯一线索。
        // 认了才收 NIN_*，否则退回旧码。返回 FALSE 不代表图标没注册成功，
        // 只代表通知码仍是旧的一套，所以不能在这里失败返回。
        g_version = (ok != FALSE) ? NOTIFYICON_VERSION_4 : 0;
        LINGWU_TRACE("tray: NIM_ADD ok, SETVERSION=%d -> version=%u", ok ? 1 : 0, g_version);
        // 必须在 NIM_ADD 之后：NotifyIconSettings 下的登记项是 Explorer 收到
        // NIM_ADD 才建的，之前查不到任何东西。
        //
        // 而且那条登记项是**异步**建的 —— NIM_ADD 返回时它往往还不存在。
        // 所以这里不是「试一次」，而是起一个定时器一直试到它出现为止。
        // 详见 TryPromoteTrayIcon 上面的长注释。
        g_promoteTries = 0;
        g_promoted = false;
        g_promotionSettled = false;
        SetTimer(g_host, kPromoteTimerId, kPromoteRetryMs, nullptr);
    } else {
        g_version = 0;
        // Explorer 未就绪（典型：开机自启抢跑）。1.5 秒后重试一次。
        LINGWU_TRACE("tray: NIM_ADD failed err=%lu, retry in 1500ms", GetLastError());
        SetTimer(g_host, 1, 1500, nullptr);
    }
    return g_added;
}

void RecreateTrayIcon() {
    if (!g_host || !IsWindow(g_host)) {
        if (!EnsureHostWindow()) return;
    }
    if (g_added) {
        NOTIFYICONDATAW del = g_nid;
        del.uFlags = 0;
        Shell_NotifyIconW(NIM_DELETE, &del);
        g_added = false;
    }
    InitTrayIcon(g_cb);
}

void ShutdownTrayIcon() {
    if (g_host && IsWindow(g_host)) KillTimer(g_host, 1);
    if (g_added) {
        NOTIFYICONDATAW del = g_nid;
        del.uFlags = 0;
        Shell_NotifyIconW(NIM_DELETE, &del);
        g_added = false;
    }
    if (g_host && IsWindow(g_host)) {
        DestroyWindow(g_host);
        g_host = nullptr;
    }
    if (g_atom) {
        UnregisterClassW(kWndClass, GetModuleHandleW(nullptr));
        g_atom = 0;
    }
    if (g_icon && g_iconOwned) {
        DestroyIcon(g_icon);
        g_icon     = nullptr;
        g_iconOwned = false;
    }
    g_cb = {};
}

bool TrayIconAdded() { return g_added; }

void MaybeShowTrayHint() {
    // 提升到可见区这件事还没出结论之前不提示 ——
    // 提升成功的机器上图标就在可见区，那时候提示「图标在 ^ 里」是错的。
    // 放弃提升时由定时器那边自己调 MaybeShowTrayHintImpl()。
    if (!g_promotionSettled) return;
    MaybeShowTrayHintImpl();
}

void OnTaskbarCreated() {
    // Explorer 重启：登记全部失效，必须重新 NIM_ADD。
    LINGWU_TRACE("tray: TaskbarCreated, re-registering");
    RecreateTrayIcon();
}

UINT TaskbarCreatedMessageId() {
    static const UINT id = RegisterWindowMessageW(L"TaskbarCreated");
    return id;
}

}  // namespace platform
