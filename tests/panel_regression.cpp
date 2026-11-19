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
        puts("PASS panel INI/custom/live multiplier roundtrip, locked-file retry, R8 forced flush and R9 FPS baseline/clamp/SourceFps");
        return 0;
    } catch (const std::exception &e) { fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
