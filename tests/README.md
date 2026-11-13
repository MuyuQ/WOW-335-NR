# 回归检查

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
