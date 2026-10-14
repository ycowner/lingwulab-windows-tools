# lingwulab-windows-tools

LingwuLab 的 Windows 系统工具集。

每个工具是仓库下的一个独立子目录，各自带完整的 README、构建脚本与验收脚本。

## 包含的工具

| 目录 | 说明 | 版本 |
|---|---|---|
| [lingwu-sysmon](lingwu-sysmon/) | Windows 系统监视器（进程 / CPU / 内存 / 网络聚合），C++/WinRT + WinUI 3 | 0.0.1 |

## 构建

构建在工具自己的子目录内进行：

```bat
cd lingwu-sysmon
build.bat
```

各工具的完整说明、环境要求与验收方法见其子目录下的 `README.md`。
