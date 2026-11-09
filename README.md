# WOW-335-NR

![License](https://img.shields.io/badge/license-Apache--2.0-blue)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-lightgrey)
![Status](https://img.shields.io/badge/status-%E5%BC%80%E5%8F%91%E5%80%99%E9%80%89-orange)

**WOW-335-NR** 为 WoW 3.3.5a HD（2008 年 32 位 D3D9 客户端）提供现代 NVIDIA 神经渲染管线：
DLSS 神经渲染（NR）、DLSS 超分（SR）/ DLAA、DLSS-G 帧生成与多帧生成（MFG 2×/3×/4×），
并配套中文桌面控制台、游戏内悬浮面板、状态遥测、恢复与诊断工具。

WOW-335 家族中，`WOW-335-CN` 是汉化工程，本仓库 `WOW-335-NR` 是神经渲染工程，两者同族并行。
载体基线为 DLSS5 ReShade AIO 2.2.4，宿主 ReShade 6.8.0，界面使用 ImGui 1.92.5。

> **当前状态：开发候选（2026-11-09）。**
> 核心链路已在隔离客户端实测：NR / FG / MFG 热切换、中文调参、补帧 2×/3×/4×。
> 主客户端部署、30 分钟级稳定性、完整安装 / 卸载验收尚未完成。
> 构建成功、隔离样本和进程状态都不等于游戏验收通过。

## 本文导航

| 目的 | 章节 |
| --- | --- |
| 了解项目定位与能力 | [项目定位](#项目定位) · [功能](#功能) |
| 理解处理链与组件关系 | [架构](#架构) · [组件](#组件) |
| 编译与安装 | [构建](#构建) · [安装与部署](#安装与部署) |
| 日常使用与调参 | [使用](#使用) · [配置参考](#配置参考) |
| 采集数据与排查问题 | [遥测与性能](#遥测与性能) · [诊断](#诊断) |
| 了解许可与上游 | [许可与署名](#许可与署名) · [参考项目](#参考项目) |

## 快速开始（开发 / 隔离测试）

> [!IMPORTANT]
> **本仓库不是独立可构建的。** 构建与安装脚本把仓库上两级视为工作区根：
> `_scratch`（构建输出、隔离批次、安装事务）、回归测试与账本都在工作区，不在仓库内。
> 独立克隆到任意目录时，构建产物会被写到仓库之外、工具脚本会直接报错。
> 请按 `src/WOW-335-NR` 的工作区布局放置仓库。下文「工作区根」均指仓库上两级目录。

1. 准备 32 位 ReShade、64 位 ReShade 和三个 NVIDIA 运行时 DLL（均需用户自备，见[运行时依赖](#运行时依赖)）。
2. 按[构建](#构建)编译原生工具与载体。
3. 按[安装与部署](#安装与部署)在隔离客户端中暂存、预览并安装组件。
4. 启动 `DLSS5控制台.exe`，选择包含 `wow.exe` 与 `host64` 的客户端根目录，再启动游戏。
5. 游戏中按 `F11` 展开悬浮面板，选择档位或进入「自定义」调参；开启补帧时先在控制台设置源帧率上限。

## 项目定位

### 它做什么

- 在 32 位 D3D9 游戏中捕获最终画面，送入 64 位 x64 载体执行 NR / SR / DLAA / FG 管线。
- 在游戏上方提供独立置顶输出窗，以原生尺寸显示处理后的画面。
- 提供中文桌面控制台与游戏内悬浮面板，支持实时调参、档位切换和状态查看。
- 提供安装预览、身份校验、备份 / 恢复、遥测与性能采集工具。

### 它不做什么

- 不是 NVIDIA 官方产品，与 NVIDIA 无隶属或背书关系。
- 不附带、不下载、不再分发 NVIDIA 运行时 DLL 或 ReShade 安装器。
- 不修改游戏 MPQ、插件、字体或账号数据；部署只涉及客户端目录中的渲染与配置文件。
- 不绕过反作弊，也不保证在线服务条款允许使用；请仅在你有权控制的客户端与环境中使用。
- 当前不生成公开分发包；控制中心只接受身份匹配的隔离客户端。

## 功能

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| DLSS 神经渲染（NR） | 已实现，隔离客户端实测 | 引擎 feature 18；3 个模型、4 个观感参数 |
| DLSS 超分（SR）/ DLAA | 已实现 | 按源分辨率与输出分辨率自动选择 SR 或 1:1 DLAA |
| DLSS-G 帧生成 | 已实现，隔离客户端实测 | 开启后默认 2×；出厂模板默认关闭 |
| 多帧生成（MFG） | 已实现，隔离客户端实测 2×/3×/4× | 3×/4× 需 RTX 50 系；其他卡自动回落 2× |
| NVIDIA Optical Flow 运动 | 已实现，试验 | 默认关闭；开启后为 NR/SR/FG 提供运动与拒绝掩码 |
| VORT 运动集成 | 已实现，试验 | 默认关闭；需额外安装 VORT 着色器 |
| 中文桌面控制台 | 已实现 | 启动游戏、保活面板、遥测、源帧率上限、日志入口 |
| 游戏内悬浮面板 | 已实现 | F11 展开；实时调参；档位与自定义 |
| 安装 / 修复 / 恢复 | 已实现，开发候选 | 仅隔离客户端；完整安装 / 卸载验收未完成 |
| 遥测与性能分析 | 已实现 | 共享内存遥测、PresentMon 关联、Python 采集工具 |

## 架构

### 组件关系

```mermaid
flowchart TB
    subgraph G32["32 位游戏进程"]
        WOW["wow.exe<br/>WoW 3.3.5a HD · D3D9"]
        RS32["d3d9.dll<br/>32 位 ReShade + addon 支持"]
        A32["standalone-dlssnr.addon32<br/>抓帧 + 设置代理"]
    end
    subgraph H64["64 位载体进程"]
        WRAP["host64/AIO DLSS5 32-bit Wrapper.exe"]
        RS64["host64/dxgi.dll<br/>64 位 ReShade + addon 支持"]
        A64["standalone-dlssnr.addon64<br/>NR → SR/DLAA → FG/MFG"]
        OUT["StandaloneDLSSNRNativeOutput<br/>原生尺寸输出窗"]
    end
    PANEL["dlss5-panel.exe<br/>游戏内悬浮面板"]
    CONSOLE["DLSS5控制台.exe<br/>启动 / 遥测 / 帧率上限"]
    WOW --> RS32 --> A32
    A32 -->|启动 + 共享 GPU 纹理| WRAP
    WRAP --> RS64 --> A64 --> OUT
    OUT -. 置顶显示 .-> WOW
    PANEL -. live-params.json .-> A64
    CONSOLE -->|启动并保活| PANEL
    CONSOLE -. 读取遥测 .-> WRAP
```

### 处理链

NR 是神经渲染阶段，不是空间放大器。它按源分辨率工作，之后再由 DLSS SR 或 DLAA 输出到原生尺寸：

```text
低分辨率游戏帧 + 深度 + 运动
        │
        ▼
   DLSS NR（feature 18）
        │
        ▼
   DLSS SR（放大到原生）          原生分辨率游戏帧 + 深度 + 运动
        │                              │
        │                              ▼
        │                         DLAA（1:1）
        └──────────────┬───────────────┘
                       ▼
              DLSS-G / MFG 2×/3×/4×
                       ▼
                原生尺寸输出窗
```

- 源分辨率等于输出分辨率时，管线使用 1:1 DLAA。
- 源分辨率低于输出分辨率时，管线使用 DLSS SR 重建到原生尺寸。
- `SourceResolutionOverride` 可在不降低游戏自身分辨率的情况下，先把捕获帧降采样再送入 NR。
- NR 关闭时，管线仍可保留 DLSS / DLAA 与可选帧生成。

### 32 位桥接

1. 32 位 ReShade 以 `d3d9.dll` 加载 `standalone-dlssnr.addon32`。
2. `addon32` 把游戏最终帧复制到共享 GPU 纹理，并显示 AIO 设置代理。
3. `addon32` 自动启动 `host64\AIO DLSS5 32-bit Wrapper.exe`（x64）。
4. 包装器建立 x64 D3D12 载体，`host64\dxgi.dll`（64 位 ReShade）加载 `standalone-dlssnr.addon64`。
5. `addon64` 在载体 Present 处执行 NR / SR / DLAA / FG / MFG，并把结果写入原生尺寸输出窗。

x64 运行时 DLL 必须放在 `host64\`，不能放在 32 位游戏可执行文件旁边。

### 热切换与状态

- 悬浮面板把参数原子写入 `host64\live-params.json`，载体在 Present 路径上约每 500 ms 检查一次，变更后热应用。
- NR / FG / 参数变化立即生效；模型切换会触发特性重建，通常约 1 秒。
- 载体把保存值写回 `host64\ReShade.ini` 的 `[Standalone.DLSSNR]`，面板可重新读取用于显示。
- 载体通过 `Local\DLSS5_AIO_Telemetry_<host PID>` 共享内存发布 320 字节只读遥测，使用 seqlock，不阻塞呈现。
- 控制台和面板读取同一份遥测，分别显示输出帧率、NR/SR/FG 状态、分辨率与图层覆盖状态。

## 仓库结构

```text
WOW-335-NR/
├─ native/                    自研原生工具源码
│  ├─ console.cpp             桌面控制台
│  ├─ panel.cpp               游戏内悬浮面板
│  ├─ window_watch.cpp        窗口助手
│  ├─ tiers.hpp               出厂档位与参数刻度（唯一事实源）
│  ├─ window_layer.hpp        置顶 / 穿透窗口样式
│  └─ imgui/                  vendored Dear ImGui 1.92.5
├─ carrier/                   AIO 载体维护源（fork 自 DLSS5-Reshade-AIO v2.2.4）
│  ├─ addon/src/nr-standalone.cpp   主处理管线
│  ├─ addon/src/nvof-motion-provider.cpp  NVIDIA Optical Flow 提供者
│  ├─ addon/src/nvngx-bridge.cpp    nvngx 调用桥
│  ├─ addon/include/          菜单 schema、光流头文件
│  ├─ addon/shaders/          DLSS5_AIO_Feed.fx、StandaloneBoundary.fx
│  ├─ x86-host/               32 位抓帧插件与 x64 包装器
│  ├─ external/DLSS5-Feeder/  vendored 上游快照（MIT）
│  ├─ lab/                    私有 NR 契约验证程序
│  ├─ tools/                  性能分析器
│  ├─ release/                上游发布说明与打包脚本
│  └─ runtime/                运行时 DLL 占位说明（不含二进制）
├─ performance/               性能与画面采集 Python 工具
├─ schemes/                   出厂档位 JSON（原画 / 轻量 / 均衡 / 浓郁）
├─ templates/                 客户端配置模板
├─ Build-Console.ps1          编译桌面控制台
├─ Build-Panel.ps1            编译悬浮面板
├─ Build-WindowWatch.ps1      编译窗口助手
├─ prepare_stage.py           私有组件暂存
├─ Dlss5.ControlCenter.ps1    PowerShell 控制中心
├─ Dlss5.Core.psm1            安装 / 恢复 / 遥测核心
├─ payload-lock.json          安装成员身份锁
├─ panel-lock.json            面板源码与产物哈希
└─ watcher-lock.json          窗口助手源码与产物哈希
```

`carrier/README.md` 是上游 AIO 的详细使用手册，保留供追溯；本 README 只描述本仓库的
WoW 3.3.5a 集成方式与日常入口。

## 组件

| 组件 | 产物 | 作用 |
| --- | --- | --- |
| 64 位载体 | `host64/standalone-dlssnr.addon64` | NR / SR / DLAA / FG / MFG 主处理管线 |
| 32 位抓帧插件 | `standalone-dlssnr.addon32` | 捕获 32 位游戏帧，启动并转发给 x64 载体 |
| x64 包装器 | `host64/AIO DLSS5 32-bit Wrapper.exe` | 建立 D3D12 载体，承接共享纹理 |
| 桌面控制台 | `DLSS5控制台.exe` | 启动游戏、保活面板、遥测、源帧率上限、日志入口 |
| 游戏内面板 | `dlss5-panel.exe` | 置顶悬浮 UI、实时调参、档位 / 模型 / 补帧控制 |
| 窗口助手 | `window_watch.exe` | 让输出窗对鼠标穿透，便于游戏接收输入 |
| PowerShell 控制中心 | `Dlss5.ControlCenter.ps1` | 隔离客户端的状态、预览、安装、修复、恢复、诊断 |
| 性能分析器 | `DLSS5-PerformanceAnalyzer.cmd` | 遥测采集、PresentMon 关联、报告生成 |
| 性能 Python 工具 | `performance/*.py` | 隔离批次采集、资源探测、画面捕获、相关性分析 |

### 桌面控制台

- 首次启动选择客户端根目录（含 `wow.exe` 与 `host64`），选择结果写入 exe 旁的
  `DLSS5控制台.ini`；也可用 `--game <wow.exe 路径>` 指定并记住。
- 检测到游戏运行后，若面板未启动，会从控制台所在目录拉起 `dlss5-panel.exe` 并保活。
- 只读展示载体遥测；参数写入由悬浮面板完成，控制台不直接改载体参数。
- 读写 `WTF\Config.wtf` 的 `SET MaxFPS` / `SET MaxFPSBackground`。
- 提供悬浮面板日志目录与载体日志目录的打开入口。

### 游戏内悬浮面板

- 独立置顶窗口，D3D11 + ImGui，微软雅黑动态字形；收起态是鼠标穿透的信息条，展开态可交互。
- 页面：状态、画面增强（NR）、补帧（DLSS-G）、模型、说明。
- `F11` 展开 / 收起。热键注册失败时自动退回按键轮询。
- 参数写入 `host64\live-params.json`；载体应用后会写回 `ReShade.ini`。
- 「从载体 ini 重新读取」只同步显示，不写入载体；刚改的值需等载体落盘（约 1 秒）。
- 单实例互斥；游戏退出后转入等待，下次启动游戏自动显示。

### 载体 Add-ons 菜单

游戏内按 `Home` 打开 ReShade 覆盖层，进入 Add-ons 页的「Standalone DLSS-NR + SR」可调高级项：
源分辨率覆盖、DLSS 渲染预设、NVIDIA Optical Flow / VORT、自适应 GPU 压力调节、基准段与
`DebugLog`。这些选项写入 `host64\ReShade.ini`；日常观感调参仍建议用悬浮面板。

### 窗口助手

`window_watch.exe` 只用于需要让置顶输出窗对鼠标穿透的场景：

```text
window_watch.exe --game <wow.exe 绝对路径>
```

它不启动游戏，会校验游戏 / 载体路径、SHA-256、PID、窗口类、前台状态、样式与几何关系，
然后给匹配的 `StandaloneDLSSNRNativeOutput` 窗口设置分层 / 穿透样式。等待十分钟或游戏退出后结束。
日常使用桌面控制台与面板时通常不需要它。

### PowerShell 控制中心

`DLSS5-ControlCenter.cmd` 是入口，核心逻辑在 `Dlss5.Core.psm1`：

- 状态检查、安装预览、安装、修复、恢复、设置、档位、诊断导出。
- 只接受 `_scratch/dlss5-aio/` 下身份匹配的隔离客户端；默认批次是
  `_scratch/dlss5-aio/20261106-093536`，可在界面修改。
- 安装前逐文件预览冲突；安装时完整暂存、备份原始 `d3d9.dll`、逐项替换并回读；
  失败按记录回退。事务记录与备份位于 `_scratch/dlss5-aio/control-operations/`。
- 已退役为日常入口：日常启动游戏和调参请用桌面控制台与悬浮面板。

## 环境要求

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Windows 10 / 11 x64 |
| GPU | 支持 DLSS 的 NVIDIA RTX 显卡；MFG 3×/4× 需 RTX 50 系 |
| 游戏 | WoW 3.3.5a HD 32 位客户端，客户端根目录含 `wow.exe` |
| ReShade | 6.8.0，32 位与 64 位均需 addon 支持 |
| NVIDIA 运行时 | `nvngx_dlssnr.dll`、`nvngx_dlss.dll`、`nvngx_dlssg.dll`，用户自备 |
| 构建 | Visual Studio C++ 工具集（x86 / x64）、PowerShell 5.1 |
| Python 工具 | Python 3.9+（仅 `prepare_stage.py` 与 `performance/` 需要） |

驱动与运行时必须支持对应的 NR / DLSS-G 特性。3×/4× MFG 还取决于 `DLSSG.MultiFrameCountMax`
能力值；运行时不支持时，载体自动把请求钳到 2×。

### 运行时依赖

以下文件**不入库、不分发、不下载**，由用户从许可允许的来源自行取得：

- `nvngx_dlssnr.dll`：神经渲染运行时。
- `nvngx_dlss.dll`：DLSS 超分运行时。
- `nvngx_dlssg.dll`：帧生成运行时。
- 32 位 / 64 位 ReShade（含 addon 支持）。

`host64\nvngx.dll` 是本项目的调用桥，不是 NVIDIA 运行时文件，随组件安装。
运行时来源、签名与使用条件记录在工作区 P0 研究文档中。

## 构建

### 前置

- Visual Studio，安装 `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`。
- PowerShell 5.1。
- 仓库按工作区布局放在 `src/WOW-335-NR`；构建脚本把仓库上两级视为工作区根。

### 原生工具

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Build-Console.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Build-Panel.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Build-WindowWatch.ps1
```

- 输出目录必须位于工作区 `_scratch/` 下；脚本拒绝已存在目录和路径中的链接 / 重解析点。
- 每次构建生成新的 GUID 目录，例如 `_scratch/dlss5-aio/console-build-<guid>/`。
- 构建记录 `build.json` 保存编译器路径、源码 SHA-256、产物 SHA-256，状态为
  `compiled_candidate_not_game_validated`。
- 编译使用 `/std:c++17 /W4 /O2 /MT /utf-8`，静态链接运行库。
- 当前工作区的日常部署件位于 `tools/dlss5/bin/`（仓库外）：`DLSS5控制台.exe`、
  `dlss5-panel.exe`、`window_watch.exe`。控制台从自身目录拉起面板，二者必须同目录。

### 部署到日常运行位

构建产物只落在 `_scratch/<guid>/`；日常运行件需要手动部署到工作区 `tools/dlss5/bin/`：

1. `dlss5-console.exe` → 复制并重命名为 `DLSS5控制台.exe`（脚本不改名，部署时处理）。
2. `dlss5-panel.exe`、`window_watch.exe` → 同名复制到同一目录（面板必须与控制台同目录）。
3. 面板 / 窗口助手部署后核对：确认锁中源码 SHA-256 与当前源一致，再把新产物的
   SHA-256 更新进 `panel-lock.json` / `watcher-lock.json`。
4. 控制台没有独立锁文件；其部署来源以构建记录 `build.json` 的产物哈希追溯。

### 载体

```bat
carrier\addon\build.bat
carrier\x86-host\build-carrier.bat
```

- `carrier\addon\build.bat` 构建 `host64/standalone-dlssnr.addon64`、`nvngx.dll`、
  shader 副本、性能分析器与 Vulkan 回退层，输出在 `carrier\addon\build\`。
- `carrier\x86-host\build-carrier.bat` 构建 `standalone-dlssnr.addon32` 与
  `host64/AIO DLSS5 32-bit Wrapper.exe`，输出在 `carrier\x86-host\build\`。
- 载体构建使用 `carrier\external\DLSS5-Feeder\tools\vcvars.bat` 定位 Visual Studio。
- `carrier\runtime\` 中没有运行时 DLL 时，构建仍会成功，但会警告并从包中省略对应文件。
- `addon64` 与 x64 载体使用 `/MD` 动态运行库；原生工具与 `addon32` 使用 `/MT` 静态运行库。

### 编码约定

- PowerShell 脚本必须 UTF-8 **带 BOM**。
- JSON 必须 UTF-8 **无 BOM**。
- `.gitattributes` 设置 `* -text`，保留文件原有编码与换行，git 不做 EOL 规范化。
- CMD / BAT 中的中文按实际运行环境核对，不批量转码。

## 安装与部署

### 隔离暂存（当前开发路径）

`prepare_stage.py` 从用户提供的 AIO 压缩包与运行时文件生成私有暂存目录。AIO 压缩包自身
有固定 SHA-256，压缩包成员与额外输入的每个文件再逐项核对 `payload-lock.json`：

```powershell
python -B prepare_stage.py `
  --aio-zip <AIO 2.2.4 ZIP> `
  --reshade32 <32 位 ReShade d3d9.dll> `
  --reshade64 <64 位 ReShade dxgi.dll> `
  --reshade-fxh <ReShade.fxh> `
  --nr <nvngx_dlssnr.dll> `
  --sr <nvngx_dlss.dll> `
  --batch _scratch/dlss5-aio/20261106-093536 `
  --name my-stage
```

- `--batch` 必须是 `_scratch/dlss5-aio/` 下已存在的隔离批次；`--name` 只允许字母、数字、`-`、`_`。
- 脚本拒绝符号链接 / 重解析点、绝对路径、`..`、重复成员和越界目标。
- 每个成员写入后立即回读哈希；失败写 `stage-failure.json`，成功写 `stage-record.json`。
- `prepare_stage.py` 只复制 `source_kind=stage` 的成员；NR / SR 运行库通过 `--nr`/`--sr` 注入。
- 需要帧生成时，把 `nvngx_dlssg.dll` 另行放入生成的暂存目录；控制中心会把
  `payload-lock.json` 中 `source_kind=runtime` 的成员一并校验并安装。
- 运行时文件只进入隔离暂存，不进入 Git、工具目录或诊断包。

随后启动控制中心：

```bat
DLSS5-ControlCenter.cmd
```

选择客户端与组件来源，先「检查状态」和「预览安装」，再执行「安装到隔离客户端」。
安装、修复、恢复前必须完全退出游戏与载体进程。修复与恢复也按同一事务记录处理。

### 客户端目录布局

```text
<客户端根目录>\
├─ wow.exe
├─ d3d9.dll                              32 位 ReShade（替换原 d3d9.dll，安装时备份原件）
├─ standalone-dlssnr.addon32             32 位抓帧插件
├─ dlss5-aio-x86.cfg                     32 位包装器配置
├─ reshade-shaders\Shaders\
│  ├─ DLSS5_Feed.fx
│  ├─ DLSS5_Feed_D3D9.fx
│  ├─ DLSS5_Feed_impl.fxh
│  └─ ReShade.fxh
└─ host64\
   ├─ AIO DLSS5 32-bit Wrapper.exe
   ├─ dxgi.dll                           64 位 ReShade
   ├─ standalone-dlssnr.addon64
   ├─ nvngx.dll                          项目调用桥（不是 NVIDIA 运行时）
   ├─ nvngx_dlssnr.dll                   用户自备
   ├─ nvngx_dlss.dll                     用户自备
   ├─ nvngx_dlssg.dll                    用户自备
   ├─ ReShade.ini
   ├─ ReShadePreset.ini
   └─ reshade-shaders\Shaders\
      ├─ DLSS5_AIO_Feed.fx
      ├─ StandaloneBoundary.fx
      └─ ReShade.fxh
```

- 32 位游戏只能加载 32 位 ReShade 与 `standalone-dlssnr.addon32`。
- 所有 x64 DLL、`addon64` 与包装器必须留在 `host64\`。
- 不要把 64 位 NVIDIA 运行库放到 32 位游戏可执行文件旁边。
- `payload-lock.json` 是安装成员与哈希的唯一事实源；上表只用于理解布局。

### 身份锁

| 文件 | 内容 |
| --- | --- |
| `payload-lock.json` | AIO / ReShade 版本、`wow.exe` 哈希、原始 `d3d9.dll` 哈希、安装成员与来源分类 |
| `panel-lock.json` | `dlss5-panel.exe`、`panel.cpp` 的 SHA-256 与 ImGui 版本 |
| `watcher-lock.json` | `window_watch.exe`、`window_watch.cpp`、`window_layer.hpp` 的 SHA-256 |

构建、部署或回读后发现哈希不一致时，先查明原因；**不要修改锁值来掩盖不匹配**。

### 客户端版本匹配

`payload-lock.json` 登记的 `game_sha256` 绑定当前开发所用 HD 客户端的 `wow.exe`；
`prepare_stage.py` 与控制中心会**拒绝身份不符的客户端**——这是设计行为，不是故障。

若你的客户端版本不同，需要按新客户端重建身份锁：核对新 `wow.exe`、原始 `d3d9.dll`
与全部安装成员的 SHA-256 并重新登记。这是一次逐文件核验工作；只改哈希值凑数会让
身份校验失去意义，属于明确禁止的做法。

## 使用

### 首次启动

1. 启动 `DLSS5控制台.exe`。
2. 若未指定客户端，按引导选择包含 `wow.exe` 与 `host64` 的根目录。
3. 控制台记住路径；之后可直接启动游戏。
4. 游戏运行后，控制台在面板未启动时自动拉起 `dlss5-panel.exe`。
5. 首次启动后确认游戏内 ReShade Add-ons 页出现「Standalone DLSS-NR + SR」，
   且控制台遥测从「未确认」变为可用。

### 桌面控制台

| 区域 | 内容 |
| --- | --- |
| 实时状态 | 输出 FPS、NR 运行状态、帧生成状态、图层覆盖、输入 / 输出分辨率、NR/SR 帧数、管线就绪 |
| 源帧率上限 | 前台 / 后台 `MaxFPS`，保存到 `WTF\Config.wtf` |
| 日志 | 打开悬浮面板日志目录或载体日志目录 |

开启帧生成时务必设置源帧率上限。推荐前台 30、后台 10；不限速会让采集队列撑爆 32 位地址空间。
建议游戏未运行时保存，避免游戏退出回写覆盖；也可在游戏内输入 `/console MaxFPS 30` 即时生效。

### 游戏内悬浮面板

| 操作 | 行为 |
| --- | --- |
| `F11` | 展开 / 收起面板 |
| 拖动顶部标题行 | 移动面板 |
| 拖动右下角 | 缩放面板 |
| 顶部 `×` | 收起 |
| 底部「退出面板」 | 关闭面板 |

页面说明：

- **状态**：真实帧率、输出帧率、模型、强度、管线、补帧、显示图层、NR/SR 帧数、分辨率。
- **画面增强（NR）**：NR 开关、四个出厂档位、自定义、四个微调滑杆、「从载体 ini 重新读取」。
- **补帧（DLSS-G）**：补帧开关、2×/3×/4× 倍数、状态与累计插帧。
- **模型**：默认 / 自然 / 电影三选一；切换后后台重建约 1 秒。
- **说明**：热键与调参要点。

### 档位

档位是完整配方；选择出厂档位会锁定微调滑杆，点「自定义」解锁。模型与补帧是独立选择器，
不参与档位匹配，也不会被档位切换改写。

| 档位 | NR | 模型 | 强度 | 色调结构 | 结构强度 | 皮肤结构 | 说明 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 原画 | 关 | 1 | 1.00 | 1.00 | 1.00 | -1.00 | 关闭神经渲染，仅保留载体管线 |
| 轻量 | 开 | 1 | 0.20 | 1.00 | 0.10 | -1.00 | 接近原画的轻度重绘 |
| 均衡 | 开 | 1 | 0.35 | 1.00 | 0.25 | -1.00 | 推荐起步档 |
| 浓郁 | 开 | 1 | 0.60 | 1.30 | 0.50 | -1.00 | 强重绘，色调偏电影感 |

档位数值来自 `native/tiers.hpp`；`schemes/*.json`、`templates/host64-ReShade.ini`
与 PowerShell 校验边界必须与它一致。

### 微调参数

| 参数 | 范围 | 中性值 | 说明 |
| --- | --- | --- | --- |
| 强度 | 0–1 | — | 整体重绘幅度；0 最弱，1 满强度；过高会放大锐化与伪影 |
| 色调结构 | 0–2 | 1.00 | 色彩与明暗层次；越低越平，越高保留越多 |
| 结构强度 | 0–1 | — | 边缘与细节；0 最平滑，1 细节最多；过高会放大噪点 |
| 皮肤结构 | -1–+1 | 0.00 | 人物皮肤；-1 更平滑，+1 保留更多纹理 |

四个参数保留各自自然量程，统一两位小数显示。零点语义不同，不强行统一刻度。

### 热键

| 热键 | 作用 |
| --- | --- |
| `F10` | 原画对比；若输出窗被隐藏，先恢复输出窗 |
| `F11` | 展开 / 收起悬浮面板 |
| `Ctrl+Alt+P` | 循环 DLSS 渲染预设 J → K → L → M |
| `Ctrl+Alt+N` | 循环 NR 模型 1 → 2 → 3 |
| `Ctrl+Alt+B` | 循环临时基准段：用户设置 → 关闭 addon → 仅 DLSS/DLAA → NR + DLSS/DLAA → DLSS/DLAA + FG → NR + DLSS/DLAA + FG → 恢复 |
| `Home` | ReShade 覆盖层合成到输出窗；载体检测到覆盖层状态后调整输入路径 |
| `Alt+X` | 隐藏置顶输出窗，便于 NVIDIA 覆盖层接收输入；`F10` 恢复 |

`F10` 是视觉诊断，不是性能旁路；切换显示时 NGX 仍在处理。

## 配置参考

### `host64\ReShade.ini`

用户可调键位于 `[Standalone.DLSSNR]`。下表是出厂模板 `templates/host64-ReShade.ini`
的值与代码默认值：

| 键 | 模板值 | 说明 |
| --- | --- | --- |
| `NeuralRendering` | `1` | NR 开关 |
| `FrameGeneration` | `0` | 帧生成开关；需要时在面板或 Add-ons 菜单开启 |
| `Model` | `1` | NR 模型 1–3 |
| `Intensity` | `1.0` | 强度 0–1 |
| `LocalTone` | `1.0` | 色调结构 0–2 |
| `LocalStructure` | `1.0` | 结构强度 0–1 |
| `SkinStructure` | `-1.0` | 皮肤结构 -1–+1 |
| `NvidiaOpticalFlowMotion` | `0` | 光流运动（试验），默认关闭 |
| `SourceResolutionOverride` | `0` | 源分辨率覆盖，0 为关闭 |
| `AdaptivePressureGovernor` | `0` | 自适应 GPU 压力调节（原型），模板关闭 |
| `DetachedPresentation` | `1` | 使用独立输出窗 |
| `TuningWindowTitles` | 未写入 | 代码默认 `WOW-335-NR\|DLSS5`；识别调参窗口的标题子串，竖线分隔 |
| `DebugLog` | 未写入 | 设为 `1` 开启载体详细日志 |

`TuningWindowTitles` 控制哪些前台窗口会让载体保持输出可见；面板窗口类
`Dlss5PanelWindow` 始终识别。旧版硬编码标题仍作为默认值保留。

### `host64\live-params.json`

悬浮面板写入、载体读取的热参数文件。面板使用同目录临时文件 + 原子替换，载体约每 500 ms
检查修改时间：

```json
{
  "neuralRendering": true,
  "model": 1,
  "intensity": 0.35,
  "localTone": 1.00,
  "localStructure": 0.25,
  "skinStructure": -1.00,
  "frameGeneration": false,
  "fgMultiplier": 2
}
```

- 数值在载体侧按 `tiers.hpp` 边界钳制。
- `fgMultiplier` 是 UI 语义的 2/3/4，载体转换为 `DLSSG.MultiFrameCount` 1/2/3。
- 模型变化通过官方切换函数触发特性重建；仅改变量不会真正切换网络。
- 载体应用后把值写回 `ReShade.ini`，面板可用「从载体 ini 重新读取」同步显示。

### `host64\custom-tier.json`

面板保存的最近一次自定义组合，格式与 `live-params.json` 相同。点「自定义」时若存在该文件，
面板会召回其中参数。

### `dlss5-aio-x86.cfg`

32 位包装器配置，位于客户端根目录。常用键：

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `enabled` | `1` | 启用 32 位抓帧传输 |
| `mode` | `2` | 传输模式 |
| `show_processed_output` | `1` | 显示处理后输出 |
| `work_resolution` | `100` | 工作分辨率百分比 |
| `classic_d3d9_fullscreen_virtualization` | `0` | 仅控制经典 D3D9 启动时的全屏改写 |
| `debug` | `0` | 设为 `1` 输出详细日志并给载体传 `--debug` |
| `log_frames` | `5` | 日志帧间隔 |

包装器每 60 帧热重载配置，约 1 秒生效。

## 遥测与性能

### 共享内存遥测

- 名称：`Local\DLSS5_AIO_Telemetry_<host PID>`。
- 大小：320 字节；magic `0x4F494144`，版本 1。
- 写入方使用 seqlock；读取方不得阻塞或修改映射。
- 内容：NR / FG 标志、管线就绪、输出可见、输入 / 输出分辨率、NR / SR / FG / 呈现计数、
  QPC 时间戳等。
- 控制台与面板读取同一映射；分析器也可以只读打开。
- 遥测只在本机共享内存中传递，不写入网络或远程服务。

### 性能分析器

```bat
DLSS5-PerformanceAnalyzer.cmd Record -ProcessName <进程名> -Duration 180 -Output telemetry.csv
DLSS5-PerformanceAnalyzer.cmd Analyze -Telemetry telemetry.csv -Log <ReShade.log> -Output report
```

可选 PresentMon：

```bat
DLSS5-PerformanceAnalyzer.cmd Record -ProcessName <进程名> -Duration 180 -Output telemetry.csv ^
  -PresentMonExecutable C:\Tools\PresentMon.exe -PresentMonOutput presentmon.csv
```

报告目录包含 `report.md` 与 `report.json`。分析器把游戏交换链与载体代理交换链分开统计，
不会把两者平均。`Ctrl+Alt+B` 循环基准段：用户设置 → 关闭 addon → 仅 DLSS/DLAA →
NR + DLSS/DLAA → DLSS/DLAA + FG → NR + DLSS/DLAA + FG → 恢复用户设置。每段建议至少停留 30 秒。

### Python 采集工具

`performance/` 下的工具用于隔离批次，要求显式传入客户端、PresentMon 与输出路径：

- `collect.py`：有界采集游戏 / 载体时序，默认预热 30 秒、采集 60 秒、重复 3 轮。
- `analyze_presentmon.py`：按进程 / 交换链保守汇总 PresentMon CSV。
- `correlate.py`：把 PresentMon 行与独立采样的 AIO 遥测按 PID、QPC、交换链关联。
- `resource_probe.py`：读取进程内存、32 位地址空间与设备级 GPU / 显存指标。
- `telemetry_reader.py`：只读解析 320 字节遥测映射。
- `capture_output.py`：捕获游戏矩形内的桌面画面，可能包含其他可见内容；输出留在 `_scratch`，
  未经确认不要分享。

## 诊断

### 调试开关

各组件默认不打详细日志；排查时按组件打开，验收后关闭。

| 组件 | 开关 | 生效方式 |
| --- | --- | --- |
| 载体插件（addon64） | `host64\ReShade.ini` 的 `[Standalone.DLSSNR]` 加 `DebugLog=1`；或 Home → Add-ons 菜单页勾选 | 菜单切换即时生效 |
| 32 位包装器（addon32） | 客户端根 `dlss5-aio-x86.cfg` 加 `debug=1` | 每 60 帧热重载，约 1 秒 |
| 64 位载体（host64） | 包装器开启调试后自动传 `--debug` | 随包装器 |
| 游戏内悬浮面板 | 桌面控制台开启调试后启动面板自动传 `--debug` | 重启面板 |
| 桌面控制台 | exe 旁 `DLSS5控制台.ini` 加一行 `debug=1` | 重启控制台 |
| PowerShell 核心 | 环境变量 `DLSS5_CORE_DEBUG=1` | 下次安装 / 恢复操作 |

### 日志位置

| 组件 | 路径 |
| --- | --- |
| 载体插件 | `%LOCALAPPDATA%\RHI\Logs\standalone-dlssnr.log` |
| 32 位包装器 | 客户端根目录 `dlss5-aio-x86.log` |
| 64 位载体 | `host64\dlss5-feed-host.log`、`host64\ReShade.log` |
| 悬浮面板 | `host64\panel-logs\<guid>\stdout.log`、`stderr.log` |
| 桌面控制台 | exe 旁 `DLSS5控制台-debug.log` |
| PowerShell 核心 | `_scratch\dlss5-aio\logs\core-debug.log` |

日志通常在组件启动时截断；需要对比时先复制保留。

### 常见问题

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| 控制台显示「载体遥测：未确认」 | 载体未运行、PID 不匹配、映射过期 | 确认游戏与 `host64` 已启动；等数秒；查看包装器与载体日志 |
| 面板显示「显示图层：F10 处于原画模式」 | 按过 F10 | 再按 F10 切回处理画面 |
| 面板显示「输出窗被游戏窗口盖住」/「已隐藏」 | 输出窗失去前台或 z 序 | 回到游戏窗口；必要时 `F10` 恢复；检查窗口助手 |
| 补帧「求值失败」 | 驱动 / 运行库不支持，或 `nvngx_dlssg.dll` 缺失 | 更新驱动与运行时；先关闭补帧确认管线；查看载体日志 |
| 3×/4× 实际仍是 2× | 显卡不是 RTX 50 系，或运行时能力上限为 1 | 属预期自钳；查看 `DLSSG.MultiFrameCountMax` 日志 |
| 开启补帧后卡顿 / 内存上涨 | 源帧率未限速，32 位地址空间被采集队列撑爆 | 控制台设前台 30 / 后台 10；降低分辨率或关闭补帧 |
| 面板没有出现 | `dlss5-panel.exe` 不在控制台旁边；已有单实例；游戏未运行 | 把面板放在控制台同目录；关闭残留面板；确认游戏进程 |
| `F11` 无反应 | 热键被其他程序占用 | 面板会改用轮询；查看 `stdout.log`；关闭冲突热键 |
| 启动游戏报错 / 黑屏 | ReShade 或运行库身份不符；`d3d9.dll` 替换异常 | 用控制中心恢复原始 `d3d9.dll`；核对锁与日志 |
| 全屏独占异常 | 独占全屏与独立输出窗不兼容 | 使用无边框窗口；模板的 `classic_d3d9_fullscreen_virtualization` 只管启动时改写 |
| 面板 / 助手「拒绝执行」 | 路径不是绝对 `wow.exe`、哈希不符、路径含链接 | 选择正确的客户端根目录；不要用快捷方式或链接路径 |

## 已知限制

- **32 位地址空间**：开启帧生成后必须限制源帧率，否则采集队列可能耗尽内存。
- **MFG 3×/4×**：仅 RTX 50 系可稳定使用；其他卡由运行时能力上限自动回落 2×。
- **独占全屏**：与独立输出窗不兼容；运行时的独占全屏请求会被转换为无边框窗口。
- **模型差异**：模型 2 / 3 的视觉差异尚未完成系统实测；切换后约 1 秒重建。
- **光流 / VORT**：均为试验特性，默认关闭；开启可能增加开销或引入运动伪影。
- **控制中心**：只面向隔离客户端，界面与事务尚未完成真实游戏交互验收。
- **安装验收**：主客户端部署、30 分钟稳定性、完整安装 / 卸载流程尚未完成。
- **运行时分发**：NVIDIA DLL 与 ReShade 不在仓库内；缺失时管线无法启动或帧生成不可用。
- **在线环境**：不绕过反作弊；是否允许使用取决于具体服务条款。

## 开发与维护

- git 身份 `MuyuQ`，主分支 `main`；版本标签使用 `v*`，不覆盖旧标签。
- 每次改动在工作区账本 `doc/记录/修改详细记录.md` 登记；本仓库提交信息与之对应。
- 设计、执行依据与验收记录在工作区 `doc/维护/`、`doc/记录/` 的 DLSS5-AIO 系列文档。
- 回归测试位于工作区 `tools/tests/`：`test_dlss5_core.ps1`、`test_dlss5_stage.py`、
  `test_dlss5_performance.py`。它们与工作区布局耦合，不随本仓库分发。
- 修改载体后按「构建 → 部署 → 回读」验证哈希，再更新 `payload-lock.json`；
  修改面板 / 窗口助手后同步更新对应锁。
- 不把 NVIDIA 运行时、日志、截图、`_scratch` 构建产物提交入库。
- 构建产物、运行时状态与用户配置由 `.gitignore` 排除。

### 载体血统

载体 fork 自上游 v2.2.4（commit `5947a3d`），在其上按分支演进。本仓库以初始导入收编
工作树现状；下表保留旧载体仓库的提交哈希以便追溯（历史保存在原工作区
`tools/dlss5/carrier/.git`）：

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
- 第三方组件、SDK 头文件与运行时分别受各自许可约束；分发任何二进制前请先核对
  `THIRD_PARTY_NOTICES.md` 与组件目录内的许可文件。

## 参考项目

| 项目 | 在本项目中的角色 | 许可 |
| --- | --- | --- |
| [kibblerz/DLSS5-Reshade-AIO](https://github.com/kibblerz/DLSS5-Reshade-AIO) | 载体基线 v2.2.4；NR / SR / FG 管线、x86 传输与上游文档 | Apache-2.0 |
| [jlrouzies-fr/DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) | 32 位跨进程抓帧与 64 位载体的来源；本仓库 vendored 快照 `75754b2` | MIT |
| [crosire/ReShade](https://github.com/crosire/ReShade) | 32 位 / 64 位 addon 宿主与效果运行时 | BSD-3-Clause |
| [ocornut/imgui](https://github.com/ocornut/imgui) | 控制台、悬浮面板与载体菜单的 UI；本仓库 vendored 1.92.5 | MIT |
| [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook) | 32 位包装器的 API 钩子 | BSD-2-Clause |
| [NIGos/dlss5-dx11-bridge](https://github.com/NIGos/dlss5-dx11-bridge) | DLSS5-Feeder 中 D3D11 桥接的来源 | MIT |
| [clshortfuse/renodx](https://github.com/clshortfuse/renodx) | DLSS5-Feeder 快照收录的 RenoDX 组件 | MIT |
| [microsoft/Detours](https://github.com/microsoft/Detours) | DLSS5-Feeder 快照收录的 Detours 组件 | MIT |
| [NVIDIA NGX / DLSS](https://developer.nvidia.com/rtx/ngx) | NR / SR / FG 运行时与 SDK 头文件 | NVIDIA 专有许可 |
| [NVIDIA Optical Flow SDK](https://developer.nvidia.com/opticalflow-sdk) | 光流运动提供者的头文件与驱动 API | NVIDIA SDK 许可 |
| [Intel PresentMon](https://github.com/GameTechDev/PresentMon) | 可选性能采集与交换链分析 | 见项目页面 |

此外，DLSS5-Feeder 快照还收录了 Deep Fried Chicken 的互操作文档与许可文件；
本项目不依赖该二进制，也不分发它。VORT Motion 是可选第三方着色器，不随本仓库分发。
