// sampler.cpp —— 进程 / 网络采样实现
//
// 采样完成不 PostMessage，只在 SwapToSnapshot() 里 InterlockedExchange(&dirty_, 1)；
// 理由见 sampler.h 的 ConsumeDirty 注释。
//
// 设计要点（均为实测结论，勿轻易改回）：
//  1. CPU/内存走 NtQuerySystemInformation(SystemProcessInformation) 一次系统调用拿全量，
//     不做任何逐进程 OpenProcess。
//  2. 内存口径用 PrivatePageCount（私有提交字节）。
//     曾改用 PDH \Process V2(*)\Working Set - Private 以对齐任务管理器「进程」页的私有工作集，
//     实测该通配查询在本机额外占用约 5.2 MB 专用工作集（占 5 MB 预算的 100%），
//     故放弃。代价：与任务管理器「进程」页有差异，等价于「详细信息」页的「内存(专用字节)」。
//  3. 网速走 GetIfTable2 / GetIfEntry2（MIB_IF_ROW2）。
//     旧版 GetIfTable(MIB_IFROW) 在本机实测 dwInOctets / dwOutOctets 全为 0
//     （调用成功但计数器失效），且 wszName 是 \DEVICE\TCPIP_{GUID} 这种不可读形式，
//     完全无法使用，因此必须换 2 代 API。包含方式见 core/iphlpapi2.h。
//  4. 网卡筛选必须用 InterfaceAndOperStatusFlags.HardwareInterface。
//     实测本机 Wi-Fi 会额外枚举出 4 行过滤层（WLAN-WFP ...-0000 / Native WiFi Filter /
//     QoS Packet Scheduler / WFP 802.3 MAC ...-0000），它们的 InOctets / OutOctets
//     与真实网卡「WLAN」完全相同。仅按 type + operStatus 过滤会把同一份流量累加 4 次。
#include "pch.h"

#include "iphlpapi2.h"   // 见文件头：本工程的 PCH 已先引入 windows.h，靠 LEAN_AND_MEAN 兜底
#include "sampler.h"
#include "top10.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")

