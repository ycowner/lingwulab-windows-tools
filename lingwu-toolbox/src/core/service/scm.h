// scm.h —— 服务控制管理器（SCM）的原始调用层。
//
// 本层只做「一次 API 调用 → 一个 Win32 错误码」，不做任何等待、重试、
// 业务判断。那些在 controller.h。
//
// 【本层的线程契约】
// SC_HANDLE 不是线程安全对象，也不该跨线程共享。本层所有非成员函数
// 各自开关句柄（一次调用一个句柄，用完即关）；唯一长期持有的会话对象是
// ScmSession，而**它归 poll 线程独占**。UI 线程要用写操作一律走非成员函数，
// 不要碰 ScmSession —— 那才是「一句 300 次 RPC」的枚举和「一次控制」混在一起
// 的典型串扰。
#pragma once

#include "core/service/service_model.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace lingwu {
namespace core {
namespace service {

// dwServiceFlags 的 bit 3：服务已被标记删除，进程退出后才真正消失。
// SDK 里有 SERVICE_MARKED_FOR_DELETE，但为了不让「本工程用到的常量是否受
// SDK 版本影响」这件事反复复核，这里写字面量并注明来源。
inline constexpr DWORD kServiceFlagMarkedForDelete = 0x8;

// ── 句柄 RAII ────────────────────────────────────────────────────────────

// 关闭句柄必须配对。用裸 SC_HANDLE 的话，EnumAll 那条路径上任何一次
// throw / 提前 return 都会漏掉一次 CloseServiceHandle —— 而 SCM 句柄
// 是有配额上限的（本机默认数千），漏几十次就能把服务管理器自己卡死。
class ScManager {
public:
    ScManager() = default;
    ~ScManager() { Close(); }
    ScManager(ScManager const&) = delete;
    ScManager& operator=(ScManager const&) = delete;

    // desiredAccess 通常给 SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE。
    // 普通用户凭这两个就能枚举出全部服务 —— 这是「默认非提权也能看列表」的基础。
    bool Open(DWORD desiredAccess, DWORD& err);
    void Close();
    SC_HANDLE get() const { return h_; }
    bool valid() const { return h_ != nullptr; }

private:
    SC_HANDLE h_ = nullptr;
};

class ScService {
public:
    ScService() = default;
    ~ScService() { Close(); }
    ScService(ScService const&) = delete;
    ScService& operator=(ScService const&) = delete;

    bool Open(ScManager const& mgr, std::wstring const& name, DWORD desiredAccess, DWORD& err);
    void Close();
    SC_HANDLE get() const { return h_; }
    bool valid() const { return h_ != nullptr; }

private:
    SC_HANDLE h_ = nullptr;
};

// ── 枚举会话（poll 线程独占）────────────────────────────────────────────

class ScmSession {
public:
    // 打开服务控制管理器。失败返回 false，err 里是 Win32 错误码。
    bool Open(DWORD& err);

    // 一趟完整枚举。列表的全部字段都来自这里。
    //
    // 【三趟 API 的分工，及其代价】
    //   ① EnumServicesStatusW —— 一次 RPC 拿到**全部**服务的
    //      名字 / 显示名 / 状态 / 退出码。
    //   ② QueryServiceStatusEx(SC_STATUS_PROCESS_INFO) —— 取 dwProcessId
    //      与 dwServiceFlags，这两个字段经典枚举里没有，**每个运行中
    //      的服务一次 RPC**。实测 157 个运行中服务全部取到，
    //      整轮耗时在 GetTickCount64 的分辨率以下。
    //   ③ QueryServiceConfigW —— 拿启动类型，每个服务一次 RPC。
    //
    // 【为什么不用 EnumServicesStatusExW 做 ①】方案原本用它（一次 RPC
    // 顺带给出 PID）。实测在同一台机器、同一工具链、非提权下：
    // 参数全对也一律返回 err=87「参数错误」，换 dwServiceType /
    // dwServiceState / 缓冲区大小六种组合全失败；而经典 API 一次就
    // 列出 345 条。本工具的默认运行模式**就是非提权**（权限模型第一条），
    // 所以 Ex 变体在主用路径上不可用。已确认不再尝试 Ex。
    //
    // ②③ 都做**增量缓存**：
    //   · 首次枚举跑满 N 次 RPC（N ≈ 300，后台线程里约几十毫秒，
    //     用户看到的是列表先出来、启动类型随后补齐）；
    //   · 稳态下服务集合几乎不变，命中缓存 = 0 次额外 RPC；
    //   · 只有**新注册**的服务才会多付一次。
    bool EnumAll(std::vector<ServiceInfo>& out, DWORD& err);

