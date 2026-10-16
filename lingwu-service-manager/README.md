# 零物服务管理器 / Lingwu Service Manager

Windows 11 风格的服务管理器。单文件免安装、解压即用，替代 `services.msc` 的日常用法。

- C++/WinRT + WinUI 3，与同仓库的 [lingwu-sysmon](../lingwu-sysmon/) 同一套技术栈与构建链
- 最低支持 **Windows 10 1809（10.0.17763）**；Mica 材质与圆角在运行期按系统版本门控，低版本自动退回纯色
- **默认以普通权限启动**：看列表不需要管理员。需要提权的操作会就地提示，由你点「以管理员身份重新启动」
- 纯按需打开：启动显示主窗口，关闭即退出。不进托盘、不开机自启、不常驻后台

## 功能

| 能力 | 说明 |
|---|---|
| 服务列表 | 默认显示全部服务（含已停止、已禁用），与服务管理器一致 |
| 搜索 | 按服务名或显示名筛选，大小写无关的**子串**匹配 |
| 状态筛选 | 全部 / 运行中 / 已停止 / 已禁用 / 第三方 |
| 排序 | 名称、状态、启动类型三列，**每列都可双向排序**，表头箭头指示方向 |
| 详情面板 | 可执行路径、运行账户、描述、依赖服务、退出代码。默认折叠，点标题展开 |
| 启动 / 停止 / 重启 | 单个服务，带等待态（不会点了没反应） |
| 启动类型切换 | 自动 / 自动（延迟启动）/ 手动 / 禁用 |
| 创建服务 | 服务名、显示名、描述、可执行路径、启动类型、运行账户、创建后立即启动 |
| 删除服务 | 分级确认：自家服务直接删；第三方服务且已停止时要求逐字输入服务名；运行中走「停止并删除」 |

**已知不支持**（有意为之，不是缺陷）：

- 自定义运行账户与密码 —— 只支持 `LocalSystem` / `LocalService` / `NetworkService` 三个内置账户。自定义账户需要用 `lpPassword` 把明文密码交给 SCM，本工具不这么做
- 依赖服务多选 —— `lpDependencies` 恒为 `nullptr`
- 驱动服务 —— 只列 Win32 服务（`SERVICE_WIN32`），不含内核驱动
- 服务恢复策略（失败后重启等）—— 只有 `services.msc` 的「恢复」标签页能做，不在本工具范围

## 使用

解压后双击 `LingwuServiceManager.exe`。没有安装步骤，不需要 .NET 运行时，删掉目录即卸载。

设置写在 `%APPDATA%\LingwuServiceManager\settings.ini`（排序方式、筛选、详情面板展开状态、窗口位置尺寸）。

### 权限模型

这是本工具与 `services.msc` 最大的设计差异，值得说清楚：

```
普通权限启动
   ├─ 看列表、搜索、筛选、排序、看详情     → 一直可用
   └─ 启动 / 停止 / 重启 / 改启动类型 /
      创建 / 删除                        → 系统会拒绝
                                          ↓
                                    弹提示说明原因
                                          ↓
                            点「以管理员身份重新启动」→ runas 提权重开
```

提权重开时会**带上你当前的搜索词与筛选**，不用重新设一遍。原窗口先关闭、进程退出、互斥量释放之后才执行 `runas` —— 顺序反了会导致新实例认为「已经有一份在跑」然后自己退出，表现是「点了提权重启没反应」。

如果想每次双击就直接提权，把 `resources/app.manifest` 里的 `requestedExecutionLevel` 改成 `requireAdministrator`。默认刻意不这么写：看列表是主要用途，而那根本不需要管理员。

## 构建

```bat
cd lingwu-service-manager
build.bat
```

产物在 `dist\Release\LingwuServiceManager.exe`。

需要 Visual Studio 2026（v145 工具集）+ Windows 11 SDK 10.0.26100。构建链会自动探测，也可以用 `LINGWU_VSPATH` 显式指定。

打包成单文件 SFX：

```powershell
pwsh -NoProfile -File .\packaging\make-sfx.ps1 -Version 0.0.1
```

产物在 `dist\package\LingwuServiceManager-0.0.1-win64.exe`。

重新生成图标（图标是几何图形，不是美术资源，所以留了可复现的脚本）：

```powershell
pwsh -NoProfile -File .\tools\make-icon.ps1 -Preview
```

## 验收

脚本都在 `tools\`，**必须用 `pwsh` 跑，不能用 Windows PowerShell 5.1**（5.1 的 UIA COM 绑定行为不同）。

```powershell
# 内核自检：引号逻辑、第三方判定、枚举、字段合法性、详情、缓存
.\dist\Release\LingwuServiceManager.exe --selftest

