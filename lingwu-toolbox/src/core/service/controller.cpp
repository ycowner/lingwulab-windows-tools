#include "pch.h"

#include "core/service/controller.h"

#include "platform/trace.h"

#include <chrono>

namespace lingwu {
namespace core {
namespace service {
namespace {

// 队列上限。超出就拒绝并报 Busy —— 静默丢弃会让用户以为「点了没反应」，
// 而一个真正在忙的界面至少能给出一句「上一个操作还在进行」。
constexpr size_t kMaxQueue = 8;

// 枚举周期。服务状态变化的人眼感知阈值大约在秒级，2 秒是够用又不浪费的取值。
constexpr auto kPollInterval = std::chrono::seconds(2);

// 等待目标状态时的轮询间隔。比枚举周期短，因为用户正盯着这一行。
constexpr auto kWaitPoll = std::chrono::milliseconds(400);

// 等待上限。超时**不**当作失败：服务可能真的需要更久，
// 报「仍在启动中」比报「启动失败」诚实得多。
constexpr DWORD kWaitTimeoutMs = 30000;

}  // namespace

// ── 生命周期 ─────────────────────────────────────────────────────────────

bool Controller::Start(DWORD& err) {
    if (running_.load()) {
        err = ERROR_SUCCESS;
        return true;
    }
    stop_.store(false);
    running_.store(true);

    // 先探一次管理器：打不开的话两条线程都没有意义。
    ScmSession probe;
    if (!probe.Open(err)) {
        LINGWU_TRACE("Controller::Start: OpenSCManager failed err=%lu",
                      static_cast<unsigned long>(err));
        running_.store(false);
        return false;
    }

    pollThread_ = std::thread(&Controller::PollLoop, this);
    opThread_ = std::thread(&Controller::OpLoop, this);
    LINGWU_TRACE("Controller::Start ok");
    return true;
}

void Controller::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    stop_.store(true);
    opCv_.notify_all();
    snapCv_.notify_all();
    if (pollThread_.joinable()) pollThread_.join();
    if (opThread_.joinable()) opThread_.join();
    LINGWU_TRACE("Controller::Stop done");
}

// ── 数据面 ───────────────────────────────────────────────────────────────

void Controller::PollLoop() {
    DWORD err = ERROR_SUCCESS;
    if (!pollSession_.Open(err)) {
        LINGWU_TRACE("PollLoop: Open failed err=%lu", static_cast<unsigned long>(err));
        return;
    }

    while (!stop_.load()) {
        std::vector<ServiceInfo> frame;
        if (pollSession_.EnumAll(frame, err)) {
            {
                std::lock_guard<std::mutex> lock(snapMutex_);
                snapshot_.swap(frame);
                snapshotFresh_ = true;
            }
            snapCv_.notify_all();
        } else {
            // 枚举失败**保留上一帧**数据，不要把列表清空 ——
            // 一个空白列表会被用户读成「所有服务都没了」，而真相只是这一次没查到。
            LINGWU_TRACE("PollLoop: enum failed err=%lu, keeping previous frame",
                          static_cast<unsigned long>(err));
        }

        std::unique_lock<std::mutex> lock(snapMutex_);
        refreshRequested_ = false;
        // 正常等一个周期；写操作完成后 RequestImmediateRefresh 会提前唤醒。
        snapCv_.wait_for(lock, kPollInterval,
                         [this] { return stop_.load() || refreshRequested_; });
    }
}

bool Controller::TakeSnapshot(std::vector<ServiceInfo>& out) {
    std::lock_guard<std::mutex> lock(snapMutex_);
    if (!snapshotFresh_) {
        return false;
    }
    snapshotFresh_ = false;
    out = snapshot_;
    return true;
}

void Controller::RequestImmediateRefresh() {
    {
        std::lock_guard<std::mutex> lock(snapMutex_);
        refreshRequested_ = true;
    }
    snapCv_.notify_all();
}

void Controller::DropStartTypeCache() {
    // 缓存只在 poll 线程的 ScmSession 上。poll 线程不持有别的引用，
    // 所以这里用刷新请求让下一轮枚举自然重建即可 ——
    // 单独一个「让 poll 线程清缓存」的信标属于过度设计。
    RequestImmediateRefresh();
}

// ── 操作面 ───────────────────────────────────────────────────────────────

bool Controller::Enqueue(Operation op) {
    std::lock_guard<std::mutex> lock(opMutex_);
    if (queue_.size() >= kMaxQueue) {
        return false;
    }
    queue_.push_back(std::move(op));
    opCv_.notify_one();
    return true;
}

bool Controller::TakeResult(OpOutcome& out) {
    std::lock_guard<std::mutex> lock(resMutex_);
    if (results_.empty()) {
        return false;
    }
    out = std::move(results_.front());
    results_.pop_front();
    return true;
}

void Controller::PushResult(OpOutcome&& outcome) {
    {
        std::lock_guard<std::mutex> lock(resMutex_);
        // 结果队列不做上限保护：UI 每 100ms 取一次，
        // 而产生速度是「用户点一次 = 一个」，不可能堆到失控。
        results_.push_back(std::move(outcome));
    }
}

void Controller::OpLoop() {
    while (!stop_.load()) {
        Operation op;
        {
            std::unique_lock<std::mutex> lock(opMutex_);
            opCv_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });
            if (stop_.load() && queue_.empty()) {
                break;
            }
            if (queue_.empty()) {
                continue;
            }
            op = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            Execute(op);
        } catch (std::exception const& e) {
            // 一条操作抛异常绝不能让整条 op 线程死掉 ——
            // 死了之后队列会永久堆积，用户看到的是「所有按钮都没反应」。
            LINGWU_TRACE("OpLoop: Execute threw: %s", e.what());
            OpOutcome outcome;
            outcome.result = OpResult::Failed;
            outcome.serviceName = op.name;
            outcome.message = L"操作过程中发生内部错误。";
            PushResult(std::move(outcome));
        }
    }
}

