# Civilization VI 2K 在线修复工具

这是一个面向 Windows Steam 版《文明 VI》的非官方社区工具，用于处理一种特定情况：电脑网络正常，但游戏启动后 2K 服务一直显示离线。

> 本项目与 2K、Firaxis Games、Take-Two Interactive 或 Valve 无隶属或背书关系。工具会验证游戏是否兼容现有修复方法，无法确认安全时不会处理游戏进程。

> **官方项目地址：** [github.com/yeeesy/Civ6-2K-Online-Fix](https://github.com/yeeesy/Civ6-2K-Online-Fix)。从其他渠道获得本工具时，请以该仓库的 [Releases](https://github.com/yeeesy/Civ6-2K-Online-Fix/releases)、源码标签和 SHA-256 校验值为准。普通问题可提交到 [GitHub Issues](https://github.com/yeeesy/Civ6-2K-Online-Fix/issues)，安全问题请按 [`SECURITY.md`](SECURITY.md) 私下报告。

## 当前版本

**1.2.1** 增加游戏更新后的自动兼容检查：EXE 哈希变化时，会先验证现有修复方法所需的代码和运行库，兼容时自动尝试修复。Windows Steam 版《文明 VI》`1.0.12.74` 的 DX11、DX12 已完成实测，保留对旧构建的支持。从 [Releases](https://github.com/yeeesy/Civ6-2K-Online-Fix/releases) 下载 ZIP，或在 1.2 中直接检查更新。感谢 [TUcookie 的前期适配贡献](https://github.com/yeeesy/Civ6-2K-Online-Fix/pull/2)。

| 渲染器 | 状态 | 工具行为 |
| --- | --- | --- |
| 1.0.12.68 DX11 | 已验证 | 精确匹配并通过运行时校验后可修复 |
| 1.0.12.68 DX12 | 已验证 | 精确匹配并通过运行时校验后可修复 |
| 1.0.12.72 DX11 / DX12 | 已验证 | 精确匹配并通过运行时校验后可修复 |
| 1.0.12.74 DX11 / DX12（Steam Build 26595377） | 已验证自动适配 | 通过结构及运行时校验后，自动使用现有方法修复 |
| 后续游戏构建 | 自动检查兼容性 | 结构兼容则尝试修复；不匹配或未确认有效时提示暂不支持 |

## 原理

在受影响的游戏构建中，2K 在线模块可能在本机初始化时遇到线程同步错误，使后续初始化无法继续，因此即使网络正常，游戏也会显示 2K 离线。

本工具会等待游戏启动，识别实际运行的 DX11 或 DX12，并核对进程、游戏文件和系统运行库。对于未登记的新 EXE，会先检查锁调用、服务对象和状态字段是否仍兼容。全部检查一致后，工具使用现有保护纠正特定的异常解锁，让游戏原有的 2K 初始化流程继续执行，再确认拦截及在线状态。

自动尝试期间如果未观察到异常拦截、也未确认服务就绪，会撤销仍属于本次会话的保护入口并提示暂不支持。如果已经拦截但服务仍未就绪，或读取不完整，则保留超时结果供排查，不会将网络、账号等问题直接归为版本不兼容。结构发生较大变化时仍可能需要工具更新，不能保证兼容所有未来版本。

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

从 1.2 起，检测结束后点击 **反馈问题**，会打开本次完整的脱敏 TXT，可直接作为附件发送。重新打开工具后可从 **报告文件夹** 找到以前的报告；1.1 用户仍可用“复制脱敏诊断”。

从 **1.2.0** 起，打开工具窗口会自动检查工具新版本（通常每天一次），也可以点 **检查更新** 立即检查。适配新版游戏的正式工具发布后，在提示中选择“是”即可下载并验证签名更新，无需手动替换 EXE。正在监控时，更新会等到本轮会话结束后安装，配置和反馈文件保留。Steam 更新游戏本身不会给工具添加兼容配置。尚无此更新功能的旧版用户仍需先下载一次 ZIP。

`1.1.0` 的增强诊断提供读取质量、相对时间线和超时分类，详见 [脱敏诊断与超时反馈](docs/DIAGNOSTICS.md)。旧版 `1.0.0` 仍采用简要报告格式，请以报告中的诊断格式为准。超时只表示未在期限内确认同时就绪，不等于数据库损坏；不要因此直接删除 SQLite 文件。

## 安全与隐私

- 不修改游戏文件、存档、Mod、Steam 配置或账号数据。
- 不安装服务、驱动、计划任务、启动项或后台常驻程序。
- 已登记构建及通过严格结构兼容检查的新构建可进入运行时校验；无法确认兼容性时停止处理。
- 处理前会重新确认目标进程、游戏映像、系统运行库及目标位置仍与预期一致。
- 遇到身份变化、所有权冲突、写入不完整或恢复结果不确定时，会停止并明确报告失败。
- 日志只保存在本机且不会自动发送；公开反馈请使用脱敏摘要。

## 局限性

- 仅面向 Windows Steam 版；自动适配要求现有锁调用、服务对象及运行库契约仍可验证，较大改动可能需要更新工具。
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

Civ6 2K Online Fix is an unofficial Windows helper for a specific local initialization failure in the Steam version of Civilization VI. Version 1.2.1 verifies structural compatibility before trying the existing repair on updated EXEs, with DX11 and DX12 of 1.0.12.74 tested. Earlier supported builds, shareable text reports and signed tool updates remain available. See the [project and releases](https://github.com/yeeesy/Civ6-2K-Online-Fix).
