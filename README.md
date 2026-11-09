# WOW-335-NR

**WOW-335-NR** 为 WoW 3.3.5a HD（2008 年 32 位 D3D9 客户端）提供现代 NVIDIA 神经渲染管线：
画面增强（DLSS NR / DLAA）、超分（DLSS SR）、补帧（DLSS-G 2×/3×/4× 多帧生成）与光流运动
稳定，配套中文桌面控制台、游戏内悬浮面板与完整的状态/恢复/诊断体系。

WOW-335 家族：`WOW-335-CN`（汉化工程）与本仓库 `WOW-335-NR`（神经渲染工程）同族并行。

**当前状态：开发候选。** 核心链路已通过隔离客户端实测（NR/FG/MFG 热切换、中文调参、
补帧倍数 2×/3×/4×）；主客户端部署、30 分钟级稳定性与完整安装/卸载验收尚未完成。

## 组成

| 目录 | 内容 |
| --- | --- |
| `native/` | 自研三端源码：`console.cpp`（桌面控制台）、`panel.cpp`（游戏内悬浮面板）、`window_watch.cpp`（窗口助手）；`imgui/` 为 vendored 1.92.5 |
| `carrier/` | AIO 载体维护源（fork 自 kibblerz/DLSS5-Reshade-AIO v2.2.4，Apache-2.0）：`addon/src/nr-standalone.cpp` 为主处理管线，`x86-host/` 为游戏内 32 位抓帧插件，`external/` 为 vendored 依赖 |
| `controlcenter/` 无（脚本在根目录） | PowerShell 控制中心 `Dlss5.ControlCenter.ps1` + `Dlss5.Core.psm1`（`DLSS5-ControlCenter.cmd` 为入口）；已退役为非日常入口，日常使用桌面控制台 |
| 根目录构建脚本 | `Build-Console.ps1` / `Build-Panel.ps1` / `Build-WindowWatch.ps1` / `prepare_stage.py`；`performance/` 为性能与画面采集工具 |
| `schemes/` `templates/` | 出厂档位与配置模板 |
| `payload-lock.json` 等 | 组件身份锁：安装成员、面板哈希、助手哈希 |

## 构建

前置：Visual Studio C++ 工具集（x86/x64）、PowerShell 5.1。

```text
Build-Console.ps1          # 桌面控制台 → _scratch 输出，部署到 bin\DLSS5控制台.exe
Build-Panel.ps1            # 悬浮面板   → _scratch 输出，部署到 bin\dlss5-panel.exe
Build-WindowWatch.ps1      # 窗口助手（非日常必需）
carrier\addon\build.bat    # AIO 载体   → carrier\addon\build\standalone-dlssnr.addon64
```

构建输出强制落在上级工作区 `_scratch`（脚本内置校验），部署前需核对
`panel-lock.json` / `watcher-lock.json` 的源码与产物哈希，不修改锁值掩盖不匹配。
PowerShell 脚本必须 UTF-8 带 BOM，JSON 必须 UTF-8 无 BOM。

## 运行时依赖（不入库、不分发）

NVIDIA 运行时 `nvngx_dlssnr.dll`、`nvngx_dlss.dll`、`nvngx_dlssg.dll` 由用户提供，
放入隔离客户端 `host64\`。本仓库不附带、不下载、不再分发这些文件；ReShade 6.8.0
安装器由用户自备。运行时来源、签名与使用条件记录于工作区 P0 研究文档。

## 安装与测试

- 隔离测试客户端与安装事务由 `prepare_stage.py` + PowerShell 控制中心管理；
  只接受身份匹配的隔离批次，安装预览逐文件显示冲突，事务记录与备份位于工作区
  `_scratch/dlss5-aio/control-operations/`。
- 回归测试位于主工作区 `tools/tests/`（`test_dlss5_core.ps1`、`test_dlss5_stage.py`、
  `test_dlss5_performance.py`），覆盖核心模块、暂存与性能采集；它们与工作区布局耦合，
  不随本仓库分发。
- 项目设计、执行依据与验收记录在主工作区 `doc/维护/`、`doc/记录/` 的 DLSS5-AIO 系列
  文档，本仓库不重复维护。

## 诊断开关

各组件默认不打详细日志；排查问题时按组件打开，验收后关掉：

| 组件 | 开关 | 生效方式 |
| --- | --- | --- |
| 载体插件（addon64） | `host64/ReShade.ini` 的 `[Standalone.DLSSNR]` 加 `DebugLog=1`；或 Home→Add-ons 菜单页勾选 | 菜单切换即时生效 |
| 32 位包装器（addon32） | 客户端根 `dlss5-aio-x86.cfg` 加 `debug=1` | 每 60 帧热重载，约 1 秒 |
| 64 位载体（host64） | 包装器开启调试后自动传 `--debug` | 随包装器 |
| 游戏内悬浮面板 | 桌面控制台开启调试后启动面板自动传 `--debug` | 重启面板 |
| 桌面控制台 | exe 旁 `DLSS5控制台.ini` 加一行 `debug=1` | 重启控制台 |
| PowerShell 核心 | 环境变量 `DLSS5_CORE_DEBUG=1` | 下次安装/恢复操作 |

调试输出位置：载体插件 `standalone-dlssnr.log`、包装器 `dlss5-aio-x86.log`、64 位载体
`dlss5-feed-host.log`（均在客户端目录）、面板 stdout.log（启动器重定向目录）、控制台
`DLSS5控制台-debug.log`（exe 旁）、核心 `_scratch/dlss5-aio/logs/core-debug.log`。

## 载体血统对照

载体 fork 自上游 v2.2.4（commit `5947a3d`），在其上按分支演进。本仓库以初始导入收编
工作树现状；下表保留旧载体仓库的提交哈希以便追溯（历史保存在原工作区 `tools/dlss5/carrier/.git`）：

| 提交 | 内容 |
| --- | --- |
| `2c92a67` / `313bc73` / `05eb638` / `ce2a793` | DLSS-G 核心路由与能力探测系列 |
| `b6a2b0c` | live-params 模型热切换触发特性重建 |
| `d791ed1` / `915f20e` / `c044189` | MFG 2×/3×/4× 多帧生成与自钳 |
| 初始导入（本仓库） | MFG 呈现遥测查询越界修复、外部抓帧插件恢复期隐藏的识别与自动重显、F10 可靠恢复、`WOW-335-NR` 白名单与标题 |

## 许可与署名

- 本仓库代码按 Apache-2.0 授权（见 `LICENSE`），延续上游条款；`NOTICE` 与
  `THIRD_PARTY_NOTICES.md` 保留上游署名并标记修改。
- 载体 fork 自 [kibblerz/DLSS5-Reshade-AIO](https://github.com/kibblerz/DLSS5-Reshade-AIO)
  （Apache-2.0），内含 [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)
  （MIT，快照 `75754b2`）与 vendored ImGui（MIT）。
- 本项目与 NVIDIA 无关；DLSS、NGX 为 NVIDIA 商标。NVIDIA 运行时文件不随本仓库分发。

## 维护约定

- git 身份 MuyuQ；`main` 分支；版本标签 `v*`，不覆盖旧标签。
- 每次改动在主工作区账本（`doc/记录/修改详细记录.md`）登记；本仓库提交信息与之对应。
- 修改载体后需按构建-部署-回读流程验证哈希，再更新对应锁文件。
