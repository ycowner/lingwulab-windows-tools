#include "pch.h"

#include "single_instance.h"

#include "trace.h"

namespace platform {
namespace {

// Global\ 命名空间让提权实例与非提权实例互相也能看到 —— 本程序默认以
// 管理员身份运行，而 Explorer 启动的普通进程与它处于不同完整性级别。
// 创建失败（权限不足等）时退到 Local\，宁可少挡一种情况也不要起不来。
constexpr const wchar_t* kMutexNameGlobal = L"Global\\LingwuToolboxSingleInstance";
constexpr const wchar_t* kMutexNameLocal  = L"Local\\LingwuToolboxSingleInstance";
constexpr const wchar_t* kMapNameGlobal   = L"Global\\LingwuToolboxMainHwnd";
constexpr const wchar_t* kMapNameLocal    = L"Local\\LingwuToolboxMainHwnd";

// 等旧进程退出的上限。
//
// 【为什么是 5 秒】本工程的退出路径是「关窗口 → Application::Start 返回 →
// 放互斥量」，中间的 XAML 框架清理实测在几百毫秒量级。
// 5 秒是给慢机器留的余量；再长就会让「真的重复启动」也卡住用户。
constexpr DWORD kWaitMs = 5000;

// 共享内存里存**两份** HWND，用来判断映射是否被写坏。
//
// 【为什么双份】第一份进程正在写、或映射被破坏时，两份会不一致。
// 此时宁可「找不到」（多开一个）也不要往一个错的 HWND 上发消息 ——
// 那个 HWND 可能是系统复用出来的另一个窗口的句柄。
constexpr DWORD kHwndMapSize = sizeof(HWND) * 2;

// 共享内存的句柄**必须留到进程结束**。
//
// 【为什么 —— 这条曾是一个真缺陷，实测才发现】
//   命名内核对象的寿命只取决于「还有几个句柄开着」。最后一个句柄一关，
//   对象就被释放 —— 对 `CreateFileMappingW(INVALID_HANDLE_VALUE, ...)`
//   这种页文件支撑的命名映射也一样。
//   原实现写完 HWND 就 `CloseHandle(m)`，于是**共享段当场消失**：
//   第二个实例 `OpenFileMappingW` 直接失败（实测 err=2 = ERROR_FILE_NOT_FOUND），
//   于是它读不到任何 HWND，打印「second instance: no valid hwnd in shared map,
//   exiting quietly」就退出了 —— **第一份实例收进托盘后再启动一份，什么都不会发生**，
//   而且没有任何报错。
//   原注释写的是「关掉它不影响已建立的映射，段对象由内核维持到最后一个句柄关闭
//   —— 也就是本进程退出」：前半句和后半句是矛盾的，前半句是错的。
//   正确做法就是后半句说的：**把句柄一直留着**。
HANDLE g_hwndMap = nullptr;

HANDLE CreateMutexBestEffort(bool& alreadyExists) {
    alreadyExists = false;
    HANDLE h = ::CreateMutexW(nullptr, TRUE, kMutexNameGlobal);
    if (!h) {
        h = ::CreateMutexW(nullptr, TRUE, kMutexNameLocal);
    }
    if (!h) {
        LINGWU_TRACE("single instance: both Global\\ and Local\\ mutex creation failed err=%lu",
                     GetLastError());
        return nullptr;
    }
    alreadyExists = (::GetLastError() == ERROR_ALREADY_EXISTS);
    return h;
}

HANDLE CreateMappingBestEffort() {
    // CreateFileMappingW 的尺寸是两个 DWORD（高 32 位 / 低 32 位），
    // 单块 HWND 只有指针大小，dwMaximumSizeHigh 必须传 0、传进 dwMaximumSizeLow。
    // 顺序写反（把尺寸塞进 flProtect 后面第一个 DWORD）的话，
    // 这个调用根本编译不过 —— 它不是运行期才炸的那类问题。
    HANDLE m = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, kHwndMapSize, kMapNameGlobal);
    if (!m) {
        m = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                 0, kHwndMapSize, kMapNameLocal);
    }
    return m;
}

HWND ReadOtherHwnd() {
    HANDLE m = ::OpenFileMappingW(FILE_MAP_READ, FALSE, kMapNameGlobal);
    if (!m) {
        m = ::OpenFileMappingW(FILE_MAP_READ, FALSE, kMapNameLocal);
    }
    if (!m) {
        return nullptr;
    }
    void* view = ::MapViewOfFile(m, FILE_MAP_READ, 0, 0, kHwndMapSize);
    ::CloseHandle(m);
    if (!view) {
        return nullptr;
    }
    HWND hwnd = nullptr;
    HWND a = static_cast<HWND*>(view)[0];
    HWND b = static_cast<HWND*>(view)[1];
    // 双份一致才认。不一致说明第一份正在写、或映射被写坏了。
    if (a == b && a && ::IsWindow(a)) {
        hwnd = a;
    }
    ::UnmapViewOfFile(view);
    return hwnd;
}

