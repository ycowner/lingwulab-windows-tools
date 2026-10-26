// selftest.cpp —— 数据层离线自检实现
//
// 把 core 层真跑起来：连续采样 N 个周期，每周期做硬断言，最后把可读表格打到控制台。
// 输出统一走「手写 UTF-8 编码 + WriteFile」，不用 printf 家族的文本模式 ——
// 控制台代码页在中文机器上是 936，直接 wprintf 会把中文打成问号，
// 而重定向到文件时 _O_U16TEXT 还会写出 UTF-16 BOM 的字节序列，两个坑都踩不起。
#include "pch.h"

#include "selftest.h"

#include "core/monitor/appicon.h"
#include "core/monitor/appnames.h"
#include "core/monitor/autostart.h"
#include "core/monitor/health.h"
#include "core/monitor/metrics.h"
#include "core/monitor/sampler.h"
#include "core/monitor/sparkline.h"
#include "core/monitor/system_metrics.h"
#include "core/monitor/terminate.h"
#include "core/monitor/top10.h"
#include "core/settings.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include <tlhelp32.h>

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
// 找一个「系统保护进程」的 pid 当候选。
//
// 【这里只负责「找候选」，不负责判定受不受保护】
// 判定必须用**与 TerminateAppPids 完全相同的访问掩码**去探 OpenProcess，
// 只有那样才能推出「探失败 ⇒ TerminateAppPids 里那一次也必然打不开」。
// 判定逻辑放在调用方，理由见那里的注释。
DWORD FindProtectedProcessCandidate() {
    const wchar_t* kNames[] = { L"wininit.exe", L"csrss.exe", L"Registry" };
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (::Process32FirstW(snap, &pe)) {
        do {
            for (const wchar_t* name : kNames) {
                if (_wcsicmp(pe.szExeFile, name) == 0) { found = pe.th32ProcessID; break; }
            }
            if (found) break;
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return found;
}

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
    Line(W(L"  LastPage      = %d", st.lastPage));
    Line(W(L"  面板Top进程行 = %d", st.trayPanelProcessCount));
    Line(W(L"  KeepAwake     = %d", st.keepAwake ? 1 : 0));
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
        // 新增的键也要一起往返 —— 少写一个键，用户就会发现
        // 「重启之后这个设置又变回去了」，而读写两边的代码看上去都没错。
        probe.lastPage               = (st.lastPage + 1) % 6;
        probe.trayPanelProcessCount  = 7;
        probe.keepAwake              = !st.keepAwake;
        probe.cleanScreenLockInput   = !st.cleanScreenLockInput;
        core::SaveSettings(probe);

        core::Settings back;
        core::LoadSettings(back);
        Check(back.aggregate == probe.aggregate &&
              back.sortMode  == probe.sortMode  &&
              back.intervalMs == probe.intervalMs,
              L"SaveSettings → LoadSettings 往返一致");
        Check(back.lastPage == probe.lastPage &&
              back.trayPanelProcessCount == probe.trayPanelProcessCount &&
              back.keepAwake == probe.keepAwake &&
              back.cleanScreenLockInput == probe.cleanScreenLockInput,
              L"新增设置项（页签/面板行数/常亮/输入锁定）往返一致");

        // 收敛：手改成越界值必须被拉回默认，不能带着非法值进运行时。
        {
            core::Settings bad = probe;
            bad.lastPage = 99;
            bad.trayPanelProcessCount = 999;
            core::SaveSettings(bad);
            core::Settings b2;
            core::LoadSettings(b2);
            Check(b2.lastPage == 4 && b2.trayPanelProcessCount == 5,
                  L"越界的页签/面板行数被收敛回默认值");
        }

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

    // ---------------- 「访问被拒」分支（结束进程） ----------------
    //
    // 【为什么单独测这一条】
    // 这条分支以前没有覆盖过（合并前叫 KILL_NEEDS_ADMIN）。它现在决定的是
    // 「结束任务」碰到受保护进程时弹什么文案 —— 合并成工具箱后整个程序恒定
    // 以管理员身份运行，那条路径从「提权就能解决」变成了「进程受系统保护」，
    // 语义变了，值得钉住。
    //
    // 【安全闸门 —— 为什么不能直接拿候选 pid 去调 TerminateAppPids】
    // TerminateAppPids 内部是「OpenProcess 成功就 TerminateProcess」。
    // 万一挑到的进程在**这台机器上不受保护**，那一调就是真的把它杀了 ——
    // 挑到 csrss / wininit 的话就是蓝屏。
    // 所以顺序必须是：先用**完全相同的访问掩码**自己探一次 OpenProcess。
    //   · 探到「打不开且 ERROR_ACCESS_DENIED」→ 证明它受保护，
    //     TerminateAppPids 里那一次 OpenProcess 必然同样失败，
    //     不可能杀到它 → 这时调用是安全的；
    //   · 探到「能打开」→ 说明本机它不受保护，**立即放弃**，绝不调用。
    // 另外 gracefulFirst 传 false：不走「给顶层窗口发 WM_CLOSE」那段，
    // 整个探针对系统没有任何副作用。
    Line(L"");
    Line(L"[3b] 结束进程：受保护进程 → KILL_ACCESS_DENIED");
    {
        const DWORD candidate = FindProtectedProcessCandidate();
        if (candidate == 0) {
            Line(L"  （本机没找到系统保护进程，跳过这一项 —— 不计失败）");
        } else {
            // 安全闸门：与 TerminateAppPids 用**同一个掩码**探一次。
            HANDLE probe = ::OpenProcess(
                PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, candidate);
            const DWORD probeErr = probe ? ERROR_SUCCESS : ::GetLastError();
            if (probe) {
                ::CloseHandle(probe);
                Line(W(L"  pid %u 在本机可打开（不受保护）→ 放弃这一项，绝不调用 TerminateAppPids",
                        candidate));
            } else if (probeErr != ERROR_ACCESS_DENIED) {
                Line(W(L"  pid %u 打不开但原因不是 ERROR_ACCESS_DENIED（%lu）→ 放弃这一项",
                        candidate, (unsigned long)probeErr));
            } else {
                int killed = -1;
                const KillResult r = TerminateAppPids(&candidate, 1, false, 0, &killed);
                Line(W(L"  pid %u（受保护）→ result=%d killed=%d", candidate, r, killed));
                Check(r == KILL_ACCESS_DENIED, L"打不开的进程返回 KILL_ACCESS_DENIED");
                Check(killed == 0, L"打不开的进程一个都没杀掉");
            }
        }
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
        Check(lastSnap.memory.totalBytes > 0, at + L"物理内存总量 > 0");
        Check(lastSnap.memory.usedPercent >= 0.0 && lastSnap.memory.usedPercent <= 100.0,
              at + L"物理内存占用率落在 0..100");
        Check(lastSnap.healthValid &&
              lastSnap.healthScore >= 0 && lastSnap.healthScore <= 100,
              at + L"健康分有效且落在 0..100");

        if (t == ticks) DumpTop(lastSnap, 15);
    }

    sampler.Stop();

    // ---------------- 系统级指标 + 健康分 ----------------
    //
    // 【为什么单独测健康分的算法边界】
    // 健康分是纯函数，不测边界就等于没测：三个因子的权重、阈值、
    // 以及「磁盘取不到时权重归一」这条分支，任何一处写反都不会报错，
    // 只会让用户看到一个说不通的分数。
    Line(L"");
    Line(L"[4b] 系统级指标（磁盘 / 电池 / 内存 / 主机信息）与健康分");
    {
        const DiskSpaceInfo& ds = lastSnap.diskSpace;
        Line(W(L"  磁盘 %ls：总量 %ls · 剩余 %ls",
               ds.volume.empty() ? L"(未知盘)" : ds.volume.c_str(),
               FormatBytes(ds.totalBytes).c_str(),
               FormatBytes(ds.freeBytes).c_str()));
        Check(!ds.valid || (ds.totalBytes > 0 && ds.freeBytes <= ds.totalBytes),
              L"磁盘剩余自洽（总量 > 0 且剩余 <= 总量）");

        const DiskActivityInfo& da = lastSnap.diskActivity;
        wchar_t busy[32] = { 0 };
        swprintf_s(busy, L"%.0f%%", da.percentBusy);
        Line(W(L"  磁盘活动：%ls · 读 %ls/s · 写 %ls/s",
               da.valid ? busy : L"(读不到，需要管理员权限)",
               FormatBytes(da.readBytesPerSec).c_str(),
               FormatBytes(da.writeBytesPerSec).c_str()));
        Check(!da.valid || (da.percentBusy >= 0.0 && da.percentBusy <= 100.0),
              L"磁盘活动率落在 0..100（多队列盘已钳过）");

        const BatteryInfo& bat = lastSnap.battery;
        wchar_t batPct[32] = { 0 };
        swprintf_s(batPct, L"%d%%", bat.percent);
        // 电池有三种状态，提示语必须区分开：
        //   有电池 → 百分比
        //   确实没电池 → 台式机
        //   读不到 → 状态未知
        // 三者混成一句话会骗用户（2026-10-26 笔记本就被误报成「本机无电池」，
        // 真凶是 BatteryFlag 的位被解释反了，详见 README 踩坑 52）。
        const std::wstring batDesc =
            bat.present ? std::wstring(batPct)
            : bat.known   ? std::wstring(L"(本机无电池)")
                          : std::wstring(L"(电池状态未知)");
        Line(W(L"  电池：%ls", batDesc.c_str()));
        Check(!bat.present || (bat.percent >= 0 && bat.percent <= 100),
              L"电池电量落在 0..100");
        // present 与 known 的组合必须自洽：有电池必然读到了状态；
        // 「读不到」不能同时说「没有电池」。
        Check(!(bat.present && !bat.known),
              L"电池状态自洽（有电池则状态一定读到了）");

        // ---------------- 电池位语义（纯逻辑，与本机有无电池无关）----------------
        //
        // 【为什么必须单独测位含义】
        // 「本机无电池」误报的真凶是 SYSTEM_POWER_STATUS.BatteryFlag 的位
        // 被解释反了（0x01 是 BATTERY_FLAG_HIGH 电量充足，0x80 才是
        // BATTERY_FLAG_NO_BATTERY）。这类错误**不改任何运行时行为**：
        // 没有电池的机器上 present 恒为 false，断言 `!present || ...`
        // 直接通过，测不出任何东西 —— 这正是它能活到今天的原因。
        //
        // 所以这里用 DecodeBatteryStatus 喂**构造出来的** flag，
        // 让 6 个位语义在任何机器上都被走一遍。
        Line(L"");
        Line(L"[4b-1] 电池位语义（构造 flag，与本机有无电池无关）");
        {
            // 0x01 = BATTERY_FLAG_HIGH（电量充足）—— 这条就是原 bug 的回归点：
            // 修复前它被判成「无电池」，笔记本被当成台式机。
            const BatteryInfo high = DecodeBatteryStatus(
                BATTERY_FLAG_HIGH, 99, BATTERY_LIFE_UNKNOWN);
            Check(high.present && high.known,
                  L"BATTERY_FLAG_HIGH(0x01) 是「电量充足」，不是「无电池」");
            Check(!high.charging, L"BATTERY_FLAG_HIGH(0x01) 不表示充电中");
            Check(high.percent == 99, L"电量百分比原样透传");

            // 0x02 = BATTERY_FLAG_LOW（电量低）
            const BatteryInfo low = DecodeBatteryStatus(
                BATTERY_FLAG_LOW, 40, BATTERY_LIFE_UNKNOWN);
            Check(low.present && low.known && !low.charging,
                  L"BATTERY_FLAG_LOW(0x02) 是「电量低」，有电池且不充电");

            // 0x04 = BATTERY_FLAG_CRITICAL（电量极低）—— 修复前被当成「充电中」。
            const BatteryInfo crit = DecodeBatteryStatus(
                BATTERY_FLAG_CRITICAL, 5, BATTERY_LIFE_UNKNOWN);
            Check(crit.present && crit.known,
                  L"BATTERY_FLAG_CRITICAL(0x04) 是「电量极低」，有电池");
            Check(!crit.charging,
                  L"BATTERY_FLAG_CRITICAL(0x04) 不是「充电中」（充电是 0x08）");

            // 0x08 = BATTERY_FLAG_CHARGING（充电中）—— 修复前读反了。
            const BatteryInfo chg = DecodeBatteryStatus(
                BATTERY_FLAG_CHARGING, 50, 3600);
            Check(chg.present && chg.known && chg.charging,
                  L"BATTERY_FLAG_CHARGING(0x08) 是「充电中」");
            Check(chg.secondsLeft == 3600, L"剩余秒数原样透传");

            // 0x80 = BATTERY_FLAG_NO_BATTERY（确实没有电池，台式机）
            const BatteryInfo none = DecodeBatteryStatus(
                BATTERY_FLAG_NO_BATTERY, 255, BATTERY_LIFE_UNKNOWN);
            Check(!none.present && none.known,
                  L"BATTERY_FLAG_NO_BATTERY(0x80) 是「确实没有电池」");
            Check(!none.charging, L"无电池时不报充电中");

            // 0xFF = BATTERY_FLAG_UNKNOWN（状态未知）——
            // 它的所有位都是 1，**包含** 0x80 位，所以判据顺序不能反：
            // 先判 NO_BATTERY 会把「未知」误吞成「没有电池」。
            const BatteryInfo unk = DecodeBatteryStatus(
                BATTERY_FLAG_UNKNOWN, 255, BATTERY_LIFE_UNKNOWN);
            Check(!unk.present && !unk.known,
                  L"BATTERY_FLAG_UNKNOWN(0xFF) 是「状态未知」，不是「没有电池」");

            // 百分比与剩余时间的哨兵值
            const BatteryInfo sentinel = DecodeBatteryStatus(
                BATTERY_FLAG_HIGH, BATTERY_PERCENTAGE_UNKNOWN, BATTERY_LIFE_UNKNOWN);
            Check(sentinel.percent == 0,
                  L"百分比未知(0xFF) 归零，不当 255 用");
            Check(sentinel.secondsLeft == 0,
                  L"剩余时间未知(0xFFFFFFFF) 归零，不当真实秒数用");
        }

        Line(W(L"  物理内存：已用 %ls / 总量 %ls（占用 %.0f%%）",
               FormatBytes(lastSnap.memory.totalBytes - lastSnap.memory.availableBytes).c_str(),
               FormatBytes(lastSnap.memory.totalBytes).c_str(),
               lastSnap.memory.usedPercent));

        Line(W(L"  CPU：%ls", lastSnap.host.cpuName.empty() ? L"(读不到)"
                                                          : lastSnap.host.cpuName.c_str()));
        Line(W(L"  系统：%ls · 已运行 %ls",
               lastSnap.host.osName.empty() ? L"(未知)" : lastSnap.host.osName.c_str(),
               FormatUptime(lastSnap.host.uptimeSeconds).c_str()));
        Check(lastSnap.host.uptimeSeconds > 0, L"运行时长 > 0");

        Line(W(L"  健康分：%d（%ls）", lastSnap.healthScore, lastSnap.healthGrade.c_str()));

        const HealthResult perfect = ComputeHealth(0.0, 0.0, 100.0);
        const HealthResult worst   = ComputeHealth(100.0, 100.0, 0.0);
        const HealthResult noDisk  = ComputeHealth(50.0, 50.0, -1.0);
        const HealthResult bad     = ComputeHealth(-1.0, 50.0, 50.0);
        Check(perfect.valid && perfect.score == 100 && perfect.grade == L"优秀",
              L"健康分：理想读数 → 100 优秀");
        Check(worst.valid && worst.score == 0 && worst.grade == L"较差",
              L"健康分：最差读数 → 0 较差");
        Check(noDisk.valid,
              L"健康分：磁盘剩余取不到时仍有效（权重归一，不作废整项）");
        Check(!bad.valid,
              L"健康分：CPU 读数非法时整项无效");
    }

    // ---------------- 趋势序列 ----------------
    //
    // 【为什么单独测环形缓冲】
    // 折线画错了（顺序反了、丢的是最新而不是最旧、窗口不滚动）时，
    // 界面上只会看到「一条形状有点怪的线」—— 不会抛异常、不会崩、不会有任何报错。
    // 唯一能钉住它的地方就是这里：环形的语义必须逐条验。
    Line(L"");
    Line(L"[4c] 趋势序列（磁贴折线的数据源）");
    {
        // ① 采样期间确实攒下了点。
        // 用 >= 而不是 ==：采样线程可能比自检循环跑得快（自检每轮要等一个脏标志，
        // 采样线程是固定 1 秒一拍），读到的那一帧可能已经多攒了几点。
        // 真正要钉的是「攒下了」，不是「正好一个不多一个不少」。
        const size_t cpuPts = lastSnap.history.cpu.Size();
        Check(cpuPts >= static_cast<size_t>(ticks),
              W(L"采样 %d 周期后 CPU 序列至少攒下 %zu 个点", ticks, cpuPts));
        Check(lastSnap.history.mem.Size() == cpuPts &&
              lastSnap.history.disk.Size() == cpuPts &&
              lastSnap.history.health.Size() == cpuPts,
              L"各条序列点数一致");

        // ② 环形语义：塞超过容量 → 只留最近的一批，最旧的被覆盖。
        Series s;
        for (int i = 0; i < 100; ++i) s.Push(static_cast<double>(i));
        Check(s.Size() == Series::Capacity(),
              W(L"环形缓冲写满后不再增长（容量 %zu，实测 %zu）",
                Series::Capacity(), s.Size()));
        Check(s.Last() == 99.0, L"环形缓冲的最新值 = 最后一次写入的 99");
        Check(s.Min() == 36.0, L"环形缓冲的最小值 = 窗口内最旧的 36");
        Check(s.Max() == 99.0, L"环形缓冲的最大值 = 99");
        Check(s.Range() == 63.0, L"极差 = 99 - 36 = 63");

        // ③ 拷贝顺序：最旧 → 最新；容量不够时丢最旧、留最新。
        double buf[16] = { 0 };
        const size_t n = s.CopyOldestFirst(buf, 16);
        bool ordered = (n == 16);
        for (size_t i = 1; ordered && i < n; ++i) ordered = (buf[i] > buf[i - 1]);
        Check(ordered && buf[0] == 84.0 && buf[n - 1] == 99.0,
              L"拷贝出来是最旧的 84 → 最新的 99，且严格递增");
        Check(s.CopyOldestFirst(nullptr, 16) == 0, L"拷贝到空指针返回 0，不崩");
    }

    // ---------------- 进程 exe 图标 ----------------
    //
    // 【为什么测图标】
    // 图标提取失败时不会抛异常、不会崩，只会让图标位空着 ——
    // 这正是最容易漏掉的一类问题，所以要钉「取得到 / 取不到 / 缓存生效」三件事。
    // 不钉具体像素 —— 像素在不同 Windows 版本上不一样，钉了就是假失败。
    Line(L"");
    Line(L"[4d] 进程 exe 图标");
    {
        // 前面 DumpTop 用掉了本帧的路径解析预算，这里重置一次，
        // 模拟「新一帧」再问路径 —— 否则会因预算耗尽而误判成「路径解析不了」。
        AppNamesBeginFrame();

        Check(GetIconForPath(L"") == nullptr, L"空路径返回空图标句柄");

        // 用系统目录下的 cmd.exe —— 路径在任何 Win11 上都成立，且它一定有图标资源。
        wchar_t sysDir[MAX_PATH] = { 0 };
        const UINT n = GetSystemDirectoryW(sysDir, MAX_PATH);
        const std::wstring cmdPath =
            (n > 0 && n < MAX_PATH) ? std::wstring(sysDir) + L"\\cmd.exe" : std::wstring();
        const HICON h1 = GetIconForPath(cmdPath);
        Check(!cmdPath.empty() && h1 != nullptr, L"cmd.exe 能取到图标");
        Check(GetIconForPath(cmdPath) == h1, L"同一路径二次取图标命中缓存（同一句柄）");

        // 自身 exe：路径走 appnames 那份 pid 缓存解析
        const DWORD self = ::GetCurrentProcessId();
        const std::wstring selfPath = ProcessImagePath(self, 0);
        Check(!selfPath.empty(), L"能解析自身 exe 路径");
        Check(GetAppIcon(self, 0, L"self.exe") != nullptr, L"自身 exe 能取到图标");

        // 不存在的路径必须返回空，不能返回野句柄
        Check(GetIconForPath(L"Z:\\__lingwu_no_such_exe__.exe") == nullptr,
              L"不存在的路径返回空图标句柄");

        ReleaseAppIcons();
        Line(L"  （图标句柄已释放）");
    }

    // ---------------- 折线几何 ----------------
    //
    // 【为什么测几何】
    // 折线画错时不会崩、不会报错 —— 屏幕上只是「一条形状有点怪的线」。
    // 顺序反了（最新在左）、丢的是最新而不是最旧、
    // 纵轴把平坦的噪声放大成锯齿 —— 这三种错在界面上都只能靠肉眼发现，
    // 所以必须在这里逐条钉死。
    Line(L"");
    Line(L"[4e] 折线几何（磁贴 mini 折线）");
    {
        // ① 点数不足 2 个时画一条基线（规格：plan.md §3.7「失败与恢复语义」
        //    —— 「sparkline 点数不足 → 只画已有的点；点数 < 2 时画一条基线」）。
        //    【为什么要基线而不是空白】折线区是磁贴里一块固定高度的容器，
        //    什么都不画会让人以为功能坏了；而采样间隔是设置窗口里的合法档位
        //    （可以选 5 秒），攒够 2 个点要 10 秒。
        //    空序列 → 垂直中间的一条线（没有数据 ≠「值为 0」）；
        //    一个点 → 落在那个值的位置上（区间 0..100、值 5 → y=19）。
        const std::wstring emptyBase = BuildSparklineData({}, 100.0, 20.0,
                                                           SparklineKind::Line, 0.0, 100.0);
        Check(!emptyBase.empty() &&
              emptyBase.find(L"M 0.0,10.0") != std::wstring::npos &&
              emptyBase.find(L"L 100.0,10.0") != std::wstring::npos,
              L"空序列画一条垂直中间的水平基线");
        const std::wstring oneBase = BuildSparklineData({5.0}, 100.0, 20.0,
                                                        SparklineKind::Line, 0.0, 100.0);
        Check(oneBase.find(L"M 0.0,19.0") != std::wstring::npos &&
              oneBase.find(L"L 100.0,19.0") != std::wstring::npos,
              L"只有一个点时基线落在该值的位置上（值 5 / 区间 0..100 → y=19）");

        // ② 显式纵轴 0..100、控件 100×20：
        //    值 0 贴底（y=20）、值 100 贴顶（y=0）、最后一列 x 等于控件宽度。
        const std::vector<double> up{ 0.0, 50.0, 100.0 };
        const std::wstring line = BuildSparklineData(up, 100.0, 20.0,
                                                      SparklineKind::Line, 0.0, 100.0);
        Check(line.rfind(L"M 0.0,20.0", 0) == 0,
              L"折线起点在左下角（值 0 → 贴底）");
        Check(line.find(L"L 100.0,0.0") != std::wstring::npos,
              L"折线终点在右上角（值 100 → 贴顶），x 等于控件宽度");
        Check(line.find(L"Z") == std::wstring::npos,
              L"折线形态不闭合");

        // ③ 面积形态要闭合到基线
        const std::wstring area = BuildSparklineData(up, 100.0, 20.0,
                                                      SparklineKind::Area, 0.0, 100.0);
        Check(area.find(L"Z") != std::wstring::npos, L"面积形态闭合到基线");
        Check(area.size() > line.size(), L"面积形态比折线多出闭合段");

        // ④ 平坦序列不能被放大成锯齿：自动纵轴给它一个居中窗口。
        //    这是最容易被忽略的一条 —— 恒定值画成锯齿会让用户误以为在剧烈波动。
        const std::vector<double> flat{ 11.0, 11.0, 11.0, 11.0 };
        double lo = 0.0, hi = 0.0;
        ComputeSparkScale(flat, 10.0, &lo, &hi);
        Check(lo == 6.0 && hi == 16.0,
              W(L"平坦序列按 minSpan=10 给居中窗口（实测 %.1f..%.1f）", lo, hi));
        const std::wstring flatLine = BuildSparklineDataAuto(flat, 100.0, 20.0,
                                                              SparklineKind::Line, 10.0);
        Check(flatLine.find(L"M 0.0,10.0") != std::wstring::npos &&
              flatLine.find(L"L 100.0,10.0") != std::wstring::npos,
              L"平坦序列画成一条水平中线（y 恒为 10）");

        // ⑤ 极差够大时按真实 min/max 缩放
        const std::vector<double> spread{ 20.0, 40.0, 60.0 };
        ComputeSparkScale(spread, 10.0, &lo, &hi);
        Check(lo == 20.0 && hi == 60.0,
              W(L"极差大于 minSpan 时按真实 min/max（实测 %.1f..%.1f）", lo, hi));

        // ⑥ 只取最近的一段，且丢的是最旧而不是最新
        std::vector<double> many;
        for (int i = 0; i < 100; ++i) many.push_back(static_cast<double>(i));
        const std::wstring manyLine = BuildSparklineData(many, 100.0, 20.0,
                                                          SparklineKind::Line, 0.0, 100.0);
        // 最近 40 个点是 60..99；首点值 60 → y = 20 - 60/100*20 = 8.0
        Check(manyLine.rfind(L"M 0.0,8.0", 0) == 0,
              L"超过 40 点时只取最近的一段（丢最旧，不丢最新）");
        Check(manyLine.find(L"L 100.0,0.2") != std::wstring::npos,
              L"最新一点（值 99）仍在折线末端");
    }

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
