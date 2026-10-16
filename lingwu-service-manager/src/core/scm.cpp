#include "pch.h"

#include "core/scm.h"

#include "platform/trace.h"

#include <cwchar>

namespace lingsvc {
namespace core {
namespace {

// 普通用户凭这两个权限就能枚举出全部服务清单（含已停止、已禁用的）。
// 这是「默认非提权也能看列表」的基础 —— 权限模型的第一条原则就是
// 「看列表不需要管理员」。
constexpr DWORD kManagerConnectEnum = SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE;

// 枚举的 dwServiceState。SERVICE_STATE_ALL 在本机 SDK 里定义为
// (SERVICE_ACTIVE | SERVICE_INACTIVE)，即「活动 + 非活动全都要」，
// 这才是与服务管理器 (services.msc) 一致的口径。只传 SERVICE_ACTIVE
// 会漏掉已停止/已禁用的一多半，界面上却同样「看起来正常」。
constexpr DWORD kAllServiceStates = SERVICE_STATE_ALL;

// 只列 Win32 服务：驱动不在本工具范围内（方案已确认）。
constexpr DWORD kWin32ServiceTypes = SERVICE_WIN32;

// 枚举每页的缓冲区大小。
//
// 【为什么分页而不是一次给大缓冲】实测这台机器上 EnumServicesStatusW
// 对 cbBufSize 有硬上限：
//   cb= 128 KB -> 成功
//   cb= 256 KB -> 成功
//   cb= 512 KB -> **失败 err=1783「占位程序接收到错误数据」**
// 也就是说「缓冲区给大点省事」这条路会直接踩雷，而 1783 长得像网络
// 错误码，根本看不出跟缓冲区大小有关。
//
// 分页（lpResumeHandle）绕开了这个上限，而且**零额外代价**：
// 本机 345 个服务在 64 KB 一页里就装得下，只调一次；实测 16 KB 一页时
// 三次调用 134 + 141 + 70 = 345，与 256 KB 单次结果**完全一致**，
// 无重复、无遗漏。顺带一条：缓冲区不够时 SCM 返回的是
// FALSE/ERROR_MORE_DATA，但 read 与 resume **都是可用的**，
// 所以别把 MORE_DATA 当成致命错误直接放弃。
constexpr DWORD kEnumPageSize = 64 * 1024;

// 分页保险丝。服务数再离谱也不该让 poll 线程无限转圈；
// 真到这一步会在 trace 里留下记录。
constexpr int kMaxEnumPages = 64;

// QueryServiceConfigW 的缓冲区。
//
// 实测同一个上限问题：cb=8192 成功，cb=8193 **就**开始失败。
// 而一份正常的服务配置只有三四百字节（RpcSs 实测 need=388），
// 所以 2 KB 起步、翻倍到 8 KB 为止，足够覆盖任何现实配置。
constexpr DWORD kConfigBufInitial = 2 * 1024;
constexpr DWORD kConfigBufMax = 8 * 1024;   // 实测上限就是 8192，别越线

// 单次查询运行中服务 PID 需要的权限。只要 QUERY_STATUS，不要更多 ——
// 非提权时这已经是绝大多数服务愿意给的最小集合。
constexpr DWORD kQueryStatusOnly = SERVICE_QUERY_STATUS;

// 逐服务取 PID / dwServiceFlags 的失败分类。分不开就没法在界面上
// 区分「这个服务没有 PID」和「我们没权限查这个服务」。
constexpr DWORD kMaxPidProbeFailuresLogged = 5;

}  // namespace

// ── 句柄 RAII ────────────────────────────────────────────────────────────

bool ScManager::Open(DWORD desiredAccess, DWORD& err) {
    Close();
    h_ = ::OpenSCManagerW(nullptr, nullptr, desiredAccess);
    if (!h_) {
        err = ::GetLastError();
        return false;
    }
    err = ERROR_SUCCESS;
    return true;
}

void ScManager::Close() {
    if (h_) {
        ::CloseServiceHandle(h_);
        h_ = nullptr;
    }
}

bool ScService::Open(ScManager const& mgr, std::wstring const& name, DWORD desiredAccess,
                     DWORD& err) {
    Close();
    if (!mgr.valid()) {
        err = ERROR_INVALID_HANDLE;
        return false;
    }
    h_ = ::OpenServiceW(mgr.get(), name.c_str(), desiredAccess);
    if (!h_) {
        err = ::GetLastError();
        return false;
    }
    err = ERROR_SUCCESS;
    return true;
}

void ScService::Close() {
    if (h_) {
        ::CloseServiceHandle(h_);
        h_ = nullptr;
    }
}

std::wstring QuoteBinaryPath(std::wstring const& path) {
    if (path.empty()) {
        return path;
    }
    // 已经包好了就原样传。重复加引号会让 SCM 把整串当成一个文件名。
    if (path.front() == L'"') {
        return path;
    }
    bool needsQuote = false;
    for (wchar_t c : path) {
        if (c == L' ' || c == L'\t') {
            needsQuote = true;
            break;
        }
    }
    if (!needsQuote) {
        return path;
    }
    std::wstring out;
    out.reserve(path.size() + 2);
    out.push_back(L'"');
    out.append(path);
    out.push_back(L'"');
    return out;
}

// 取单个服务的 SERVICE_STATUS_PROCESS。这是经典枚举缺的那块
// （dwProcessId / dwServiceFlags 只有它有）。
//
// 【为什么不批量做】EnumServicesStatusExW 本来就是干这个的，
// 但它在非提权下必然失败（见 EnumAll 里的实测记录）。
// 逐条查的成本实测是 157 次 RPC / 整轮耗时在计时器分辨率以下。
bool ScmQueryProcessInfo(ScManager const& mgr, std::wstring const& name,
                         SERVICE_STATUS_PROCESS& out, DWORD& err) {
    ScService svc;
    if (!svc.Open(mgr, name, kQueryStatusOnly, err)) {
        return false;
    }
    DWORD got = 0;
    ::SetLastError(0);
    if (!::QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO,
                                reinterpret_cast<LPBYTE>(&out),
                                sizeof(out), &got)) {
        err = ::GetLastError();
        return false;
    }
    if (got < sizeof(out)) {
        // 返回的字节数比结构体还小，说明服务端版本给不出完整记录。
        err = ERROR_INVALID_DATA;
        return false;
    }
    err = ERROR_SUCCESS;
    return true;
}

// ── ScmSession ───────────────────────────────────────────────────────────

// 读一份 QUERY_SERVICE_CONFIG。
//
// 【本函数存在的唯一理由是把「缓冲区上限」这件事收敛到一处】
// 这台机器上 QueryServiceConfigW 的 cbBufSize 上限正好是 8192：
// 8192 成功，8193 起 err=1783。早期版本在两个调用点各自写死
// `std::vector<BYTE> buf(16 * 1024)`，正好踩在线外，于是
// **每一个**服务的启动类型都查不到，列表那一列全空，
// 而日志只有一条看不出所以然的 1783。
//
// 真实需求只有三四百字节（RpcSs 实测 need=388），2 KB 起步足够；
// 翻倍是给路径特别长的服务留的余地，封顶 8 KB 是硬约束。
// 成功时结构体留在 out 里，调用方 reinterpret_cast 取用。
bool ReadServiceConfigCapped(SC_HANDLE svc, std::vector<BYTE>& out, DWORD& err) {
    out.assign(kConfigBufInitial, 0);
    DWORD last = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD need = 0;
        ::SetLastError(0);
        if (::QueryServiceConfigW(svc,
                                  reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(out.data()),
                                  static_cast<DWORD>(out.size()), &need)) {
            err = ERROR_SUCCESS;
            return true;
        }
        last = ::GetLastError();
        if (last != ERROR_MORE_DATA || need == 0 || out.size() >= kConfigBufMax) {
            break;
        }
        size_t next = out.size() * 2;
        if (static_cast<DWORD>(need) > next) {
            next = need;
        }
        if (next > kConfigBufMax) {
            last = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        out.assign(next, 0);
    }
    err = last;
    return false;
}

// 读一段纯字符串型的 InfoLevel（SERVICE_CONFIG_DESCRIPTION 等）。
// 同样的 8 KB 上限，同样收敛在这里。
bool ReadConfigStringCapped(SC_HANDLE svc, DWORD infoLevel, std::wstring& out,
                             DWORD& err) {
    out.clear();
    std::vector<BYTE> buf(2 * 1024);
    DWORD last = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD need = 0;
        ::SetLastError(0);
        if (::QueryServiceConfig2W(svc, infoLevel, buf.data(),
                                   static_cast<DWORD>(buf.size()), &need)) {
            out.assign(reinterpret_cast<wchar_t const*>(buf.data()));
            err = ERROR_SUCCESS;
            return true;
        }
        last = ::GetLastError();
        if (last != ERROR_MORE_DATA || need == 0 || buf.size() >= kConfigBufMax) {
            break;
        }
        size_t next = buf.size() * 2;
        if (static_cast<DWORD>(need) > next) {
            next = need;
        }
        if (next > kConfigBufMax) {
            last = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        buf.assign(next, 0);
    }
    err = last;
    return false;
}

bool ScmSession::Open(DWORD& err) {
    open_ = mgr_.Open(kManagerConnectEnum, err);
    return open_;
}

void ScmSession::DropStartTypeCache() {
    cache_.clear();
}

bool ScmSession::ResolveStartTypeLocked(std::wstring const& name, ServiceInfo& info) {
    auto it = cache_.find(name);
    if (it != cache_.end() && it->second.startTypeKnown) {
        info.startType = it->second.startType;
        info.startTypeKnown = true;
        info.delayedAutoStart = it->second.delayed;
        return true;
    }

    ScService svc;
    DWORD err = ERROR_SUCCESS;
    // 只要 QUERY_CONFIG。权限最小化让非提权时也能查到尽可能多的服务 ——
    // 很多服务对 SERVICE_STOP / SERVICE_CHANGE_CONFIG 是拒绝的，但对查询是放行的。
    if (!svc.Open(mgr_, name, SERVICE_QUERY_CONFIG, err)) {
        // 查不到不算失败：把这条服务的启动类型留成「未知」，列表里显示「—」。
        // 强行整趟枚举失败会让非提权用户看到**空列表**，那是更糟的退化。
        info.startTypeKnown = false;
        Cached c;
        c.startTypeKnown = false;
        cache_[name] = c;
        return false;
    }

    std::vector<BYTE> cfgBuf;
    if (ReadServiceConfigCapped(svc.get(), cfgBuf, err)) {
        auto const* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW const*>(cfgBuf.data());
        info.startType = cfg->dwStartType;
        info.startTypeKnown = true;
        info.delayedAutoStart = false;

        // 延迟启动标志在 SERVICE_CONFIG_DELAYED_AUTO_START_INFO 里，
        // 需要第二次调用。查不到不影响主结果（自动 vs 延迟自动是次要信息）。
        SERVICE_DELAYED_AUTO_START_INFO delayed{};
        DWORD delayedNeed = 0;
        if (::QueryServiceConfig2W(svc.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                   reinterpret_cast<LPBYTE>(&delayed),
                                   sizeof(delayed), &delayedNeed)) {
            info.delayedAutoStart = (delayed.fDelayedAutostart != FALSE);
        }

        Cached c;
        c.startType = info.startType;
        c.delayed = info.delayedAutoStart;
        c.startTypeKnown = true;
        c.details.displayName = (cfg->lpDisplayName ? cfg->lpDisplayName : L"");
        c.details.binaryPath = (cfg->lpBinaryPathName ? cfg->lpBinaryPathName : L"");
        c.details.account = (cfg->lpServiceStartName ? cfg->lpServiceStartName : L"");
        c.details.accessible = true;
        cache_[name] = c;
        return true;
    }
    // 注意 %ls 不是 %s：Trace 用的是窄 vsnprintf，喂 wchar_t* 配 %s
    // 只会打印出第一个字符（看起来像服务名被截成了 "A"）。
    LINGSVC_TRACE("QueryServiceConfigW(%ls) failed err=%lu",
                  name.c_str(), static_cast<unsigned long>(err));

    info.startTypeKnown = false;
    Cached c;
    c.startTypeKnown = false;
    cache_[name] = c;
    return false;
}

bool ScmSession::EnumAll(std::vector<ServiceInfo>& out, DWORD& err) {
    if (!open_ && !Open(err)) {
        return false;
    }

    // ── 第 1 步：枚举名字 / 显示名 / 状态（分页）─────────────────────────
    //
    // 【为什么用经典 EnumServicesStatusW 而不是 EnumServicesStatusExW】
    // 原方案用的是 Ex 变体（一次 RPC 顺带给出 PID）。实测（同一台机器、
    // 同一个 VS 2026 工具链、同样非提权）：
    //   EnumServicesStatusExW(..., pcbBytesNeeded = &need)  -> 失败 err=87
    //   换 dwServiceType / dwServiceState / 缓冲区大小，六种组合全是 err=87
    //   EnumServicesStatusW                               -> 成功，345 条
    // 本工具的默认模式**就是非提权**（权限模型第一条），所以 Ex 变体在
    // 主用路径上不可用，只能换成经典 API。
    //
    // 代价是 PID 要另取，见第 2 步 —— 实测 158 个运行中服务全取到。
    std::vector<BYTE> buf(kEnumPageSize);
    DWORD resumeHandle = 0;
    DWORD pages = 0;
    out.clear();

    while (true) {
        DWORD needed = 0;
        DWORD servicesRead = 0;
        const DWORD inResume = resumeHandle;
        ::SetLastError(0);
        const BOOL got =
            ::EnumServicesStatusW(mgr_.get(), kWin32ServiceTypes, kAllServiceStates,
                                  reinterpret_cast<LPENUM_SERVICE_STATUSW>(buf.data()),
                                  kEnumPageSize, &needed, &servicesRead, &resumeHandle);
        ++pages;

        // ERROR_MORE_DATA 不是致命错误：这一页的内容已经写进缓冲区了，
        // read 与 resume 都可用（实测 16 KB 一页时靠它拿全了 345 条）。
        if (!got && ::GetLastError() != ERROR_MORE_DATA) {
            err = ::GetLastError();
            LINGSVC_TRACE("EnumServicesStatusW failed err=%lu page=%lu",
                          static_cast<unsigned long>(err), pages);
            return false;
        }

        // ── 第 2 步：解析本页 ──────────────────────────────────────────
        //
        // 【缓冲区布局：结构体数组在前，字符串统一堆在尾部 —— 不能按步长走】
        // 实测（345 条）：
        //   前 48*n 字节是 ENUM_SERVICE_STATUSW 数组，第 i 条在 i*48；
        //   两个名字指针指向**缓冲区末尾**的一块字符串区，而且是
        //   **从后往前**排的：
        //       [0] nameOff=262132  displayOff=262074
        //       [1] nameOff=262060  displayOff=262042
        //       [7] nameOff=261704  displayOff=261658
        //   （buf.size()=262144，第一条名字正好结束在最后一个字节）
        //
        // 所以「sizeof(结构体) + 两个字符串长度」这种步长推算是错的：
        // 它假设字符串紧跟在自己那条结构体后面。照那么写，第一次迭代
        // 就越界，症状是**访问冲突 0xC0000005** 而不是读到空名字 ——
        // 越界处的指针值本身是个非法地址，printf("%ls") 当场崩。
        // 正确写法只有一个：把缓冲区当结构体数组直接下标取。
        const size_t arrayBytes =
            static_cast<size_t>(servicesRead) * sizeof(ENUM_SERVICE_STATUSW);
        if (servicesRead > 0 && arrayBytes > buf.size()) {
            // servicesRead 与缓冲区不自洽属于 SCM 违约，宁可空手回也不能越界读。
            LINGSVC_TRACE("enum page=%lu array %zu exceeds buffer %zu", pages, arrayBytes,
                          buf.size());
            err = ERROR_INVALID_DATA;
            return false;
        }
        auto const* arr =
            reinterpret_cast<ENUM_SERVICE_STATUSW const*>(buf.data());

        for (DWORD i = 0; i < servicesRead; ++i) {
            const ENUM_SERVICE_STATUSW& e = arr[i];
            if (!e.lpServiceName) {
                LINGSVC_TRACE("enum page=%lu entry %lu has a null service name", pages,
                              static_cast<unsigned long>(i));
                continue;
            }

            ServiceInfo info;
            info.name = e.lpServiceName;
            info.displayName = e.lpDisplayName ? e.lpDisplayName : L"";
            info.state = e.ServiceStatus.dwCurrentState;
            info.win32ExitCode = e.ServiceStatus.dwWin32ExitCode;
            info.serviceSpecificExitCode = e.ServiceStatus.dwServiceSpecificExitCode;
            // SERVICE_STATUS 里**没有** dwProcessId，也没有 dwServiceFlags，
            // 这两个字段只能从 SERVICE_STATUS_PROCESS 拿，见第 3 步。
            info.pid = 0;
            info.markedForDelete = false;

            out.push_back(std::move(info));
        }

        if (resumeHandle == 0) {
            break;   // 枚举到底了
        }
        if (resumeHandle == inResume) {
            LINGSVC_TRACE("enum resume handle did not advance at page=%lu", pages);
            err = ERROR_INVALID_DATA;
            return false;
        }
        if (pages >= kMaxEnumPages) {
            LINGSVC_TRACE("enum stopped at page cap %d, %zu services so far",
                          kMaxEnumPages, out.size());
            break;
        }
    }

    // ── 第 3 步：补 PID / dwServiceFlags（逐个运行中的服务一次 RPC）────
    //
    // SERVICE_STATUS 里没有这两个字段，而经典枚举一次只给这么多。
    // 实测 158 个运行中服务全部取到，整轮耗时远低于 GetTickCount64 的分辨率。
    DWORD pidFailures = 0;
    for (auto& info : out) {
        if (info.state != SERVICE_RUNNING) {
            continue;
        }
        SERVICE_STATUS_PROCESS ssp{};
        if (ScmQueryProcessInfo(mgr_, info.name, ssp, err)) {
            info.pid = ssp.dwProcessId;
            info.markedForDelete = (ssp.dwServiceFlags & kServiceFlagMarkedForDelete) != 0;
            // 以 PROCESS 结构体为准：两者重合的字段也要取同一份，
            // 否则同一个服务在不同刷新轮里可能显示不同的退出码。
            info.win32ExitCode = ssp.dwWin32ExitCode;
            info.serviceSpecificExitCode = ssp.dwServiceSpecificExitCode;
        } else if (pidFailures < kMaxPidProbeFailuresLogged) {
            ++pidFailures;
            LINGSVC_TRACE("pid probe failed for %ls err=%lu", info.name.c_str(),
                          static_cast<unsigned long>(err));
        }

        ResolveStartTypeLocked(info.name, info);
    }

    // 第 4 步：启动类型对**全部**服务生效，不只是运行中的那些。
    // 漏了这一步的话，列表里绝大多数「已停止」服务的启动类型会永远空着，
    // 而那正是用户最需要看到的一列。
    for (auto& info : out) {
        if (info.state != SERVICE_RUNNING) {
            ResolveStartTypeLocked(info.name, info);
        }
    }

    LINGSVC_TRACE("EnumAll: %zu services in %lu page(s), %lu pid probe failures",
                  out.size(), static_cast<unsigned long>(pages),
                  static_cast<unsigned long>(pidFailures));
    err = ERROR_SUCCESS;
    return true;
}

bool ScmSession::RefreshAllStartTypes(DWORD& err) {
    if (!open_ && !Open(err)) {
        return false;
    }
    std::vector<ServiceInfo> all;
    if (!EnumAll(all, err)) {
        return false;
    }
    // 强制丢弃已缓存的「未知」条目，其余保持不动。
    for (auto const& s : all) {
        auto it = cache_.find(s.name);
        if (it == cache_.end() || !it->second.startTypeKnown) {
            cache_.erase(s.name);
        }
    }
    return true;
}

bool ScmSession::RefreshAllBinaryPaths(
    std::unordered_map<std::wstring, std::wstring>& paths, DWORD& err) {
    if (!open_ && !Open(err)) {
        return false;
    }
    std::vector<ServiceInfo> all;
    if (!EnumAll(all, err)) {
        return false;
    }
    for (auto const& s : all) {
        ServiceDetails d;
        DWORD e = ERROR_SUCCESS;
        if (QueryDetails(s.name, d, e) && d.accessible) {
            paths[s.name] = StripQuotes(d.binaryPath);
        } else {
            // 查不到路径的服务按「系统服务」处理（IsThirdPartyBinary 对空串返回
            // false），而不是算成第三方 —— 宁可少显示几个第三方，也不要把
            // 系统服务错标成第三方让人去乱点。
            paths[s.name] = L"";
        }
    }
    return true;
}

bool ScmSession::CachedDetails(std::wstring const& name, ServiceDetails const** out) const {
    auto it = cache_.find(name);
    if (it == cache_.end() || !it->second.details.accessible) {
        return false;
    }
    *out = &it->second.details;
    return true;
}

bool ScmSession::QueryDetails(std::wstring const& name, ServiceDetails& out, DWORD& err) {
    if (!open_ && !Open(err)) {
        return false;
    }

    ScService svc;
    if (!svc.Open(mgr_, name, SERVICE_QUERY_CONFIG, err)) {
        out.accessible = false;
        out.win32 = err;
        return false;
    }

    out = ServiceDetails{};
    out.accessible = true;
    out.win32 = ERROR_SUCCESS;

    std::vector<BYTE> cfgBuf;
    if (ReadServiceConfigCapped(svc.get(), cfgBuf, err)) {
        auto const* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW const*>(cfgBuf.data());
        out.displayName = (cfg->lpDisplayName ? cfg->lpDisplayName : L"");
        out.binaryPath = (cfg->lpBinaryPathName ? cfg->lpBinaryPathName : L"");
        out.account = (cfg->lpServiceStartName ? cfg->lpServiceStartName : L"");

        // 依赖就在同一个 QUERY_SERVICE_CONFIG 结构里，没有单独的 API。
        //
        // 【不要去找 QueryServiceDependencies】本机 SDK 的 winsvc.h 里
        // **没有**这个函数（实测 grep 零命中），只有 QueryServiceConfigW
        // 返回的 lpDependencies 字段。它是**双 NUL 结尾**的字符串数组，
        // 不是分隔符串：遍历时每读到空串就是一个终点。
        if (cfg->lpDependencies) {
            auto const* p = cfg->lpDependencies;
            while (*p) {
                out.dependencies.append(p);
                out.dependencies.push_back(L'\n');
                p += ::wcslen(p) + 1;
            }
        }
    } else {
        out.win32 = err;
    }

    // 描述是独立的 InfoLevel，不在 QueryServiceConfigW 里。
    // 查不到就留空 —— 描述本来就是可选字段，不该让整块详情失败。
    {
        DWORD derr = ERROR_SUCCESS;
        std::wstring desc;
        if (ReadConfigStringCapped(svc.get(), SERVICE_CONFIG_DESCRIPTION, desc, derr)) {
            out.description = std::move(desc);
        }
    }

    // 依赖已在上面随 QUERY_SERVICE_CONFIG 一起读出，这里不用再单独查。

    Cached c;
    c.startTypeKnown = true;
    c.details = out;
    cache_[name] = c;

    err = ERROR_SUCCESS;
    return true;
}

// ── 写操作 ───────────────────────────────────────────────────────────────

namespace {

// 打开一个用于写操作的服务句柄。desiredAccess 由调用方给最小集合。
bool OpenForWrite(std::wstring const& name, DWORD access, ScService& svc, DWORD& err) {
    ScManager mgr;
    if (!mgr.Open(SC_MANAGER_CONNECT, err)) {
        return false;
    }
    return svc.Open(mgr, name, access, err);
}

}  // namespace

bool ScmQueryState(std::wstring const& name, DWORD& state, DWORD& err) {
    ScService svc;
    if (!OpenForWrite(name, SERVICE_QUERY_STATUS, svc, err)) {
        return false;
    }
    // 【为什么用 QueryServiceStatus 而不是 QueryServiceStatusEx】
    // 后者要多给一个 pcbBytesNeeded 参数之外，还带着版本守卫，在本工程的
    // MinVersion=10.0.17763 组合下**没被声明**（编译报 C2660「函数不接受 4 个参数」）。
    // 前者是 Win2000 起就有的无条件声明，且 dwCurrentState 就在里面 ——
    // 本函数只需要状态，PID 由枚举路径单独提供，够用。
    SERVICE_STATUS st{};
    if (!::QueryServiceStatus(svc.get(), &st)) {
        err = ::GetLastError();
        return false;
    }
    state = st.dwCurrentState;
    err = ERROR_SUCCESS;
    return true;
}

bool ScmStart(std::wstring const& name, DWORD& err) {
    ScService svc;
    if (!OpenForWrite(name, SERVICE_START | SERVICE_QUERY_STATUS, svc, err)) {
        return false;
    }
    if (::StartServiceW(svc.get(), 0, nullptr)) {
        err = ERROR_SUCCESS;
        return true;
    }
    err = ::GetLastError();
    // 本来就在跑，按「达到目标状态」算成功 —— 否则用户点启动一个已启动的
    // 服务会得到一个莫名其妙的失败提示。
    if (err == ERROR_SERVICE_ALREADY_RUNNING) {
        err = ERROR_SUCCESS;
        return true;
    }
    return false;
}

bool ScmStop(std::wstring const& name, DWORD& err) {
    ScService svc;
    if (!OpenForWrite(name, SERVICE_STOP | SERVICE_QUERY_STATUS, svc, err)) {
        return false;
    }
    SERVICE_STATUS st{};
    if (::ControlService(svc.get(), SERVICE_CONTROL_STOP, &st)) {
        err = ERROR_SUCCESS;
        return true;
    }
    err = ::GetLastError();
    // 已经停了同样算成功（见上面的理由）。
    if (err == ERROR_SERVICE_NOT_ACTIVE) {
        err = ERROR_SUCCESS;
        return true;
    }
    return false;
}

bool ScmSetStartType(std::wstring const& name, DWORD startType, bool delayed, DWORD& err) {
    ScService svc;
    if (!OpenForWrite(name, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG, svc, err)) {
        return false;
    }

    // 【第 11 个参数是本机 SDK 才有的】
    // MSDN 与绝大多数代码库里的 ChangeServiceConfigW 是 **10** 个参数；
    // Windows 10 1709 起 SDK 末尾多了 lpDisplayName，本机 10.0.26100 是 11 个。
    // 照文档写 10 个会得到 C2660「函数不接受 10 个参数」——
    // 与其记参数个数，不如把 lpDisplayName 显式传 nullptr（不改显示名）。
    if (!::ChangeServiceConfigW(svc.get(), SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE,
                                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                nullptr)) {
        err = ::GetLastError();
        return false;
    }

    // 【两处都要写，缺一不可】
    // ChangeServiceConfigW 只动 dwStartType。延迟自动是**另一个** API：
    // ChangeServiceConfig2W(hService, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &info)，
    // 只有**三个**形参 —— cbBufSize / pcbBytesNeeded 是 Query 版才有的，别照抄。
    // 只写前者：用户选「自动（延迟启动）」，实际存成普通自动，下次开机时机变了；
    // 只写后者：用户选「手动」，延迟标志还留着，行为依旧不对。
    SERVICE_DELAYED_AUTO_START_INFO info{};
    info.fDelayedAutostart = delayed ? TRUE : FALSE;
    if (!::ChangeServiceConfig2W(svc.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                 &info)) {
        // 主变更已经生效了，这里的失败只影响「延迟」这一项。
        // 不能整体返回 false —— 那会让界面把一次**已经成功**的修改报成失败。
        LINGSVC_TRACE("ChangeServiceConfig2W(DELAYED=%d) for %s failed err=%lu",
                      delayed ? 1 : 0, name.c_str(), static_cast<unsigned long>(::GetLastError()));
    }

    err = ERROR_SUCCESS;
    return true;
}

bool ScmCreate(CreateServiceRequest const& req, DWORD& err) {
    ScManager mgr;
    if (!mgr.Open(SC_MANAGER_CREATE_SERVICE, err)) {
        return false;
    }

    // 权限取本次真正需要的最小集合。传 SERVICE_ALL_ACCESS 是文档常见写法，
    // 但权限不足时它会比必要更早失败，错误信息也更难解读成「缺哪个权限」。
    constexpr DWORD kCreateAccess = SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS |
                                    SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG;

    const std::wstring quoted = QuoteBinaryPath(req.binaryPath);

    SC_HANDLE h = ::CreateServiceW(
        mgr.get(),
        req.serviceName.c_str(),
        req.displayName.empty() ? nullptr : req.displayName.c_str(),
        kCreateAccess,
        SERVICE_WIN32_OWN_PROCESS,
        req.startType,
        SERVICE_ERROR_NORMAL,
        quoted.c_str(),
        nullptr,   // lpLoadOrderGroup：不分组
        nullptr,   // lpdwTagId：无本地服务
        nullptr,   // lpDependencies：方案已确认不做依赖多选
        req.account.empty() ? nullptr : req.account.c_str(),
        nullptr);  // lpPassword：方案已确认只支持内置账户，不传密码
    if (!h) {
        err = ::GetLastError();
        return false;
    }
    ::CloseServiceHandle(h);

    // 延迟启动标志要在服务建好之后、句柄还开着时写。
    if (req.delayedAutoStart) {
        ScService svc;
        DWORD e = ERROR_SUCCESS;
        if (svc.Open(mgr, req.serviceName, SERVICE_CHANGE_CONFIG, e)) {
            SERVICE_DELAYED_AUTO_START_INFO info{};
            info.fDelayedAutostart = TRUE;
            if (!::ChangeServiceConfig2W(svc.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                         &info)) {
                LINGSVC_TRACE("Create: delayed autostart set failed for %s err=%lu",
                              req.serviceName.c_str(),
                              static_cast<unsigned long>(::GetLastError()));
            }
        }
    }

    // 描述同样是建好之后写的独立 InfoLevel。
    if (!req.description.empty()) {
        ScService svc;
        DWORD e = ERROR_SUCCESS;
        if (svc.Open(mgr, req.serviceName, SERVICE_CHANGE_CONFIG, e)) {
            // ChangeServiceConfig2W 只有三个形参（cbBufSize / pcbBytesNeeded
            // 是 Query 版才有的，别照抄 MSDN 里混在一起的写法）。
            // 描述字符串要交给 API，但 SDK 把形参声明成非 const 的 LPVOID，
            // 而这个 API 实际只读它 —— const_cast 去掉顶层 const 是安全的，
            // 但要分两步：const wchar_t* → const void*（reinterpret）→ void*（const_cast）。
            const wchar_t* desc = req.description.c_str();
            if (!::ChangeServiceConfig2W(svc.get(), SERVICE_CONFIG_DESCRIPTION,
                                         const_cast<LPVOID>(
                                             static_cast<void const*>(desc)))) {
                LINGSVC_TRACE("Create: description set failed for %s err=%lu",
                              req.serviceName.c_str(),
                              static_cast<unsigned long>(::GetLastError()));
            }
        }
    }

    err = ERROR_SUCCESS;
    return true;
}

bool ScmDelete(std::wstring const& name, DWORD& err) {
    // 【权限是标准权限 DELETE，不是 SERVICE_DELETE】
    // winsvc.h 里只定义了 SERVICE_QUERY_CONFIG / CHANGE_CONFIG / QUERY_STATUS /
    // ENUMERATE_DEPENDENTS / START / STOP / PAUSE_CONTINUE / INTERROGATE /
    // USER_DEFINED_CONTROL，**没有** SERVICE_DELETE —— 删除服务用的是
    // winnt.h 里的标准权限 DELETE (0x00010000)。
    // 照着「服务专属权限」那一堆去猜名字，只会得到 C2065「未声明的标识符」。
    ScService svc;
    if (!OpenForWrite(name, DELETE | SERVICE_QUERY_STATUS, svc, err)) {
        return false;
    }

    // 服务在运行时 DeleteService 也会成功，语义是「标记删除」，
    // 真正消失要等进程退出。所以这里先看一眼状态，让调用方能提示用户。
    if (!::DeleteService(svc.get())) {
        err = ::GetLastError();
        return false;
    }
    err = ERROR_SUCCESS;
    return true;
}

}  // namespace core
}  // namespace lingsvc