void ActivateExistingInstance() {
    HWND other = ReadOtherHwnd();
    // 读到的 HWND 必须重新校验：写共享内存的那份进程可能已经死了，
    // 留下的 HWND 值会被系统复用成一个毫不相干的窗口。
    if (!other || !::IsWindow(other)) {
        LINGWU_TRACE("second instance: no valid hwnd in shared map, exiting quietly");
        return;
    }
    LINGWU_TRACE("second instance: activating hwnd=%p", (void*)other);
    // 先还原再激活：窗口是最小化状态时，只 PostMessage 不会自动还原。
    ::ShowWindow(other, SW_RESTORE);
    ::SetForegroundWindow(other);

    // 再补一条自定义消息，让主窗口把自己的界面切到前台页。
    // 光靠 ShowWindow/SetForegroundWindow 只能把窗口带到前面，
    // 它自己并不知道「有人叫我」。
    const UINT msg = ActivateMessageId();
    if (msg) {
        ::PostMessageW(other, msg, 0, 0);
    }
}

}  // namespace

bool AcquireSingleInstance(HANDLE& mutex, bool& alreadyRunning) {
    alreadyRunning = false;
    mutex = nullptr;

    bool alreadyExists = false;
    HANDLE h = CreateMutexBestEffort(alreadyExists);

    if (!alreadyExists) {
        mutex = h;   // 独占，哪怕 h 为 nullptr
        return true;
    }

    if (!h) {
        alreadyRunning = true;
        return false;
    }

    // 已经在跑。等它退出。
    const DWORD wr = ::WaitForSingleObject(h, kWaitMs);
    if (wr == WAIT_OBJECT_0) {
        // 旧进程在这 5 秒内退出了，互斥量已归我们 —— 说明用户刚刚触发了重启。
        LINGWU_TRACE("single instance: previous instance exited, taking over");
        mutex = h;
        return true;
    }

    if (wr == WAIT_TIMEOUT) {
        LINGWU_TRACE("single instance: another instance still alive after %lu ms",
                     (unsigned long)kWaitMs);
        ::CloseHandle(h);
        alreadyRunning = true;
        ActivateExistingInstance();
        return false;
    }

    // WAIT_FAILED / 其它：按「拿不到独占」处理，不冒险起第二份。
    LINGWU_TRACE("single instance: WaitForSingleObject failed err=%lu waitResult=0x%08X",
                 (unsigned long)((wr == WAIT_FAILED) ? ::GetLastError() : 0),
                 (unsigned long)wr);
    ::CloseHandle(h);
    alreadyRunning = true;
    return false;
}

void PublishMainHwnd(HWND hwnd) {
    // 句柄只建一次，之后一直留着（见 g_hwndMap 上面的长注释）。
    if (!g_hwndMap) {
        g_hwndMap = CreateMappingBestEffort();
        if (!g_hwndMap) {
            LINGWU_TRACE("PublishMainHwnd: CreateFileMapping failed err=%lu",
                         (unsigned long)::GetLastError());
            return;
        }
    }
    void* view = ::MapViewOfFile(g_hwndMap, FILE_MAP_WRITE, 0, 0, kHwndMapSize);
    if (view) {
        static_cast<HWND*>(view)[0] = hwnd;
        static_cast<HWND*>(view)[1] = hwnd;   // 双份：校验用
        ::UnmapViewOfFile(view);
    }
    // **这里不能 CloseHandle** —— 关掉唯一的句柄，命名映射就当场消失，
    // 第二个实例再也打不开它（实测 OpenFileMappingW 直接 err=2）。
    // 句柄留给进程退出时由内核回收。
    LINGWU_TRACE("single instance: published main hwnd=%p", (void*)hwnd);
}

void ClearPublishedHwnd() {
    // 用自己留着的那个句柄去清，不要另外 Open 一个 ——
    // 万一这个进程是唯一持有者，Open 出来的句柄反而会把映射的生命周期搞乱。
    HANDLE m = g_hwndMap;
    if (!m) {
        m = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, kMapNameGlobal);
        if (!m) {
            m = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, kMapNameLocal);
        }
    }
    if (!m) return;
    void* view = ::MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, kHwndMapSize);
    if (view) {
        static_cast<HWND*>(view)[0] = nullptr;
        static_cast<HWND*>(view)[1] = nullptr;
        ::UnmapViewOfFile(view);
    }
    // g_hwndMap 是本进程持有的，关掉它等于让这一份实例不再维持这个映射；
    // 但如果关掉，别的进程可能正在读 —— 所以这里只在「不是自己留着的那个」
    // 情况下才关。
    if (m != g_hwndMap) {
        ::CloseHandle(m);
    }
}

HWND FindOtherInstanceWindow() {
    return ReadOtherHwnd();
}

UINT ActivateMessageId() {
    static const UINT id = RegisterWindowMessageW(L"LingwuToolbox_ShowMain");
    return id;
}

}  // namespace platform
