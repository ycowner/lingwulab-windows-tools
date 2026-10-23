// controller.h —— 服务操作的业务层：异步队列 + 等待态 + 结果码。
//
// 【为什么整个 core 层是异步的】
// 服务写操作全部会阻塞，而且阻塞时间不可预估：
//   · StartServiceW   要等 SCM 确认启动请求被接受（挂起的服务可能更久）
//   · ControlService(STOP) 要走服务主循环去处理 stop 事件
//   · DeleteService   对运行中的服务会等到进程真正结束才返回
// 任何一个放进按钮点击处理器里，窗口都会假死十几秒，用户会以为程序崩了。
// 所以：UI 线程只负责投递与呈现，全部 SCM 往返都在工作线程上。
//
// 【线程契约】
//   · poll 线程独占 ScmSession，周期枚举，产出快照
//   · op 线程独占操作队列，串行执行全部写操作与详情查询
//   · UI 线程只调 Enqueue / TakeSnapshot / TakeResult，永远不碰 SCM
// 串行的理由不只是简单：并发地对同一个服务发「停止」和「启动」，
// SCM 会照单全收，结果是一个用户没要求的状态。
#pragma once

#include "core/service/scm.h"
#include "core/service/service_model.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace lingwu {
namespace core {
namespace service {

// 操作结果。UI 只按这个枚举分流，不直接看 Win32 错误码。
enum class OpResult {
    Ok,
    NeedsAdmin,     // ERROR_ACCESS_DENIED → 弹提权提示
    Failed,         // 其它错误 → 弹错误提示，message 里带中文说明
    TimedOut,       // 等待目标状态超时 → 提示「仍在进行中」，不谎报失败
    NotFound,       // 服务已被删除
    Busy,           // 队列已满，上一个操作还在进行
    InvalidInput,   // 参数校验没过（只有创建走得到）
};

struct OpOutcome {
    OpResult result = OpResult::Ok;
    DWORD win32 = 0;
    std::wstring serviceName;
    std::wstring message;        // 中文，直接可以显示

    // 删除专用的两个事实：
    //   原本是否在运行 —— 决定提示是「已删除」还是「已标记删除」
    // wasRunning 为真时服务只是被标记删除，列表刷新后它可能**仍在**，
    // 这是 Windows 的语义不是缺陷，文案必须说清。
    bool wasRunning = false;
};

enum class OpKind {
    Start,
    Stop,
    Restart,
    SetStartType,
    Create,
    Delete,
    QueryDetails,
    ScanThirdParty,
};

struct Operation {
    OpKind kind = OpKind::Start;
    std::wstring name;
    StartTypeChoice choice = StartTypeChoice::Auto;
    CreateServiceRequest create{};
    // 删除时：true = 先停止再删；false = 服务在跑就直接拒绝
    bool stopFirst = false;
    // SetStartType 的目标：0=自动 1=延迟自动 2=手动 3=禁用
    int startTypeChoice = 0;
};

class Controller {
public:
    ~Controller() { Stop(); }

    // 起 poll 与 op 两条线程。返回 false 表示连服务控制管理器都打不开
    // （这种情况连列表都列不出来，直接失败比开一个空窗口好）。
    bool Start(DWORD& err);
    void Stop();
    bool running() const { return running_.load(); }

    // ---- 数据面 ----

    // 取最新一帧枚举结果。非阻塞；返回 false 表示这一轮还没产出新数据。
    bool TakeSnapshot(std::vector<ServiceInfo>& out);

    // 强制下一轮立刻枚举（写操作完成后调用，让界面尽快落到真实状态）。
    void RequestImmediateRefresh();

    // ---- 操作面 ----

    // 投递一个操作。队列已满返回 false（Busy）——静默丢弃会让用户以为点没生效。
    bool Enqueue(Operation op);

    // 取一个已完成的结果。非阻塞。
    bool TakeResult(OpOutcome& out);

    // ---- 详情面 ----

    // 详情缓存。切行时 RequestDetails，随后轮询 DetailsReady。
    // 走队列而不是同步查，是因为 QueryServiceConfigW 是一次 RPC，
    // 在网络路径的机器上单次可能到几十毫秒。
    void RequestDetails(std::wstring const& name);
    bool DetailsReady(std::wstring const& name, ServiceDetails& out);

    // ---- 第三方筛选面 ----

    // 触发一次全量路径扫描。切到「第三方」筛选时才做——
    // 每条服务一次 QueryServiceConfigW，为一个偶尔用的筛选器每 2 秒跑一遍
    // 是纯粹的浪费。
    void RequestThirdPartyScan();
    bool ThirdPartyReady() const;
    bool IsThirdParty(std::wstring const& name) const;

    // 丢弃启动类型缓存。提权状态变化后调用：之前因权限不足查不到的服务
    // 现在能查到了。
    void DropStartTypeCache();

    // ---- 权限 ----
    bool Elevated() const { return elevated_.load(); }
    void SetElevated(bool v) { elevated_.store(v); }

    // 等待某服务到达目标状态。**只允许在 op 线程上调用**
    // （它会阻塞最多 timeoutMs，正是「串行执行」这条约束的来源）。
    static bool WaitForState(std::wstring const& name, DWORD targetState, DWORD timeoutMs);

    // 把 Win32 错误码翻成 (结果, 中文文案)。
    static OpResult ClassifyWin32(DWORD err, std::wstring& message);

private:
    void PollLoop();
    void OpLoop();
    void Execute(Operation const& op);

    void RunStart(Operation const& op);
    void RunStop(Operation const& op);
    void RunRestart(Operation const& op);
    void RunSetStartType(Operation const& op);
    void RunCreate(Operation const& op);
    void RunDelete(Operation const& op);
    void RunQueryDetails(Operation const& op);
    void RunScanThirdParty();

    void PushResult(OpOutcome&& outcome);

    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<bool> elevated_{ false };

    std::thread pollThread_{};
    std::thread opThread_{};

    // poll 线程独占（会话自己管 SC_MANAGER 句柄）。
    //
    // 【为什么 op 线程也要一个而不是每次现开】
    // 详情查询、第三方扫描都是 N 次 QueryServiceConfigW，
    // 每次现开 OpenSCManager 等于每条服务多一次管理器连接建立。
    ScmSession pollSession_{};
    ScmSession opSession_{};

    // 快照：poll 线程写、UI 线程读。
    std::mutex snapMutex_{};
    std::vector<ServiceInfo> snapshot_{};
    bool snapshotFresh_ = false;
    std::condition_variable snapCv_{};
    bool refreshRequested_ = false;

    // 操作队列
    std::mutex opMutex_{};
    std::condition_variable opCv_{};
    std::deque<Operation> queue_{};

    // 结果队列
    std::mutex resMutex_{};
    std::deque<OpOutcome> results_{};

    // 详情缓存（op 线程写、UI 线程读）
    mutable std::mutex detailMutex_{};
    std::unordered_map<std::wstring, ServiceDetails> details_{};
    std::unordered_map<std::wstring, std::wstring> binaryPaths_{};

    // 第三方判定（atomic<bool>：UI 线程只读一个标志，
    // 具体查表在持 detailMutex_ 时做）
    std::atomic<bool> thirdPartyReady_{ false };
};

}  // namespace service
}  // namespace core
}  // namespace lingwu
