<div align="center">

# OptiScaler 极光版

**RTX 40 多帧生成 · DLSS 神经渲染 · 游戏兼容性增强**

**简体中文** | [English](https://github.com/abc354402600/OptiScaler-Aurora/blob/aurora/README.en.md)

[![Aurora v1.1.2](https://img.shields.io/badge/Aurora-v1.1.2-7c3aed?style=flat-square)](https://github.com/abc354402600/OptiScaler-Aurora/releases/tag/aurora-v1.1.2)
![DLSS 310.9.1](https://img.shields.io/badge/DLSS-310.9.1-76b900?style=flat-square)
![Streamline 2.14.1](https://img.shields.io/badge/Streamline-2.14.1-2563eb?style=flat-square)
[![极光x aurora-plus](https://img.shields.io/badge/%E6%9E%81%E5%85%89x-aurora--plus-0ea5e9?style=flat-square)](docs/AURORA_X.zh-CN.md)

**[下载 v1.1.2](https://github.com/abc354402600/OptiScaler-Aurora/releases/tag/aurora-v1.1.2) · [安装与使用](https://github.com/abc354402600/OptiScaler-Aurora/blob/aurora/docs/GUIDE.zh-CN.md) · [游戏兼容性](https://github.com/abc354402600/OptiScaler-Aurora/blob/aurora/docs/COMPATIBILITY.zh-CN.md) · [更新说明](https://github.com/abc354402600/OptiScaler-Aurora/releases/tag/aurora-v1.1.2)**

</div>

极光版是基于 [OptiScaler](https://github.com/optiscaler/OptiScaler) 及社区分支开发的免费开源项目，提供超分辨率替换、帧生成和画质调节，并针对 RTX 40 多帧生成、DLSS 神经渲染及游戏兼容性做了扩展。它是社区版本，不是 OptiScaler 或 NVIDIA 官方发行版。

## 极光x 更新（`aurora-plus` 分支）

> 极光x 是本复刻自己维护的分支。它以极光 `aurora` 分支的 `bde8bcdc` 为底座，这是 v1.1.2 之后的版本，已经包含下文提到的两项未发版 D3D12 修正。极光x 主要移植官方 OptiScaler 和社区分支的修复；自己加的功能只限于把极光已有的能力放进面板，并单独标明。运行库（DLSS 310.9.1、Streamline 2.14.1、神经渲染 310.8）和使用方法都跟极光 v1.1.2 相同。目前没有发布安装包，需要自行编译 `aurora-plus` 分支。每项改动的上游来源、移植说明和验证记录见[极光x 改动记录](docs/AURORA_X.zh-CN.md)。

**当前版本：极光X 1.1.3（`1e66579b`，2026-10-18）**，面板标题显示 "OptiScaler Aurora X v1.1.3"。上一版 `7bb280dc` 已在霍格沃茨之遗里实测（RTX 4090，神经渲染加 4X）。

| 类别 | 改动 | 来源 |
|---|---|---|
| 稳定性 | 游戏给 Reflex 帧标记传空指针时不再崩溃 | 官方 OptiScaler `12b248c2` |
| 稳定性 | "正在创建 D3D 设备"的标记改为每个线程各自一份，多线程同时创建设备时不再卡住 | 官方 OptiScaler `2d91b8e3` |
| Streamline | 游戏不在 C 盘时，不再把 C 盘的 Streamline 插件误认成 OptiScaler 自带的 | 官方 OptiScaler `880402ba` |
| Streamline | `[NvApi] DisableOTA` 默认开启：游戏只用自带的或运行库同步装上的 Streamline 插件，不加载 NVIDIA 自动下载的版本 | 极光x |
| RTX 40 多帧生成 | 驱动自动下载的 `.bin` 插帧 DLL 和改过名的插帧 DLL 也能识别并修补 | Susemi（源自 KleberMotta 和 RenoDX MFG 解锁插件） |
| RTX 40 多帧生成 | 解除 Streamline 插件自带的倍率上限，避免 3X、4X 请求被拒 | 同上 |
| RTX 40 多帧生成 | 新增可选的软件帧调度 `[DLSSG] AdaFlipMeteringPatch`，默认关闭，高倍率下画面卡住时再打开 | 同上 |
| RTX 40 多帧生成 | 补强：两处倍率限制先定位、确认各只出现一次再修改；插帧内核没替换成功就不开放高倍率；替换内核时先检查边界，再一次写入 | 参照 Susemi |
| RTX 40 多帧生成 | 已经卸载的插帧 DLL 副本（比如驱动探测用的那份）不再计入判定；同一地址重新加载的副本会重新修补 | 极光x |
| 游戏内面板 | MFG 状态以实际开放的倍率为准，没开放时写明原因；新增"Streamline 插件倍率上限"和"软件帧调度"两行状态 | 极光x |
| 游戏内面板（1.1.3 新功能） | 神经渲染"分遍设置"里每一遍可以单独选风格（默认/自然/电影感）；修好帮助标记重复显示的问题 | 极光x |

这些改动对性能和延迟没有可感知的影响，增加的开销都在微秒以下，详见改动记录。

## 主要功能

| 功能 | 说明 |
|---|---|
| RTX 40 多帧生成 | 在已验证的游戏配置中支持最高 **6X**，使用 NVIDIA DLSSG／MFG 路线；具体支持取决于游戏、显卡与运行库。 |
| DLSS 神经渲染 | 支持模型分辨率等参数调节，用于权衡画质与处理开销；属于实验性功能。 |
| 超分辨率与画质调节 | 继承 OptiScaler 的 DLSS、XeSS、FSR 后端及锐化、预设、输出缩放等功能；不同图形 API 的支持范围不同。 |
| 运行库同步 | 安装后检查兼容的 DLSS／Streamline 文件，备份、同步并校验；保留较新版本、原生 SL1 和无法安全识别的组。 |
| 游戏内面板 | 按 `Insert` 调节参数并保存配置。 |
| 恢复与排错 | 提供运行库检查和卸载恢复流程，便于处理游戏更新后的文件变化。 |

## v1.1.2 维护更新

> 主分支另有待发布修正：D3D12 跟踪开关按线程隔离、修正两处 UAV 跟踪条件，已通过 Windows 构建和专项检查。**当前 v1.1.2 下载包尚不包含这两项修正**；大型状态跟踪重构仍未启用。

吸收官方 XeLL 模块选择修复，移除普通加载路径的全导出重定向；Vulkan 菜单不再提供不支持的 FFX／Combo 和强制 XeLL 选项。运行库与原有功能保持不变。

## v1.1.1 维护更新

修复零宽高交换链误判，补充 Control Resonant 帧生成兼容策略与可配置的 Streamline 自动下载控制。保留 v1.1 的运行库和功能；详情见[维护版发行说明](https://github.com/abc354402600/OptiScaler-Aurora/releases/tag/aurora-v1.1.2)。

## v1.1 功能基础

- **更新运行库**：DLSS SR／RR／FG 310.9.1、Streamline 2.14.1 及配套文件；可选 Neural Rendering 保持 310.8 Runtime／2.13 插件。
- **避免运行库降级**：不再用包内较旧版本覆盖游戏较新的运行库，继续保护原生 Streamline 1.x。
- **加强初始化与关闭处理**：修正部分失败被当作成功、清理责任丢失、句柄创建与重复释放等问题。
- **增加资源访问保护**：完善帧状态、Reflex、HUD／深度复制和设备归属检查，改善共享对象的 CPU 并发处理。
- **保留默认兼容策略**：`DualFeature=false`；不包含已停止开发的 RC3 自动多入口安装器。

本版已通过完整 Windows 编译、配置的兼容性专项检查、运行库清单与压缩包校验。[完整发行说明](https://github.com/abc354402600/OptiScaler-Aurora/releases/tag/aurora-v1.1.2)记录了改动和未解决限制。

## 快速开始

1. 从上方发行页下载 `.7z` 包，退出游戏，把**全部文件**解压到实际游戏 `.exe` 所在目录。可在游戏运行时通过任务管理器的“打开文件所在的位置”确认目录，然后退出游戏再安装。
2. 运行 `setup_windows.bat`，按提示选择加载入口。一般可从 `dxgi.dll` 开始，**异环使用 `winmm.dll`**；不要在同一目录同时部署多个 Aurora Proxy。
3. 检查安装和运行库同步结果，正常启动游戏，按 `Insert` 打开面板。首次先确认基础功能正常，再逐项调整帧生成和神经渲染设置。

安装提示完成不等于运行库同步一定成功；若出现同步警告，请先查看报告。详细步骤、更新与卸载见[使用指南](https://github.com/abc354402600/OptiScaler-Aurora/blob/aurora/docs/GUIDE.zh-CN.md)。

## 游戏兼容性

**“存在正常运行反馈”不等于“所有版本、显卡和切换场景均已验证”。**

| 游戏 | 已有记录与当前边界 |
|---|---|
| 巫师3 | 用户反馈 RTX 4080 Laptop、DLSSG 310.9.1、6X 正常运行；读图、动态 MFG 切换与轻微闪烁仍未完成全面回归。旧版 SL1 按实际文件保护，不把所有游戏版本固定判为 SL1。 |
| 异环 | 曾确认面板可打开及 6X 可运行，推荐 `winmm.dll`；角色、抽卡菜单卡顿另有社区缓解办法，未确认所有菜单均恢复多帧生成。 |
| 鬼武者：剑之道、黎明行者之血 | 保留此前 6X／神经渲染的兼容性记录，不视为 v1.1 全场景重新验证。 |
| 绝区零 | 组件异常 `11008`、闪退和文件消失反馈尚未确认解决。 |
| 原神、崩铁 | 不同桥接、运行库和游戏版本存在差异，没有覆盖全部环境的稳定性保证。 |

[查看兼容性细节与已知问题](https://github.com/abc354402600/OptiScaler-Aurora/blob/aurora/docs/COMPATIBILITY.zh-CN.md)。Aurora 不提供反作弊绕过；受反作弊保护的游戏可能拒绝加载，不应将上述记录理解为使用许可或安全保证。

## 文档与反馈

- [文档导航](https://github.com/abc354402600/OptiScaler-Aurora/blob/aurora/docs/README.md)：使用指南、兼容性、版本说明与开发资料。
- [更新记录](https://github.com/abc354402600/OptiScaler-Aurora/blob/aurora/Changelog.md) · [英文参数说明](https://github.com/abc354402600/OptiScaler-Aurora/blob/aurora/Config.md)。
- [提交问题](https://github.com/abc354402600/OptiScaler-Aurora/issues)：请附 Aurora 版本、游戏版本、显卡／驱动、Proxy 名称、FG 输入／输出、复现步骤及相关日志。

## 开源与致谢

感谢 [OptiScaler](https://github.com/optiscaler/OptiScaler)、原作者、社区 fork 和相关组件贡献者。上游原文与完整致谢保存在[上游说明存档](https://github.com/abc354402600/OptiScaler-Aurora/blob/aurora/README.upstream.md)，旧 fork 安装文档另作历史存档，不作为本版安装教程。

项目使用 [GPL-3.0](https://github.com/abc354402600/OptiScaler-Aurora/blob/aurora/LICENSE)；各随附组件依照各自许可证，参见 [Licenses](https://github.com/abc354402600/OptiScaler-Aurora/tree/aurora/Licenses)。
