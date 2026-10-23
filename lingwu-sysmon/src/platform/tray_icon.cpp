#include "pch.h"

#include "tray_icon.h"

#include "callback_guard.h"
#include "core/settings.h"
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
constexpr wchar_t kMutexName[] = L"Global\\LingwuSysMon_SingleInstance";
constexpr wchar_t kWndClass[]  = L"LingwuSysMon_TrayHost";

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

// ---------------------------------------------------------------- 自启动提示
// trayHintShown 在 ini 里，所以这里直接读写 core::Settings。
// 托盘模块与主界面共用同一份设置文件，没有第二份真相。
void MaybeShowTrayHintImpl() {
    if (!g_added) return;
    lingsysmon::core::Settings s;
    if (!lingsysmon::core::LoadSettings(s)) return;
    if (s.trayHintShown) return;

    s.trayHintShown = true;
    lingsysmon::core::SaveSettings(s);

    // 不用 MessageBoxW：它会弹一个**模态**的 Win32 对话框，出现在托盘初始化
    // 路径上会把 XAML 的启动时序搅乱（开机自启时主窗口还没 Activate）。
    // 改成非模态的提示气泡，挂在托盘图标上，10 秒自动消失。
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, L"托盘图标在哪儿", _TRUNCATE);
    wcsncpy_s(nid.szInfo,
              L"图标默认被 Windows 收进了「隐藏图标」区（任务栏右侧的 ^ 里）。\n"
              L"点开 ^ 把 Lingwu SysMon 拖到可见区域即可。",
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
void TryPromoteTrayIcon() {
    wchar_t self[MAX_PATH] = {0};
    if (!SelfPath(self, MAX_PATH)) return;

    const wchar_t* rootPath = L"Control Panel\\NotifyIconSettings";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, rootPath, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return;

    for (DWORD i = 0;; ++i) {
        wchar_t name[128] = {0};
        DWORD cbName = ARRAYSIZE(name);
        if (RegEnumKeyExW(root, i, name, &cbName, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS)
            break;   // ERROR_NO_MORE_ITEMS

        wchar_t sub[256] = {0};
        if (swprintf_s(sub, ARRAYSIZE(sub), L"%s\\%s", rootPath, name) < 0) continue;
        if (!SubKeyMatchesSelf(sub, self)) continue;

        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            const DWORD one = 1;
            RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&one), sizeof(one));
            RegCloseKey(k);
        }
    }
    RegCloseKey(root);
}