namespace lingsysmon {
namespace core {

// ---------------------------------------------------------------- NT 内部结构

namespace {

typedef LONG (NTAPI *PFN_NQSISys)(ULONG, PVOID, ULONG, PULONG);

const ULONG kSystemProcessInformation = 5;
const ULONG kStatusInfoLengthMismatch = 0xC0000004UL;

struct MY_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

// SYSTEM_PROCESS_INFORMATION 的 x64 布局（偏移经核对：ImageName.Buffer@64，PrivatePageCount@200）
struct SYSTEM_PROCESS_INFORMATION {
    ULONG                NextEntryOffset;
    ULONG                NumberOfThreads;
    LONGLONG             WorkingSetPrivateSize;
    ULONG                HardFaultCount;
    ULONG                NumberOfThreadsHighWatermark;
    ULONGLONG            CycleTime;
    LONGLONG             CreateTime;
    LONGLONG             UserTime;
    LONGLONG             KernelTime;
    MY_UNICODE_STRING    ImageName;
    LONG                 BasePriority;
    HANDLE               UniqueProcessId;
    HANDLE               InheritedFromUniqueProcessId;
    ULONG                HandleCount;
    ULONG                SessionId;
    ULONG_PTR            UniqueProcessKey;
    SIZE_T               PeakVirtualSize;
    SIZE_T               VirtualSize;
    ULONG                PageFaultCount;
    SIZE_T               PeakWorkingSetSize;
    SIZE_T               WorkingSetSize;
    SIZE_T               QuotaPeakPagedPoolUsage;
    SIZE_T               QuotaPagedPoolUsage;
    SIZE_T               QuotaPeakNonPagedPoolUsage;
    SIZE_T               QuotaNonPagedPoolUsage;
    SIZE_T               PagefileUsage;
    SIZE_T               PeakPagefileUsage;
    SIZE_T               PrivatePageCount;
};

PFN_NQSISys ResolveNqsi() {
    static PFN_NQSISys fn = nullptr;
    if (!fn) {
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
            fn = (PFN_NQSISys)GetProcAddress(ntdll, "NtQuerySystemInformation");
    }
    return fn;
}

// ---------------------------------------------------------------- CPU 基线

struct CpuEntry {
    ULONGLONG totalTime  = 0;   // (user+kernel)，单位 100ns
    ULONGLONG createTime = 0;   // 用于识别 PID 复用
    ULONGLONG tick       = 0;   // 本 tick 序号
};

struct NicEntry {
    NET_LUID     luid{};
    std::wstring name;        // Alias（友好名），为空时退回 Description
    uint64_t     inBytes  = 0;
    uint64_t     outBytes = 0;
};

// ---------------------------------------------------------------- 网卡筛选
// 只统计已连接的物理 / 无线网卡，排除回环、隧道、VPN、虚拟网卡、蓝牙 PAN。

const ULONG kIfOperStatusUp = 1;

#ifndef IF_TYPE_ETHERNET_CSMACD
#define IF_TYPE_ETHERNET_CSMACD 6
#endif
#ifndef IF_TYPE_IEEE80211
#define IF_TYPE_IEEE80211 71
#endif

// 大小写无关的子串匹配（宽串；别名与描述都是宽串）
bool ContainsCI(const std::wstring& h, const wchar_t* needle) {
    if (!needle || !*needle) return false;
    const size_t m = wcslen(needle);
    if (h.size() < m) return false;
    for (size_t i = 0; i + m <= h.size(); ++i) {
        if (_wcsnicmp(h.c_str() + i, needle, m) == 0) return true;
    }
    return false;
}

bool IsCountedNic(const MIB_IF_ROW2& r) {
    if (r.OperStatus != kIfOperStatusUp) return false;

    // 关键判据：只有 HardwareInterface=1 的才是真实适配器本体。
    // Windows 会为每块物理网卡额外枚举若干「过滤层」行（轻量 MAC 过滤器、QoS 调度器…），
    // 它们上报的字节数与网卡本体完全相同，不过滤就会把同一份流量重复累加。
    if (!r.InterfaceAndOperStatusFlags.HardwareInterface) return false;

    if (r.Type != IF_TYPE_IEEE80211 && r.Type != IF_TYPE_ETHERNET_CSMACD) return false;

    // 虚拟 / 隧道 / VPN / 蓝牙 PAN 一律排除
    static const wchar_t* kBad[] = {
        L"Virtual", L"VMware", L"Hyper-V", L"VirtualBox", L"TAP-",
        L"Tailscale", L"Loopback", L"Bluetooth", L"VPN", L"WSL",
        L"Npcap", L"vEthernet", L"Generic", L"Tunnel", L"Miniport"
    };
    std::wstring desc(r.Description);
    for (const wchar_t* b : kBad) {
        if (ContainsCI(desc, b) || ContainsCI(std::wstring(r.Alias), b)) return false;
    }
    return true;
}

std::wstring Basename(std::wstring path) {
    const size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

}  // namespace

// ---------------------------------------------------------------- Sampler

Sampler::Sampler() {
    InitializeCriticalSection(&cs_);
    csInit_ = true;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    coreCount_ = si.dwNumberOfProcessors;
    if (coreCount_ == 0) coreCount_ = 1;
    prevTickMs_ = GetTickCount64();
}

Sampler::~Sampler() {
    Stop();
    if (csInit_) { DeleteCriticalSection(&cs_); csInit_ = false; }
}

bool Sampler::Start(int intervalMs) {
    if (thread_) return true;
    if (intervalMs < 250) intervalMs = 250;
    if (intervalMs > 10000) intervalMs = 10000;
    intervalMs_ = intervalMs;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return false;
    prevTickMs_ = GetTickCount64();
    thread_ = CreateThread(nullptr, 0, &Sampler::ThreadProc, this, 0, nullptr);
    return thread_ != nullptr;
}

void Sampler::Stop() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void Sampler::SetInterval(int ms) {
    if (ms < 250) ms = 250;
    if (ms > 10000) ms = 10000;
    InterlockedExchange(&intervalMs_, ms);
}

void Sampler::SetGroupMode(bool aggregate) {
    InterlockedExchange(&aggregate_, aggregate ? 1 : 0);
}

void Sampler::SetSortMode(int sortMode) {
    if (sortMode < 0 || sortMode > 4) sortMode = 0;
    InterlockedExchange(&sortMode_, sortMode);
}

void Sampler::Lock()   { if (csInit_) EnterCriticalSection(&cs_); }
void Sampler::Unlock() { if (csInit_) LeaveCriticalSection(&cs_); }

bool Sampler::ConsumeDirty() {
    return InterlockedExchange(&dirty_, 0) != 0;
}

DWORD WINAPI Sampler::ThreadProc(LPVOID self) {
    Sampler* s = static_cast<Sampler*>(self);
    for (;;) {
        const int iv = s->intervalMs_;
        if (WaitForSingleObject(s->stopEvent_, iv) != WAIT_TIMEOUT) break;
        s->Tick();
    }
    return 0;
}

void Sampler::Tick() {
    // 三个稳态缓冲做成函数内 static：跨 tick 复用容量，稳态下不产生堆抖动。
    // 全进程只有一个 Sampler，因此不存在跨实例共享状态的问题。
    static std::vector<BYTE> nqsiBuf;
    static std::vector<NicEntry> nicList;
    static std::unordered_map<DWORD, CpuEntry> cpuBase;
    static ULONGLONG tickSeq = 0;

    if (nqsiBuf.empty()) nqsiBuf.resize(256 * 1024);   // 约可容纳 1000 个进程

    const ULONGLONG now = GetTickCount64();
    double elapsedSec = static_cast<double>(now - prevTickMs_) / 1000.0;
    if (elapsedSec < 0.001) elapsedSec = 0.001;
    const double elapsedMs = elapsedSec * 1000.0;
    // 整数除数必须**单独**钳位，不能复用上面那个 0.001。
    // 下面网速两行要的是整数分母 (uint64_t)elapsedSec：它在 elapsedSec < 1 时
    // 截断成 0，而 1 秒定时器只要早触发十几毫秒（Windows 定时器本来就会早）
    // 就是 0.98x —— 实测必崩 STATUS_INTEGER_DIVIDE_BY_ZERO（0xC0000094），
    // 空载启动 6 秒内必现、6/6 复现。0.001 只保护了 CPU 那行的浮点除法。
    const uint64_t elapsedSecInt =
        (elapsedSec < 1.0) ? 1u : static_cast<uint64_t>(elapsedSec);
    prevTickMs_ = now;
    ++tickSeq;

    const bool aggregate = (InterlockedCompareExchange(&aggregate_, 0, 0) != 0);
    const int sortMode = static_cast<int>(InterlockedCompareExchange(&sortMode_, 0, 0));

    work_.coreCount = coreCount_;
    work_.tickMs    = now;
    work_.memIsPrivateWorkingSet = false;   // 当前口径 = 私有提交字节

    // ---------- 进程 ----------
    PFN_NQSISys nqsi = ResolveNqsi();
    work_.procs.clear();

    if (nqsi) {
        LONG st = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            ULONG need = 0;
            st = nqsi(kSystemProcessInformation, nqsiBuf.data(),
                      static_cast<ULONG>(nqsiBuf.size()), &need);
            if (st == 0) break;
            if (static_cast<ULONG>(st) == kStatusInfoLengthMismatch && need > nqsiBuf.size()) {
                nqsiBuf.resize(need + (need >> 2) + 4096);
                continue;
            }
            break;
        }

        if (st == 0) {
            BYTE* base = nqsiBuf.data();
            size_t offset = 0;
            const BYTE* end = base + nqsiBuf.size();
            while (offset < nqsiBuf.size()) {
                const auto* spi = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(base + offset);
                if (reinterpret_cast<const BYTE*>(spi + 1) > end) break;

                const DWORD pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(spi->UniqueProcessId));
                const LONGLONG createTime = spi->CreateTime;
                const ULONGLONG total = static_cast<ULONGLONG>(
                    spi->UserTime + spi->KernelTime);

                // Idle(pid 0) 不展示：它恒占一核，会污染排名
                if (pid != 0) {
                    ProcInfo p;
                    p.pid        = pid;
                    p.ppid       = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(spi->InheritedFromUniqueProcessId));
                    p.createTime = static_cast<ULONGLONG>(createTime);
                    p.privBytes  = static_cast<uint64_t>(spi->PrivatePageCount);

                    if (spi->ImageName.Length > 0 && spi->ImageName.Buffer) {
                        p.name.assign(spi->ImageName.Buffer,
                                      spi->ImageName.Length / sizeof(WCHAR));
                    } else {
                        // 受保护进程拿不到 ImageName，回退查可执行文件路径
                        std::wstring full;
                        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                                   FALSE, pid)) {
                            wchar_t buf[MAX_PATH] = { 0 };
                            DWORD cch = MAX_PATH;
                            if (QueryFullProcessImageNameW(h, 0, buf, &cch))
                                full.assign(buf, cch);
                            CloseHandle(h);
                        }
                        p.name = full.empty()
                               ? (L"PID-" + std::to_wstring(pid))
                               : Basename(std::move(full));
                    }

