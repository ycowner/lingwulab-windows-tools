#include "pch.h"

#include "single_instance.h"

#include "trace.h"

namespace platform {
namespace {

// Global\ 命名空间让提权实例与非提权实例互相也能看到 —— 本工程两种权限的
// 实例必须被同一把锁挡住（提权路径是 runas 拉起的，权限级别不同）。
// 创建失败（权限不足等）时退到 Local\，宁可少挡一种情况也不要起不来。
constexpr const wchar_t* kMutexNameGlobal = L"Global\\LingwuServiceManagerSingleInstance";
constexpr const wchar_t* kMutexNameLocal = L"Local\\LingwuServiceManagerSingleInstance";
constexpr const wchar_t* kMapNameGlobal = L"Global\\LingwuServiceManagerMainHwnd";
constexpr const wchar_t* kMapNameLocal = L"Local\\LingwuServiceManagerMainHwnd";

// 等旧进程退出的上限。
//
// 【为什么是 5 秒】本工程的退出路径是「关窗口 → Application::Start 返回 →
// 放互斥量 → 起 runas 实例」，中间的 XAML 框架清理实测在几百毫秒量级。
// 5 秒是给慢机器留的余量；再长就会让「真的重复启动」也卡住用户。
constexpr DWORD kWaitMs = 5000;

HANDLE CreateMutexBestEffort(bool& alreadyExists) {
    alreadyExists = false;
    HANDLE h = ::CreateMutexW(nullptr, TRUE, kMutexNameGlobal);
    if (!h) {
        h = ::CreateMutexW(nullptr, TRUE, kMutexNameLocal);
    }
    if (!h) {
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
                                    0, sizeof(HWND), kMapNameGlobal);
    if (!m) {
        m = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                 0, sizeof(HWND), kMapNameLocal);
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
    void* view = ::MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(HWND));
    ::CloseHandle(m);
    if (!view) {
        return nullptr;
    }
    HWND hwnd = nullptr;
    ::memcpy(&hwnd, view, sizeof(HWND));
    ::UnmapViewOfFile(view);
    return hwnd;
}

void ActivateExistingInstance() {
    HWND other = ReadOtherHwnd();
    // 读到的 HWND 必须重新校验：写共享内存的那份进程可能已经死了，
    // 留下的 HWND 值会被系统复用成一个毫不相干的窗口。
    if (!other || !::IsWindow(other)) {
        LINGSVC_TRACE("second instance: no valid hwnd in shared map, exiting quietly");
        return;
    }
    LINGSVC_TRACE("second instance: activating hwnd=%p", (void*)other);
    ::ShowWindow(other, SW_RESTORE);
    ::SetForegroundWindow(other);
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
        // 旧进程在这 5 秒内退出了，互斥量已归我们 —— 说明用户刚刚点了提权重启。
        LINGSVC_TRACE("single instance: previous instance exited, taking over");
        mutex = h;
        return true;
    }

    if (wr == WAIT_TIMEOUT) {
        LINGSVC_TRACE("single instance: another instance still alive after %lu ms",
                      (unsigned long)kWaitMs);
        ::CloseHandle(h);
        alreadyRunning = true;
        ActivateExistingInstance();
        return false;
    }

    // WAIT_FAILED / 其它：按「拿不到独占」处理，不冒险起第二份。
    LINGSVC_TRACE("single instance: WaitForSingleObject failed err=%lu waitResult=0x%08X",
                  (unsigned long)(wr == WAIT_FAILED ? ::GetLastError() : 0),
                  (unsigned long)wr);
    ::CloseHandle(h);
    alreadyRunning = true;
    return false;
}

void PublishMainHwnd(HWND hwnd) {
    HANDLE m = CreateMappingBestEffort();
    if (!m) {
        LINGSVC_TRACE("PublishMainHwnd: CreateFileMapping failed err=%lu",
                      (unsigned long)::GetLastError());
        return;
    }
    void* view = ::MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, sizeof(HWND));
    if (view) {
        ::memcpy(view, &hwnd, sizeof(HWND));
        ::UnmapViewOfFile(view);
    }
    // 映射对象句柄刻意**不保留**：关掉它不影响已建立的映射，
    // 段对象由内核维持到最后一个句柄关闭 —— 也就是本进程退出。
    ::CloseHandle(m);
}

}  // namespace platform
