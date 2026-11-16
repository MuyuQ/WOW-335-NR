// Exercise the actual application request callbacks, without opening the UI.
#define wWinMain ControlCenterUnusedEntry
#include "../native/control_center/app/cc_main.cpp"
#undef wWinMain
#include <stdexcept>

static void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void Write(const std::wstring& path, const std::string& bytes) {
    DWORD error = 0;
    Check(control_center::config_wtf_detail::WriteAllBytes(path, bytes, CREATE_ALWAYS, error), "write fixture");
}
static void WaitJobs(cc::App& app) {
    const auto start = GetTickCount64();
    while ((app.advanced.loading || app.advanced.schemesLoading || app.telemetryPolling) && GetTickCount64() - start < 5000) {
        app.jobs.DrainApply(); Sleep(5);
    }
    Check(!app.advanced.loading && !app.advanced.schemesLoading && !app.telemetryPolling, "request flags must finish");
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 3, "output and repo required");
        cc::App app;
        app.clientRoot = std::wstring(argv[1]) + L"\\app-client";
        CreateDirectoryW(app.clientRoot.c_str(), nullptr);
        CreateDirectoryW((app.clientRoot + L"\\host64").c_str(), nullptr);
        app.paths.exeDir = argv[2]; app.paths.dataDir = argv[1];
        Write(app.clientRoot + L"\\host64\\ReShade.ini", "[Standalone.DLSSNR]\nNeuralRendering=0\nModel=1\nIntensity=0.35\nLocalTone=1\nLocalStructure=0.25\nSkinStructure=-1\n");
        app.jobs.Start(nullptr);
        for (int reverse = 0; reverse < 2; ++reverse) {
            app.advanced = cc::AdvancedSummary{};
            HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            Check(app.jobs.Post([=] { SetEvent(entered); WaitForSingleObject(release, 5000); }, {}), "block queue");
            Check(WaitForSingleObject(entered, 1000) == WAIT_OBJECT_0, "worker started");
            if (reverse) { cc::AppRequestSchemes(app); cc::AppRequestAdvanced(app); }
            else { cc::AppRequestAdvanced(app); cc::AppRequestSchemes(app); }
            SetEvent(release); WaitJobs(app);
            CloseHandle(entered); CloseHandle(release);
            Check(app.advanced.loaded && app.advanced.settings.readable, "parameters loaded");
            Check(app.advanced.schemesLoaded && app.advanced.schemes.size() == 4, "four built-in schemes loaded");
        }
        control_center::TelemetrySample sample;
        sample.available = true; sample.sourceFps = 60; sample.nrFrames = 0;
        sample.timestampSeconds = 1; sample.framesPresented = 100;
        cc::ApplyTelemetryRates(app, sample);
        Check(app.fpsSource == 60, "NR-off input rate must use telemetry SourceFps");
        sample.timestampSeconds = 2; sample.framesPresented = 220;
        cc::ApplyTelemetryRates(app, sample);
        Check(app.fpsSource == 60 && app.fpsOutput == 120, "input and output rates remain independent");
        sample.available = false; cc::ApplyTelemetryRates(app, sample);
        Check(app.fpsSource < 0 && app.fpsOutput < 0 && app.sampleAt < 0, "missing telemetry clears rates");
        // A stale generation must not overwrite the selected client's state.
        cc::AppRequestSchemes(app);
        ++app.generation; app.advanced = cc::AdvancedSummary{};
        bool drained = false;
        Check(app.jobs.Post([] {}, [&](const std::wstring&) { drained = true; }), "post result barrier");
        const auto staleStart = GetTickCount64();
        while (!drained && GetTickCount64() - staleStart < 5000) { app.jobs.DrainApply(); Sleep(1); }
        Check(drained && !app.advanced.schemesLoaded, "stale callback rejected");
        app.jobs.Stop();
        // Stop cancels a queued write but lets the active task and its apply finish.
        app.jobs.Start(nullptr);
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr), release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<bool> ran{false}; bool applied = false;
        Check(app.jobs.Post([=] { SetEvent(entered); WaitForSingleObject(release, 5000); }, [&](const std::wstring&) { applied = true; }), "post active");
        Check(WaitForSingleObject(entered, 1000) == WAIT_OBJECT_0, "active started");
        Check(app.jobs.Post([&] { ran = true; }, {}), "post queued");
        app.jobs.RequestStop();
        Check(!app.jobs.HasExited(), "active work retained");
        Check(!app.jobs.Post([] {}, {}), "new work rejected");
        SetEvent(release);
        while (!app.jobs.HasExited()) Sleep(1);
        app.jobs.DrainApply(); app.jobs.Stop();
        CloseHandle(entered); CloseHandle(release);
        Check(applied && !ran, "active result applied and queued write cancelled");
        // Force a Direct2D wrong-state error so EndDraw discards the target.
        HWND window = CreateWindowExW(0, L"STATIC", L"review fixture", WS_OVERLAPPEDWINDOW, 0, 0, 1000, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "hidden render window"); app.hwnd = window;
        Check(app.gfx.Initialize(window, 96), "initialize D2D");
        Check(app.gfx.BeginDraw(), "begin draw"); app.gfx.RT()->BeginDraw(); app.gfx.EndDraw();
        Check(!app.gfx.DeviceReady(), "simulate target loss");
        cc::AppRender(app);
        Check(app.gfx.DeviceReady(), "AppRender must recreate target without resizing");
        app.gfx.Shutdown(); DestroyWindow(window);
        // Exercise the actual WM_CLOSE path: keep a window until work and apply finish.
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = WndProc;
        wc.lpszClassName = L"WOW335NR.Test.Close";
        Check(RegisterClassW(&wc) != 0, "register close fixture");
        gApp = &app;
        window = CreateWindowExW(0, wc.lpszClassName, L"close fixture", WS_OVERLAPPEDWINDOW, 0, 0, 1000, 800, nullptr, nullptr, wc.hInstance, nullptr);
        Check(window != nullptr, "create close fixture");
        app.hwnd = window; app.closeRequested = false; gReady = true;
        app.jobs.Start(window);
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr); release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool closeResultApplied = false;
        Check(app.jobs.Post([=] { SetEvent(entered); WaitForSingleObject(release, 5000); }, [&](const std::wstring&) { closeResultApplied = true; }), "post closing work");
        Check(WaitForSingleObject(entered, 1000) == WAIT_OBJECT_0, "closing work started");
        SendMessageW(window, WM_CLOSE, 0, 0);
        Check(IsWindow(window) && app.closeRequested && !closeResultApplied, "closing must retain the window during work");
        app.advanced = cc::AdvancedSummary{};
        cc::AppRequestAdvanced(app); cc::AppRequestSchemes(app);
        Check(!app.advanced.loading && !app.advanced.schemesLoading, "closing must suppress follow-up read requests");
        SetEvent(release);
        const auto closeStart = GetTickCount64();
        while (IsWindow(window) && GetTickCount64() - closeStart < 3000) { cc::AppTick(app); Sleep(1); }
        Check(!IsWindow(window) && closeResultApplied, "closing applies result before destroying window");
        gReady = false; gApp = nullptr; app.jobs.Stop();
        CloseHandle(entered); CloseHandle(release); UnregisterClassW(wc.lpszClassName, wc.hInstance);
        std::puts("PASS application bidirectional requests, queue shutdown and D2D target recovery");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "app regression: %s\n", error.what()); return 1; }
}