                    // ---- CPU delta ----
                    auto it = cpuBase.find(pid);
                    double cpu = 0.0;
                    if (it != cpuBase.end() &&
                        it->second.createTime == p.createTime &&
                        total >= it->second.totalTime) {
                        const double d = static_cast<double>(total - it->second.totalTime);
                        // 分母 = 单核时间 → 单核口径，合计可 >100
                        cpu = d / (elapsedMs * 100.0);
                    }
                    if (cpu < 0.0) cpu = 0.0;
                    p.cpuSingle = cpu;
                    p.cpuNorm   = cpu / static_cast<double>(coreCount_);
                    p.privWorkSet = p.privBytes;   // 当前口径：两者相同
                    work_.procs.push_back(std::move(p));

                    CpuEntry e;
                    e.totalTime  = total;
                    e.createTime = p.createTime;
                    e.tick       = tickSeq;
                    cpuBase[pid] = e;
                }

                if (spi->NextEntryOffset == 0) break;
                offset += spi->NextEntryOffset;
            }

            // 清理本 tick 未出现的 PID，避免 map 无限增长
            for (auto it = cpuBase.begin(); it != cpuBase.end(); ) {
                if (it->second.tick != tickSeq) it = cpuBase.erase(it);
                else ++it;
            }
        }
    }

    // ---------- 网络 ----------
    // InOctets / OutOctets 是 64 位计数器，做差即本周期增量。
    // LUID 列表只在首次 / 每 30 个周期重新枚举，之后每周期只用 GetIfEntry2
    // 读已缓存的网卡，避免每周期都分配约 50 KB 的 MIB_IF_TABLE2。
    work_.net = NetInfo{};
    {
        if (nicList.empty() || (tickSeq % 30) == 0) {
            nicList.clear();
            MIB_IF_TABLE2* tbl = nullptr;
            if (GetIfTable2(&tbl) == NO_ERROR && tbl) {
                for (ULONG i = 0; i < tbl->NumEntries; ++i) {
                    const MIB_IF_ROW2& r = tbl->Table[i];
                    if (!IsCountedNic(r)) continue;
                    NicEntry e;
                    e.luid = r.InterfaceLuid;
                    e.name  = r.Alias[0] ? std::wstring(r.Alias) : std::wstring(r.Description);
                    e.inBytes  = r.InOctets;
                    e.outBytes = r.OutOctets;
                    nicList.push_back(e);
                }
                FreeMibTable(tbl);
            }
        }

        for (NicEntry& e : nicList) {
            MIB_IF_ROW2 row{};
            row.InterfaceLuid = e.luid;
            if (GetIfEntry2(&row) != NO_ERROR) continue;

            if (row.OperStatus == kIfOperStatusUp &&
                row.InOctets >= e.inBytes && row.OutOctets >= e.outBytes) {
                work_.net.downBps += (row.InOctets  - e.inBytes) / elapsedSecInt;
                work_.net.upBps   += (row.OutOctets - e.outBytes) / elapsedSecInt;
                if (!e.name.empty()) {
                    if (!work_.net.nic.empty()) work_.net.nic += L", ";
                    work_.net.nic += e.name;
                }
            }
            // 无论 up 与否都刷新基线：断线期间不累计，
            // 重新连上时也不会把断线期间的全量计数当成瞬时速率。
            e.inBytes  = row.InOctets;
            e.outBytes = row.OutOctets;
        }
    }

    // ---------- 聚合 + 排序 ----------
    BuildViews(work_, aggregate, sortMode);

    // ---------- 发布 ----------
    SwapToSnapshot();
}

void Sampler::SwapToSnapshot() {
    Lock();
    snap_.procs.swap(work_.procs);
    snap_.apps.swap(work_.apps);
    snap_.pids.swap(work_.pids);
    snap_.net                    = work_.net;
    snap_.tickMs                 = work_.tickMs;
    snap_.coreCount              = work_.coreCount;
    snap_.memIsPrivateWorkingSet = work_.memIsPrivateWorkingSet;
    Unlock();
    InterlockedExchange(&dirty_, 1);
}

}  // namespace core
}  // namespace lingsysmon
