# 回归检查

审查整改的针对性检查：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/Test-ReviewRegression.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/Test-CoreRecordSafety.ps1
```

前者编译并测试实际管道 I/O、控制中心应用回调与旧桌面控制台的配置读写，再编译实际 x86 桥接插件。覆盖管道分段读取、读写超时、取消后的下一次读取、对端退出、token 双向请求与过期结果、NR 关闭时的输入帧率、关闭窗口等待当前任务并应用结果、取消排队任务，以及不依赖调整窗口尺寸的 D2D 绘制目标恢复。测试仅创建隐藏窗口和自身的短时子进程，不启动游戏或私有运行库。后者检查 PS 异常记录、未授权残留、个人数据路径、类型不符与目录链接均不会扩大删除范围，失败时保留记录。两者输出仅在工作区 `_scratch/dlss5-aio/`。

`Build-ControlCenter.ps1` 另负责最终产物的四个内置方案复制与哈希检查；编译与隔离检查不代替真实游戏、驱动挂起和干净系统验收。

原生控制中心核心检查在维护仓库运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/Test-ControlCenter.ps1
```

该入口需要 VS 2022 C++ 工具，在 `_scratch/dlss5-aio/` 中新建隔离目录，不接触游戏客户端。检查包括成员路径、文件身份、目录联接、客户端与进程、锁、复制和文件替换失败、严格 JSON、清单与组件锁、事务发现、恢复调度、安装作业及补装修复来源；也检查记录发布的 15 个模拟中断位置、提交的 12 个模拟中断位置，以及记录发布三处、提交四处和普通成员事务六处实际进程终止后的恢复。修复检查包括旧记录预检、当前事务匹配、配置保留、用户修改备份、计划身份绑定、扫描与恢复调度、单成员和双成员修复作业，以及 15 个成员写入模拟中断、5 个提交模拟中断和 6 处实际进程终止。双成员检查覆盖第二项中断、来源消失、提交后清理重试和隔离原件变化。卸载检查包括预检、计划、日志、成员写入及回滚、最终提交准备，覆盖八个写入、四个回滚及四个最终准备模拟中断，九处实际子进程终止、用户修改冲突、多成员回滚及未完成事务协调。卸载最终提交与完整作业已完成并通过隔离检查；阶段二工具检查覆盖运行遥测（伪共享内存与进程关联）、GPU 统计、AIO 管线开关、面板进程与日志尾读、鼠标助手、诊断导出、移入清单、恢复安装前文件与生产编译。安装文件预览本身不验证客户端身份和游戏进程。断电尚未验证；检查通过不代表干净系统与真实游戏验收已经完成。

在工作区 `src/WOW-335-NR` 布局下运行。原生检查在仓库内执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/Test-Native.ps1
```

该入口编译并运行两个隔离程序，不启动游戏或面板窗口。它检查配置读取失败、短写和替换失败时保留原件，成功保存后保留备份；拒绝非法热参数；用三种像素格式和非对齐宽度核对 GPU 纹理读回；直接调用面板源码检查 INI、自定义档和热参数中的补帧倍数，以及文件锁释放后无需再次操作的保存重试。需要 MSVC、Windows SDK 和可创建 D3D12 设备的环境。可用时启用 D3D12 调试层，缺失时会明确报告。

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