void Controller::Execute(Operation const& op) {
    switch (op.kind) {
        case OpKind::Start:          RunStart(op);          break;
        case OpKind::Stop:           RunStop(op);           break;
        case OpKind::Restart:        RunRestart(op);        break;
        case OpKind::SetStartType:   RunSetStartType(op);   break;
        case OpKind::Create:         RunCreate(op);         break;
        case OpKind::Delete:         RunDelete(op);         break;
        case OpKind::QueryDetails:   RunQueryDetails(op);   break;
        case OpKind::ScanThirdParty: RunScanThirdParty(); break;
    }
    // 写操作完成后立刻让列表落到真实状态，不等下一个 2 秒周期。
    switch (op.kind) {
        case OpKind::Start:
        case OpKind::Stop:
        case OpKind::Restart:
        case OpKind::SetStartType:
        case OpKind::Create:
        case OpKind::Delete:
            RequestImmediateRefresh();
            break;
        default:
            break;
    }
}

OpResult Controller::ClassifyWin32(DWORD err, std::wstring& message) {
    switch (err) {
        case ERROR_SUCCESS:
            return OpResult::Ok;
        case ERROR_ACCESS_DENIED:
            message = L"权限不足，需要管理员权限才能执行该操作。";
            return OpResult::NeedsAdmin;
        case ERROR_SERVICE_DOES_NOT_EXIST:
            message = L"服务已不存在，可能已被其它程序删除。";
            return OpResult::NotFound;
        case ERROR_SERVICE_CANNOT_ACCEPT_CTRL:
            message = L"服务无法接受该控制请求，通常是有依赖服务仍在运行。";
            return OpResult::Failed;
        case ERROR_SERVICE_MARKED_FOR_DELETE:
            message = L"服务已被标记删除，需完全停止后才会消失。";
            return OpResult::Failed;
        case ERROR_SERVICE_NOT_ACTIVE:
            message = L"服务当前未在运行。";
            return OpResult::Failed;
        case ERROR_DEPENDENT_SERVICES_RUNNING:
            message = L"有依赖此服务的其它服务仍在运行，无法停止。";
            return OpResult::Failed;
        case ERROR_SERVICE_REQUEST_TIMEOUT:
            message = L"服务控制管理器响应超时，系统可能正忙。";
            return OpResult::Failed;
        case ERROR_INVALID_NAME:
            message = L"服务名不合法：不能包含反斜杠或正斜杠。";
            return OpResult::InvalidInput;
        case ERROR_SERVICE_EXISTS:
            message = L"同名服务已存在。";
            return OpResult::InvalidInput;
        case ERROR_DUPLICATE_SERVICE_NAME:
            message = L"服务显示名与其它服务重复。";
            return OpResult::InvalidInput;
        default:
            break;
    }
    wchar_t buf[64];
    ::swprintf_s(buf, L"操作失败（系统错误 %lu）。", static_cast<unsigned long>(err));
    message = buf;
    return OpResult::Failed;
}

