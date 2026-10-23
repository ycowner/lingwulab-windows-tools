// selftest.cpp —— 数据层离线自检实现
//
// 把 core 层真跑起来：连续采样 N 个周期，每周期做硬断言，最后把可读表格打到控制台。
// 输出统一走「手写 UTF-8 编码 + WriteFile」，不用 printf 家族的文本模式 ——
// 控制台代码页在中文机器上是 936，直接 wprintf 会把中文打成问号，
// 而重定向到文件时 _O_U16TEXT 还会写出 UTF-16 BOM 的字节序列，两个坑都踩不起。
#include "pch.h"

#include "selftest.h"

#include "core/monitor/appnames.h"
#include "core/monitor/autostart.h"
#include "core/monitor/metrics.h"
#include "core/monitor/sampler.h"
#include "core/monitor/terminate.h"
#include "core/monitor/top10.h"
#include "core/settings.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace lingwu {
namespace core {
namespace monitor {
namespace {

// ---------------------------------------------------------------- 输出

std::string  g_out;
std::wstring g_wbuf(16384, L'\0');
int          g_fail = 0;
int          g_pass = 0;

std::string Utf8(const wchar_t* s) {
    if (!s || !*s) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string out(static_cast<size_t>(need - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring W(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // 两个坑叠在一起，都是这版 UCRT（10.0.26100.0）实测出来的：
    //  1. 不能写 _vswprintf_s —— VC++2005 时代的下划线名已从 UCRT 移除，
    //     stdio.h / wchar.h 全都查无此声明，症状是 C2039 + C3861。
    //  2. vswprintf_s 是 **4 参**形式 (buffer, count, format, arglist)，
    //     _TRUNCATE 重载已经没有了 —— 实测 corecrt_wstdio.h:1268。
    //     仍按 5 参写会报 C2660「函数不接受 5 个参数」。
    // 代价：不再静默截断，溢出走 invalid parameter handler。缓冲开足即可。
    const int n = ::vswprintf_s(g_wbuf.data(), g_wbuf.size(), fmt, ap);
    va_end(ap);
    if (n < 0) return std::wstring();
    return std::wstring(g_wbuf.data(), static_cast<size_t>(n));
}

void Line(const std::wstring& s) {
    g_out += Utf8(s.c_str());
    g_out += '\n';
}

void Check(bool ok, const std::wstring& what) {
    if (ok) { ++g_pass; Line(L"  [PASS] " + what); }
    else    { ++g_fail; Line(L"  [FAIL] " + what); }
}

void FlushToParentConsole() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD ft = h ? ::GetFileType(h) : FILE_TYPE_UNKNOWN;
    // FILE_TYPE_UNKNOWN(0) = 没重定向（控制台或无）；FILE_TYPE_DISK/PIPE = 已被父进程接走。
    // 只有前者才需要 AttachConsole —— 反过来做会把已有的重定向句柄顶掉，
    // 症状是自检跑了但输出全丢。
    if (!h || ft == FILE_TYPE_UNKNOWN) {
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) h = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(h, g_out.data(), static_cast<DWORD>(g_out.size()), &written, nullptr);
    ::FlushFileBuffers(h);
}

// ---------------------------------------------------------------- 断言工具

// apps 的 pid 扁平数组必须与 app 行严格自洽。
bool PidsConsistent(const Snapshot& s) {
    uint64_t sumCount = 0;
    for (const AppInfo& a : s.apps) {
        if (static_cast<uint64_t>(a.pidStart) + a.pidCount > s.pids.size()) return false;
        if (a.pidCount != a.procCount) return false;
        sumCount += a.pidCount;
    }
    return sumCount == s.pids.size();
}

// 聚合口径下，同一行的所有 pid 必须来自同名进程。
bool AggregationHolds(const Snapshot& s) {
    // 建 pid -> name 映射（pid 复用由 createTime 兜底，但本进程生命周期内足够）
    for (const AppInfo& a : s.apps) {
        for (uint32_t i = 0; i < a.pidCount; ++i) {
            const DWORD pid = s.pids[a.pidStart + i];
            const std::wstring* n = nullptr;
            for (const ProcInfo& p : s.procs) {
                if (p.pid == pid) { n = &p.name; break; }
            }
            if (!n) return false;
            if (_wcsicmp(n->c_str(), a.name.c_str()) != 0) return false;
        }
    }
    return true;
}

bool NonIncreasing(const Snapshot& s, int mode) {
    for (size_t i = 0; i + 1 < s.apps.size(); ++i) {
        const AppInfo& a = s.apps[i];
        const AppInfo& b = s.apps[i + 1];
        switch (mode) {
            case SORT_MEM_DESC:
                if (AppMemKey(a) < AppMemKey(b)) return false;
                break;
            case SORT_MEM_ASC:
                if (AppMemKey(a) > AppMemKey(b)) return false;
                break;
            case SORT_CPU_DESC:
                // 必须与 AppLess 用同一把尺子（量化键），不能用裸 cpuNorm：
                // 量化后同一档内按名称排，档内两行的裸值可能差到 0.005%，
                // 拿裸值校验会把正确的顺序判成逆序，自检假失败。
                if (AppCpuSortKey(a.cpuNorm) < AppCpuSortKey(b.cpuNorm)) return false;
                break;
            case SORT_CPU_ASC:
                if (AppCpuSortKey(a.cpuNorm) > AppCpuSortKey(b.cpuNorm)) return false;
                break;
            case SORT_NAME_ASC:
                if (!a.name.empty() && !b.name.empty() &&
                    _wcsicmp(a.name.c_str(), b.name.c_str()) > 0) return false;
                break;
            default:
                return false;
        }
    }
    return true;
}

void DumpTop(const Snapshot& s, int rows) {
    AppNamesBeginFrame();   // 每帧预算重置：只解析 4 个，其余先显示 exe 名
    Line(L"  排名  名称                          CPU      内存        进程数  PID(前 6 个)");
    Line(L"  ----  ----------------------------  -------  ----------  ------  ------------------------");
    for (int i = 0; i < rows && i < static_cast<int>(s.apps.size()); ++i) {
        const AppInfo& a = s.apps[i];

        // 先从快照里取 createTime，再在锁外问显示名 —— 契约 7。
        ULONGLONG ct = 0;
        if (a.pidCount > 0) {
            const DWORD pid = s.pids[a.pidStart];
            for (const ProcInfo& p : s.procs) {
                if (p.pid == pid) { ct = p.createTime; break; }
            }
        }
        const std::wstring friendly = FriendlyAppName(
            a.pidCount ? s.pids[a.pidStart] : 0, ct, a.name);
        const std::wstring disp = Ellipsize(AppDisplayName(a.name, friendly), 28);

        std::wstring pids;
        for (uint32_t k = 0; k < a.pidCount && k < 6; ++k) {
            if (!pids.empty()) pids += L",";
            pids += std::to_wstring(s.pids[a.pidStart + k]);
        }
        if (a.pidCount > 6) pids += L",...";

        Line(W(L"  %4d  %-28ls  %6ls  %10ls  %6u  %ls",
               i + 1, disp.c_str(), FormatCpu(a.cpuNorm).c_str(),
               FormatBytes(a.privWorkSet).c_str(), a.procCount, pids.c_str()));
    }
}

}  // namespace

int RunSelfTest(int ticks) {
    if (ticks < 2) ticks = 2;

    Line(L"");
    Line(L"================ LingwuToolbox · 数据层自检（性能监视侧） ================");

    // ---------------- 设置持久化 ----------------
    Line(L"");
    Line(L"[1] 设置持久化  %APPDATA%\\LingwuToolbox\\settings.ini");
    core::Settings st;
    core::LoadSettings(st);
    Line(W(L"  目录          = %ls", core::SettingsDir().c_str()));
    Line(W(L"  Aggregate     = %s", st.aggregate ? L"1" : L"0"));
    Line(W(L"  SortMode      = %d  (%ls)", st.sortMode, SortModeLabel(st.sortMode)));
    Line(W(L"  IntervalMs    = %d", st.intervalMs));
    Line(W(L"  TrayHintShown = %s", st.trayHintShown ? L"1" : L"0"));
    Line(W(L"  CloseAction   = %d", st.closeAction));
    Check(st.sortMode >= 0 && st.sortMode <= 4, L"sortMode 收敛在 0..4");
    Check(st.intervalMs >= 250 && st.intervalMs <= 10000, L"intervalMs 收敛在 250..10000");

    // SaveSettings 往返：先备份再写、校验后还原，异常也保证用户的 ini 不丢。
    {
        const std::wstring ini = core::SettingsDir() + L"\\settings.ini";
        const std::wstring bak = ini + L".selftest.bak";
        const bool hadIni = (::GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES);
        if (hadIni) ::CopyFileW(ini.c_str(), bak.c_str(), FALSE);

        core::Settings probe = st;
        probe.aggregate   = !st.aggregate;
        probe.sortMode    = (st.sortMode + 2) % 5;
        probe.intervalMs  = 1500;
        core::SaveSettings(probe);

        core::Settings back;
        core::LoadSettings(back);
        Check(back.aggregate == probe.aggregate &&
              back.sortMode  == probe.sortMode  &&
              back.intervalMs == probe.intervalMs,
              L"SaveSettings → LoadSettings 往返一致");

        if (hadIni) {
            ::CopyFileW(bak.c_str(), ini.c_str(), FALSE);
            ::DeleteFileW(bak.c_str());
        } else {
            ::DeleteFileW(ini.c_str());
        }
        core::LoadSettings(st);   // 把内存里的值恢复成磁盘真实值
        Line(L"  （往返测试已还原原始 ini）");
    }

    // ---------------- 开机自启 ----------------
    Line(L"");
    Line(L"[2] 开机自启  HKCU\\...\\CurrentVersion\\Run");
    bool asEnabled = false, asStale = false;
    AutostartReconcile(asEnabled, asStale);
    const std::wstring regCmd = AutostartRegisteredCommand();
    Line(W(L"  已注册        = %s", asEnabled ? L"是" : L"否"));
    Line(W(L"  注册表值      = %ls", regCmd.empty() ? L"(无)" : regCmd.c_str()));
    Line(W(L"  本 exe 应写入  = %ls", AutostartCommandForSelf().c_str()));
    Line(W(L"  stale(路径不符) = %s", asStale ? L"是" : L"否"));
    Check(!asStale, L"注册表路径与当前 exe 一致（stale=false）");
    Check(AutostartIsEnabled() == asEnabled, L"AutostartIsEnabled 与 Reconcile 一致");

    // ---------------- 结束进程的无害路径 ----------------
    Line(L"");
    Line(L"[3] 结束进程（只测受保护/自身被跳过的无害路径，不杀任何真实进程）");
    {
        const DWORD self = ::GetCurrentProcessId();
        const DWORD probes[] = { 0, 4, self };
        int killed = -1;
        const KillResult r = TerminateAppPids(probes, 3, false, 0, &killed);
        Line(W(L"  pid {0, 4, %u(自身)} → result=%d killed=%d", self, r, killed));
        Check(killed == 0, L"pid 0 / 4 / 自身全部被跳过（killed=0）");
        Check(r == KILL_FAILED, L"无可杀进程时返回 KILL_FAILED");
    }

    // ---------------- 采样 ----------------
    Line(L"");
    Line(W(L"[4] 采样 %d 个周期（每秒一次）", ticks));
    Sampler sampler;
    sampler.SetGroupMode(st.aggregate);
    sampler.SetSortMode(st.sortMode);
    if (!sampler.Start(1000)) {
        Line(L"  [FAIL] 采样线程启动失败");
        FlushToParentConsole();
        return ++g_fail;
    }

    uint64_t peakPrivBytes = 0;
    uint64_t lastDown = 0, lastUp = 0;
    int cpuSeenCycles = 0;
    Snapshot lastSnap;

    for (int t = 1; t <= ticks; ++t) {
        // 等这一周期的快照落地。这里曾崩过 0xC0000094：
        // 1 秒定时器实际会早触发十几毫秒，整数分母截断成 0。
        while (!sampler.ConsumeDirty()) ::Sleep(20);

        sampler.Lock();
        const Snapshot& s = sampler.View();
        uint64_t privSum = 0;
        for (const ProcInfo& p : s.procs) privSum += p.privBytes;
        if (privSum > peakPrivBytes) peakPrivBytes = privSum;
        lastDown = s.net.downBps;
        lastUp   = s.net.upBps;
        lastSnap = s;              // 深拷贝一份，锁外再排序/断言
        sampler.Unlock();

        bool anyCpu = false;
        for (const AppInfo& a : lastSnap.apps) { if (a.cpuNorm > 0.0) { anyCpu = true; break; } }
        if (anyCpu) ++cpuSeenCycles;

        Line(L"");
        Line(W(L"  ── 周期 %d/%d ── 进程 %zu · 应用 %zu · pid 表 %zu · 核心 %u",
               t, ticks, lastSnap.procs.size(), lastSnap.apps.size(),
               lastSnap.pids.size(), lastSnap.coreCount));
        Line(W(L"     内存口径：%ls（PrivatePageCount，私有提交字节）",
               lastSnap.memIsPrivateWorkingSet ? L"私有工作集" : L"私有提交"));
        Line(W(L"     下行 %ls   上行 %ls   网卡：%ls",
               FormatSpeed(lastDown).c_str(), FormatSpeed(lastUp).c_str(),
               lastSnap.net.nic.empty() ? L"(未匹配到物理网卡)" : lastSnap.net.nic.c_str()));

        // 每个周期都做结构性断言，坏了要能定位到第几个周期。
        const std::wstring at = W(L"[周期 %d] ", t);
        Check(!lastSnap.procs.empty(), at + L"采到进程（排除 Idle 后仍 > 0）");
        Check(!lastSnap.apps.empty(),  at + L"聚合出行");
        Check(PidsConsistent(lastSnap), at + L"pid 扁平表与各行 pidStart/pidCount 自洽");
        Check(lastSnap.coreCount >= 1, at + L"逻辑核数 >= 1");

        if (t == ticks) DumpTop(lastSnap, 15);
    }

    sampler.Stop();

    Line(L"");
    Line(L"[5] 跨周期断言");
    Check(cpuSeenCycles >= ticks - 1,
          W(L"除首个周期外每个周期都有非零 CPU（差分链路 %d/%d 有效）",
            cpuSeenCycles, ticks));
    Check(peakPrivBytes > 100ull * 1024 * 1024,
          W(L"私有提交总量 > 100 MB（实测 %ls）", FormatBytes(peakPrivBytes).c_str()));
    Check(AggregationHolds(lastSnap),
          st.aggregate ? L"聚合口径下每行内所有进程同名" : L"逐进程口径无需同名校验");

    // 排序：五个档位都要真的把顺序排对（对同一份快照重排，不重新采样）
    Line(L"");
    Line(L"[6] 五档排序（对同一份快照重排）");
    for (int m = 0; m <= 4; ++m) {
        Snapshot copy = lastSnap;
        BuildViews(copy, st.aggregate, m);
        Line(W(L"  %d %-16ls 首行 = %ls (%ls, %ls)",
               m, SortModeLabel(m),
               copy.apps.empty() ? L"(空)" : copy.apps.front().name.c_str(),
               copy.apps.empty() ? L"-" : FormatCpu(copy.apps.front().cpuNorm).c_str(),
               copy.apps.empty() ? L"-" : FormatBytes(copy.apps.front().privWorkSet).c_str()));
        Check(NonIncreasing(copy, m), W(L"档位 %d 顺序单调正确", m));
        Check(PidsConsistent(copy), W(L"档位 %d 重排后 pid 表仍自洽", m));
    }

    // 逐进程口径
    {
        Snapshot copy = lastSnap;
        BuildViews(copy, false, st.sortMode);
        Line(W(L"  逐进程口径：%zu 行（聚合口径 %zu 行，进程 %zu）",
               copy.apps.size(), lastSnap.apps.size(), lastSnap.procs.size()));
        Check(copy.apps.size() == lastSnap.procs.size(),
              L"逐进程口径行数 == 进程数");
    }

    Line(L"");
    Line(W(L"================ 结果：通过 %d，失败 %d ================", g_pass, g_fail));
    Line(L"");
    FlushToParentConsole();
    return g_fail;
}

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
