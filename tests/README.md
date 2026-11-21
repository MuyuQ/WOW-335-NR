# 回归检查

审查整改的针对性检查：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/Test-ReviewRegression.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/Test-CoreRecordSafety.ps1
python -X utf8 -B tests/test_stage_override.py
```

前者编译并测试实际管道 I/O、控制中心应用回调与旧桌面控制台的配置读写，再编译实际 x86 桥接插件。覆盖管道分段读取、读写超时、取消后的下一次读取、对端退出、token 双向请求与过期结果、NR 关闭时的输入帧率、关闭窗口等待当前任务并应用结果、取消排队任务，以及不依赖调整窗口尺寸的 D2D 绘制目标恢复。测试仅创建隐藏窗口和自身的短时子进程，不启动游戏或私有运行库。后者检查 PS 异常记录、未授权残留、个人数据路径、类型不符与目录链接均不会扩大删除范围，失败时保留记录。两者输出仅在工作区 `_scratch/dlss5-aio/`。

`Build-ControlCenter.ps1` 另负责最终产物的四个内置方案复制与哈希检查；编译与隔离检查不代替真实游戏、驱动挂起和干净系统验收。

发布打包检查（工作区 `src/WOW-335-NR` 布局下）：

```powershell
pwsh -NoProfile -File tests/Test-ReleasePackaging.ps1
```

该入口按 `panel-lock.json` 从 `_scratch/dlss5-aio/panel-build-*` 选出哈希匹配的面板候选并实际执行严格构建，核对包内 `bin/dlss5-panel.exe` 与 `build.json` 的 `panel_candidate` 及锁校验字段；再验证旧部署面板作为候选时严格构建必须失败、不创建输出目录、不改写 `tools/dlss5/bin/`，并覆盖互斥参数与缺候选的快速失败。可用 `-CandidatePath` 指定候选、`-IncludeUnverifiedContrast` 追加 `-AllowUnverified` 旧行为对照（仅对照，不作为验收包）。

原生控制中心核心检查在维护仓库运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/Test-ControlCenter.ps1
```

该入口需要 VS 2022 C++ 工具，在 `_scratch/dlss5-aio/` 中新建隔离目录，不接触游戏客户端。检查包括成员路径、文件身份、目录联接、客户端与进程、锁、复制和文件替换失败、严格 JSON、清单与组件锁、事务发现、恢复调度、安装作业及补装修复来源；也检查记录发布的 15 个模拟中断位置、提交的 12 个模拟中断位置，以及记录发布三处、提交四处和普通成员事务六处实际进程终止后的恢复。修复检查包括旧记录预检、当前事务匹配、配置保留、用户修改备份、计划身份绑定、扫描与恢复调度、单成员和双成员修复作业，以及 15 个成员写入模拟中断、5 个提交模拟中断和 6 处实际进程终止。双成员检查覆盖第二项中断、来源消失、提交后清理重试和隔离原件变化。卸载检查包括预检、计划、日志、成员写入及回滚、最终提交准备，覆盖八个写入、四个回滚及四个最终准备模拟中断，九处实际子进程终止、用户修改冲突、多成员回滚及未完成事务协调。另检查重复安装时保留已登记的个性化 ReShade.ini（PreservedConfig）：现场重装报告无需写入且不改写 INI／记录／备份，混合安装只补缺失组件，第三方同名 INI 与损坏源模板仍分别报目标冲突和源校验失败，卸载按原记录保存用户改动。卸载最终提交与完整作业已完成并通过隔离检查；阶段二工具检查覆盖运行遥测（伪共享内存与进程关联）、GPU 统计、AIO 管线开关、面板进程与日志尾读、鼠标助手、诊断导出、移入清单、恢复安装前文件与生产编译。安装文件预览本身不验证客户端身份和游戏进程。面板进程检查覆盖控制中心写入的实例归属标记、隐藏/等待态旧实例的安全接管、无归属标记实例的识别，以及「重新打开面板」入口的接管路径；并覆盖面板窗口带 AIO owner 时的可见性识别（不因 owner 过滤而误判）、普通打开复用同一 PID、显式重新打开时优雅关闭旧实例并落盘待保存参数、退出超时后的有界终止、其它客户端/无法核验归属/创建时间不符（PID 复用）/另一目录同名进程均保持存活且不被接管。游戏状态区分运行、已退出与无法判断：只要有 wow.exe 的映像路径不可读即按无法判断处理并保留原面板；旧版本归属标记缺少创建时间时不自动交接，显式重开也拒绝终止。图层按钮回归从生产脚本抽取 `Get-AioHostId` 与 `PanelLayerNative`，用两个假载具夹具验证选中 A 时只向 A 的宿主发送一次显示状态请求、B 收不到消息，且只运行 B 时选 A 不会解析到任何宿主。安装记录归属用跨客户端夹具覆盖：把另一客户端的有效记录复制进来后，生产入口必须报告“记录属于其他客户端”、保留记录与组件文件且不判无需写入。面板回归另覆盖 F10 胶囊命中矩形（100%/150% 缩放下中心、边缘与相邻拖动区）与向 AIO 输出窗发送一次明确显示状态请求（WM_APP+0x5A，受控接收窗口断言只发一次且 wparam 正确）。断电尚未验证；检查通过不代表干净系统与真实游戏验收已经完成。

