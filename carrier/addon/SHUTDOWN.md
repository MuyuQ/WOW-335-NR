# Addon 关闭与模块所有权

本说明对应 2026-11-13 的 R01 修复。关闭入口先停止接收新工作，等待已接收的工作、线程和 GPU 完成，再释放资源。遇到超时或释放失败，保留仍可能被使用的资源和模块，允许调用方重试。

## 调用顺序

ReShade 6.8.0 在加载 addon 后调用 `bool AddonInit(HMODULE addon, HMODULE reshade)`，在卸载前调用 `void AddonUninit(HMODULE addon, HMODULE reshade)`。配置读取、注册事件和日志初始化已移到 `AddonInit`；`DllMain` 不再承担正常初始化与关闭工作。

载体正常退出时，取得自己的 addon 加载引用，然后调用：

1. `DWORD WINAPI NRShutdown(DWORD timeout_ms)`，关闭渲染资源。
2. 返回 `ERROR_SUCCESS` 后调用 `AddonUninit`，注销 ReShade 事件与 overlay。
3. 两个调用均返回后，释放调用方自己的加载引用。

调用方必须在整个调用期间持有加载引用，并在销毁宿主设备和队列前发起关闭。不要从 `DllMain`、渲染工作线程或普通 addon 回调中调用这两个入口。生产代码中的主设备销毁回调使用内部入口，明确排除自己持有的一个回调计数；嵌套回调仍会使关闭超时并保留资源。

addon 在初始化成功后另持有自身和 ReShade 的加载引用。排队任务在提交前登记计数并取得独立模块引用；任务通过 `FreeLibraryWhenCallbackReturns` 延迟释放该引用，覆盖回调返回时的函数尾部代码。

## 关闭顺序与失败行为

关闭请求使用同一把锁串行执行；其他并发关闭返回 `ERROR_BUSY`。关闭接收入口与登记工作共用一把锁，已排队但尚未执行的任务也计入待完成工作。

1. 关闭接收入口，设置停止和唤醒事件，等待已有 ReShade 回调及排队任务返回。
2. 再次发出停止信号，等待神经渲染调度线程、呈现线程和输出窗口线程退出。检查每次等待的结果；注销输出窗口类，移除保存的窗口过程地址。
3. 在去重后的 D3D12 主队列、计算队列和补帧队列提交 fence 标记；D3D11/D3D9 采用事件查询，Vulkan 采用共享 fence 标记。等待这些工作及 NVOF 消费完成。
4. 恢复窗口 subclass，禁用原生 hook；释放 NGX feature 和 NVOF 会话，再关闭句柄并释放持有的渲染资源。

所有主动等待共用本次调用的 `timeout_ms`。这限制的是代码主动等待的时长；驱动、COM 或私有运行库函数本身可能阻塞，不能据此承诺整个函数硬实时返回。默认宿主关闭和 `AddonUninit` 各使用 5000ms 等待预算。

| 返回值 | 含义与处理 |
| --- | --- |
| `ERROR_SUCCESS` | 资源关闭已完成；重复调用仍成功。是否允许映像卸载，还取决于外部回调地址。 |
| `ERROR_TIMEOUT` | 回调、线程或 GPU 未完成，或 GPU 完成检查失败。记录失败阶段并保留资源；解决依赖后可重试。 |
| `ERROR_BUSY` | 另一个关闭请求持有关闭锁。 |
| `ERROR_INVALID_STATE` | 原生 hook 禁用或 NGX feature 释放失败。保留剩余资源；已成功释放的 feature 不会重复释放。 |
| 其他 Win32 错误 | 线程等待或窗口类注销失败，日志给出阶段与错误码。 |

超时后不强制终止生产线程，不关闭仍使用中的线程句柄或停止事件，也不把局部资源释放当作完整关闭。关闭接收入口后不重新开放；重试用于完成关闭，不用于恢复当前会话。

## 外部回调与进程退出

原生 hook 的调用方可能已进入跳板，其他 subclass 也可能保存了本 addon 的窗口过程地址。私有运行库接收过 `Log.Callback` 后，当前接口无法证明其所有回调已返回。因此，这些会话关闭资源后仍保留 addon、ReShade、日志锁和 hook 跳板到进程退出。只禁用 hook，不在关闭时删除跳板。

这意味着资源关闭成功不一定导致 DLL 映像立即卸载。同一进程内不能重新初始化已关闭的 addon；需要重新启用时重启载体进程。没有这些外部回调地址的隔离会话可在注销后正常卸载，测试覆盖了连续五次装卸。

进程终止时，`DllMain` 不等待线程、不删除 hook、不执行 COM `Release` 或驱动清理。它只放弃全局 COM 引用的自动释放，防止 CRT 后续析构在 loader lock 内进入驱动；由操作系统回收该进程。未完成显式关闭时保留启动恢复标记，供下次诊断。动态卸载不依赖 `DllMain` 补救。

## 验证与适用范围

在维护仓库运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/Test-Shutdown.ps1
```

需要 MSVC、Windows SDK 和可创建 D3D12 设备的环境。入口编译实际 addon 源码与 NVOF 源码，使用最小 ReShade 注册/配置/日志替身；不加载私有 NVIDIA DLL，不启动游戏。D3D12 队列、fence、D3D11 WARP 查询、Windows 线程和 DLL 装卸均为实际调用。

测试覆盖正常及重复关闭、拥有关闭回调的计数、窗口类注销、排队任务与工作线程超时后重试、两条 GPU 队列未完成、并发关闭、D3D11 查询、feature 释放失败、挂起线程和 GPU 下的进程退出，以及两类原生 hook 和私有回调的代码保留。所有输出和正常 addon 日志都写入工作区 `_scratch/dlss5-aio/shutdown-test-*`。

真实 ReShade、WoW、NVIDIA 私有运行库、D3D9/Vulkan 运行时关闭、设备移除和游戏内频繁切换尚未验收。这些场景不能由最小宿主替代，仍属于 R02。维护源和本地构建完成不表示客户端已部署。

## 生命周期依据

- [ReShade 6.8.0 addon_manager.cpp](https://github.com/crosire/reshade/blob/v6.8.0/source/addon_manager.cpp)：`AddonInit` / `AddonUninit` 与加载引用的调用顺序。
- [Dynamic-Link Library Best Practices](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices)：loader lock 内初始化和同步的限制。
- [FreeLibraryWhenCallbackReturns](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-freelibrarywhencallbackreturns)：线程池回调返回后的模块引用释放。
