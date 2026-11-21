// Compile the panel's actual persistence functions. The renamed application
// entry point is never called: this test opens no window or game process.
#define wmain NrPanelApplicationMain
#include "../native/panel.cpp"
#undef wmain
#include <stdexcept>
static void Check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
static bool Near(float a, float b) { return fabsf(a - b) < 0.01f; }
static bool ReadParamsFile(const fs::path &path, nr_params::Values &values) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char text[512]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, text, sizeof text - 1, &read, nullptr);
    CloseHandle(file);
    return ok && nr_params::Parse(std::string_view(text, read), values);
}

// R9：帧率差分必须识别宿主重启后的计数回退，不得把无符号回绕当成帧率；
// 宿主 pid 变化时基线全清；同页源帧率以遥测 SourceFps 为准、缺失时回退差分。
static void TestFpsBaselineLogic() {
    // 正常差分：1 MHz QPC 下 0.5s 内 +100 帧 → 200 fps
    FpsDelta delta = ComputeFpsDelta(1100, 1000, 2000000, 1500000, 1000000, 0.25);
    Check(delta.sampled && !delta.reset && Near(delta.fps, 200.0f), "normal FPS delta wrong");

    // 宿主重启：计数器从 0 重来。旧实现直接做无符号相减，这里会回绕成 ~8.4e18。
    const unsigned long long last = 10000000000000000000ULL;
    const unsigned long long current = 1000ULL;
    Check(current - last > 1000000000000000000ULL, "underflow fixture no longer reproduces old bug");
    delta = ComputeFpsDelta(current, last, 2000000, 1500000, 1000000, 0.25);
    Check(delta.reset && !delta.sampled && delta.fps == 0.0f, "counter regression must reset, not wrap");

    // 时间窗不足：不发布（调用方仍负责推进基线）
    delta = ComputeFpsDelta(1100, 1000, 1100000, 1000000, 1000000, 0.25);
    Check(!delta.sampled && !delta.reset, "short interval should not publish FPS");

    // 无效/倒退的时钟与频率不得产生样本
    delta = ComputeFpsDelta(1100, 1000, 100, 100, 0, 0.25);
    Check(!delta.sampled && !delta.reset, "zero frequency must not sample");
    delta = ComputeFpsDelta(1100, 1000, 900000, 1000000, 1000000, 0.25);
    Check(!delta.sampled && !delta.reset, "backwards clock must not sample");

    // 宿主 pid 变化：差分基线连同 FPS 平滑值一起清零
    g_last_src_frames = 12345;
    g_last_src_qpc = 67890;
    g_last_presented = 24680;
    g_last_presented_qpc = 13579;
    g_src_fps = 12.5f;
    g_game_fps = 44.5f;
    ResetFpsBaselines();
    Check(g_last_src_frames == 0 && g_last_src_qpc == 0 && g_last_presented == 0 &&
          g_last_presented_qpc == 0 && g_src_fps == 0.0f && g_game_fps == 0.0f,
          "host restart did not clear FPS baselines");

    // 同页口径：遥测 SourceFps 优先；缺失或为 0 时回退 SR 差分
    g_tele = Telemetry{};
    g_src_fps = 57.5f;
    Check(Near(SourceFps(), 57.5f), "missing telemetry should fall back to delta FPS");
    g_tele.available = true;
    g_tele.source_fps = 60;
    Check(Near(SourceFps(), 60.0f), "telemetry SourceFps must win");
    g_tele.source_fps = 0;
    Check(Near(SourceFps(), 57.5f), "zero telemetry SourceFps should fall back");
    g_tele = Telemetry{};
    g_src_fps = 0.0f;
}

// R8：pending_save 层的强制落盘 —— 不等重试间隔、写完关闭通道、失败保留 pending。
static void TestForcedFlushPendingLayer(const fs::path &dir) {
    const fs::path path = dir / L"flush-probe.json";
    Params probe;
    probe.intensity = .66f;
    int calls = 0;

    nr_files::PendingSave pending;
    Check(pending.Request() && pending.dirty && !pending.closed, "request not queued");
    Check(pending.Flush([&] { ++calls; return WriteParamsAtomic(path, probe); }), "forced flush failed");
    Check(calls == 1 && !pending.dirty && !pending.failed && pending.closed, "flush did not close channel");
    nr_params::Values values;
    Check(ReadParamsFile(path, values) && fabsf(values.intensity - .66f) < .005f,
          "forced flush did not write file");

    // flush 后不再接受新 pending，定时 Try 也不再写盘
    Check(!pending.Request() && !pending.dirty, "closed channel still accepts pending");
    Check(!pending.Try([&] { ++calls; return true; }) && calls == 1, "closed channel still retries");
    Check(pending.Flush([&] { ++calls; return true; }) && calls == 1, "flush is not idempotent");

    // 没有 pending 时 flush 不写盘
    nr_files::PendingSave clean;
    Check(clean.Flush([&] { ++calls; return true; }) && calls == 1 && clean.closed, "empty flush wrote");

    // 写失败：返回 false、保留 pending、通道仍关闭（日志由 panel 层输出）
    nr_files::PendingSave failing;
    failing.Request();
    Check(!failing.Flush([] { return false; }) && failing.dirty && failing.failed && failing.closed,
          "failed flush lost pending state");

    // 注销被取消后可恢复保存通道
    failing.Reopen();
    Check(failing.Request() && failing.dirty, "reopen did not restore requests");
}