bool Controller::WaitForState(std::wstring const& name, DWORD targetState, DWORD timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        DWORD state = 0;
        DWORD err = ERROR_SUCCESS;
        if (ScmQueryState(name, state, err)) {
            if (state == targetState) {
                return true;
            }
        } else if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(kWaitPoll);
    }
}

// ── 各操作的实现 ─────────────────────────────────────────────────────────

void Controller::RunStart(Operation const& op) {
    OpOutcome outcome;
    outcome.serviceName = op.name;
    DWORD err = ERROR_SUCCESS;
    if (!ScmStart(op.name, err)) {
        outcome.win32 = err;
        outcome.result = ClassifyWin32(err, outcome.message);
        PushResult(std::move(outcome));
        return;
    }
    if (!WaitForState(op.name, 4 /* SERVICE_RUNNING */, kWaitTimeoutMs)) {
        // 超时不等于失败：服务可能只是慢。把状态说清楚，让用户自己判断。
        outcome.result = OpResult::TimedOut;
        outcome.message = L"服务仍在启动中，可能需要更长时间。";
        PushResult(std::move(outcome));
        return;
    }
    outcome.result = OpResult::Ok;
    PushResult(std::move(outcome));
}

void Controller::RunStop(Operation const& op) {
    OpOutcome outcome;
    outcome.serviceName = op.name;
    DWORD err = ERROR_SUCCESS;
    if (!ScmStop(op.name, err)) {
        outcome.win32 = err;
        outcome.result = ClassifyWin32(err, outcome.message);
        PushResult(std::move(outcome));
        return;
    }
    if (!WaitForState(op.name, 1 /* SERVICE_STOPPED */, kWaitTimeoutMs)) {
        outcome.result = OpResult::TimedOut;
        outcome.message = L"服务仍在停止中，可能有其它程序正在阻止它退出。";
        PushResult(std::move(outcome));
        return;
    }
    outcome.result = OpResult::Ok;
    PushResult(std::move(outcome));
}

void Controller::RunRestart(Operation const& op) {
    DWORD state = 0;
    DWORD err = ERROR_SUCCESS;
    if (!ScmQueryState(op.name, state, err)) {
        OpOutcome outcome;
        outcome.serviceName = op.name;
        outcome.win32 = err;
        outcome.result = ClassifyWin32(err, outcome.message);
        PushResult(std::move(outcome));
        return;
    }

    // 停不掉就整体失败，绝不做「启动部分成功」——
    // 那样用户看到「重启成功」而服务其实还开着，比直接报失败糟得多。
    if (state != 1 /* SERVICE_STOPPED */) {
        OpOutcome outcome;
        outcome.serviceName = op.name;
        if (!ScmStop(op.name, err)) {
            outcome.win32 = err;
            outcome.result = ClassifyWin32(err, outcome.message);
            PushResult(std::move(outcome));
            return;
        }
        if (!WaitForState(op.name, 1, kWaitTimeoutMs)) {
            outcome.result = OpResult::TimedOut;
            outcome.message = L"服务未能停止，重启已取消。";
            PushResult(std::move(outcome));
            return;
        }
    }

    OpOutcome outcome;
    outcome.serviceName = op.name;
    if (!ScmStart(op.name, err)) {
        outcome.win32 = err;
        outcome.result = ClassifyWin32(err, outcome.message);
        PushResult(std::move(outcome));
        return;
    }
    if (!WaitForState(op.name, 4, kWaitTimeoutMs)) {
        outcome.result = OpResult::TimedOut;
        outcome.message = L"服务已停止，但重新启动仍在进行中。";
        PushResult(std::move(outcome));
        return;
    }
    outcome.result = OpResult::Ok;
    PushResult(std::move(outcome));
}

