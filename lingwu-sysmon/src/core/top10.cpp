// top10.cpp —— 聚合与排序
#include "pch.h"

#include "top10.h"

#include <algorithm>
#include <vector>

namespace lingsysmon {
namespace core {
namespace {

bool NameLess(const std::wstring& a, const std::wstring& b) {
    const int c = _wcsicmp(a.c_str(), b.c_str());
    return c < 0;
}

bool AppLess(const AppInfo& a, const AppInfo& b, int sortMode) {
    switch (sortMode) {
        case SORT_MEM_ASC:
            if (AppMemKey(a) != AppMemKey(b)) return AppMemKey(a) < AppMemKey(b);
            break;
        case SORT_CPU_DESC: {
            // 量化后再比，理由见 metrics.h 里 AppCpuSortKey 的注释：
            // 裸 double 比较会让「≈0%」的大多数进程按浮点噪声全表重排。
            const double ka = AppCpuSortKey(a.cpuNorm);
            const double kb = AppCpuSortKey(b.cpuNorm);
            if (ka != kb) return ka > kb;
            break;
        }
        case SORT_CPU_ASC: {
            const double ka = AppCpuSortKey(a.cpuNorm);
            const double kb = AppCpuSortKey(b.cpuNorm);
            if (ka != kb) return ka < kb;
            break;
        }
        case SORT_NAME_ASC:
            if (!a.name.empty() && !b.name.empty()) {
                const int c = _wcsicmp(a.name.c_str(), b.name.c_str());
                if (c != 0) return c < 0;
            }
            break;
        case SORT_MEM_DESC:
        default:
            if (AppMemKey(a) != AppMemKey(b)) return AppMemKey(a) > AppMemKey(b);
            break;
    }
    // 稳定的次级键。
    //
    // **CPU 也必须走量化键**，不能用裸 cpuNorm：量化只作用于主键，
    // 若次级键仍比裸值，同一量化档内又会按浮点噪声排一遍 ——
    // 实测 replaced 仍是 153~168，量化等于白做（2026-10-14 踩过）。
    // 内存档位的次级键同理保持裸值：内存键是字节级整数，几乎不会相等，
    // 次级键很少被触发，且 MEM_DESC 实测 123.7 MB 已达标。
    if (sortMode == SORT_CPU_DESC || sortMode == SORT_CPU_ASC) {
        const double ka = AppCpuSortKey(a.cpuNorm);
        const double kb = AppCpuSortKey(b.cpuNorm);
        if (ka != kb) return ka > kb;
    } else if (a.cpuNorm != b.cpuNorm) {
        return a.cpuNorm > b.cpuNorm;
    }
    if (_wcsicmp(a.name.c_str(), b.name.c_str()) != 0) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    }
    // 终极 tiebreaker：组内首个 pid。
    //
    // 为什么必须有这一条：std::sort **不稳定**，比较器一旦返回 false 就
    // 意味着「两者无序」，具体谁前谁后由算法内部决定，帧与帧之间会变。
    // 逐进程口径（Aggregate=0）下有大量同名进程（多个 chrome.exe），
    // 名称这一级也分不出胜负，不补一个稳定键就又回到随机顺序 ——
    // 表现为「关掉聚合后列表照跳」。pidStart 在同一份快照内唯一，
    // 进程重启才会变，作为终极键足够稳。
    return a.pidStart < b.pidStart;
}

}  // namespace

void BuildViews(Snapshot& s, bool aggregate, int sortMode) {
    const size_t n = s.procs.size();
    s.apps.clear();
    s.pids.clear();
    if (n == 0) return;

    s.apps.reserve(n);
    s.pids.reserve(n);

    if (!aggregate) {
        // ---- 逐进程：一行一个进程 ----
        for (size_t i = 0; i < n; ++i) {
            const ProcInfo& p = s.procs[i];
            AppInfo a;
            a.name        = p.name;
            a.cpuSingle   = p.cpuSingle;
            a.cpuNorm     = p.cpuNorm;
            a.privWorkSet = p.privWorkSet;
            a.procCount   = 1;
            a.pidStart    = static_cast<uint32_t>(s.pids.size());
            a.pidCount    = 1;
            s.pids.push_back(p.pid);
            s.apps.push_back(a);
        }
    } else {
        // ---- 按应用聚合：先按名排序，再扫描同名连续段 ----
        std::vector<uint32_t> idx;
        idx.resize(n);
        for (size_t i = 0; i < n; ++i) idx[i] = static_cast<uint32_t>(i);
        std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
            const std::wstring& na = s.procs[a].name;
            const std::wstring& nb = s.procs[b].name;
            if (NameLess(na, nb)) return true;
            if (NameLess(nb, na)) return false;
            return s.procs[a].pid < s.procs[b].pid;
        });

        size_t i = 0;
        while (i < n) {
            const std::wstring& key = s.procs[idx[i]].name;
            AppInfo a;
            a.name        = key;
            a.pidStart    = static_cast<uint32_t>(s.pids.size());
            size_t j = i;
            while (j < n) {
                const ProcInfo& p = s.procs[idx[j]];
                if (!NameLess(key, p.name) && !NameLess(p.name, key)) {
                    a.cpuSingle   += p.cpuSingle;
                    a.cpuNorm     += p.cpuNorm;
                    a.privWorkSet += p.privWorkSet;
                    a.procCount++;
                    s.pids.push_back(p.pid);
                    ++j;
                } else {
                    break;
                }
            }
            a.pidCount = a.procCount;
            s.apps.push_back(a);
            i = j;
        }
    }

    std::sort(s.apps.begin(), s.apps.end(),
              [sortMode](const AppInfo& a, const AppInfo& b) {
                  return AppLess(a, b, sortMode);
              });
}

}  // namespace core
}  // namespace lingsysmon