// R8：面板层强制落盘 —— 真实写 live-params.json + custom-tier.json，失败保留 pending。
static void TestForcedFlushPanel(const fs::path &dir) {
    ParamsPath = (dir / L"live-params.json").wstring();
    CustomPath = (dir / L"custom-tier.json").wstring();
    g_params = Params{};
    g_params.intensity = .42f;
    g_params.fg = true;
    g_params.fg_multiplier = 3;
    g_save = nr_files::PendingSave{};
    g_save.Request();
    Check(FlushPendingParams(L"test"), "panel forced flush failed");
    Check(g_save.closed && !g_save.dirty && !g_save.failed, "panel flush left channel open");
    Check(!g_save.Request() && !g_save.dirty, "panel accepted pending after exit flush");

    nr_params::Values values;
    Check(ReadParamsFile(ParamsPath, values) && fabsf(values.intensity - .42f) < .005f &&
          values.multiplier == 3, "panel flush wrote wrong live-params.json");
    Check(ReadParamsFile(CustomPath, values) && fabsf(values.intensity - .42f) < .005f,
          "panel flush did not write custom-tier.json");
    Check(FlushPendingParams(L"test-again") && g_save.closed, "panel flush not idempotent");

    // 写失败路径：锁住 live-params.json → flush 返回 false、保留 pending、文件不被破坏
    HANDLE busy = CreateFileW(ParamsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(busy != INVALID_HANDLE_VALUE, "flush failure lock setup failed");
    g_save = nr_files::PendingSave{};
    g_params.intensity = .55f;
    g_save.Request();
    Check(!FlushPendingParams(L"test-fail"), "flush reported success while file locked");
    Check(g_save.dirty && g_save.failed && g_save.closed, "failed panel flush lost pending state");
    CloseHandle(busy);
    Check(ReadParamsFile(ParamsPath, values) && fabsf(values.intensity - .42f) < .005f,
          "failed flush corrupted live-params.json");

    // 定时落盘路径（主循环节流）成功时清 dirty，失败时保留 pending 供 1s 重试
    g_save = nr_files::PendingSave{};
    g_params.intensity = .37f;
    g_save.Request();
    DWORD last_flush = 0;
    SavePendingIfDue(last_flush, 200);
    Check(!g_save.dirty && !g_save.failed && !g_save.closed && last_flush == 200,
          "timer save did not persist pending parameters");
    Check(ReadParamsFile(ParamsPath, values) && fabsf(values.intensity - .37f) < .005f,
          "timer save wrote wrong value");
    g_save = nr_files::PendingSave{};
}
// zh: F10 胶囊的命中矩形必须与绘制坐标同源（客户区坐标），且按钮只向载具的
// AIO 主输出窗发送一次明确的“显示处理画面/原画”请求，不注入模拟按键。
static int g_probe_messages = 0;
static WPARAM g_probe_wparam = 0;
static LRESULT CALLBACK ProbeOutputProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_APP + 0x5A) {
        ++g_probe_messages;
        g_probe_wparam = wparam;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
static void PumpProbeMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void TestF10HitRectAndCarrierMessage() {
    // 100% 缩放：中心、边缘在内，外扩 1px 在外。
    SetF10HitRect(100.0f, 20.0f, 80.0f, 23.0f);
    Check(PointInF10Chip(140, 31), "f10 hit center");
    Check(PointInF10Chip(100, 20) && PointInF10Chip(180, 43), "f10 hit top-left/bottom-right edge");
    Check(!PointInF10Chip(99, 20) && !PointInF10Chip(181, 20) && !PointInF10Chip(140, 19) &&
              !PointInF10Chip(140, 44),
          "f10 hit outside must be caption");
    // 150% 缩放：同一套客户区坐标，不做二次缩放。
    SetF10HitRect(150.0f, 30.0f, 120.0f, 34.0f);
    Check(PointInF10Chip(210, 47) && PointInF10Chip(150, 30) && PointInF10Chip(270, 64),
          "f10 hit at 150% scale");
    Check(!PointInF10Chip(149, 30) && !PointInF10Chip(271, 64) && !PointInF10Chip(210, 65),
          "f10 hit outside at 150% scale");
    ClearF10HitRect();
    Check(!PointInF10Chip(160, 40), "cleared f10 hit rect still active");

    WNDCLASSW cls{};
    cls.lpfnWndProc = ProbeOutputProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"StandaloneDLSSNRNativeOutput";
    const ATOM atom = RegisterClassW(&cls);
    Check(atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register probe output class");
    HWND main_output = CreateWindowExW(0, cls.lpszClassName, L"probe-main-output", WS_POPUP, 0, 0, 64, 64,
                                       nullptr, nullptr, cls.hInstance, nullptr);
    HWND preview = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, cls.lpszClassName, L"probe-preview",
                                   WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, cls.hInstance, nullptr);
    Check(main_output && preview, "create probe output windows");
    Check(FindAioMainOutput(GetCurrentProcessId()) == main_output,
          "FindAioMainOutput must skip the NOREDIRECTIONBITMAP preview window");

    const DWORD saved_pid = g_last_host_pid;
    const Telemetry saved_tele = g_tele;
    g_last_host_pid = GetCurrentProcessId();
    g_tele = Telemetry{};
    g_tele.available = true;
    g_tele.processed_visible = false;
    g_probe_messages = 0;
    g_probe_wparam = 99;
    ToggleCarrierOverlay();
    PumpProbeMessages();
    Check(g_probe_messages == 1 && g_probe_wparam == 1, "toggle must post exactly one processed-view request");

    g_tele.processed_visible = true;
    g_probe_messages = 0;
    g_probe_wparam = 99;
    ToggleCarrierOverlay();
    PumpProbeMessages();
    Check(g_probe_messages == 1 && g_probe_wparam == 0, "toggle must post the target raw-view request");

    // 无遥测时不可用：不发消息，也不把失败当成已切换。
    g_tele.available = false;
    g_probe_messages = 0;
    ToggleCarrierOverlay();
    PumpProbeMessages();
    Check(g_probe_messages == 0, "no carrier request without telemetry");
    Check(!PostAioShowProcessed(true), "posting without telemetry must report failure");

    g_last_host_pid = saved_pid;
    g_tele = saved_tele;
    DestroyWindow(preview);
    DestroyWindow(main_output);
}
int wmain(int argc, wchar_t **argv) {
    try {
        Check(argc == 2, "test directory required");
        const fs::path dir(argv[1]);
        IniPath = (dir / L"ReShade.ini").wstring();
        ParamsPath = (dir / L"live-params.json").wstring();
        CustomPath = (dir / L"custom-tier.json").wstring();
        Check(WritePrivateProfileStringW(L"Standalone.DLSSNR", L"FgMultiplier", L"4", IniPath.c_str()) != 0, "INI setup failed");
        Params loaded;
        ReadIniParams(loaded);
        Check(loaded.fg_multiplier == 4, "INI lost multiplier");
        loaded.intensity = .72f;
        loaded.fg = true;
        Check(SaveCustomTier(loaded), "custom save failed");
        g_custom = Params{};
        g_custom_valid = false;
        LoadCustomTier();
        Check(g_custom_valid && g_custom.Equals(loaded), "custom reload lost fields");
        Params other = loaded;
        other.fg_multiplier = 2;
        Check(!other.Equals(loaded), "equality ignores multiplier");
        Check(WriteParamsAtomic(ParamsPath, loaded), "initial live save failed");
        HANDLE busy = CreateFileW(ParamsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(busy != INVALID_HANDLE_VALUE, "lock setup failed");
        g_save.dirty = true;
        loaded.intensity = .81f;
        Check(!g_save.Try([&] { return WriteParamsAtomic(ParamsPath, loaded); }) && g_save.dirty,
              "failed write lost pending parameters");
        CloseHandle(busy);
        Check(g_save.Try([&] { return WriteParamsAtomic(ParamsPath, loaded); }) && !g_save.dirty,
              "retry did not save after lock release");
        HANDLE file = CreateFileW(ParamsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "readback open failed");
        char text[512]{}; DWORD n = 0;
        const bool read = ReadFile(file, text, sizeof(text)-1, &n, nullptr) != 0;
        CloseHandle(file);
        nr_params::Values values;
        Check(read && nr_params::Parse(std::string_view(text, n), values) &&
              values.multiplier == 4 && fabsf(values.intensity-.81f)<.005f, "retry saved wrong parameters");

        TestFpsBaselineLogic();
        TestForcedFlushPendingLayer(dir);
        TestForcedFlushPanel(dir);
        TestF10HitRectAndCarrierMessage();
        puts("PASS panel INI/custom/live multiplier roundtrip, locked-file retry, R8 forced flush, R9 FPS baseline/clamp/SourceFps and F10 hit-test/carrier message");
        return 0;
    } catch (const std::exception &e) { fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