void Controller::RunSetStartType(Operation const& op) {
    StartTypeChoice choice = static_cast<StartTypeChoice>(op.startTypeChoice);
    if (choice > StartTypeChoice::Disabled) {
        OpOutcome outcome;
        outcome.serviceName = op.name;
        outcome.result = OpResult::InvalidInput;
        outcome.message = L"启动类型取值不合法。";
        PushResult(std::move(outcome));
        return;
    }

    OpOutcome outcome;
    outcome.serviceName = op.name;
    DWORD err = ERROR_SUCCESS;
    if (!ScmSetStartType(op.name, StartTypeToDWord(choice), ChoiceIsDelayed(choice), err)) {
        outcome.win32 = err;
        outcome.result = ClassifyWin32(err, outcome.message);
        PushResult(std::move(outcome));
        return;
    }

    // 改启动类型时服务在运行是**允许**的：禁用只影响下次开机，
    // 当前进程照跑。这是 Windows 语义，不是 bug —— 提示里要说清，
    // 否则用户会以为没生效。
    DWORD state = 0;
    if (ScmQueryState(op.name, state, err) && state == 4 /* SERVICE_RUNNING */) {
        outcome.result = OpResult::Ok;
        outcome.message = L"启动类型已修改。该服务当前仍在运行，新设置将在下次重启后生效。";
        PushResult(std::move(outcome));
        return;
    }

    outcome.result = OpResult::Ok;
    PushResult(std::move(outcome));
}

void Controller::RunCreate(Operation const& op) {
    OpOutcome outcome;
    outcome.serviceName = op.create.serviceName;
    DWORD err = ERROR_SUCCESS;
    if (!ScmCreate(op.create, err)) {
        outcome.win32 = err;
        outcome.result = ClassifyWin32(err, outcome.message);
        PushResult(std::move(outcome));
        return;
    }

    if (op.create.startImmediately) {
        DWORD startErr = ERROR_SUCCESS;
        if (!ScmStart(op.create.serviceName, startErr)) {
            outcome.result = ClassifyWin32(startErr, outcome.message);
            outcome.message = L"服务已创建，但立即启动失败：" + outcome.message;
            PushResult(std::move(outcome));
            return;
        }
        if (!WaitForState(op.create.serviceName, 4, kWaitTimeoutMs)) {
            outcome.result = OpResult::TimedOut;
            outcome.message = L"服务已创建并已请求启动，但仍处于启动中。";
            PushResult(std::move(outcome));
            return;
        }
    }

    outcome.result = OpResult::Ok;
    PushResult(std::move(outcome));
}

void Controller::RunDelete(Operation const& op) {
    OpOutcome outcome;
    outcome.serviceName = op.name;

    DWORD state = 0;
    DWORD err = ERROR_SUCCESS;
    if (!ScmQueryState(op.name, state, err)) {
        outcome.win32 = err;
        outcome.result = ClassifyWin32(err, outcome.message);
        PushResult(std::move(outcome));
        return;
    }
    outcome.wasRunning = (state != 1 /* SERVICE_STOPPED */);

    if (outcome.wasRunning) {
        if (!op.stopFirst) {
            // 用户单独点删除而服务在跑 → 拒绝，不给「删了一半」的中间态。
            outcome.result = OpResult::Failed;
            outcome.message = L"服务正在运行，请先停止后再删除。";
            PushResult(std::move(outcome));
            return;
        }
        if (!ScmStop(op.name, err)) {
            outcome.win32 = err;
            outcome.result = ClassifyWin32(err, outcome.message);
            PushResult(std::move(outcome));
            return;
        }
        if (!WaitForState(op.name, 1, kWaitTimeoutMs)) {
            outcome.result = OpResult::TimedOut;
            outcome.message = L"服务未能停止，删除已取消。";
            PushResult(std::move(outcome));
            return;
        }
    }

    if (!ScmDelete(op.name, err)) {
        outcome.win32 = err;
        outcome.result = ClassifyWin32(err, outcome.message);
        PushResult(std::move(outcome));
        return;
    }

    outcome.result = OpResult::Ok;
    // wasRunning 为真时 DeleteService 只是「标记删除」，进程退出后才真消失，
    // 此时刷新列表它可能仍在。这不是失败，文案必须区分开。
    outcome.message = outcome.wasRunning
        ? L"已标记删除。服务完全停止后才会真正消失。"
        : L"服务已删除。";
    PushResult(std::move(outcome));
}

