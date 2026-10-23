#pragma once
//
// 轻量运行期追踪。只在显式打开时才落盘，正常运行零开销（除了一次 getenv）。
//
// 为什么需要它：WinUI 3 的大部分失败（XAML 加载、类型激活、SystemBackdrop、
// 窗口创建）在 Release 下**不会**崩溃、不会写事件日志、也没有任何 stdout，
// 表现只是「进程活着但一个窗口都没有」。没有 trace 就只能靠猜。
//
// 打开方式（满足其一即可）：环境变量 LINGWU_TRACE 非空，
// 或存在标记文件 %LOCALAPPDATA%\LingwuToolbox\trace.on。
// 文件写到   %LOCALAPPDATA%\LingwuToolbox\trace.log
// 每次调用追加一行，形如：
//   2026-10-13 15:04:11.123 [pid=1234 tid=5678] MainWindow ctor: step=1
//
// 为什么同时支持标记文件：非交互 shell / 构建脚本里用 $env: 改环境变量
// 不一定能传到 Start-Process 起的子进程（实测在 agent 的 shell 里就传不过去，
// trace 静默不写，极易误判成「代码没执行」）。文件存在性没有这种歧义。
//
// 刻意不依赖 OutputDebugString：非交互 shell 里跑时没有调试器，
// 写文件才是能自动取证的那条路。
//
#include <windows.h>

#include <cstdio>
#include <cstdarg>
#include <cstdlib>

namespace platform {

namespace detail {

// 追加工具函数、标记文件路径要复用到两处，先收在这里。
inline bool TryGetAppDataDir(char* out, int cap) noexcept {
    const DWORD n = ::GetEnvironmentVariableA("LOCALAPPDATA", out, static_cast<DWORD>(cap));
    return n > 0 && n < static_cast<DWORD>(cap);
}

// 取 %LOCALAPPDATA%\LingwuToolbox\trace.log
inline void BuildTraceFilePath(char* out, int cap) noexcept {
    char dir[MAX_PATH]{};
    if (!TryGetAppDataDir(dir, static_cast<int>(sizeof(dir)))) {
        out[0] = '\0';
        return;
    }
    ::lstrcatA(dir, "\\LingwuToolbox");
    ::CreateDirectoryA(dir, nullptr);
    ::lstrcatA(dir, "\\trace.log");
    ::lstrcpynA(out, dir, static_cast<int>(cap));
}

// 取 %LOCALAPPDATA%\LingwuToolbox\trace.on（标记文件本身，不创建）
inline void BuildTraceFlagPath(char* out, int cap) noexcept {
    char dir[MAX_PATH]{};
    if (!TryGetAppDataDir(dir, static_cast<int>(sizeof(dir)))) {
        out[0] = '\0';
        return;
    }
    ::lstrcatA(dir, "\\LingwuToolbox\\trace.on");
    ::lstrcpynA(out, dir, static_cast<int>(cap));
}

// 构造期单线程，静态局部足够；用 lambda 缓存，避免每条 trace 都查一遍环境。
inline bool TraceEnabled() noexcept {
    static const bool enabled = [] {
        char buf[8] = {};
        const DWORD n = ::GetEnvironmentVariableA("LINGWU_TRACE", buf,
                                                  static_cast<DWORD>(sizeof(buf)));
        if (n > 0) {
            return true;
        }
        char flag[MAX_PATH]{};
        BuildTraceFlagPath(flag, static_cast<int>(sizeof(flag)));
        return flag[0] != '\0' && ::GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES;
    }();
    return enabled;
}

}  // namespace detail

inline void Trace(const char* fmt, ...) noexcept {
    if (!detail::TraceEnabled()) {
        return;
    }

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = ::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    const DWORD pid = ::GetCurrentProcessId();
    const DWORD tid = ::GetCurrentThreadId();

    char path[MAX_PATH]{};
    detail::BuildTraceFilePath(path, static_cast<int>(sizeof(path)));
    if (path[0] == '\0') {
        return;
    }

    // 以 FILE_SHARE_READ|FILE_SHARE_WRITE 打开并追加，多个实例同时写不会失败。
    HANDLE h = ::CreateFileA(path, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    char line[1280];
    const int ln = ::snprintf(line, sizeof(line),
                              "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid=%lu tid=%lu] %s\r\n",
                              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                              st.wSecond, st.wMilliseconds,
                              static_cast<unsigned long>(pid),
                              static_cast<unsigned long>(tid), msg);
    if (ln > 0) {
        DWORD written = 0;
        ::WriteFile(h, line, static_cast<DWORD>(ln), &written, nullptr);
    }
    ::CloseHandle(h);
}

}  // namespace platform

#define LINGWU_TRACE(...) ::platform::Trace(__VA_ARGS__)
