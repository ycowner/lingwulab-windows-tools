# lingwulab-windows-tools

LingwuLab 的 Windows 系统工具集。

## 工具

| 目录 | 说明 | 版本 | 下载 |
|---|---|---|---|
| [lingwu-toolbox](lingwu-toolbox/) | **零物工具箱**：性能监视（进程 / CPU / 内存 / 网络聚合）+ 服务管理（列表 / 搜索 / 筛选 / 启停 / 启动类型 / 创建 / 删除），合为一个 exe、一个窗口、两条导航，C++/WinRT + WinUI 3 | 0.0.3 | 见 [lingwu-toolbox/README.md](lingwu-toolbox/README.md) |

两个工具原本是分开的两个程序（`lingwu-sysmon` 与 `lingwu-service-manager`），
2026-10-23 合并成上面这一个。合并前已发布的两条 release 保留原处，tag 不动：

- [LingwuSysmon-0.0.1-win64.exe](https://github.com/ycowner/lingwulab-windows-tools/releases/download/lingwu-sysmon-v0.0.1/LingwuSysmon-0.0.1-win64.exe)
- `lingwu-service-manager-v0.0.1`（见该仓库的 Releases 列表）

## 构建

构建在工具自己的子目录内进行：

```bat
cd lingwu-toolbox
build.bat
```

需要 Visual Studio 2026（v144 工具链）加 Windows 11 SDK 10.0.36100。
`build.bat` 与 `packaging\build-sfx.bat` 都会先读环境变量 `LINGWU_VSPATH`，
未设置时再退回 `vswhere` 自动探测——CI 必须显式设置它，见下。

完整说明、环境要求、权限模型与验收方法见 [lingwu-toolbox/README.md](lingwu-toolbox/README.md)。

## 发布

### tag 约定

一条 tag 命名空间：`<工具目录名>-vX.Y.Z`。

```bat
git tag lingwu-toolbox-v0.0.3
git push origin lingwu-toolbox-v0.0.3
```

推送 tag 即触发 `.github/workflows/release-toolbox.yml`：在 `windows-2025`
runner 上重新构建、打包成单文件 SFX，然后创建 GitHub Release 并挂上
`LingwuToolbox-<版本>-win64.exe`。不需要手动传任何密钥，用的是
`GITHUB_TOKEN`。

用 `-` 而不是 `/` 分隔是有意的：tag 里带斜杠虽然更常见，但下载链接里的
斜杠必须编码成 `%2F`，任何脚本化引用都会踩坑。

### commit 约定

**commit 标题写中文。** 这一点直接决定 release notes 长什么样：notes 里的
提交列表是 `git log --pretty=format:'%s'` 的原样输出，workflow 不做任何翻译。
所以标题是英文，notes 就永远半中半英。

不改标题语言的两条退路：把 notes 里的提交列表删掉只留 compare 链接，
或者在 workflow 里维护一份人工撰写的中文 changelog。前者丢信息，后者
需要人每次发版都记得维护。

产品名、版本号、文件名、tag 名、URL 和代码标识符保持原样，不翻译。

### 版本号从哪来

发布时 **tag 是唯一权威**。CI 从 tag 解析出版本号并传给
`make-sfx.ps1 -Version`，所以产物文件名、`VERSIONINFO`、Release 标题三者
不可能和 tag 对不上。

`make-sfx.ps1` 里那个 `$Version = '0.0.3'` 默认值只服务于**本地**构建。
发布工作流在收尾时会比对两者，不一致就打一条 warning 提醒更新——但不会
因此让发版失败，因为记账不一致不该拦住一个 tag 正确的发布。改动版本后记得
同步更新本文件上方的版本表格。

### 合并前后的发布线

合并之前每个工具各有一条 workflow 与一条 tag 命名空间；合并之后只剩一条
（`release-toolbox.yml` + `lingwu-toolbox-v*`）。已经发布的历史 release
**不会被删除也不会被移动** —— 已发布的 tag 是既成事实。

`.gh-verify.ps1` 里同时保留了三条映射（toolbox / sysmon / service-manager），
就是为了让历史 release 仍然可被校验。
