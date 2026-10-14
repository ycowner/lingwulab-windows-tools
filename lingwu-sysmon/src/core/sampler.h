// sampler.h —— 采样核心：单后台线程，产出共享快照
//
// 「采样完成 → 通知 UI」的通道是原子标志而非窗口消息，理由见下方 ConsumeDirty。
#pragma once

#include <windows.h>
#include <string>

#include "metrics.h"

namespace lingsysmon {
namespace core {

class Sampler {
public:
    Sampler();
    ~Sampler();

    bool Start(int intervalMs);
    void Stop();

    void SetInterval(int ms);
    void SetGroupMode(bool aggregate);
    void SetSortMode(int sortMode);

    // UI 线程读取快照：Lock → View → Unlock
    void Lock();
    void Unlock();
    const Snapshot& View() const { return snap_; }

    // 采样线程每发布一版新快照就置位；UI 取走时清零并返回 true。
    //
    // **不要改回窗口消息**：桌面 WinUI 应用虽然也走消息泵，但窗口过程由 WinUI 接管，
    // 自定义消息最终落到 DefWindowProc，我们自己的 WndProc 根本收不到
    // （实测：窗口能渲染，但没有任何自定义消息回调入口）。
    //
    // 「采样线程置位 + UI 侧 DispatcherTimer 短周期轮询」把跨线程通知放到一个
    // 与 UI 框架无关的位置，也不需要把 HWND 传进数据层。UI 侧用 ~100 ms 的
    // 定时器轮询即可：置位到被读到的延迟远小于一个采样周期，视觉上无差别，
    // 且没有数据竞争。
    bool ConsumeDirty();

private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    void Tick();
    void SampleProcesses();
    void SampleNetwork();
    void SwapToSnapshot();

    CRITICAL_SECTION cs_{};
    bool csInit_ = false;

    Snapshot snap_;   // 对外共享（读需持锁）
    Snapshot work_;   // 采样线程私有，稳态复用缓冲

    HANDLE thread_    = nullptr;
    HANDLE stopEvent_ = nullptr;

    // 原子成员一律用 LONG：Interlocked* 是 4 字节操作，
    // 直接作用在 bool 上会越界写坏后面的成员。
    volatile LONG intervalMs_ = 1000;
    volatile LONG aggregate_  = 1;
    volatile LONG sortMode_   = SORT_MEM_DESC;
    volatile LONG dirty_      = 0;   // 见 ConsumeDirty()

    ULONGLONG prevTickMs_ = 0;
    int coreCount_ = 1;
};

}  // namespace core
}  // namespace lingsysmon
