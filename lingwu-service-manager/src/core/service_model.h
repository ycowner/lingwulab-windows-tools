// service_model.h —— 服务数据模型、枚举取值、中文文案与纯判定函数。
//
// 本头**只放不依赖任何 IO 的纯函数与 POD**，因此可以整个塞进 --selftest
// 而不碰 SCM、不碰 COM、不碰 XAML 框架。真正会调 SCM 的部分在 scm.h。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace lingsvc {
namespace core {

// ── 枚举与文案的映射 ──────────────────────────────────────────────────────
//
// 【为什么不直接用 SERVICE_* 常量做 switch 的 case】
// 这些常量在最新 SDK 里有，但服务的运行期状态值是 SCM 的公开契约、
// 从 Vista 起就没变过；直接写字面量可以让本文件不依赖 SDK 版本差异
// （与 platform/window_decor.cpp 里那套 kDwmwa* 字面量同一个理由）。

// dwCurrentState
inline bool IsValidState(DWORD state) {
    return state >= 1 && state <= 7;
}

inline std::wstring StateText(DWORD state) {
    switch (state) {
        case 1: return L"已停止";        // SERVICE_STOPPED
        case 2: return L"启动中";        // SERVICE_START_PENDING
        case 3: return L"停止中";        // SERVICE_STOP_PENDING
        case 4: return L"运行中";        // SERVICE_RUNNING
        case 5: return L"恢复中";        // SERVICE_CONTINUE_PENDING
        case 6: return L"暂停中";        // SERVICE_PAUSE_PENDING
        case 7: return L"已暂停";        // SERVICE_PAUSED
    }
    return L"未知";
}

// dwStartType
inline bool IsValidStartType(DWORD startType) {
    return startType <= 4;
}

// 启动类型文案。delayedAutoStart 为真时 dwStartType 一定是 3（自动），
// 这里仍按 3 输出主标签、把「（延迟启动）」交给调用方拼，调用方决定放哪一列。
inline std::wstring StartTypeText(DWORD startType) {
    switch (startType) {
        case 0: return L"引导";          // SERVICE_BOOT_START
        case 1: return L"系统";          // SERVICE_SYSTEM_START
        case 2: return L"禁用";          // SERVICE_DISABLED
        case 3: return L"自动";          // SERVICE_AUTO_START
        case 4: return L"手动";          // SERVICE_DEMAND_START
    }
    return L"未知";
}

// 列表里直接显示的那一格：延迟自动要一眼看得出来。
inline std::wstring StartTypeCellText(DWORD startType, bool delayedAutoStart, bool known) {
    if (!known) return L"—";   // 查询不到（多半是权限不足），不要编一个值出来
    if (startType == 3 && delayedAutoStart) return L"自动（延迟）";
    return StartTypeText(startType);
}

// 启动类型下拉只暴露这 4 档。引导 / 系统属于系统启动阶段的服务，
// 不该让用户在普通下拉里选到（方案已确认）。
enum class StartTypeChoice {
    Auto = 0,
    DelayedAuto,
    Demand,
    Disabled,
};

inline DWORD StartTypeToDWord(StartTypeChoice c) {
    switch (c) {
        case StartTypeChoice::Auto:       return 3;   // SERVICE_AUTO_START
        case StartTypeChoice::DelayedAuto:return 3;   // 同上，靠 delayed 标志区分
        case StartTypeChoice::Demand:     return 4;   // SERVICE_DEMAND_START
        case StartTypeChoice::Disabled:   return 2;   // SERVICE_DISABLED
    }
    return 3;
}

inline bool ChoiceIsDelayed(StartTypeChoice c) {
    return c == StartTypeChoice::DelayedAuto;
}

inline std::wstring StartTypeChoiceText(StartTypeChoice c) {
    switch (c) {
        case StartTypeChoice::Auto:        return L"自动";
        case StartTypeChoice::DelayedAuto: return L"自动（延迟启动）";
        case StartTypeChoice::Demand:      return L"手动";
        case StartTypeChoice::Disabled:    return L"禁用";
    }
    return L"自动";
}

// ── 可执行路径与「第三方」判定 ────────────────────────────────────────────

// 去掉成对的包围引号。CreateServiceW 允许 lpBinaryPathName 自带引号，
// 读回来时也会带着，所以判定「是不是系统文件」之前必须先剥掉。
inline std::wstring StripQuotes(std::wstring const& s) {
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// 取 %WinDir%。取不到就返回空串，调用方一律按「判不出」处理而不是猜。
inline std::wstring WindowsDir() {
    wchar_t buf[MAX_PATH] = {};
    const UINT n = ::GetWindowsDirectoryW(buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(buf, n);
}

// 路径是否在 %WinDir% 之下。
//
// 【为什么不比较整个字符串而要小心大小写】Windows 路径不区分大小写，
// 这里做大小写无关比较。用 CompareStringW 带 NORM_IGNORECASE 而不是
// 逐字符 towlower —— 后者在非 ASCII 路径上会踩 locale 的坑。
inline bool PathUnder(const std::wstring& path, const std::wstring& dir) {
    if (dir.empty() || path.size() < dir.size()) return false;
    const int cmp = ::CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE,
                                      path.c_str(), static_cast<int>(dir.size()),
                                      dir.c_str(), static_cast<int>(dir.size()));
    if (cmp != CSTR_LESS_THAN && cmp != CSTR_EQUAL) return false;
    // 目录必须以反斜杠收尾或后面紧跟分隔符，否则 C:\Windows 被 C:\WindowsOld 命中。
    if (path.size() == dir.size()) return true;
    const wchar_t next = path[dir.size()];
    return next == L'\\' || next == L'/';
}

// 可执行路径落在 %WinDir% 之下 = 系统自带；否则算第三方。
inline bool IsThirdPartyBinary(std::wstring const& binaryPath) {
    const std::wstring clean = StripQuotes(binaryPath);
    if (clean.empty()) return false;   // 查不到就**不算**第三方，宁可漏判也不误伤系统服务
    return !PathUnder(clean, WindowsDir());
}

// ── 筛选与排序 ────────────────────────────────────────────────────────────

enum class FilterMode {
    All = 0,
    Running,
    Stopped,
    Disabled,
    ThirdParty,
};

inline std::wstring FilterText(FilterMode m) {
    switch (m) {
        case FilterMode::All:        return L"全部";
        case FilterMode::Running:    return L"运行中";
        case FilterMode::Stopped:    return L"已停止";
        case FilterMode::Disabled:   return L"已禁用";
        case FilterMode::ThirdParty: return L"第三方";
    }
    return L"全部";
}

// 三列 × 两个方向。
//
// 【为什么是六个值而不是「三列 + 一个 bool 倒序」】
// 一个全局的倒序标志不够用：用户点「名称」时想要降序，不代表他点
// 「启动类型」之后还想要降序。方向是**每列各自的**，所以它得待在
// 排序状态里，而不是另开一个字段。
//
// 枚举值是 settings.ini 里的对外契约（SortMode=N）。本工具尚未发布，
// 所以直接把 NameDesc 插在 1 位不会影响任何已有用户；真要改这个布局，
// 必须先想清楚怎么迁移旧的 ini 值。
enum class SortMode {
    NameAsc = 0,      // 默认
    NameDesc,
    StateAsc,
    StateDesc,
    StartTypeAsc,
    StartTypeDesc,
};

// 当前排序列（0=名称 1=状态 2=启动类型），供表头指示器与设置回显共用。
inline int SortColumn(SortMode m) {
    switch (m) {
        case SortMode::NameAsc:
        case SortMode::NameDesc:
            return 0;
        case SortMode::StateAsc:
        case SortMode::StateDesc:
            return 1;
        case SortMode::StartTypeAsc:
        case SortMode::StartTypeDesc:
            return 2;
    }
    return 0;
}

inline bool SortDescending(SortMode m) {
    return m == SortMode::NameDesc || m == SortMode::StateDesc ||
           m == SortMode::StartTypeDesc;
}

// 表头箭头的角度：升序朝上（0°），降序朝下（180°）。
inline double SortArrowAngle(SortMode m) {
    return SortDescending(m) ? 180.0 : 0.0;
}

inline std::wstring SortText(SortMode m) {
    switch (SortColumn(m)) {
        case 1: return L"状态";
        case 2: return L"启动类型";
        default: return L"名称";
    }
}

// ── 数据结构 ──────────────────────────────────────────────────────────────

// 列表行需要的全部字段。
//
// 【为什么 startTypeKnown 是独立的标志而不是把 startType 设成一个哨兵值】
// 非提权时 OpenService 对一部分服务返回 ERROR_ACCESS_DENIED，拿不到启动类型。
// 用哨兵值（比如 99）的话，排序比较器必须处处记得排除它；
// 独立标志让「不知道」这件事在数据上就是显式的，漏处理的地方一眼能看出来。
struct ServiceInfo {
    std::wstring name;
    std::wstring displayName;
    DWORD state = 1;                      // dwCurrentState
    DWORD win32ExitCode = 0;
    DWORD serviceSpecificExitCode = 0;
    DWORD pid = 0;
    DWORD startType = 3;                  // 仅当 startTypeKnown
    bool  startTypeKnown = false;
    bool  delayedAutoStart = false;
    bool  markedForDelete = false;         // SERVICE_STATUS_PROCESS.dwServiceFlags 的 bit 3
};

// 详情面板的字段。只在选中某一行时查一次，**不进列表刷新循环**。
struct ServiceDetails {
    std::wstring displayName;
    std::wstring binaryPath;
    std::wstring account;
    std::wstring description;
    std::wstring dependencies;   // 多项以 L"\n" 分隔；空表示没有或查不到
    std::wstring dependents;
    bool  accessible = false;    // false = OpenService 就被拒，界面显示「（无权查看）」
    DWORD win32 = 0;
};

// 排序比较器需要的可读名。服务名是 SCM 的稳定标识符，跨启动不变，
// 所以它是唯一的行 key（见 MainWindow.xaml.h 的 rowByKey_）。
inline std::wstring SortKeyName(ServiceInfo const& s) {
    return s.name;
}

}  // namespace core
}  // namespace lingsvc