    // 丢弃启动类型缓存。提权状态变化后要调 —— 之前因权限不足
    // 拿到空值的服务，现在能查到了。
    void DropStartTypeCache();

    // 强制为全部服务重查启动类型（切到「第三方」筛选时用，
    // 因为那要读每条服务的可执行路径，见下）。
    bool RefreshAllStartTypes(DWORD& err);

    // 第三方筛选需要**每条**服务的可执行路径 → 每条一次 QueryServiceConfigW。
    // 这个代价只在用户真的切到该筛选时付一次，之后进缓存。
    bool RefreshAllBinaryPaths(std::unordered_map<std::wstring, std::wstring>& paths,
                                DWORD& err);

    // 一次单服务的完整详情，供详情面板使用。
    bool QueryDetails(std::wstring const& name, ServiceDetails& out, DWORD& err);

    // 缓存命中查询（详情面板切行时先问这个，避免重复 RPC）。
    bool CachedDetails(std::wstring const& name, ServiceDetails const** out) const;

private:
    // 补一个服务的启动类型。失败返回 false 并写 err，但**不**让整趟枚举失败：
    // 单个服务查不到启动类型（多半是权限不足）不该让整个列表消失。
    bool ResolveStartTypeLocked(std::wstring const& name, ServiceInfo& info);

    struct Cached {
        DWORD startType = 3;
        bool  delayed = false;
        bool  startTypeKnown = false;
        ServiceDetails details;   // binaryPath/account/description/dependencies
    };

    ScManager mgr_{};
    bool open_ = false;

    mutable std::unordered_map<std::wstring, Cached> cache_{};
};

// ── 写操作（UI 线程投递到 op 线程后调用，各自开关句柄）──────────────────

// 创建服务的入参。
//
// 【为什么不支持自定义账户与密码】这是用户明确否掉的：密码会以明文
// 形式穿过本进程内存并交给 SCM。lpPassword 因此恒为 nullptr。
struct CreateServiceRequest {
    std::wstring serviceName;     // SCM 标识符，创建后不可改
    std::wstring displayName;     // 仅显示用
    std::wstring description;
    std::wstring binaryPath;      // 不含引号，本层负责补
    DWORD startType = 3;
    bool  delayedAutoStart = false;
    std::wstring account = L"LocalSystem";
    bool  startImmediately = false;
};

bool ScmQueryState(std::wstring const& name, DWORD& state, DWORD& err);
bool ScmStart(std::wstring const& name, DWORD& err);
bool ScmStop(std::wstring const& name, DWORD& err);
bool ScmSetStartType(std::wstring const& name, DWORD startType, bool delayed, DWORD& err);
bool ScmCreate(CreateServiceRequest const& req, DWORD& err);
bool ScmDelete(std::wstring const& name, DWORD& err);

// 给 lpBinaryPathName 补引号。
//
// 【CreateServiceW 不会自动加引号，这是本工程最贵的一个坑】
// 路径 C:\Program Files\My App\svc.exe 不加引号时，SCM 会把它按空格切成
// "C:\Program" 和 "Files\My" 两个参数，服务的 ImagePath 存成半截，
// 表现是服务启动失败、事件日志只有「找不到指定文件」，**没有任何提示说
// 是引号问题**。反过来，如果路径已经被引号包住（用户可能自己粘进来的），
// 重复加引号会让 SCM 把它整个当成路径而找不到文件。
// 判据：首字符是引号就原样传，否则含空格/制表符就补一对。
std::wstring QuoteBinaryPath(std::wstring const& path);

}  // namespace service
}  // namespace core
}  // namespace lingwu