在工作区 `src/WOW-335-NR` 布局下运行。原生检查在仓库内执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/Test-Native.ps1
```

该入口编译并运行两个隔离程序，不启动游戏或面板窗口。它检查配置读取失败、短写和替换失败时保留原件，成功保存后保留备份；拒绝非法热参数；用三种像素格式和非对齐宽度核对 GPU 纹理读回；直接调用面板源码检查 INI、自定义档和热参数中的补帧倍数，以及文件锁释放后无需再次操作的保存重试；并检查 F10 胶囊命中区域与「处理画面」按钮只向 AIO 输出窗发送一次目标显示状态请求（不注入模拟按键）。需要 MSVC、Windows SDK 和可创建 D3D12 设备的环境。可用时启用 D3D12 调试层，缺失时会明确报告。

Addon 关闭生命周期检查在维护仓库运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/Test-Shutdown.ps1
```

该入口编译实际 addon 源码，通过最小 ReShade API 替身检查 13 组关闭场景。覆盖五次 DLL 装卸、重复关闭、拥有回调的计数、窗口类注销、排队任务与线程超时后重试、实际 D3D12 双队列与 D3D11 WARP 查询、并发关闭、feature 释放失败、挂起线程和 GPU 下的进程退出，以及原生 hook 跳板和外部回调代码保留。私有运行库不加载，feature 释放错误由测试注入。需要 MSVC、Windows SDK 和 D3D12；日志与构建产物仅写入工作区 `_scratch/dlss5-aio/shutdown-test-*`。调用约定和保留 DLL 的条件见 [关闭说明](../carrier/addon/SHUTDOWN.md)。

核心、性能和暂存检查位于工作区 `tools/tests`，入口已固定加载本维护仓库。在工作区根执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/tests/test_dlss5_core.ps1
python -X utf8 -B tools/tests/test_dlss5_performance.py
python -X utf8 -B tools/tests/test_dlss5_stage.py
```

核心检查包含补装、补装失败回退、补装事务恢复、后加可选运行库、完整卸载和缺失修复来源。性能检查区分完整时间跨度与有效帧间隔覆盖。测试使用合成文件、ZIP 和遥测，不加载 NVIDIA 私有运行库；全部输出保留在工作区 `_scratch/dlss5-aio/`。

这些检查不替代真实游戏中的 PNG 截图、模型切换、补帧呈现、设备移除和长期稳定性验收。

暂存桥接覆盖检查验证 `--addon32` 只接收锁匹配的本地构建，并拒绝旧压缩包插件和错误身份的本地文件。
