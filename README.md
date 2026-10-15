# lingwulab-windows-tools

LingwuLab 的 Windows 系统工具集。

每个工具是仓库下的一个独立子目录，各自带完整的 README、构建脚本与验收脚本。

## 包含的工具

| 目录 | 说明 | 版本 | 下载 |
|---|---|---|---|
| [lingwu-sysmon](lingwu-sysmon/) | Windows 系统监视器（进程 / CPU / 内存 / 网络聚合），C++/WinRT + WinUI 3 | 0.0.1 | [LingwuSysmon-0.0.1-win64.exe](https://github.com/ycowner/lingwulab-windows-tools/releases/download/lingwu-sysmon-v0.0.1/LingwuSysmon-0.0.1-win64.exe) |

## 构建

构建在工具自己的子目录内进行：

```bat
cd lingwu-sysmon
build.bat
```

需要 Visual Studio 2026（v144 工具链）加 Windows 11 SDK 10.0.26100。
`build.bat` 与 `packaging\build-sfx.bat` 都会先读环境变量 `LINGWU_VSPATH`，
未设置时再退回 `vswhere` 自动探测——CI 必须显式设置它，见下。

各工具的完整说明、环境要求与验收方法见其子目录下的 `README.md`。

## 发布

### tag 约定

每个工具拥有一条独立的 tag 命名空间：`<工具目录名>-vX.Y.Z`。

```bat
git tag lingwu-sysmon-v0.0.1
git push origin lingwu-sysmon-v0.0.1
```

推送 tag 即触发 `.github/workflows/release-sysmon.yml`：在 `windows-2025`
runner 上重新构建、打包成单文件 SFX，然后创建 GitHub Release 并挂上
`LingwuSysmon-<版本>-win64.exe`。不需要手动传任何密钥，用的是
`GITHUB_TOKEN`。

用 `-` 而不是 `/` 分隔是有意的：tag 里带斜杠虽然更常见，但下载链接里的
斜杠必须编码成 `%2F`，任何脚本化引用都会踩坑。

### 版本号从哪来

发布时 **tag 是唯一权威**。CI 从 tag 解析出版本号并传给
`make-sfx.ps1 -Version`，所以产物文件名、`VERSIONINFO`、Release 标题三者
不可能和 tag 对不上。

`make-sfx.ps1` 里那个 `$Version = '0.0.1'` 默认值只服务于**本地**构建。
发布工作流在收尾时会比对两者，不一致就打一条 warning 提醒更新——但不会
因此让发版失败，因为记账不一致不该拦住一个 tag 正确的发布。改动版本后记得
同步更新本文件上方的版本表格。

### 已知取舍

GitHub Release 挂在 tag 上，而 tag 是整个仓库共享的扁平命名空间，**目录名
不参与**。因此：

- 各工具的版本线互相独立，不会互相顶掉——靠的是 tag 前缀约定。
- 但所有 Release 出现在同一个列表里，**无法按目录分区显示**。
- 全仓库只有一个 `Latest` 标记，发任一工具都会把它移到该工具的新版上。

工具数量变多后，可以在根目录加一个下载索引页（GitHub Pages + 从 Release
API 拉取）来提供分区视图；tag 约定不用改。

### 新增一个工具的发布

1. 建 `<新工具>/` 子目录，结构照 `lingwu-sysmon/`。
2. 复制 `.github/workflows/release-sysmon.yml`，改三处：`name`、
   tag 触发模式、Release 标题；VS 发现步骤可以原样保留。
3. `build.bat` / `make-sfx.ps1` 也需要一份，或者抽到仓库根做成共享脚本
   （多个工具共用同一套打包链时值得做）。
