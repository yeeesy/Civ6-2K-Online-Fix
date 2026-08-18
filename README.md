# Civilization VI 2K 在线修复工具

这是一个面向 Windows Steam 版《文明 VI》的非官方社区工具，用于处理一种特定情况：电脑网络正常，但游戏启动后 2K 服务一直显示离线。

> 本项目与 2K、Firaxis Games、Take-Two Interactive 或 Valve 无隶属或背书关系。工具只支持源码中明确列出的游戏构建，无法确认安全时不会处理游戏进程。

> **官方项目地址：** [github.com/yeeesy/Civ6-2K-Online-Fix](https://github.com/yeeesy/Civ6-2K-Online-Fix)。从其他渠道获得本工具时，请以该仓库的 [Releases](https://github.com/yeeesy/Civ6-2K-Online-Fix/releases)、源码标签和 SHA-256 校验值为准。普通问题可提交到 [GitHub Issues](https://github.com/yeeesy/Civ6-2K-Online-Fix/issues)，安全问题请按 [`SECURITY.md`](SECURITY.md) 私下报告。

## 当前版本

`1.0.0` 是本项目第一次正式公开发布。

| 渲染器 | 状态 | 工具行为 |
| --- | --- | --- |
| DX11 | 支持 | 精确识别并完成安全校验后处理 |
| DX12 | 支持 | 精确识别并完成安全校验后处理 |
| 未知或更新后的游戏构建 | 不支持 | 拒绝处理 |

## 原理

在受影响的游戏构建中，2K 在线模块可能在本机初始化时遇到线程同步错误，使后续初始化无法继续，因此即使网络正常，游戏也会显示 2K 离线。

本工具会等待游戏启动，确认实际运行的是受支持的 DX11 或 DX12 构建，并核对进程、游戏文件和系统运行库。全部检查一致后，它只纠正这一次已确认的本地同步错误，让游戏原有的 2K 初始化流程继续执行。

工具不会伪造登录结果，也不会绕过 Steam/2K 的账号、游戏所有权或服务器认证。不同电脑、系统运行库和启动时序会影响故障是否出现，所以并非每位玩家都会遇到该问题。

## 使用方法

1. 完全退出《文明 VI》。
2. 从官方 Releases 下载 ZIP，核对 SHA-256 后解压。
3. 运行 `Civ6_2K_Online_Fix.exe`，无需管理员权限。
4. 工具进入监听后，在 Steam 中点击“开始游戏”并选择 DX11 或 DX12。
5. 等待工具显示最终结果。成功后可以关闭工具窗口；本次保护会随游戏进程退出而结束。

GUI 默认只监听，不会自行启动游戏。这是推荐方式，因为你可以在 Steam 中明确选择 DX11 或 DX12，工具会按实际启动的游戏自动识别。也可以点击“用 Steam 默认项启动”或使用 `--auto-launch-steam`，但该方式使用 Steam 当前的默认启动项，不能保证渲染器。

常用命令：

```text
Civ6_2K_Online_Fix.exe                      图形界面；默认只监听
Civ6_2K_Online_Fix.exe --watch-only         显式只监听
Civ6_2K_Online_Fix.exe --auto-launch-steam  请求 Steam 使用默认启动项
Civ6_2K_Online_Fix.exe --no-gui             无图形界面
Civ6_2K_Online_Fix.exe --diagnose           只读诊断
Civ6_2K_Online_Fix.exe --self-test          纯离线自检
```

如需让无参数启动也自动请求 Steam，可将 `Civ6_2K_Online_Fix.ini.example` 复制为同目录下的 `Civ6_2K_Online_Fix.ini`，再把 `auto_launch_steam` 改为 `1`。默认值为 `0`。

报告普通问题时，优先使用 GUI 的“复制脱敏诊断”。`--diagnose` 输出和原始 JSONL 可能包含本机路径、PID、内存地址及文件哈希；工具不会自动上传这些资料，请勿将原始日志直接发布到公开 Issue。

## 安全与隐私

- 不修改游戏文件、存档、Mod、Steam 配置或账号数据。
- 不安装服务、驱动、计划任务、启动项或后台常驻程序。
- 只有精确匹配的受支持构建能够进入处理流程；未知构建会失败关闭。
- 处理前会重新确认目标进程、游戏映像、系统运行库及目标位置仍与预期一致。
- 遇到身份变化、所有权冲突、写入不完整或恢复结果不确定时，会停止并明确报告失败。
- 日志只保存在本机且不会自动发送；公开反馈请使用脱敏摘要。

## 局限性

- 仅支持源码中列出的 Windows Steam 构建；游戏更新后可能需要新增兼容配置。
- 只处理上述本地初始化故障，不能修复 2K/Steam 服务中断、网络封锁、账号问题、Mod 冲突或多人数据不同步。
- 当前发布未做商业 Authenticode 代码签名，Windows SmartScreen 或安全软件可能显示“未知发布者”。请只从官方 Releases 下载并核对 SHA-256，不要因此关闭安全软件。
- 任何进程内修复都无法完全消除并发风险；本项目通过严格识别和失败关闭来限制风险。

## 从源码构建

需要 Visual Studio 2022 C++ x64 工具链、Windows 10/11 SDK 和 PowerShell：

```powershell
& '.\build.ps1' -Analyze
& '.\build\Civ6_2K_Online_Fix.exe' --self-test
```

产品版本的唯一来源是根目录 `version.json`。开发和发布规则见 [`docs/MAINTAINER.md`](docs/MAINTAINER.md)，安全报告见 [`SECURITY.md`](SECURITY.md)，贡献方式见 [`CONTRIBUTING.md`](CONTRIBUTING.md)。项目使用 [MIT 许可证](LICENSE)。

## English summary

Civ6 2K Online Fix is an unofficial Windows helper for a specific local initialization failure in the Steam version of Civilization VI. It recognizes supported DX11 and DX12 builds, validates the target process and runtime environment, and fails closed when any required identity or ownership check is uncertain. It does not bypass authentication, account, or game-ownership checks. Version 1.0.0 is the first official public release. Source code, releases, checksums, and issue tracking are available at [github.com/yeeesy/Civ6-2K-Online-Fix](https://github.com/yeeesy/Civ6-2K-Online-Fix).