# 基线：窗口、345 条服务、三列双向排序、搜索、筛选、详情面板
pwsh -NoProfile -File .\tools\uicheck.ps1

# 创建服务表单（非提权时验拒绝路径，提权时验真的建出来）
pwsh -NoProfile -File .\tools\uicreate.ps1

# 提权模型（非提权时验拒绝+提权入口，提权时验操作生效）
pwsh -NoProfile -File .\tools\uiadmin.ps1

# 内存：干净实例，私有内存红线 160 MB
pwsh -NoProfile -File .\tools\uimem.ps1
```

`uicreate.ps1` 会**真的创建服务**，服务名统一是 `LingwuSmokeTest` 前缀，脚本结束时会删掉并确认删除成功。

### 实测数据（参考机：Win11，345 个服务）

| 指标 | 数值 |
|---|---|
| 私有内存（稳态） | 106.4 MB |
| 工作集（稳态） | 161.3 MB |
| 私有内存（交互后瞬时） | ~149 MB |
| 句柄 / 线程 | 896 / 38 |
| 60 秒漂移 | 0 MB |
| 枚举一趟 | 1 次 RPC（345 条，64 KB 一页装完） |
| 二次枚举 | 15 ms（全走启动类型缓存） |

**红线卡私有内存 160 MB，不卡工作集。** 工作集里含从 Windows App Runtime 框架包映射进来的只读共享页（`Microsoft.UI.Xaml.dll` 等），那些页跨进程共享、随应用个数增长而与本应用无关；拿它当红线等于惩罚「用了框架」，而框架依赖部署正是本项目的既定方向。

## 打包链与发布

`packaging\` 三件套（`make-sfx.ps1` / `build-sfx.bat` / `sfx_main.cpp`）把 `dist\Release` 收进一个自解压 exe，格式是自定义的 LSFX v1，不压缩（内容本来就全是已压缩的 DLL，deflate 收益约等于零，却要在启动器里塞一个解压器）。

推送 tag 触发发布：

```bat
git tag lingwu-service-manager-v0.0.1
git push origin lingwu-service-manager-v0.0.1
```

`.github/workflows/release-svcmgr.yml` 会在 `windows-2025` runner 上重建、打包、生成 release notes 并发布。**tag 是版本的唯一权威**：包名、VERSIONINFO、release 标题全部从 tag 派生，所以已发布的产物不可能和它下面的 tag 不一致。

`.github/workflows/release-sysmon.yml` 用独立 tag 前缀，推本工具的 tag 不会碰 sysmon 的 release。

## 目录

```
lingwu-service-manager/
├─ build.bat                    构建入口
├─ LingwuServiceManager.vcxproj
├─ resources/                   app.manifest / app.rc / app.ico
├─ src/
│  ├─ main.cpp                  入口：--selftest 前置、单实例、提权收尾
│  ├─ core/                     不依赖 UI 的逻辑层
│  │  ├─ service_model.h        纯函数与数据结构（文案、判定、排序枚举）
│  │  ├─ scm.h/.cpp             SCM 原始调用层（本层只做 API→错误码）
│  │  ├─ controller.h/.cpp      poll 线程 + op 线程 + 队列
│  │  ├─ privilege.h/.cpp       提权判定与 runas 重启
│  │  ├─ settings.h/.cpp        settings.ini
│  │  └─ selftest.h/.cpp        --selftest 的 7 组断言
│  ├─ platform/                 trace / 窗口装饰 / 单实例
│  └─ ui/                       App / MainWindow / CreateServiceWindow / ServiceRow
├─ packaging/                   SFX 自解压打包
├─ tools/                       验收脚本 + 图标生成
└─ AGENTS.md                    给 AI agent 的硬约束与踩坑清单
```

## 已知限制

- **Windows 10 1809–1903 没有实测环境**（与 lingwu-sysmon 同样状态）。Mica 与圆角的门控逻辑是按系统版本判断的，但这两个版本上没人跑过。1809 是最低支持版本，不是验证过的版本。
- **本机无法用合成鼠标输入取证**，所有 UI 验收都走 UI Automation 的 Invoke / Value / SelectionItem 模式。右键菜单的弹出路径（Win32 `WM_CONTEXTMENU` + 命中测试）因此**只做了逻辑验证，没有端到端截图证据**。
- 服务列表的启动类型需要每服务一次 `QueryServiceConfigW`。首次约 345 次 RPC（在后台线程里，用户看到的是列表先出来、启动类型随后补齐），稳态命中缓存为 0 次。非提权时个别服务查不到，那一格显示 `—` 而不是猜一个值。
- 删除服务后进程可能短暂残留（SCM 的「已标记删除」状态），列表里会显示「（已标记删除）」，完全停止后消失。
