// Test-only exports exercise the production shutdown and loader entry points.
// No private runtime is loaded: the host supplies a minimal ReShade API double.
#include "../carrier/addon/src/nr-standalone.cpp"
struct TestWork { HANDLE entered, release; };
static DWORD WINAPI TestBlockedWork(void *parameter)
{
    auto *work = static_cast<TestWork *>(parameter);
    SetEvent(work->entered);
    WaitForSingleObject(work->release, INFINITE);
    return 0;
}
extern "C" __declspec(dllexport) BOOL NRTestQueue(TestWork *work)
{
    return QueueShutdownWork(TestBlockedWork, work, WT_EXECUTEDEFAULT);
}
extern "C" __declspec(dllexport) BOOL NRTestWorker(TestWork *work)
{
    g_neural_dispatch_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_neural_dispatch_thread = CreateThread(nullptr, 0, TestBlockedWork, work, 0, nullptr);
    return g_neural_dispatch_stop_event && g_neural_dispatch_thread;
}
// Test-owned external dependency. Release it explicitly outside FreeLibrary;
// an automatic COM destructor here would itself run under the loader lock.
static ID3D12Fence *g_test_gpu_gate;
extern "C" __declspec(dllexport) BOOL NRTestGpu()
{
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_neural_device)))) return FALSE;
    D3D12_COMMAND_QUEUE_DESC desc = {};
    if (FAILED(g_neural_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_command_queue))) ||
        FAILED(g_neural_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_async_fg_queue))) ||
        FAILED(g_neural_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_test_gpu_gate)))) return FALSE;
    D3D12_RESOURCE_DESC resource = {};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; resource.Width = 512;
    resource.Height = resource.DepthOrArraySize = resource.MipLevels = 1;
    resource.SampleDesc.Count = 1; resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(g_neural_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_screenshot_readback)))) return FALSE;
    return SUCCEEDED(g_command_queue->Wait(g_test_gpu_gate, 1)) &&
        SUCCEEDED(g_async_fg_queue->Wait(g_test_gpu_gate, 1));
}
extern "C" __declspec(dllexport) BOOL NRTestSignalGpu()
{
    return g_test_gpu_gate && SUCCEEDED(g_test_gpu_gate->Signal(1));
}
extern "C" __declspec(dllexport) DWORD NRTestOwnedClose()
{
    nr_shutdown::Lease admission(g_admission);
    if (!admission) return ERROR_BUSY;
    return ShutdownResources(1000, 1);
}
extern "C" __declspec(dllexport) BOOL NRTestLegacy()
{
    D3D_FEATURE_LEVEL level;
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &g_legacy_device11, &level, &g_legacy_context11));
}
static bool g_test_release_allowed;
static NVSDK_NGX_Result NVSDK_CONV TestFeatureRelease(NVSDK_NGX_Handle *)
{
    return static_cast<NVSDK_NGX_Result>(g_test_release_allowed ? 1u : 0xBAD00001u);
}
static NVSDK_NGX_Result NVSDK_CONV TestBridgeRelease(NgxReleaseFeature release, NVSDK_NGX_Handle *feature)
{
    return release(feature);
}
extern "C" __declspec(dllexport) void NRTestFeatureFailure(BOOL allowed)
{
    static unsigned fake_feature;
    g_test_release_allowed = allowed != FALSE;
    g_nr_feature = reinterpret_cast<NVSDK_NGX_Handle *>(&fake_feature);
    g_nr_release = TestFeatureRelease;
    g_bridge_release = TestBridgeRelease;
}
extern "C" __declspec(dllexport) unsigned NRTestState()
{
    if (g_shutdown_complete && g_test_gpu_gate) { g_test_gpu_gate->Release(); g_test_gpu_gate = nullptr; }
    DWORD flags = 0;
    const bool worker_alive = g_neural_dispatch_thread && GetHandleInformation(g_neural_dispatch_thread, &flags);
    const bool event_alive = g_neural_dispatch_stop_event && GetHandleInformation(g_neural_dispatch_stop_event, &flags);
    return (g_admission.Active() ? 1u : 0u) | ((g_screenshot_readback || g_legacy_device11) ? 2u : 0u) |
        (worker_alive ? 4u : 0u) | (event_alive ? 8u : 0u) | (g_shutdown_complete ? 16u : 0u);
}
static volatile int g_test_native_bias = 35;
static __declspec(noinline) int TestNativeTarget(int value) { return value + g_test_native_bias; }
static __declspec(noinline) int TestNativeDetour(int) { return 99; }
extern "C" __declspec(dllexport) BOOL NRTestHook()
{
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return FALSE;
    g_vk_create_device_target = reinterpret_cast<void *>(TestNativeTarget);
    return MH_CreateHook(g_vk_create_device_target, reinterpret_cast<void *>(TestNativeDetour),
               reinterpret_cast<void **>(&g_vk_create_device_orig)) == MH_OK &&
        MH_EnableHook(g_vk_create_device_target) == MH_OK;
}
extern "C" __declspec(dllexport) int NRTestCallHook() { return TestNativeTarget(7); }
extern "C" __declspec(dllexport) BOOL NRTestWindowHook()
{
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return FALSE;
    g_get_client_rect_target = reinterpret_cast<void *>(TestNativeTarget);
    if (MH_CreateHook(g_get_client_rect_target, reinterpret_cast<void *>(TestNativeDetour),
            reinterpret_cast<void **>(&g_original_get_client_rect)) != MH_OK ||
        MH_EnableHook(g_get_client_rect_target) != MH_OK) return FALSE;
    g_window_query_hooks_installed = true;
    return TRUE;
}
extern "C" __declspec(dllexport) int NRTestTrampoline()
{
    return g_original_get_client_rect ? reinterpret_cast<int (*)(int)>(g_original_get_client_rect)(7) : -1;
}
extern "C" __declspec(dllexport) BOOL NRTestWindowClass()
{
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.hInstance = g_self; wc.lpfnWndProc = ProxyWindowProc;
    wc.lpszClassName = L"StandaloneDLSSNRNativeOutput";
    return RegisterClassExW(&wc) != 0;
}
extern "C" __declspec(dllexport) void NRTestExternalCallback()
{
    g_runtime_log_callback_registered = true;
}