// ---------------------------------------------------------------- 菜单
constexpr UINT IDM_TRAY_MAIN      = 0x1001;
constexpr UINT IDM_TRAY_SETTINGS  = 0x1002;
constexpr UINT IDM_TRAY_RECREATE  = 0x1003;
constexpr UINT IDM_TRAY_AGGREGATE = 0x1004;
constexpr UINT IDM_TRAY_EXIT      = 0x1005;

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

    const bool agg = g_cb.getAggregate ? g_cb.getAggregate() : false;

    AppendMenuW(m, MF_STRING, IDM_TRAY_MAIN, L"显示主界面");
    AppendMenuW(m, MF_STRING | (agg ? MF_CHECKED : 0), IDM_TRAY_AGGREGATE, L"按应用聚合");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_SETTINGS, L"设置…");
    AppendMenuW(m, MF_STRING, IDM_TRAY_RECREATE, L"重新添加托盘图标");
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
        case IDM_TRAY_SETTINGS:
            platform::GuardCallback("tray.onSettings", [&]{
                if (g_cb.onSettings) g_cb.onSettings();
            });
            break;
        case IDM_TRAY_RECREATE:
            platform::GuardCallback("tray.onRecreate", [&]{
                RecreateTrayIcon();
            });
            break;
        case IDM_TRAY_AGGREGATE:
            platform::GuardCallback("tray.onAggregate", [&]{
                if (g_cb.setAggregate) g_cb.setAggregate(!agg);
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
            break;

        case WM_TRAY_CB: {
            const UINT code = LOWORD(lp);
            LINGSYSMON_TRACE("tray: callback code=0x%04X version=%u", code, g_version);
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
            LINGSYSMON_TRACE("tray: RegisterClassExW failed err=%lu", GetLastError());
            return false;
        }
    }

    // HWND_MESSAGE：消息窗口。没有可见性、不进任务栏、不进 Alt+Tab。
    g_host = CreateWindowExW(0, kWndClass, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_host) {
        LINGSYSMON_TRACE("tray: CreateWindowExW(HWND_MESSAGE) failed err=%lu", GetLastError());
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
    wcsncpy_s(g_nid.szTip, L"Lingwu SysMon · 系统监控", _TRUNCATE);

    g_added = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);
    if (g_added) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        const BOOL ok = Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
        // NIM_SETVERSION 的返回值是「shell 认不认 4.0」的唯一线索。
        // 认了才收 NIN_*，否则退回旧码。返回 FALSE 不代表图标没注册成功，
        // 只代表通知码仍是旧的一套，所以不能在这里失败返回。
        g_version = (ok != FALSE) ? NOTIFYICON_VERSION_4 : 0;
        LINGSYSMON_TRACE("tray: NIM_ADD ok, SETVERSION=%d -> version=%u", ok ? 1 : 0, g_version);
        // 必须在 NIM_ADD 之后：NotifyIconSettings 下的登记项是 Explorer 收到
        // NIM_ADD 才建的，之前查不到任何东西。
        TryPromoteTrayIcon();
    } else {
        g_version = 0;
        // Explorer 未就绪（典型：开机自启抢跑）。1.5 秒后重试一次。
        LINGSYSMON_TRACE("tray: NIM_ADD failed err=%lu, retry in 1500ms", GetLastError());
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

void MaybeShowTrayHint() { MaybeShowTrayHintImpl(); }

void OnTaskbarCreated() {
    // Explorer 重启：登记全部失效，必须重新 NIM_ADD。
    LINGSYSMON_TRACE("tray: TaskbarCreated, re-registering");
    RecreateTrayIcon();
}

UINT ActivateMessageId() {
    static const UINT id = RegisterWindowMessageW(L"LingwuSysMon_ShowMain");
    return id;
}

UINT TaskbarCreatedMessageId() {
    static const UINT id = RegisterWindowMessageW(L"TaskbarCreated");
    return id;
}

// ---------------------------------------------------------------- 单实例
// 【为什么用共享内存存 HWND，而不是枚举窗口】
// 枚举顶层窗口再比对进程映像名，这条路有两个坑：
//   · WinUI 3 的窗口类名是系统统一的 "WinUIDesktopWin32WindowClass"，
//     机器上每个 WinUI 应用都叫这个名字，按类名找会命中别的应用；
//   · 自启静默模式下主窗口建了但没 Activate，没有 WS_VISIBLE，
//     按「可见顶层窗口」筛会漏掉它。
// 共享内存是确定性的：第一份把 HWND 写进去，第二份直接读，没有启发式。
constexpr wchar_t kHwndMapName[] = L"Local\\LingwuSysMon_MainHwnd";
constexpr DWORD    kHwndMapSize  = sizeof(HWND) * 2;

void PublishMainHwnd(HWND h) {
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, kHwndMapSize, kHwndMapName);
    if (!map) {
        LINGSYSMON_TRACE("tray: CreateFileMappingW failed err=%lu", GetLastError());
        return;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已经有人在管了（理论上单实例下不会发生），只更新值不改映射。
        void* p = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, kHwndMapSize);
        if (p) {
            *static_cast<HWND*>(p) = h;
            UnmapViewOfFile(p);
        }
        CloseHandle(map);
        return;
    }
    void* p = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, kHwndMapSize);
    if (p) {
        static_cast<HWND*>(p)[0] = h;
        static_cast<HWND*>(p)[1] = h;   // 双份：校验用，读到不一致就当作无效
        UnmapViewOfFile(p);
    }
    // 映射句柄**故意不关**：本进程退出后系统会自动回收，但提前关掉会让
    // 另一个进程的 MapView 变成访问已删除的节。保持打开直到进程结束。
    LINGSYSMON_TRACE("tray: published main hwnd=%p", (void*)h);
}

HWND FindOtherInstanceWindow() {
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, kHwndMapName);
    if (!map) return nullptr;

    HWND h = nullptr;
    void* p = MapViewOfFile(map, FILE_MAP_READ, 0, 0, kHwndMapSize);
    if (p) {
        HWND a = static_cast<HWND*>(p)[0];
        HWND b = static_cast<HWND*>(p)[1];
        // 双份一致才认。不一致说明第一份正在写、或映射被写坏了，
        // 此时宁可找不到（多开一个）也不要往一个错的 HWND 上发消息。
        if (a == b && a && IsWindow(a)) h = a;
        UnmapViewOfFile(p);
    }
    CloseHandle(map);
    return h;
}

void ClearPublishedHwnd() {
    HANDLE map = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kHwndMapName);
    if (!map) return;
    void* p = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, kHwndMapSize);
    if (p) {
        static_cast<HWND*>(p)[0] = nullptr;
        static_cast<HWND*>(p)[1] = nullptr;
        UnmapViewOfFile(p);
    }
    CloseHandle(map);
}

// 单实例互斥量。返回的句柄必须由调用方持有到进程结束（持有即占用）。
// Global\ 命名空间让提权实例与非提权实例也能互相看到 —— 本应用有
// RelaunchElevated 这条路径，两份权限不同的实例必须被同一把锁挡住。
// 创建失败（权限不足等）时退到 Local\，宁可少挡一种情况也不要起不来。
HANDLE AcquireSingleInstanceMutex(bool& alreadyRunning) {
    alreadyRunning = false;
    HANDLE h = CreateMutexW(nullptr, TRUE, kMutexName);
    if (h) {
        alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);
        return h;
    }
    LINGSYSMON_TRACE("tray: Global mutex failed err=%lu, falling back to Local",
                     GetLastError());
    h = CreateMutexW(nullptr, TRUE, kMutexName + 6);   // 跳过 "Global\\"
    if (h) {
        alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);
    } else {
        LINGSYSMON_TRACE("tray: Local mutex also failed err=%lu", GetLastError());
    }
    return h;
}

}  // namespace platform