void Controller::RunQueryDetails(Operation const& op) {
    DWORD err = ERROR_SUCCESS;
    if (!opSession_.Open(err)) {
        LINGWU_TRACE("RunQueryDetails: op session open failed err=%lu",
                      static_cast<unsigned long>(err));
        return;
    }

    ServiceDetails details;
    // QueryDetails 失败不是错误：非提权时大量服务 OpenService 就被拒，
    // 这时 details.accessible = false，界面显示「（无权查看）」。
    opSession_.QueryDetails(op.name, details, err);

    {
        std::lock_guard<std::mutex> lock(detailMutex_);
        details_[op.name] = details;
        if (details.accessible && !details.binaryPath.empty()) {
            binaryPaths_[op.name] = StripQuotes(details.binaryPath);
        }
    }
}

void Controller::RunScanThirdParty() {
    DWORD err = ERROR_SUCCESS;
    if (!opSession_.Open(err)) {
        return;
    }
    std::unordered_map<std::wstring, std::wstring> paths;
    if (!opSession_.RefreshAllBinaryPaths(paths, err)) {
        LINGWU_TRACE("RunScanThirdParty failed err=%lu", static_cast<unsigned long>(err));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(detailMutex_);
        for (auto const& kv : paths) {
            binaryPaths_[kv.first] = kv.second;
        }
    }
    thirdPartyReady_.store(true);
    LINGWU_TRACE("third-party scan done, %zu paths", static_cast<size_t>(paths.size()));
}

// ---- 第三方筛选 ----

void Controller::RequestThirdPartyScan() {
    // 扫描走 op 线程的队列而不是另起线程：另起线程会在 Controller 析构后
    // 继续访问 this，而 Controller 是 MainWindow 的成员 —— 那是一次必崩的悬垂。
    // 代价是这次扫描会占住 op 线程约 1 秒，期间用户点别的操作会拿到 Busy。
    // 用户切筛选时通常没有别的操作要发，这个代价可以接受。
    thirdPartyReady_.store(false);
    Operation op;
    op.kind = OpKind::ScanThirdParty;
    Enqueue(std::move(op));
}

bool Controller::ThirdPartyReady() const {
    return thirdPartyReady_.load();
}

bool Controller::IsThirdParty(std::wstring const& name) const {
    std::lock_guard<std::mutex> lock(detailMutex_);
    auto it = binaryPaths_.find(name);
    if (it == binaryPaths_.end() || it->second.empty()) {
        // 查不到路径 → 不算第三方。宁可少显示几个，也不要把系统服务
        // 错标成第三方、让人对着关键服务去点删除。
        return false;
    }
    return IsThirdPartyBinary(it->second);
}

// ---- 详情 ----

void Controller::RequestDetails(std::wstring const& name) {
    Operation op;
    op.kind = OpKind::QueryDetails;
    op.name = name;
    // 详情查询不占满队列：切行时连点十下不该把队列撑爆。
    {
        std::lock_guard<std::mutex> lock(opMutex_);
        if (queue_.size() >= kMaxQueue) {
            return;
        }
        queue_.push_back(std::move(op));
    }
    opCv_.notify_one();
}

bool Controller::DetailsReady(std::wstring const& name, ServiceDetails& out) {
    std::lock_guard<std::mutex> lock(detailMutex_);
    auto it = details_.find(name);
    if (it == details_.end()) {
        return false;
    }
    out = it->second;
    return true;
}

}  // namespace service
}  // namespace core
}  // namespace lingwu
