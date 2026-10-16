#include "pch.h"

#include "core/selftest.h"

#include <cstdio>

#include "core/scm.h"
#include "core/service_model.h"

namespace lingsvc {
namespace core {
namespace {

int g_failures = 0;

void Check(bool ok, char const* what) {
    if (ok) {
        std::printf("  [ok]   %s\n", what);
    } else {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

void Report(char const* what, unsigned long value) {
    std::printf("  [FAIL] %s (value=%lu)\n", what, value);
}

// 交叉验证用的对照服务名。挑选原则：**必须存在**且状态会变。
//
// 为什么用「枚举结果里的第一条」而不是硬编码服务名：硬编码一个名字会在
// 某个系统上恰好没有它（服务被精简掉的 N 版镜像 / 容器环境），
// 于是自检在别人机器上无故失败，而那种失败会被当成「代码有问题」白排查半天。
std::wstring PickProbeName(std::vector<ServiceInfo> const& all) {
    for (auto const& s : all) {
        if (s.state == 4 /* SERVICE_RUNNING */) {
            return s.name;
        }
    }
    return all.empty() ? std::wstring() : all.front().name;
}

}  // namespace

int RunSelfTest(int /*ticks*/) {
    g_failures = 0;
    std::printf("LingwuServiceManager --selftest\n");
    std::printf("  OS: ");
    {
        wchar_t buf[MAX_PATH] = {};
        const UINT n = ::GetWindowsDirectoryW(buf, MAX_PATH);
        std::printf("WinDir=%ls\n", (n > 0 && n < MAX_PATH) ? buf : L"?");
    }

    // ---- 1. 纯函数的引号逻辑（不碰 SCM，先跑，失败最好定位）----
    std::printf("\n[1] QuoteBinaryPath\n");
    {
        Check(QuoteBinaryPath(L"C:\\svc.exe") == L"C:\\svc.exe",
              "no-space path stays unquoted");
        Check(QuoteBinaryPath(L"C:\\Program Files\\a.exe") == L"\"C:\\Program Files\\a.exe\"",
              "path with space gets quoted");
        Check(QuoteBinaryPath(L"\"C:\\Program Files\\a.exe\"") == L"\"C:\\Program Files\\a.exe\"",
              "already-quoted path is not double-quoted");
        Check(QuoteBinaryPath(L"").empty(), "empty path stays empty");
    }

    // ---- 2. 第三方判定 ----
    std::printf("\n[2] IsThirdPartyBinary\n");
    {
        const std::wstring wd = WindowsDir();
        Check(!wd.empty(), "GetWindowsDirectory succeeded");
        if (!wd.empty()) {
            const std::wstring under = wd + L"\\System32\\svchost.exe";
            Check(!IsThirdPartyBinary(under), "path under %WinDir% is NOT third-party");
            Check(!IsThirdPartyBinary(L"\"" + under + L"\""),
                  "quoted path under %WinDir% is NOT third-party");
            // 极性容易写反：IsThirdPartyBinary 返回 **true** 才是第三方，
            // 所以这里要断言它为 true。写 ! 会得到一条永远「不通过」的断言 ——
            // 它看起来像 IsThirdPartyBinary 坏了，其实是断言反了。
            Check(IsThirdPartyBinary(L"C:\\Users\\me\\app\\svc.exe"),
                  "path outside %WinDir% IS third-party");
            Check(!IsThirdPartyBinary(L""), "empty path is NOT third-party (fail-safe)");
            // C:\WindowsOld 不该被 C:\Windows 命中 —— 分隔符边界
            Check(!PathUnder(L"C:\\WindowsOld\\a.exe", wd),
                  "sibling dir C:\\WindowsOld is not treated as under %WinDir%");
        }
    }

    // ---- 3. 枚举 ----
    std::printf("\n[3] EnumServicesStatusW\n");
    ScmSession session;
    DWORD err = ERROR_SUCCESS;
    std::vector<ServiceInfo> all;
    if (!session.Open(err)) {
        std::printf("  [FAIL] OpenSCManager failed err=%lu\n", (unsigned long)err);
        return ++g_failures;
    }
    if (!session.EnumAll(all, err)) {
        std::printf("  [FAIL] EnumAll failed err=%lu\n", (unsigned long)err);
        return ++g_failures;
    }

    std::printf("  enumerated %zu services\n", (size_t)all.size());
    // 这个下限是**状态过滤参数**的判据：传错成只列活动服务时本机约 150 条，
    // 传 0（不过滤）应该在 250 条以上。两边都"看起来正常"，只有数字能区分。
    Check(all.size() > 100, "enumerated more than 100 services (state filter is off)");

    // ---- 4. 字段合法性 ----
    std::printf("\n[4] field validity\n");
    {
        size_t badState = 0;
        size_t badStart = 0;
        size_t noName = 0;
        size_t knownStart = 0;
        for (auto const& s : all) {
            if (!IsValidState(s.state)) ++badState;
            if (s.name.empty()) ++noName;
            if (s.startTypeKnown) {
                ++knownStart;
                if (!IsValidStartType(s.startType)) ++badStart;
            }
        }
        std::printf("  startType known for %zu / %zu\n", knownStart, (size_t)all.size());
        if (badState) Report("services with invalid dwCurrentState", (unsigned long)badState);
        Check(badState == 0, "every dwCurrentState is in 1..7");
        if (badStart) Report("services with invalid dwStartType", (unsigned long)badStart);
        Check(badStart == 0, "every dwStartType is in 0..4");
        if (noName) Report("services with empty name", (unsigned long)noName);
        Check(noName == 0, "every service has a non-empty name");

        // 非提权时一部分服务查不到启动类型是**预期**的，
        // 但不能是全部 —— 全空说明查询逻辑根本没跑。
        Check(knownStart > all.size() / 4,
              "at least a quarter of services resolved their start type");

        // PID 现在来自逐服务 QueryServiceStatusEx（经典枚举给不了）。
        // 只要有运行中的服务，就必须一条不落地拿到 PID：
        // 这条断言是「EnumServicesStatusExW 换成经典 API」那次改动的护栏，
        // 少了它，最坏情况是列表全空也照样 PASS。
        size_t running = 0;
        size_t runningWithPid = 0;
        for (auto const& s : all) {
            if (s.state != SERVICE_RUNNING) continue;
            ++running;
            if (s.pid != 0) ++runningWithPid;
        }
        if (running > 0) {
            std::printf("  running=%zu  with pid=%zu\n", running, runningWithPid);
            Check(runningWithPid == running, "every RUNNING service resolved its PID");
        } else {
            std::printf("  (no RUNNING services on this machine; PID check skipped)\n");
        }
    }

    // ---- 5. 与单服务查询交叉验证 ----
    std::printf("\n[5] cross-check EnumAll vs QueryServiceStatus\n");
    {
        const std::wstring probe = PickProbeName(all);
        Check(!probe.empty(), "found a probe service");
        if (!probe.empty()) {
            DWORD state = 0;
            DWORD qerr = ERROR_SUCCESS;
            const bool ok = ScmQueryState(probe, state, qerr);
            Check(ok, "ScmQueryState succeeded on the probe service");
            if (ok) {
                DWORD fromEnum = 0;
                for (auto const& s : all) {
                    if (s.name == probe) {
                        fromEnum = s.state;
                        break;
                    }
                }
                std::printf("  probe=%ls  enum=%lu  query=%lu\n", probe.c_str(),
                            (unsigned long)fromEnum, (unsigned long)state);
                Check(state == fromEnum,
                      "probe service state matches between enum and query");
            }
        }
    }

    // ---- 6. 详情 ----
    std::printf("\n[6] QueryServiceConfigW details\n");
    {
        ServiceDetails d;
        DWORD derr = ERROR_SUCCESS;
        const std::wstring probe = PickProbeName(all);
        const bool ok = !probe.empty() && session.QueryDetails(probe, d, derr);
        Check(ok, "QueryDetails succeeded on the probe service");
        if (ok) {
            std::printf("  account=%ls\n", d.account.c_str());
            std::printf("  binary =%ls\n", d.binaryPath.c_str());
            Check(!d.binaryPath.empty(), "details carry a binary path");
            // 内置账户的取值就是 SCM 存下来的字面量，大小写按 SCM 的写法
            Check(!d.account.empty(), "details carry a logon account");
        }
    }

    // ---- 7. 第二次枚举走缓存路径 ----
    std::printf("\n[7] second enumeration reuses the start-type cache\n");
    {
        std::vector<ServiceInfo> again;
        const auto t0 = GetTickCount64();
        const bool ok = session.EnumAll(again, err);
        const auto dt = GetTickCount64() - t0;
        Check(ok, "second EnumAll succeeded");
        Check(again.size() == all.size(), "second enumeration returns the same count");
        std::printf("  second pass took %llu ms (cached start types)\n",
                    (unsigned long long)dt);
    }

    std::printf("\n%s: %d failure(s)\n", (g_failures == 0) ? "PASS" : "FAIL", g_failures);
    return g_failures;
}

}  // namespace core
}  // namespace lingsvc
