// WOW-335-NR 原生控制中心：程序入口、窗口过程、后台任务接线与动作分发。
//
// 线程模型：
//   - UI 线程：只做 D2D 绘制、命中区域、状态 apply 与动作分发；
//   - 后台单线程（cc_jobs.hpp）：目录扫描、哈希校验、安装/卸载预览等重活；
//   - 工作线程只写 shared_ptr 结果，真正修改 App 的状态在 UI 线程的 apply 回调里，
//     并校验 generation 与客户端路径，切换客户端后旧结果直接丢弃。
//
// 本阶段（第二阶段）已接通安装/修复/卸载执行、Config.wtf 写入、卸载提交与恢复、
// 档位与方案、遥测/进程关联、启动游戏与游戏内面板、鼠标助手、诊断/清单导出、
// 恢复安装前文件与 AIO 管线开关等真实能力；任何失败都给出可读原因，不用假进度冒充完成。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "cc_app.hpp"

#include <d2d1.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include "cc_jobs.hpp"
#include <vector>

#include "app_icon.h"
#include "capture_client_record.hpp"
#include "client_identity.hpp"
#include "client_lock.hpp"
#include "client_record.hpp"
#include "strict_json.hpp"

// ===========================================================================
// 小工具与 UI 线程全局状态
// ===========================================================================
namespace {

cc::App* gApp = nullptr;
bool gReady = false;
bool gTrackingMouse = false;
bool gDebugHits = true;
// 性能探针：启动到首帧、切页到首帧的耗时（毫秒）。
ULONGLONG gStartTick = 0;
ULONGLONG gSwitchTick = 0;
int gRenderedPage = -1;
HBRUSH gEditBrush = nullptr;

constexpr wchar_t kWindowClass[] = L"WOW335NR.ControlCenter.Native";
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kSnapshotPollMs = 2500;
// 主页遥测轮询间隔：旧界面 1 s 定时器 + 最小 0.5 s 采样；这里两次读取间隔 500 ms，
// 在 apply 中用遥测时间戳再判断是否满 0.5 s，界面数值约每秒刷新一次。
constexpr UINT kTelemetryPollMs = 500;
// 切页过渡：新页面横向滑入的时长与最大偏移（DIP）；短促的缓出，避免生硬跳变。
// 切页过渡：样式与时长。F9 在运行时循环样式，NR_CC_TRANSITION 环境变量可指定初值。
constexpr int kPageTransitionStyleCount = 4;
constexpr ULONGLONG kPageTransitionDurationMs[kPageTransitionStyleCount] = {220, 200, 300, 340};
constexpr const wchar_t* kPageTransitionStyleNames[kPageTransitionStyleCount] = {
    L"顺滑滑入", L"缩放淡入", L"弹性滑入", L"长距滑入"};

struct PageTransitionFrame {
    float offset = 0.0f;
    float scale = 1.0f;
    float opacity = 1.0f;
};

inline float EaseInOutCubic(float t) {
    if (t < 0.5f) return 4.0f * t * t * t;
    const float u = -2.0f * t + 2.0f;
    return 1.0f - (u * u * u) * 0.5f;
}

inline float EaseOutCubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

inline float EaseOutQuint(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u * u * u;
}

inline float EaseOutBack(float t) {
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

inline PageTransitionFrame ComputePageTransition(int style, float t) {
    PageTransitionFrame frame;
    switch (style) {
        case 1: {
            const float eased = EaseOutCubic(t);
            frame.offset = 10.0f * (1.0f - eased);
            frame.scale = 1.0f + 0.022f * (1.0f - eased);
            frame.opacity = 0.86f + 0.14f * eased;
            break;
        }
        case 2: {
            frame.offset = 22.0f * (1.0f - EaseOutBack(t));
            break;
        }
        case 3: {
            const float eased = EaseOutQuint(t);
            frame.offset = 40.0f * (1.0f - eased);
            frame.opacity = 0.78f + 0.22f * eased;
            break;
        }
        default: {
            frame.offset = 26.0f * (1.0f - EaseInOutCubic(t));
            break;
        }
    }
    return frame;
}

// 最近一次记录到日志的状态，避免每 2.5 秒重复刷同样的行。
control_center::ProcessState gLoggedProcess = control_center::ProcessState::Unknown;
bool gLoggedProcessValid = false;
std::uint64_t gLoggedRecordEntries = 0;
bool gLoggedRecordPresent = false;
bool gLoggedRecordValid = false;

double NowMs() {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    if (frequency.QuadPart == 0) return 0.0;
    return static_cast<double>(counter.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

template <typename... Args>
std::wstring Fmt(const wchar_t* format, Args... args) {
    wchar_t buffer[1024]{};
    swprintf_s(buffer, format, args...);
    return std::wstring(buffer);
}

BYTE Channel(unsigned rgb, int shift) {
    return static_cast<BYTE>((rgb >> shift) & 0xFFu);
}

// 临时诊断日志（定位首帧不显示问题后保留为最小化日志用途）。
std::wstring gDiagPath;
void DiagLine(const std::wstring& text) {
    if (gDiagPath.empty()) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return;
    std::string line(static_cast<size_t>(bytes) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, line.data(), bytes, nullptr, nullptr);
    line += "\r\n";
    HANDLE file = CreateFileW(gDiagPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

}  // namespace

// ===========================================================================
// App 生命周期与后台任务
// ===========================================================================
namespace cc {

using namespace theme;

// 文件夹选择：先声明，供向导步骤的动作分发使用（定义见文件后半部分）。
std::wstring PickFolder(HWND owner, const wchar_t* title);

namespace {

// 后台安装预览结果（工作线程只写这里，UI 线程读取后拷进 App）。
struct InstallWork {
    control_center::InstallPreview preview;
};

// 后台卸载预览结果。recordState: 0=没有记录, 1=记录存在但不可用, 2=记录可用。
struct UninstallWork {
    int recordState = 0;
    control_center::UninstallPreview preview;
    std::string error;
};

// 后台运行时遥测结果：进程关联 + 载体共享内存快照。
struct TelemetryWork {
    control_center::ProcessAssociation association;
    control_center::TelemetrySample sample;
};

}  // namespace

bool AppInit(App& app, HINSTANCE instance, int argc, wchar_t** argv) {
    app.instance = instance;
    if (!app.paths.Init()) {
        // 路径初始化失败时依然允许窗口启动，但后续读取都会失败并在界面上报告。
        app.log.Add(L"数据目录初始化失败，设置与日志不会保存。");
    }

    app.layoutScale = 1.0f;
    std::wstring initialClient;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--dpi-scale") == 0 && i + 1 < argc) {
            const double value = std::wcstod(argv[i + 1], nullptr);
            if (value >= 0.75 && value <= 3.0) app.layoutScale = static_cast<float>(value);
            ++i;
        } else if (wcscmp(argv[i], L"--client") == 0 && i + 1 < argc) {
            initialClient = argv[i + 1];
            ++i;
        } else if (wcscmp(argv[i], L"--debug") == 0) {
            app.debugOverlay = true;
        }
    }
    gDebugHits = app.debugOverlay;

    // 过渡样式初值：环境变量便于对比不同效果，默认 0（顺滑滑入）。
    wchar_t transitionEnv[32] = {};
    const DWORD transitionLen = GetEnvironmentVariableW(L"NR_CC_TRANSITION", transitionEnv, 32);
    if (transitionLen > 0 && transitionLen < 32) {
        const std::wstring style(transitionEnv, transitionLen);
        if (style == L"zoom") app.pageTransitionStyle = 1;
        else if (style == L"spring") app.pageTransitionStyle = 2;
        else if (style == L"glide") app.pageTransitionStyle = 3;
    }

    app.settings = LoadSettings(app.paths);
    app.catalog = LoadCatalogBundle(app.paths.exeDir);
    app.protectedRoots = ProductProtectedRoots(app.paths);

    app.templateRoot = app.paths.exeDir + L"\\templates";
    if (!DirectoryExistsNoReparse(app.templateRoot)) app.templateRoot.clear();

    app.stageRoot = app.settings.stage;
    if (app.stageRoot.empty()) {
        const std::wstring staged = app.paths.exeDir + L"\\staged";
        if (DirectoryExistsNoReparse(staged)) app.stageRoot = staged;
    }

    app.clientRoot = initialClient.empty() ? app.settings.client : initialClient;
    if (!app.clientRoot.empty() && !TestClientRoot(app.clientRoot)) {
        app.log.Add(L"上次选择的客户端目录已不可用：" + app.clientRoot);
        app.clientRoot.clear();
    }

    app.log.Add(L"控制台启动：" + app.paths.exePath);
    if (!app.catalog.ok) {
        app.log.Add(L"发行清单读取失败：" + Utf8ToWide(app.catalog.error));
    } else {
        app.log.Add(Fmt(L"发行清单：%zu 个成员。", app.catalog.catalog.members.size()));
    }
    if (!app.clientRoot.empty()) {
        app.log.Add(L"客户端：" + app.clientRoot);
        app.settings.AddRecent(app.clientRoot, DisplayNameForClientPath(app.clientRoot));
    }

    app.page = Page::Home;
    app.wizardStep = WizardStep::Client;
    app.dirty = true;
    return true;
}

void AppShutdown(App& app) {
    app.jobs.Stop();
    if (app.editSchemeName && IsWindow(app.editSchemeName)) DestroyWindow(app.editSchemeName);
    app.editSchemeName = nullptr;
    if (app.editFont) {
        DeleteObject(app.editFont);
        app.editFont = nullptr;
    }
    app.gfx.Shutdown();
    if (!app.clientRoot.empty()) app.settings.client = app.clientRoot;
    if (!app.stageRoot.empty()) app.settings.stage = app.stageRoot;
    if (!app.paths.settingsPath.empty()) SaveSettings(app.paths, app.settings);
}

void AppSetStatus(App& app, const std::wstring& text) {
    app.statusNote = text;
    DiagLine(text);
    app.log.Add(text);
    app.dirty = true;
}

void AppDiagLog(const std::wstring& text) { DiagLine(text); }

void AppBumpGeneration(App& app) {
    ++app.generation;
    app.snapshotLoading = false;
    app.installLoading = false;
    app.uninstallLoading = false;
    ++app.installToken;
    ++app.uninstallToken;
    app.snapshot = ClientSnapshot{};
    app.snapshotReady = false;
    app.install = InstallSummary{};
    app.uninstall = UninstallSummary{};
    app.advanced = AdvancedSummary{};
    // 运行时遥测属于旧客户端：清空并让在途结果失效。
    ++app.runtimeToken;
    app.telemetryPolling = false;
    app.gameRunning = false;
    app.gameProcessCount = 0;
    app.hostProcessCount = 0;
    app.unidentifiedProcesses = 0;
    app.telemetry = TelemetrySample{};
    app.telemetryNote.clear();
    app.fpsSource = -1.0;
    app.fpsOutput = -1.0;
    app.sampleAt = -1.0;
    // 工具动作与日志查看器属于旧客户端：清空状态并作废在途回调。
    app.toolRun.running = false;
    app.toolRun.finished = false;
    app.toolRun.ok = false;
    app.toolRun.action.clear();
    app.toolRun.error.clear();
    app.toolRun.detail.clear();
    app.toolRun.token = ++app.toolToken;
    app.restoreArmed = false;
    app.logViewer = App::LogViewer{};
    app.gpuStats = control_center::GpuStats{};
    app.gpuPolling = false;
    app.panelAutoLaunchTried = false;
    app.panelAutoRequest = false;
    app.lastPanelCheck = 0;
    app.lastPanelLog.clear();
    app.lastWatchLog.clear();
    app.fpsSaving = false;
    app.fpsFrontText.clear();
    app.fpsBackText.clear();
    app.fpsEditFocus = 0;
    app.fpsCaretOn = true;
    app.fpsCaretTick = 0;
    app.syncedFpsFront = -1;
    app.syncedFpsBack = -1;
}

void AppSelectClient(App& app, const std::wstring& root, bool remember) {
    std::wstring normalized = root;
    while (!normalized.empty() && (normalized.back() == L'\\' || normalized.back() == L'/')) normalized.pop_back();

    AppBumpGeneration(app);
    app.clientRoot = normalized;
    app.scroll = 0.0f;
    app.scrollMax = 0.0f;

    if (remember && !normalized.empty()) {
        app.settings.client = normalized;
        app.settings.AddRecent(normalized, DisplayNameForClientPath(normalized));
        if (!app.paths.settingsPath.empty() && !SaveSettings(app.paths, app.settings)) {
            app.log.Add(L"设置保存失败：" + app.paths.settingsPath);
        }
    }

    if (!normalized.empty()) {
        AppSetStatus(app, L"已选择客户端：" + normalized);
        AppRequestSnapshot(app, false);
    } else {
        AppSetStatus(app, L"已清除客户端选择。");
    }
    app.dirty = true;
}

void AppRequestSnapshot(App& app, bool verifyHashes) {
    if (app.closeRequested) return;
    if (app.clientRoot.empty()) {
        app.snapshot = ClientSnapshot{};
        app.snapshotReady = false;
        app.snapshotLoading = false;
        app.dirty = true;
        AppSyncFpsText(app);
        return;
    }
    if (app.snapshotLoading) {
        if (verifyHashes) app.verifyPending = true;
        return;
    }

    app.snapshotLoading = true;
    const std::wstring root = app.clientRoot;
    const std::uint64_t generation = app.generation;
    auto result = std::make_shared<ClientSnapshot>();
    auto catalog = std::make_shared<CatalogBundle>(app.catalog);
    auto protectedRoots = app.protectedRoots;

    const bool posted = app.jobs.Post(
        [result, root, catalog, protectedRoots, generation, verifyHashes]() {
            *result = SnapshotClient(root, *catalog, protectedRoots, generation);
            if (verifyHashes) VerifySnapshotHashes(*result, *catalog);
        },
        [&app, result, root, generation, verifyHashes](const std::wstring& error) {
            if (generation != app.generation || !SamePath(root, app.clientRoot)) return;
            app.snapshotLoading = false;
            if (!error.empty()) {
                AppSetStatus(app, L"状态读取失败：" + error);
                return;
            }
            app.snapshot = std::move(*result);
            app.snapshotReady = true;

            const bool recordPresent = app.snapshot.recordState == control_center::CapturedRecordState::Present;
            if (!gLoggedRecordValid || recordPresent != gLoggedRecordPresent ||
                (recordPresent && app.snapshot.recordEntries != gLoggedRecordEntries)) {
                gLoggedRecordValid = true;
                gLoggedRecordPresent = recordPresent;
                gLoggedRecordEntries = app.snapshot.recordEntries;
                if (recordPresent) {
                    AppSetStatus(app, Fmt(L"安装记录：%zu 项；客户端状态已读取。", app.snapshot.recordEntries));
                } else {
                    AppSetStatus(app, L"客户端尚未安装组件。");
                }
            }
            if (!gLoggedProcessValid || app.snapshot.processState != gLoggedProcess) {
                gLoggedProcessValid = true;
                gLoggedProcess = app.snapshot.processState;
                if (app.snapshot.processState == control_center::ProcessState::Running) {
                    AppSetStatus(app, L"检测到游戏或载体正在运行。");
                } else if (app.snapshot.processState == control_center::ProcessState::Stopped) {
                    AppSetStatus(app, L"游戏与载体均已退出。");
                } else {
                    AppSetStatus(app, L"无法确认游戏进程状态，请手动确认。");
                }
            }
            app.dirty = true;
            AppSyncFpsText(app);
            if (app.verifyPending) {
                app.verifyPending = false;
                AppRequestSnapshot(app, true);
            } else if (verifyHashes) {
                AppSetStatus(app, Fmt(L"校验完成：%zu / %zu 项与清单一致。", app.snapshot.verified,
                                      app.snapshot.catalogMembers));
            }
        });
    if (!posted) {
        app.snapshotLoading = false;
        AppSetStatus(app, L"后台任务队列已满，稍后自动重试。");
    }
}

void AppRequestClientScan(App& app) {
    if (app.closeRequested) return;
    if (app.scanLoading) return;
    app.scanLoading = true;
    auto result = std::make_shared<std::vector<ClientCandidate>>();
    const bool posted = app.jobs.Post(
        [result]() { *result = ScanClientRoots(4, 700); },
        [&app, result](const std::wstring& error) {
            app.scanLoading = false;
            if (!error.empty()) {
                AppSetStatus(app, L"扫描客户端失败：" + error);
                return;
            }
            app.candidates = *result;
            app.candidatesReady = true;
            app.log.Add(Fmt(L"扫描完成：发现 %zu 个客户端。", app.candidates.size()));
            if (app.clientRoot.empty() && !app.candidates.empty()) {
                AppSelectClient(app, app.candidates.front().path, true);
            }
            app.dirty = true;
        });
    if (!posted) {
        app.scanLoading = false;
        app.log.Add(L"后台任务队列已满，未能开始扫描客户端。");
        app.dirty = true;
    }
}

void AppRequestInstallPreview(App& app) {
    if (app.closeRequested) return;
    if (app.clientRoot.empty()) {
        app.install = InstallSummary{};
        app.install.ready = true;
        app.install.ok = false;
        app.install.error = L"尚未选择客户端。";
        app.dirty = true;
        return;
    }
    if (!app.catalog.ok) {
        app.install = InstallSummary{};
        app.install.ready = true;
        app.install.ok = false;
        app.install.error = L"发行清单读取失败：" + Utf8ToWide(app.catalog.error);
        app.dirty = true;
        return;
    }
    if (app.installLoading) return;

    app.installLoading = true;
    ++app.installToken;
    const std::uint64_t token = app.installToken;
    const std::uint64_t generation = app.generation;
    const std::wstring client = app.clientRoot;
    const std::wstring stage = app.stageRoot;
    const std::wstring templ = app.templateRoot;
    auto catalog = std::make_shared<CatalogBundle>(app.catalog);
    auto result = std::make_shared<InstallWork>();

    const bool posted = app.jobs.Post(
        [result, client, stage, templ, catalog]() {
            result->preview = PreviewInstall(client, stage, templ, catalog->catalog);
        },
        [&app, result, token, generation, client, stage, templ](const std::wstring& error) {
            if (token != app.installToken || generation != app.generation) return;
            app.installLoading = false;
            app.install.ready = true;
            app.install.stageRoot = stage;
            app.install.templateRoot = templ;
            if (!error.empty()) {
                app.install.ok = false;
                app.install.error = error;
                AppSetStatus(app, L"安装预检失败：" + error);
                return;
            }
            app.install.preview = std::move(result->preview);
            app.install.ok = app.install.preview.error.empty();
            app.install.error = Utf8ToWide(app.install.preview.error);
            if (app.install.error.empty() && !app.install.preview.filesReady) {
                app.install.error = PreviewIssueText(app.install.preview);
            }
            app.log.Add(Fmt(L"安装预检：%zu 项，%s。", app.install.preview.items.size(),
                            app.install.preview.filesReady ? L"文件齐全"
                                                            : L"存在冲突或缺失，详见安装页"));
            app.dirty = true;
        });
    if (!posted) {
        app.installLoading = false;
        AppSetStatus(app, L"后台任务队列已满，稍后重试安装预检。");
    }
}

void AppRequestUninstallPreview(App& app) {
    if (app.closeRequested) return;
    if (app.clientRoot.empty()) {
        app.uninstall = UninstallSummary{};
        app.uninstall.ready = true;
        app.uninstall.hasRecord = false;
        app.dirty = true;
        return;
    }
    if (app.uninstallLoading) return;

    app.uninstallLoading = true;
    ++app.uninstallToken;
    const std::uint64_t token = app.uninstallToken;
    const std::uint64_t generation = app.generation;
    const std::wstring client = app.clientRoot;
    const std::wstring manifestPath = app.catalog.manifestPath;
    auto catalog = std::make_shared<CatalogBundle>(app.catalog);
    auto protectedRoots = app.protectedRoots;
    auto result = std::make_shared<UninstallWork>();

    const bool posted = app.jobs.Post(
        [result, client, catalog, protectedRoots, manifestPath]() {
            std::string manifestText;
            DWORD manifestError = ERROR_SUCCESS;
            if (!ReadWholeFile(manifestPath, manifestText, manifestError)) {
                result->recordState = 1;
                result->error = "install manifest unreadable";
                return;
            }
            const auto manifestJson = control_center::ParseJsonObject(manifestText);
            if (!manifestJson) {
                result->recordState = 1;
                result->error = "install manifest invalid";
                return;
            }
            const auto captured = control_center::CaptureClientRecord(client);
            if (captured.state == control_center::CapturedRecordState::Absent) {
                result->recordState = 0;
                result->error = "client installation record absent";
                return;
            }
            if (captured.state != control_center::CapturedRecordState::Present) {
                result->recordState = 1;
                result->error = "client installation record unavailable";
                return;
            }
            const auto record = control_center::ReadClientRecord(captured.bytes);
            if (!record) {
                result->recordState = 1;
                result->error = record.error.empty() ? "invalid client record" : record.error;
                return;
            }
            result->recordState = 2;
            control_center::ClientLock lock;
            if (!lock.Acquire(client)) {
                result->error = "client lock busy or requires recovery";
                return;
            }
            result->preview = control_center::PreviewUninstall(client, protectedRoots, catalog->catalog,
                                                              manifestJson.value, lock);
            (void)record;
            lock.Release();
        },
        [&app, result, token, generation, client](const std::wstring& error) {
            if (token != app.uninstallToken || generation != app.generation) return;
            app.uninstallLoading = false;
            app.uninstall.ready = true;
            app.uninstall.clientRoot = client;
            app.uninstall.hasRecord = result->recordState != 0;
            app.uninstall.preview = std::move(result->preview);
            if (!error.empty()) {
                app.uninstall.ok = false;
                app.uninstall.error = error;
                AppSetStatus(app, L"卸载清单读取失败：" + error);
                return;
            }
            app.uninstall.ok = app.uninstall.preview.valid && app.uninstall.preview.error.empty();
            app.uninstall.error = Utf8ToWide(app.uninstall.preview.error);
            if (app.uninstall.ok) {
                app.log.Add(Fmt(L"卸载清单：%zu 项，残留 %zu 项，%s。", app.uninstall.preview.items.size(),
                                app.uninstall.preview.residue.size(),
                                app.uninstall.preview.canUninstall ? L"可以卸载" : L"存在需要人工处理的项"));
            } else if (result->recordState != 0) {
                AppSetStatus(app, L"卸载清单读取失败：" +
                                      (app.uninstall.error.empty() ? std::wstring(L"未知原因") : app.uninstall.error));
            }
            app.dirty = true;
        });
    if (!posted) {
        app.uninstallLoading = false;
        AppSetStatus(app, L"后台任务队列已满，稍后重试卸载预检。");
    }
}

// ===========================================================================
// 档位 / 高级参数 / 方案（维护页「高级工具」）
//
// 只读读取与方案目录扫描都放在后台线程；写入由 ds5_settings.hpp 自己完成
// 备份、原子替换与回读校验，这里只负责调度与把结果转成界面状态。
// ===========================================================================

void AppRequestAdvanced(App& app) {
    if (app.closeRequested) return;
    if (app.advanced.loading) return;
    if (app.clientRoot.empty()) {
        app.advanced.loaded = true;
        app.advanced.error = L"尚未选择客户端。";
        app.dirty = true;
        return;
    }
    app.advanced.loading = true;
    app.advanced.error.clear();
    const std::wstring client = app.clientRoot;
    const std::uint64_t generation = app.generation;
    const std::uint64_t token = ++app.advanced.readToken;
    auto result = std::make_shared<AdvancedReadTaskResult>();
    const bool posted = app.jobs.Post(
        [result, client]() { *result = RunAdvancedReadTask(client); },
        [&app, result, generation, token](const std::wstring& error) {
            if (generation != app.generation || token != app.advanced.readToken) return;
            app.advanced.loading = false;
            app.advanced.loaded = true;
            app.dirty = true;
            if (!error.empty()) {
                app.advanced.error = error;
                return;
            }
            if (!result->ok) {
                app.advanced.error = result->error;
                return;
            }
            app.advanced.settings = result->settings;
            app.advanced.tierName = result->tierName;
            app.advanced.modelName = result->modelName;
            app.advanced.edit = Ds5ValuesFrom(result->settings);
            app.advanced.editDetached = result->settings.detachedPresent ? result->settings.detachedPresentation : true;
            app.advanced.editOpticalFlow = result->settings.opticalPresent ? result->settings.opticalFlow : false;
            app.advanced.error.clear();
        });
    if (!posted) {
        app.advanced.loading = false;
        AppSetStatus(app, L"后台任务队列已满，稍后重试读取档位。");
    }
}

void AppRequestSchemes(App& app) {
    if (app.closeRequested) return;
    if (app.advanced.schemesLoading) return;
    app.advanced.schemesLoading = true;
    app.advanced.schemesError.clear();
    const std::wstring builtinRoot = Ds5BuiltinSchemesRoot(app.paths.exeDir);
    const std::wstring userRoot = Ds5UserSchemesRoot(app.paths.dataDir);
    const std::uint64_t token = ++app.advanced.schemesToken;
    const std::uint64_t generation = app.generation;
    auto result = std::make_shared<SchemesTaskResult>();
    const bool posted = app.jobs.Post(
        [result, builtinRoot, userRoot]() { *result = RunSchemesListTask(builtinRoot, userRoot); },
        [&app, result, token, generation](const std::wstring& error) {
            if (generation != app.generation || token != app.advanced.schemesToken) return;
            app.advanced.schemesLoading = false;
            app.advanced.schemesLoaded = true;
            app.dirty = true;
            if (!error.empty()) {
                app.advanced.schemesError = error;
                return;
            }
            if (!result->ok) {
                app.advanced.schemesError = result->error;
                return;
            }
            app.advanced.schemes = result->schemes;
            app.advanced.schemesError.clear();
        });
    if (!posted) {
        app.advanced.schemesLoading = false;
        AppSetStatus(app, L"后台任务队列已满，稍后重试扫描方案。");
    }
}

void AppSaveAdvanced(App& app) {
    if (app.advanced.saving) return;
    if (app.clientRoot.empty()) {
        AppSetStatus(app, L"尚未选择客户端。");
        return;
    }
    if (!app.advanced.loaded || !app.advanced.settings.readable) {
        app.log.Add(L"操作未完成：请先读取当前参数再保存。");
        AppSetStatus(app, L"操作未完成：请先读取当前参数再保存。");
        return;
    }
    app.advanced.saving = true;
    app.advanced.status = L"正在写入 host64\\ReShade.ini…";
    const std::wstring client = app.clientRoot;
    const std::wstring operations = OperationsRoot(app.paths);
    const Ds5Values values = app.advanced.edit;
    const bool detached = app.advanced.editDetached;
    const bool optical = app.advanced.editOpticalFlow;
    const std::uint64_t generation = app.generation;
    const std::uint64_t token = ++app.advanced.actionToken;
    auto result = std::make_shared<AdvancedSaveTaskResult>();
    const bool posted = app.jobs.Post(
        [result, client, operations, values, detached, optical]() {
            *result = RunAdvancedSaveTask(client, operations, values, detached, optical);
        },
        [&app, result, generation, token](const std::wstring& error) {
            if (generation != app.generation || token != app.advanced.actionToken) return;
            app.advanced.saving = false;
            app.dirty = true;
            std::wstring failure = error;
            if (failure.empty() && !result->ok) failure = result->error;
            if (!failure.empty()) {
                app.advanced.status.clear();
                app.log.Add(L"操作未完成：" + failure);
                AppSetStatus(app, L"操作未完成：" + failure);
                return;
            }
            if (result->write.changed) {
                app.advanced.status = result->write.backupPath.empty()
                                          ? L"已保存 ReShade.ini 参数。"
                                          : (L"已保存 ReShade.ini 参数；备份：" + result->write.backupPath);
                app.log.Add(app.advanced.status);
            } else {
                app.advanced.status = L"参数没有变化，未写入文件。";
            }
            AppRequestAdvanced(app);
        });
    if (!posted) {
        app.advanced.saving = false;
        AppSetStatus(app, L"后台任务队列已满，稍后重试保存参数。");
    }
}

void AppSaveScheme(App& app) {
    if (app.advanced.schemeSaving) return;
    if (app.clientRoot.empty()) {
        AppSetStatus(app, L"尚未选择客户端。");
        return;
    }
    std::wstring name = app.advanced.schemeName;
    if (app.editSchemeName != nullptr) {
        wchar_t buffer[64] = {};
        GetWindowTextW(app.editSchemeName, buffer, 64);
        name = buffer;
    }
    if (name.empty()) {
        AppSetStatus(app, L"操作未完成：请输入方案名（1-24 个字符）。");
        return;
    }
    app.advanced.schemeSaving = true;
    const std::wstring client = app.clientRoot;
    const std::wstring userRoot = Ds5UserSchemesRoot(app.paths.dataDir);
    const std::uint64_t generation = app.generation;
    const std::uint64_t token = ++app.advanced.actionToken;
    auto result = std::make_shared<SchemeSaveTaskResult>();
    const bool posted = app.jobs.Post(
        [result, client, userRoot, name]() {
            *result = RunSchemeSaveTask(client, userRoot, name, std::wstring());
        },
        [&app, result, generation, token](const std::wstring& error) {
            if (generation != app.generation || token != app.advanced.actionToken) return;
            app.advanced.schemeSaving = false;
            app.dirty = true;
            std::wstring failure = error;
            if (failure.empty() && !result->ok) failure = result->error;
            if (!failure.empty()) {
                app.advanced.schemesError = failure;
                app.log.Add(L"操作未完成：" + failure);
                AppSetStatus(app, L"操作未完成：" + failure);
                return;
            }
            app.advanced.schemeName.clear();
            if (app.editSchemeName != nullptr) SetWindowTextW(app.editSchemeName, L"");
            app.advanced.status = L"已保存方案：" + result->path;
            app.log.Add(app.advanced.status);
            AppRequestSchemes(app);
        });
    if (!posted) {
        app.advanced.schemeSaving = false;
        AppSetStatus(app, L"后台任务队列已满，稍后重试保存方案。");
    }
}

void AppApplyScheme(App& app, const std::wstring& path) {
    if (app.advanced.schemeSaving) return;
    if (app.clientRoot.empty() || path.empty()) return;
    app.advanced.schemeSaving = true;
    const std::wstring client = app.clientRoot;
    const std::wstring operations = OperationsRoot(app.paths);
    const std::wstring builtinRoot = Ds5BuiltinSchemesRoot(app.paths.exeDir);
    const std::wstring userRoot = Ds5UserSchemesRoot(app.paths.dataDir);
    const std::uint64_t generation = app.generation;
    const std::uint64_t token = ++app.advanced.actionToken;
    auto result = std::make_shared<SchemeApplyTaskResult>();
    const bool posted = app.jobs.Post(
        [result, client, operations, path, builtinRoot, userRoot]() {
            *result = RunSchemeApplyTask(client, operations, path, builtinRoot, userRoot);
        },
        [&app, result, generation, token](const std::wstring& error) {
            if (generation != app.generation || token != app.advanced.actionToken) return;
            app.advanced.schemeSaving = false;
            app.dirty = true;
            std::wstring failure = error;
            if (failure.empty() && !result->ok) failure = result->error;
            if (!failure.empty()) {
                app.advanced.status.clear();
                app.log.Add(L"操作未完成：" + failure);
                AppSetStatus(app, L"操作未完成：" + failure);
                return;
            }
            app.advanced.status = result->write.changed ? L"已应用方案并写入 ReShade.ini。"
                                                       : L"方案内容与当前参数一致，未写入文件。";
            app.log.Add(app.advanced.status);
            AppRequestAdvanced(app);
        });
    if (!posted) {
        app.advanced.schemeSaving = false;
        AppSetStatus(app, L"后台任务队列已满，稍后重试应用方案。");
    }
}

void AppRestoreCandidateDefaults(App& app) {
    app.advanced.edit = Ds5Values{};
    app.advanced.editDetached = true;
    app.advanced.editOpticalFlow = false;
    app.advanced.status = L"已填入候选默认值（尚未写入）。";
    app.dirty = true;
}

// 运行时遥测：进程关联 + 载体共享内存快照。只读、可丢弃；主页约每秒两次。
// 结果 apply 时校验令牌与客户端，避免切换客户端后显示旧任务的数值。
// SourceFps measures input Presents at the carrier, not successful NR evaluations.
void ApplyTelemetryRates(App& app, const control_center::TelemetrySample& sample) {
    if (sample.available) {
        app.fpsSource = static_cast<double>(sample.sourceFps);
        if (app.sampleAt < 0.0) {
            // 输出帧率先建基线；输入帧率直接使用载体公布的测量值。
            app.sampleAt = sample.timestampSeconds;
            app.sampleFrames = static_cast<double>(sample.framesPresented);
        } else {
            const double delta = sample.timestampSeconds - app.sampleAt;
            if (delta >= 0.5) {
                const double output =
                    (static_cast<double>(sample.framesPresented) - app.sampleFrames) / delta;
                if (output >= 0.0 && output < 1000.0) app.fpsOutput = output;
                app.sampleAt = sample.timestampSeconds;
                app.sampleFrames = static_cast<double>(sample.framesPresented);
            }
        }
    } else {
        app.fpsSource = -1.0;
        app.fpsOutput = -1.0;
        app.sampleAt = -1.0;
    }
}

void AppRequestTelemetry(App& app) {
    if (app.closeRequested) return;
    if (app.clientRoot.empty()) {
        app.telemetryPolling = false;
        app.gameRunning = false;
        app.gameProcessCount = 0;
        app.hostProcessCount = 0;
        app.unidentifiedProcesses = 0;
        app.telemetry = TelemetrySample{};
        app.telemetryNote.clear();
        app.fpsSource = -1.0;
        app.fpsOutput = -1.0;
        app.sampleAt = -1.0;
        return;
    }
    if (app.telemetryPolling) return;
    app.telemetryPolling = true;
    ++app.runtimeToken;
    const std::uint64_t token = app.runtimeToken;
    const std::wstring client = app.clientRoot;
    auto result = std::make_shared<TelemetryWork>();

    const bool posted = app.jobs.Post(
        [result, client]() {
            result->association = control_center::AssociateClientProcesses(client);
            if (result->association.error.empty() && result->association.hosts.size() == 1 &&
                result->association.unidentified == 0) {
                result->sample = control_center::ReadAioTelemetry(result->association.hosts[0]);
            }
        },
        [&app, result, token, client](const std::wstring& error) {
            app.telemetryPolling = false;
            if (token != app.runtimeToken || !SamePath(client, app.clientRoot)) return;
            if (!error.empty()) {
                app.telemetryNote = L"遥测读取失败：" + error;
                app.dirty = true;
                return;
            }
            const auto& association = result->association;
            app.gameProcessCount = static_cast<int>(association.games.size());
            app.hostProcessCount = static_cast<int>(association.hosts.size());
            app.unidentifiedProcesses = association.unidentified;
            app.gameRunning = !association.games.empty();
            app.telemetryHostPid =
                association.hosts.size() == 1 ? association.hosts.front() : 0u;
            if (!association.error.empty()) {
                app.telemetryNote = Utf8ToWide(association.error);
            } else if (association.hosts.empty()) {
                app.telemetryNote = association.games.empty()
                                        ? std::wstring()
                                        : std::wstring(L"游戏已运行；等待 AIO 载体启动…");
            } else if (association.hosts.size() > 1 || association.unidentified > 0) {
                app.telemetryNote = L"载体数量不唯一或身份不可读。";
            } else if (!result->sample.available) {
                app.telemetryNote = Utf8ToWide(result->sample.reason);
            } else {
                app.telemetryNote.clear();
            }

            const control_center::TelemetrySample& sample = result->sample;
            app.telemetry = sample;
            ApplyTelemetryRates(app, sample);
            if (app.debugOverlay) {
                DiagLine(Fmt(L"遥测：游戏=%d 载体=%d 未识别=%d 快照=%d 源=%.1f 输出=%.1f 备注=%s",
                             app.gameProcessCount, app.hostProcessCount, app.unidentifiedProcesses,
                             result->sample.available ? 1 : 0, app.fpsSource, app.fpsOutput,
                             app.telemetryNote.c_str()));
            }
            app.dirty = true;
        });
    if (!posted) {
        app.telemetryPolling = false;
        app.telemetryNote = L"后台任务队列已满，稍后重试遥测。";
    }
}

void AppDrainJobs(App& app) { app.jobs.DrainApply(); }

// ===========================================================================
// 写事务：安装 / 修复 / 卸载
// ===========================================================================
namespace {

bool AnyOperationRunning(const App& app) {
    return app.installRun.running || app.repairRun.running || app.uninstallRun.running;
}

void FailOperation(App& app, App::OperationRun& run, const std::wstring& message) {
    run.running = false;
    run.finished = true;
    run.ok = false;
    run.error = message;
    app.log.Add(message);
    AppSetStatus(app, message);
    app.dirty = true;
}

}  // namespace

void AppStartInstall(App& app) {
    if (app.installRun.running) return;
    if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
        FailOperation(app, app.installRun, L"安装失败：请先选择有效的客户端目录。");
        return;
    }
    if (!app.catalog.ok) {
        FailOperation(app, app.installRun, L"安装失败：发行清单不可用。");
        return;
    }
    if (app.stageRoot.empty()) {
        FailOperation(app, app.installRun, L"安装失败：没有选择组件来源目录。");
        return;
    }
    if (app.snapshotReady && app.snapshot.processState == ProcessState::Running) {
        FailOperation(app, app.installRun, L"安装失败：请先退出游戏与载体进程。");
        return;
    }
    auto& run = app.installRun;
    run.running = true;
    run.finished = false;
    run.ok = false;
    run.error.clear();
    run.detail.clear();
    run.progress = std::make_shared<TaskProgress>();
    ++run.token;
    const std::uint64_t token = run.token;
    const std::uint64_t generation = app.generation;
    const std::wstring client = app.clientRoot;
    const std::wstring stage = app.stageRoot;
    const std::wstring templ = app.templateRoot;
    const std::wstring manifestPath = app.catalog.manifestPath;
    auto catalog = app.catalog.catalog;
    auto protectedRoots = app.protectedRoots;
    auto paths = app.paths;
    auto progress = run.progress;
    auto result = std::make_shared<InstallTaskResult>();
    AppGotoWizard(app, WizardStep::Install);
    app.log.Add(L"开始安装：正在后台校验源文件并写入客户端。");
    const bool posted = app.jobs.Post(
        [client, stage, templ, manifestPath, catalog, protectedRoots, paths, progress, result]() mutable {
            *result = RunInstallTask(client, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        },
        [&app, token, generation, client, result](const std::wstring& error) {
            if (token != app.installRun.token || generation != app.generation) return;
            auto& state = app.installRun;
            state.running = false;
            state.finished = true;
            state.ok = result->ok;
            state.error = result->error;
            if (result->ok)
                state.detail = Fmt(L"已应用 %zu 个成员", result->applied);
            else if (state.error.empty())
                state.error = error.empty() ? L"安装未完成。" : error;
            if (!state.ok) {
                app.log.Add(L"安装失败：" + state.error);
                AppSetStatus(app, L"安装失败：" + state.error);
                app.dirty = true;
                return;
            }
            app.log.Add(Fmt(L"安装完成：%s，%s。", state.detail.c_str(),
                            result->committedAfterRecovery ? L"恢复后已提交" : L"已提交"));
            AppSetStatus(app, L"安装完成。");
            AppGotoWizard(app, WizardStep::Done);
            AppRequestSnapshot(app, true);
            app.dirty = true;
        });
    if (!posted) {
        app.installRun.running = false;
        app.installRun.finished = false;
        FailOperation(app, app.installRun, L"后台任务队列已满，安装未开始。");
    }
}

void AppStartRepair(App& app) {
    if (app.repairRun.running) return;
    if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
        FailOperation(app, app.repairRun, L"修复失败：请先选择有效的客户端目录。");
        return;
    }
    if (!app.catalog.ok) {
        FailOperation(app, app.repairRun, L"修复失败：发行清单不可用。");
        return;
    }
    if (app.snapshotReady && app.snapshot.processState == ProcessState::Running) {
        FailOperation(app, app.repairRun, L"修复失败：请先退出游戏与载体进程。");
        return;
    }
    auto& run = app.repairRun;
    run.running = true;
    run.finished = false;
    run.ok = false;
    run.error.clear();
    run.detail.clear();
    run.progress = std::make_shared<TaskProgress>();
    ++run.token;
    const std::uint64_t token = run.token;
    const std::uint64_t generation = app.generation;
    const std::wstring client = app.clientRoot;
    const std::wstring manifestPath = app.catalog.manifestPath;
    auto catalog = app.catalog.catalog;
    auto protectedRoots = app.protectedRoots;
    auto paths = app.paths;
    auto progress = run.progress;
    auto result = std::make_shared<RepairTaskResult>();
    app.log.Add(L"开始修复：正在后台定位安装来源并校验文件。");
    const bool posted = app.jobs.Post(
        [client, manifestPath, catalog, protectedRoots, paths, progress, result]() mutable {
            *result = RunRepairTask(client, catalog, manifestPath, protectedRoots, paths, progress);
        },
        [&app, token, generation, result](const std::wstring& error) {
            if (token != app.repairRun.token || generation != app.generation) return;
            auto& state = app.repairRun;
            state.running = false;
            state.finished = true;
            state.ok = result->ok;
            state.error = result->error;
            if (result->ok) {
                state.detail = Fmt(L"已修复 %zu 个成员", result->repaired);
            } else if (state.error.empty()) {
                state.error = error.empty() ? L"修复未完成。" : error;
            }
            if (!state.ok) {
                app.log.Add(L"修复失败：" + state.error);
                AppSetStatus(app, L"修复失败：" + state.error);
                app.dirty = true;
                return;
            }
            app.log.Add(L"修复完成：" + state.detail +
                        (result->cleanupPending ? L"，旧文件清理留待下次（不影响已安装内容）" : L""));
            AppSetStatus(app, L"修复完成。");
            AppRequestSnapshot(app, true);
            app.dirty = true;
        });
    if (!posted) {
        app.repairRun.running = false;
        FailOperation(app, app.repairRun, L"后台任务队列已满，修复未开始。");
    }
}

void AppStartUninstall(App& app) {
    if (app.uninstallRun.running) return;
    if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
        FailOperation(app, app.uninstallRun, L"卸载失败：客户端目录无效。");
        return;
    }
    if (!app.catalog.ok) {
        FailOperation(app, app.uninstallRun, L"卸载失败：发行清单不可用。");
        return;
    }
    if (!app.uninstall.ready || !app.uninstall.ok || !app.uninstall.preview.canUninstall) {
        FailOperation(app, app.uninstallRun, L"卸载失败：清单不可用或存在无法安全还原的文件。");
        return;
    }
    if (app.snapshotReady && app.snapshot.processState == ProcessState::Running) {
        FailOperation(app, app.uninstallRun, L"卸载失败：请先退出游戏与载体进程。");
        return;
    }
    auto& run = app.uninstallRun;
    run.running = true;
    run.finished = false;
    run.ok = false;
    run.error.clear();
    run.detail.clear();
    run.progress = std::make_shared<TaskProgress>();
    ++run.token;
    const std::uint64_t token = run.token;
    const std::uint64_t generation = app.generation;
    const std::wstring client = app.clientRoot;
    const std::wstring manifestPath = app.catalog.manifestPath;
    auto catalog = app.catalog.catalog;
    auto protectedRoots = app.protectedRoots;
    auto paths = app.paths;
    auto progress = run.progress;
    auto result = std::make_shared<UninstallTaskResult>();
    app.log.Add(L"开始卸载：正在后台执行逐项处理与最终化。");
    const bool posted = app.jobs.Post(
        [client, manifestPath, catalog, protectedRoots, paths, progress, result]() mutable {
            *result = RunUninstallTask(client, catalog, manifestPath, protectedRoots, paths, progress);
        },
        [&app, token, generation, result](const std::wstring& error) {
            if (token != app.uninstallRun.token || generation != app.generation) return;
            auto& state = app.uninstallRun;
            state.running = false;
            state.finished = true;
            state.ok = result->ok;
            state.error = result->error;
            if (result->ok) {
                if (result->recovered) {
                    state.detail = L"已恢复并完成上一笔中断的卸载；安装记录与备份已清理。";
                } else {
                    state.detail = Fmt(L"已删除 %zu · 已还原 %zu · 保留改动 %zu · 清理残留 %zu",
                                       result->finalize.deleted, result->finalize.restored,
                                       result->finalize.keptModified, result->finalize.residueDeleted);
                }
            } else if (state.error.empty()) {
                state.error = error.empty() ? L"卸载未完成。" : error;
            }
            if (!state.ok) {
                app.log.Add(L"卸载失败：" + state.error);
                AppSetStatus(app, L"卸载失败：" + state.error);
                app.dirty = true;
                return;
            }
            app.log.Add(L"卸载完成：" + state.detail + L"。");
            AppSetStatus(app, L"卸载完成。");
            app.uninstall = UninstallSummary{};
            app.uninstall.ready = true;
            AppRequestSnapshot(app, true);
            app.dirty = true;
        });
    if (!posted) {
        app.uninstallRun.running = false;
        FailOperation(app, app.uninstallRun, L"后台任务队列已满，卸载未开始。");
    }
}


void AppGotoPage(App& app, Page page) {
    if (page == Page::Wizard) {
        AppGotoWizard(app, WizardStep::Client);
        return;
    }
    if (app.page == page) {
        app.dirty = true;
        return;
    }
    const int fromPage = static_cast<int>(app.page);
    AppBlurFpsEdit(app);
    gSwitchTick = GetTickCount64();
    app.page = page;
    app.scroll = 0.0f;
    app.scrollMax = 0.0f;
    app.pageAnimating = true;
    app.pageAnimStart = gSwitchTick;
    app.pageAnimDir = static_cast<int>(page) >= fromPage ? 1.0f : -1.0f;
    app.dirty = true;
    SetTimer(app.hwnd, kTickTimer, 16, nullptr);  // 过渡期间临时提到约 60fps，落位后再恢复。
    DiagLine(Fmt(L"切页：page=%d 请求@%llu ms", static_cast<int>(page), gSwitchTick - gStartTick));
    if (page == Page::Home) {
        AppRequestSnapshot(app, false);
        if (!app.advanced.loaded && !app.advanced.loading && !app.clientRoot.empty()) AppRequestAdvanced(app);
        AppRequestTelemetry(app);
        AppRequestGpuStats(app);
    } else if (page == Page::Maintenance) {
        AppRequestSnapshot(app, !app.snapshotReady);
        if (!app.advanced.loaded && !app.advanced.loading) AppRequestAdvanced(app);
        if (!app.advanced.schemesLoaded && !app.advanced.schemesLoading) AppRequestSchemes(app);
    } else if (page == Page::Uninstall) {
        AppRequestSnapshot(app, false);
        AppRequestUninstallPreview(app);
    }
}

void AppGotoWizard(App& app, WizardStep step) {
    const int fromPage = static_cast<int>(app.page);
    const int fromStep = static_cast<int>(app.wizardStep);
    const bool wasWizard = app.page == Page::Wizard;
    AppBlurFpsEdit(app);
    gSwitchTick = GetTickCount64();
    app.page = Page::Wizard;
    app.wizardStep = step;
    app.scroll = 0.0f;
    app.scrollMax = 0.0f;
    app.pageAnimating = true;
    app.pageAnimStart = gSwitchTick;
    if (wasWizard) {
        app.pageAnimDir = static_cast<int>(step) >= fromStep ? 1.0f : -1.0f;
    } else {
        app.pageAnimDir = static_cast<int>(Page::Wizard) >= fromPage ? 1.0f : -1.0f;
    }
    app.dirty = true;
    SetTimer(app.hwnd, kTickTimer, 16, nullptr);  // 过渡期间临时提到约 60fps，落位后再恢复。
    DiagLine(Fmt(L"切页：wizard step=%d 请求@%llu ms", static_cast<int>(step), gSwitchTick - gStartTick));
    if (step == WizardStep::Client && !app.candidatesReady && !app.scanLoading) {
        AppRequestClientScan(app);
    }
    if (step == WizardStep::Checks) {
        AppRequestSnapshot(app, false);
        AppRequestInstallPreview(app);
    }
    if (step == WizardStep::Files && !app.install.ready) {
        AppRequestInstallPreview(app);
    }
}

void AppHandleWheel(App& app, int wheelDelta) {
    if (app.scrollMax <= 0.0f) return;
    const float step = static_cast<float>(wheelDelta) / 120.0f * 60.0f;
    app.scroll -= step;
    if (app.scroll < 0.0f) app.scroll = 0.0f;
    if (app.scroll > app.scrollMax) app.scroll = app.scrollMax;
    app.dirty = true;
}

void AppHandleKey(App& app, WPARAM key) {
    if (app.closeRequested) return;
    if (key == VK_F9) {
        const int next = (app.pageTransitionStyle + 1) % kPageTransitionStyleCount;
        app.pageTransitionStyle = next;
        const std::wstring name = kPageTransitionStyleNames[next];
        AppSetStatus(app, L"切页过渡：" + name + L"（按 F9 继续切换）");
        app.log.Add(L"切页过渡样式：" + name);
        DiagLine(L"切页过渡样式：" + name);
        app.dirty = true;
        return;
    }
    if (key == VK_ESCAPE) {
        if (app.page == Page::Uninstall) {
            AppGotoPage(app, Page::Maintenance);
        } else if (app.page == Page::Wizard) {
            AppGotoPage(app, Page::Home);
        } else if (app.page == Page::Maintenance) {
            AppGotoPage(app, Page::Home);
        } else {
            PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
        }
        return;
    }
    if (key == VK_F5) {
        if (app.page == Page::Uninstall) {
            AppRequestSnapshot(app, false);
            AppRequestUninstallPreview(app);
        } else if (app.page == Page::Wizard) {
            AppRequestSnapshot(app, false);
            AppRequestInstallPreview(app);
        } else {
            AppRequestSnapshot(app, true);
        }
        return;
    }
}

void AppTick(App& app) {
    static ULONGLONG lastPoll = 0;
    static ULONGLONG lastTelemetry = 0;
    const ULONGLONG now = GetTickCount64();
    if (app.closeRequested) {
        app.jobs.DrainApply();
        if (app.jobs.HasExited()) { app.jobs.DrainApply(); DestroyWindow(app.hwnd); return; }
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    // 帧率输入框的插入符闪烁。
    if (app.fpsEditFocus != 0 && now - app.fpsCaretTick >= 500) {
        app.fpsCaretTick = now;
        app.fpsCaretOn = !app.fpsCaretOn;
        app.dirty = true;
    }
    // 主页实时遥测：与快照轮询独立，只读、可在任意页面切换后丢弃。
    if (app.page == Page::Home && !app.clientRoot.empty() && !app.telemetryPolling &&
        now - lastTelemetry >= kTelemetryPollMs) {
        lastTelemetry = now;
        AppRequestTelemetry(app);
    }
    // 主页 GPU 统计：核心内部有 5 秒缓存，这里按相同节奏读取。
    static ULONGLONG lastGpu = 0;
    if (app.page == Page::Home && !app.gpuPolling && now - lastGpu >= 5000) {
        lastGpu = now;
        AppRequestGpuStats(app);
    }
    // 面板自动拉起：游戏已运行且没有面板进程时，每 10 秒尝试一次。
    if (!app.toolRun.running && now - app.lastPanelCheck >= 10000) {
        app.lastPanelCheck = now;
        if (app.gameRunning) {
            if (!app.panelAutoLaunchTried && !control_center::PanelProcessRunning()) {
                app.panelAutoLaunchTried = true;
                app.panelAutoRequest = true;
                AppStartPanel(app);
            }
        } else {
            app.panelAutoLaunchTried = false;
        }
    }
    if (!app.clientRoot.empty() && !app.snapshotLoading && !AnyOperationRunning(app) &&
        now - lastPoll >= kSnapshotPollMs) {
        lastPoll = now;
        if (app.snapshotReady && app.snapshot.hashesChecked) {
            // 已完成哈希校验：轮询只刷新进程状态，避免把校验结果覆盖回未校验。
            const control_center::ProcessCheck check = control_center::CheckBlockingProcesses();
            if (check.state != app.snapshot.processState) {
                app.snapshot.processState = check.state;
                app.snapshot.blockingProcess = check.name;
                app.snapshot.processWin32 = check.win32Error;
                if (check.state == control_center::ProcessState::Running) {
                    AppSetStatus(app, L"检测到游戏或载体正在运行。");
                } else if (check.state == control_center::ProcessState::Stopped) {
                    AppSetStatus(app, L"游戏与载体均已退出。");
                } else {
                    AppSetStatus(app, L"无法确认游戏进程状态，请手动确认。");
                }
                app.dirty = true;
            }
        } else {
            AppRequestSnapshot(app, false);
        }
    }
    // 切页过渡：动画期间保持重绘，到时结束并恢复常规刷新节奏。
    if (app.pageAnimating) {
        const int style = app.pageTransitionStyle;
        const ULONGLONG duration =
            kPageTransitionDurationMs[(style >= 0 && style < kPageTransitionStyleCount) ? style : 0];
        if (now >= app.pageAnimStart + duration) {
            app.pageAnimating = false;
            SetTimer(app.hwnd, kTickTimer, 33, nullptr);
        }
        app.dirty = true;
    }
    if (app.dirty) {
        app.dirty = false;
        InvalidateRect(app.hwnd, nullptr, FALSE);
    }
}

void AppRender(App& app) {
    static int renderCount = 0;
    if (app.gfx.ClientWidthDip() <= 1.0f || app.gfx.ClientHeightDip() <= 1.0f) return;

    const double frameStart = NowMs();
    if (!app.gfx.BeginDraw()) {
        if (renderCount < 5) DiagLine(L"渲染：BeginDraw 失败");
        app.dirty = true;
        return;
    }
    if (renderCount < 5) {
        const cc::Rect body = cc::AppBodyRect(app);
        const cc::Rect hero = cc::AppChromeButtonRect(app, 2);
        DiagLine(Fmt(L"渲染#%d：DIP=%.1fx%.1f body=%.1f..%.1f chromeBtn=%.1f,%.1f page=%d", renderCount,
                     app.gfx.ClientWidthDip(), app.gfx.ClientHeightDip(), body.top, body.bottom, hero.left, hero.top,
                     static_cast<int>(app.page)));
    }
    app.ui.BeginFrame(&app.gfx);
    app.gfx.Clear(Color(kWindow));
    DrawChrome(app);

    // 切页过渡：标题栏不动，页面与页脚按当前样式移动/缩放/淡入。
    PageTransitionFrame transitionFrame;
    const bool animating = app.pageAnimating;
    if (animating) {
        const int style = app.pageTransitionStyle;
        const ULONGLONG duration =
            kPageTransitionDurationMs[(style >= 0 && style < kPageTransitionStyleCount) ? style : 0];
        const ULONGLONG nowTick = GetTickCount64();
        const ULONGLONG elapsed = nowTick >= app.pageAnimStart ? nowTick - app.pageAnimStart : 0;
        if (elapsed >= duration) {
            app.pageAnimating = false;
            SetTimer(app.hwnd, kTickTimer, 33, nullptr);
        } else {
            transitionFrame = ComputePageTransition(
                style, static_cast<float>(elapsed) / static_cast<float>(duration));
        }
    }
    const bool transformed = animating && app.pageAnimating;
    app.pageAnimOffsetX = transformed ? transitionFrame.offset * app.pageAnimDir : 0.0f;
    app.pageAnimScale = transformed ? transitionFrame.scale : 1.0f;
    app.pageAnimOpacity = transformed ? transitionFrame.opacity : 1.0f;
    if (transformed) {
        const float centerX = app.gfx.ClientWidthDip() * 0.5f;
        const float centerY = app.gfx.ClientHeightDip() * 0.5f;
        app.gfx.SetPageTransform(app.pageAnimOffsetX, 0.0f, transitionFrame.scale, centerX, centerY);
        if (transitionFrame.opacity < 0.999f) app.gfx.PushOpacity(transitionFrame.opacity);
    }

    // 页面正文可能有滚动，超出可视区（正文区）的控件不参与点击；页脚随后登记自己的命中区。
    app.ui.SetHitClip(AppBodyRect(app));
    switch (app.page) {
        case Page::Home: DrawHome(app); break;
        case Page::Wizard: DrawWizard(app); break;
        case Page::Maintenance: DrawMaintenance(app); break;
        case Page::Uninstall: DrawUninstall(app); break;
    }
    app.ui.ClearHitClip();

    DrawFooter(app);
    if (transformed) {
        if (transitionFrame.opacity < 0.999f) app.gfx.PopOpacity();
        app.gfx.EndPageTransform();
    }

    DrawLogViewer(app);

    if (app.debugOverlay) {
        auto* mono = app.gfx.Format(FontMono(), kFontSmall * AppScale(app), 400, HAlign::Right, VAlign::Top);
        const std::wstring text =
            Fmt(L"帧 %.1f ms · 缩放 %.2f · 页面 %d", app.frameMs, app.layoutScale, static_cast<int>(app.page));
        const Rect body = AppBodyRect(app);
        app.gfx.Text(Rect(body.left, body.top + 4.0f, body.right - 8.0f, body.top + 24.0f), text, mono,
                     Color(kAccentBright));
    }

    app.ui.EndFrame();
    app.gfx.EndDraw();
    if (!app.gfx.DeviceReady()) DiagLine(L"渲染：EndDraw 后设备丢失，下一帧重建目标。");
    if (renderCount < 5) {
        DiagLine(Fmt(L"渲染#%d：EndDraw 后设备=%s", renderCount, app.gfx.DeviceReady() ? L"ready" : L"null"));
        ++renderCount;
    }
    app.frameMs = NowMs() - frameStart;
    const int pageNow = static_cast<int>(app.page);
    if (pageNow != gRenderedPage) {
        gRenderedPage = pageNow;
        const ULONGLONG nowTick = GetTickCount64();
        DiagLine(Fmt(L"首帧：page=%d 启动后 %llu ms，切页耗时 %llu ms，帧 %.1f ms", pageNow,
                     nowTick - gStartTick, gSwitchTick ? nowTick - gSwitchTick : 0, app.frameMs));
    }
    AppSyncFpsText(app);
    AppSyncSchemeNameEdit(app);

    if (!app.ui.clicked.empty()) {
        AppHandleClick(app);
        app.ui.clicked.clear();
        app.dirty = true;
        InvalidateRect(app.hwnd, nullptr, FALSE);
    }
}

void AppBlurFpsEdit(App& app) {
    if (app.fpsEditFocus == 0) return;
    app.fpsEditFocus = 0;
    app.fpsCaretOn = true;
    app.dirty = true;
}

namespace {

std::wstring& FpsEditText(App& app) { return app.fpsEditFocus == 2 ? app.fpsBackText : app.fpsFrontText; }

}  // namespace

bool AppHandleFpsKey(App& app, WPARAM key) {
    if (app.fpsEditFocus == 0) return false;
    if (key == VK_ESCAPE || key == VK_RETURN) {
        AppBlurFpsEdit(app);
        return true;
    }
    if (key == VK_BACK || key == VK_DELETE) {
        std::wstring& text = FpsEditText(app);
        if (!text.empty()) text.pop_back();
        app.fpsCaretOn = true;
        app.fpsCaretTick = GetTickCount64();
        app.dirty = true;
        return true;
    }
    // 聚焦期间吞掉其它按键，避免触发全局快捷键（F5/F9 等）。
    return true;
}

bool AppHandleFpsChar(App& app, wchar_t ch) {
    if (app.fpsEditFocus == 0) return false;
    if (ch >= L'0' && ch <= L'9') {
        std::wstring& text = FpsEditText(app);
        if (text.size() < 3) {
            text.push_back(ch);
            app.fpsCaretOn = true;
            app.fpsCaretTick = GetTickCount64();
            app.dirty = true;
        }
        return true;
    }
    return true;  // 非数字输入一律忽略（含空格与注入字符）。
}

// 焦点不在输入框时把快照值同步到自绘文本，避免每帧覆盖玩家正在输入的内容。
void AppSyncFpsText(App& app) {
    if (!app.snapshotReady || !app.snapshot.fpsCap.exists) return;
    if (app.fpsEditFocus != 1 && app.syncedFpsFront != app.snapshot.fpsCap.front) {
        app.fpsFrontText = app.snapshot.fpsCap.front >= 0 ? std::to_wstring(app.snapshot.fpsCap.front) : L"";
        app.syncedFpsFront = app.snapshot.fpsCap.front;
    }
    if (app.fpsEditFocus != 2 && app.syncedFpsBack != app.snapshot.fpsCap.back) {
        app.fpsBackText = app.snapshot.fpsCap.back >= 0 ? std::to_wstring(app.snapshot.fpsCap.back) : L"";
        app.syncedFpsBack = app.snapshot.fpsCap.back;
    }
}

// 维护页「高级工具」里的方案名输入框：只在高级工具可见且页面给了矩形时显示。
// 文字不在这里回写（用户在输入过程中会失焦），保存方案时直接从控件读文本。
void AppSyncSchemeNameEdit(App& app) {
    if (app.editSchemeName == nullptr) return;
    const bool translationOnly = !app.pageAnimating || (app.pageAnimScale == 1.0f && app.pageAnimOpacity >= 0.999f);
    const bool want = translationOnly && app.page == Page::Maintenance && app.schemeNameVisible &&
                      !app.editSchemeNameRect.Empty() && app.dpiScale > 0.0f;
    if (!want) {
        ShowWindow(app.editSchemeName, SW_HIDE);
        return;
    }
    const float dpi = app.dpiScale;
    const float offset = app.pageAnimating ? app.pageAnimOffsetX : 0.0f;
    const Rect& rect = app.editSchemeNameRect;
    const int left = static_cast<int>(std::lround((rect.left + offset) * dpi)) + 1;
    const int top = static_cast<int>(std::lround(rect.top * dpi)) + 1;
    const int width = (std::max)(8, static_cast<int>(std::lround(rect.Width() * dpi)) - 2);
    const int height = (std::max)(8, static_cast<int>(std::lround(rect.Height() * dpi)) - 2);
    SetWindowPos(app.editSchemeName, HWND_TOP, left, top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// ===========================================================================
// 动作分发
// ===========================================================================
void AppHandleClick(App& app) {
    if (app.closeRequested) return;
    const std::string id = app.ui.clicked;
    if (id.empty()) return;
    DiagLine(Fmt(L"动作：id=%s page=%d wizardStep=%d", cc::Utf8ToWide(id).c_str(),
                 static_cast<int>(app.page), static_cast<int>(app.wizardStep)));

    // 过渡期间页面与页脚整体平移，登记过的命中矩形还停留在上一位置：
    // 这段时间只允许窗口按钮（标题栏不参与动画），其余点击丢弃，避免误触。
    if (app.pageAnimating && id.compare(0, 7, "chrome.") != 0) return;

    // 帧率输入框：点击聚焦；点击其它位置先让输入框失焦（文本保留）。
    if (id == "home.fpsfront" || id == "home.fpsback") {
        if (!app.snapshot.fpsCap.exists || !app.snapshot.clientValid || app.fpsSaving) return;
        app.fpsEditFocus = id == "home.fpsfront" ? 1 : 2;
        app.fpsCaretOn = true;
        app.fpsCaretTick = GetTickCount64();
        SetFocus(app.hwnd);
        app.dirty = true;
        return;
    }
    AppBlurFpsEdit(app);

    if (id == "chrome.min") {
        ShowWindow(app.hwnd, SW_MINIMIZE);
        return;
    }
    if (id == "chrome.max") {
        ShowWindow(app.hwnd, IsZoomed(app.hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        return;
    }
    if (id == "chrome.close") {
        PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
        return;
    }

    // ---------- 页脚 ----------
    if (id == "ftr.back") {
        AppGotoPage(app, Page::Home);
        return;
    }
    if (id == "ftr.switch") {
        AppGotoWizard(app, WizardStep::Client);
        return;
    }
    if (id == "ftr.maint") {
        if (app.snapshot.recordState == control_center::CapturedRecordState::Present) {
            AppGotoPage(app, Page::Maintenance);
        } else {
            AppSetStatus(app, L"当前客户端还没有安装记录。");
        }
        return;
    }
    if (id == "ftr.prev") {
        if (app.page == Page::Wizard && !AnyOperationRunning(app)) {
            if (app.wizardStep == WizardStep::Checks) {
                AppGotoWizard(app, WizardStep::Client);
            } else if (app.wizardStep == WizardStep::Files) {
                AppGotoWizard(app, WizardStep::Checks);
            }
        }
        return;
    }
    if (id == "ftr.next") {
        if (app.page == Page::Wizard) {
            if (AnyOperationRunning(app)) {
                AppSetStatus(app, L"正在执行写事务，请等待完成。");
                return;
            }
            if (app.wizardStep == WizardStep::Client) {
                if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
                    AppSetStatus(app, L"请先选择一个包含 wow.exe 与 Data 目录的客户端。");
                    return;
                }
                AppGotoWizard(app, WizardStep::Checks);
            } else if (app.wizardStep == WizardStep::Checks) {
                AppGotoWizard(app, WizardStep::Files);
            } else {
                app.wizardStep = WizardStep::Client;
                AppGotoPage(app, Page::Home);
            }
        }
        return;
    }
    if (id == "ftr.install") {
        if (app.page == Page::Wizard && app.wizardStep == WizardStep::Files) AppStartInstall(app);
        return;
    }
    if (id == "ftr.confirm") {
        if (app.page == Page::Uninstall) AppStartUninstall(app);
        return;
    }

    // 除“恢复安装前文件”的第二次确认外，任何动作都会取消待确认状态。
    if (app.restoreArmed && id != "maint.tools.restore") {
        app.restoreArmed = false;
        app.dirty = true;
    }

    // ---------- 主页 ----------
    if (id == "home.launch" || id == "home.panel") {
        AppLaunchOrPanel(app);
        return;
    }
    if (id == "home.switch") {
        AppGotoWizard(app, WizardStep::Client);
        return;
    }
    if (id == "home.refresh") {
        if (app.page == Page::Uninstall) {
            AppRequestSnapshot(app, false);
            AppRequestUninstallPreview(app);
        } else {
            AppRequestSnapshot(app, true);
            AppSetStatus(app, L"正在后台检查客户端状态与文件哈希…");
        }
        return;
    }
    if (id == "home.savefps") {
        AppSaveFpsCap(app);
        return;
    }
    if (id == "home.panellog") {
        AppOpenPanelLog(app);
        return;
    }
    if (id == "home.carrierlog") {
        AppOpenCarrierLog(app);
        return;
    }
    if (id == "logview.close") {
        AppCloseLogViewer(app);
        return;
    }

    // ---------- 向导：选择客户端 ----------
    if (id == "wiz.browse") {
        AppBrowseClient(app);
        return;
    }
    if (id == "wiz.browsestage") {
        AppBrowseStage(app);
        return;
    }
    if (id == "wiz.scan") {
        if (!app.scanLoading) {
            app.candidatesReady = false;
            AppRequestClientScan(app);
        }
        return;
    }
    if (id.rfind("wiz.recent.", 0) == 0 || id.rfind("wiz.cand.", 0) == 0) {
        const bool recent = id.rfind("wiz.recent.", 0) == 0;
        const size_t index = static_cast<size_t>(std::atoi(id.c_str() + (recent ? 11 : 9)));
        if (recent) {
            if (index < app.settings.recent.size()) {
                const std::wstring path = app.settings.recent[index].path;
                if (TestClientRoot(path)) {
                    AppSelectClient(app, path, true);
                } else {
                    AppSetStatus(app, L"该最近客户端目录已不可用：" + path);
                }
            }
        } else if (index < app.candidates.size()) {
            AppSelectClient(app, app.candidates[index].path, true);
        }
        return;
    }

    // ---------- 维护 ----------
    if (id == "maint.backup") {
        if (!app.clientRoot.empty()) {
            AppOpenFolder(app.clientRoot + L"\\host64\\dlss5-backup");
        }
        return;
    }
    if (id == "maint.uninstall") {
        AppGotoPage(app, Page::Uninstall);
        return;
    }
    if (id == "maint.check" || id == "maint.repair") {
        if (id == "maint.repair") {
            AppStartRepair(app);
            return;
        }
        if (AnyOperationRunning(app)) {
            AppSetStatus(app, L"正在执行写事务，请等待完成。");
            return;
        }
        AppRequestSnapshot(app, true);
        AppRequestInstallPreview(app);
        AppSetStatus(app, L"正在后台检查客户端状态与组件来源…");
        return;
    }

    // ---------- 维护：阶段二工具行 ----------
    if (id.rfind("maint.tools.", 0) == 0) {
        const std::string tool = id.substr(12);
        if (tool == "diagnostics") {
            AppExportDiagnostics(app);
            return;
        }
        if (tool == "manifest") {
            AppExportManifestList(app);
            return;
        }
        if (tool == "restore") {
            if (!app.restoreArmed) {
                app.restoreArmed = true;
                app.dirty = true;
                app.log.Add(L"恢复安装前文件需要二次确认：再次点击后按最近一次安装事务撤销写入。");
                AppSetStatus(app, L"再次点击「恢复安装前文件」确认撤销最近一次安装。");
                return;
            }
            app.restoreArmed = false;
            app.dirty = true;
            AppRestoreInstall(app);
            return;
        }
        if (tool == "panel") {
            AppStartPanel(app);
            return;
        }
        if (tool == "watch") {
            AppStartWindowWatch(app);
            return;
        }
        if (tool == "pipeline") {
            AppTogglePipeline(app);
            return;
        }
        AppSetStatus(app, L"未知的工具操作。");
        return;
    }

    // ---------- 维护：高级工具（ReShade.ini 档位/参数/方案） ----------
    if (id.rfind("maint.adv.", 0) == 0) {
        const std::string action = id.substr(10);
        if (action == "read") {
            AppRequestAdvanced(app);
            return;
        }
        if (action == "save") {
            AppSaveAdvanced(app);
            return;
        }
        if (action == "defaults") {
            AppRestoreCandidateDefaults(app);
            return;
        }
        if (action == "schemes.refresh") {
            AppRequestSchemes(app);
            return;
        }
        if (action == "scheme.save") {
            AppSaveScheme(app);
            return;
        }
        if (action == "fill") {
            if (!app.advanced.loaded || !app.advanced.settings.readable) {
                AppSetStatus(app, L"还没有读到当前参数；请先点「读取当前值」。");
                return;
            }
            app.advanced.edit = Ds5ValuesFrom(app.advanced.settings);
            app.advanced.editDetached =
                app.advanced.settings.detachedPresent ? app.advanced.settings.detachedPresentation : true;
            app.advanced.editOpticalFlow =
                app.advanced.settings.opticalPresent ? app.advanced.settings.opticalFlow : false;
            app.advanced.status = L"已填入当前客户端的值（尚未写入）。";
            app.dirty = true;
            return;
        }
        if (action == "nr.toggle") {
            app.advanced.edit.neuralRendering = !app.advanced.edit.neuralRendering;
            app.advanced.status = L"已切换补帧开关（尚未写入）。";
            app.dirty = true;
            return;
        }
        if (action == "detached.toggle") {
            app.advanced.editDetached = !app.advanced.editDetached;
            app.advanced.status = L"已切换独立呈现开关（尚未写入）。";
            app.dirty = true;
            return;
        }
        if (action == "optical.toggle") {
            app.advanced.editOpticalFlow = !app.advanced.editOpticalFlow;
            app.advanced.status = L"已切换 Nvidia Optical Flow 开关（尚未写入）。";
            app.dirty = true;
            return;
        }
        if (action.rfind("tier.", 0) == 0) {
            const size_t index = static_cast<size_t>(std::atoi(action.c_str() + 5));
            if (index < Ds5Tiers().size()) {
                const auto& tier = Ds5Tiers()[index];
                app.advanced.edit.neuralRendering = tier.neuralRendering;
                app.advanced.edit.intensity = tier.intensity;
                app.advanced.edit.localTone = tier.localTone;
                app.advanced.edit.localStructure = tier.localStructure;
                app.advanced.status = std::wstring(L"已套用档位「") + tier.name + L"」（尚未写入）。";
                app.dirty = true;
            }
            return;
        }
        if (action.rfind("scheme.apply.", 0) == 0) {
            const size_t index = static_cast<size_t>(std::atoi(action.c_str() + 13));
            if (index < app.advanced.schemes.size()) AppApplyScheme(app, app.advanced.schemes[index].path);
            return;
        }
        const bool plus = action.size() > 5 && action.compare(action.size() - 5, 5, ".plus") == 0;
        const bool minus = action.size() > 6 && action.compare(action.size() - 6, 6, ".minus") == 0;
        if (plus || minus) {
            const double delta = plus ? 1.0 : -1.0;
            const std::string key = action.substr(0, action.size() - (plus ? 5 : 6));
            auto step = [delta](double& value, double amount, double minimum, double maximum) {
                value += delta * amount;
                if (value < minimum) value = minimum;
                if (value > maximum) value = maximum;
            };
            if (key == "model") {
                int model = app.advanced.edit.model + (plus ? 1 : -1);
                if (model < 1) model = 1;
                if (model > 3) model = 3;
                app.advanced.edit.model = model;
            } else if (key == "intensity") {
                step(app.advanced.edit.intensity, 0.05, 0.0, 1.0);
            } else if (key == "tone") {
                step(app.advanced.edit.localTone, 0.10, 0.0, 2.0);
            } else if (key == "structure") {
                step(app.advanced.edit.localStructure, 0.05, 0.0, 1.0);
            } else if (key == "skin") {
                step(app.advanced.edit.skinStructure, 0.10, -1.0, 1.0);
            } else {
                return;
            }
            app.advanced.status = L"参数已修改（尚未写入）。";
            app.dirty = true;
            return;
        }
        return;
    }
}

// ===========================================================================
// 动作实现
// ===========================================================================
void AppBrowseClient(App& app) {
    const std::wstring picked = PickFolder(app.hwnd, L"选择客户端根目录（包含 wow.exe 与 Data）");
    if (picked.empty()) return;
    AppSelectClient(app, picked, true);
}

void AppBrowseStage(App& app) {
    const std::wstring picked = PickFolder(app.hwnd, L"选择组件来源目录（包含 install-manifest.json）");
    if (picked.empty()) return;
    app.stageRoot = picked;
    app.settings.stage = picked;
    if (!app.paths.settingsPath.empty() && !SaveSettings(app.paths, app.settings)) {
        app.log.Add(L"设置保存失败：" + app.paths.settingsPath);
    }
    AppSetStatus(app, L"已选择组件来源：" + picked);
    if (app.page == Page::Wizard) {
        AppRequestInstallPreview(app);
    }
    app.dirty = true;
}

void AppLaunchGame(App& app) {
    if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
        AppSetStatus(app, L"客户端目录无效，无法启动游戏。");
        return;
    }
    const std::wstring exe = app.clientRoot + L"\\wow.exe";
    std::wstring command = L"\"" + exe + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        app.clientRoot.c_str(), &startup, &process)) {
        AppSetStatus(app, Fmt(L"启动游戏失败（Win32 %lu）。", GetLastError()));
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    AppSetStatus(app, L"已启动游戏，正在等待进程状态更新。");
}

namespace {

// Parses one FPS input: decimal digits only, 0..999, surrounding whitespace ignored.
// Empty text is accepted as 0 (0 means unlimited).
bool ParseFpsText(const std::wstring& input, int& value) {
    std::wstring text = input;
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.erase(text.begin());
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.pop_back();
    if (text.empty()) {
        value = 0;
        return true;
    }
    if (text.size() > 3) return false;
    int parsed = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        parsed = parsed * 10 + (c - L'0');
    }
    if (parsed > 999) return false;
    value = parsed;
    return true;
}

}  // namespace

void AppSaveFpsCap(App& app) {
    if (app.fpsSaving) return;
    if (app.clientRoot.empty() || !app.snapshotReady || !app.snapshot.clientValid) {
        AppSetStatus(app, L"请先选择有效的客户端目录。");
        return;
    }
    if (!app.snapshot.fpsCap.exists) {
        app.log.Add(L"操作未完成：客户端内没有 WTF\\Config.wtf，请先启动一次游戏。");
        AppSetStatus(app, L"操作未完成：客户端内没有 WTF\\Config.wtf，请先启动一次游戏。");
        app.dirty = true;
        return;
    }
    int front = 0;
    int back = 0;
    if (!ParseFpsText(app.fpsFrontText, front)) {
        app.log.Add(L"操作未完成：前台帧率上限需为整数。");
        AppSetStatus(app, L"操作未完成：前台帧率上限需为整数。");
        app.dirty = true;
        return;
    }
    if (!ParseFpsText(app.fpsBackText, back)) {
        app.log.Add(L"操作未完成：后台帧率上限需为整数。");
        AppSetStatus(app, L"操作未完成：后台帧率上限需为整数。");
        app.dirty = true;
        return;
    }
    app.fpsSaving = true;
    app.dirty = true;
    const std::wstring clientRoot = app.clientRoot;
    const std::uint64_t generation = app.generation;
    auto result = std::make_shared<control_center::ConfigWtfWriteResult>();
    const bool posted = app.jobs.Post(
        [clientRoot, front, back, result]() { *result = control_center::SaveConfigFpsCap(clientRoot, front, back); },
        [&app, result, front, back, generation, clientRoot](const std::wstring& error) {
            if (generation != app.generation || !SamePath(clientRoot, app.clientRoot)) return;
            app.fpsSaving = false;
            app.dirty = true;
            if (!error.empty()) {
                app.log.Add(L"操作未完成：" + error);
                AppSetStatus(app, L"操作未完成：" + error);
                return;
            }
            std::wstring message;
            switch (result->error) {
                case control_center::ConfigWtfWriteError::None:
                    app.log.Add(Fmt(L"已写入源帧率上限：前台 %d，后台 %d；备份：%ls", front, back,
                                    result->backupPath.c_str()));
                    AppSetStatus(app, L"已保存；重启游戏后生效。");
                    AppRequestSnapshot(app, false);
                    return;
                case control_center::ConfigWtfWriteError::InvalidRange:
                    message = L"帧率上限需在 0 到 999 之间。";
                    break;
                case control_center::ConfigWtfWriteError::MissingFile:
                    message = L"客户端内没有 WTF\\Config.wtf，请先启动一次游戏。";
                    break;
                case control_center::ConfigWtfWriteError::GameRunning:
                    message = L"请先退出游戏再写入 Config.wtf。";
                    break;
                case control_center::ConfigWtfWriteError::VerifyFront:
                    message = L"Config.wtf 写入后校验失败，已保留备份。";
                    break;
                case control_center::ConfigWtfWriteError::VerifyBack:
                    message = L"Config.wtf 后台帧率写入后校验失败，已保留备份。";
                    break;
                default:
                    message = Fmt(L"写入 Config.wtf 失败（Win32 %lu）。", result->win32Error);
                    break;
            }
            app.log.Add(L"操作未完成：" + message);
            AppSetStatus(app, L"操作未完成：" + message);
        });
    if (!posted) {
        app.fpsSaving = false;
        AppSetStatus(app, L"后台任务队列已满，请稍后重试。");
    }
}

namespace {

// 保存文件对话框：只问位置，不询问覆盖（已存在时由写入函数返回“已存在”错误）。
std::wstring PickSavePath(HWND owner, const wchar_t* title, const wchar_t* defaultName) {
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
        dialog == nullptr) {
        return std::wstring();
    }
    std::wstring result;
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions((options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST) & ~FOS_OVERWRITEPROMPT);
    }
    dialog->SetTitle(title);
    dialog->SetFileName(defaultName);
    COMDLG_FILTERSPEC filter = {L"所有文件", L"*.*"};
    dialog->SetFileTypes(1, &filter);
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

}  // namespace

void AppShowLog(App& app, const std::wstring& title, const std::vector<std::wstring>& lines) {
    app.logViewer.visible = true;
    app.logViewer.title = title;
    app.logViewer.lines = lines;
    app.dirty = true;
}

void AppCloseLogViewer(App& app) {
    app.logViewer.visible = false;
    app.logViewer.lines.clear();
    app.dirty = true;
}

void AppLaunchOrPanel(App& app) {
    if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
        AppSetStatus(app, L"请先在「切换客户端」中选择含 wow.exe 的目录。");
        return;
    }
    // R6：主按钮只跟随“所选客户端”的游戏进程（app.gameRunning 由按客户端路径的
    // 进程关联得到）；系统级进程快照只用于安装/修复/卸载等破坏性操作的拦截。
    const bool running = app.gameRunning;
    if (running) {
        AppStartPanel(app);
        return;
    }
    if (app.toolRun.running) {
        AppSetStatus(app, L"另一个工具操作正在执行，请等待完成。");
        return;
    }
    app.toolRun = App::ToolRun{};
    app.toolRun.running = true;
    app.toolRun.action = L"game";
    app.toolRun.token = ++app.toolToken;
    const std::uint64_t token = app.toolRun.token;
    app.dirty = true;
    auto result = std::make_shared<GameStartTaskResult>();
    const std::wstring client = app.clientRoot;
    const bool posted = app.jobs.Post(
        [client, result]() { *result = RunStartGameTask(client); },
        [&app, token, result](const std::wstring& error) {
            if (token != app.toolRun.token) return;
            app.toolRun.running = false;
            app.toolRun.finished = true;
            app.dirty = true;
            if (!error.empty()) {
                app.toolRun.error = error;
                app.log.Add(L"操作未完成：" + error);
                AppSetStatus(app, L"操作未完成：" + error);
                return;
            }
            if (!result->ok) {
                app.toolRun.error = result->error;
                app.log.Add(L"操作未完成：" + result->error);
                AppSetStatus(app, L"操作未完成：" + result->error);
                return;
            }
            app.toolRun.ok = true;
            app.panelAutoLaunchTried = false;
            app.lastPanelCheck = GetTickCount64();
            app.log.Add(L"已启动游戏，等待面板上报实时数据。");
            AppSetStatus(app, L"已启动游戏，等待面板上报实时数据。");
            AppRequestSnapshot(app, false);
        });
    if (!posted) {
        app.toolRun = App::ToolRun{};
        AppSetStatus(app, L"后台任务队列已满，请稍后重试。");
    }
}

void AppStartPanel(App& app) {
    const bool autoRequest = app.panelAutoRequest;
    app.panelAutoRequest = false;
    if (app.toolRun.running) {
        AppSetStatus(app, L"另一个工具操作正在执行，请等待完成。");
        return;
    }
    if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
        AppSetStatus(app, autoRequest ? L"面板自动拉起未执行：请先选择含 wow.exe 的客户端目录。"
                                      : L"请先选择含 wow.exe 的客户端目录。");
        return;
    }
    app.toolRun = App::ToolRun{};
    app.toolRun.running = true;
    app.toolRun.action = L"panel";
    app.toolRun.token = ++app.toolToken;
    const std::uint64_t token = app.toolRun.token;
    app.dirty = true;
    auto result = std::make_shared<control_center::PanelStartResult>();
    const std::wstring exeDir = app.paths.exeDir;
    const std::wstring dataDir = app.paths.dataDir;
    const std::wstring client = app.clientRoot;
    const bool posted = app.jobs.Post(
        [exeDir, dataDir, client, result]() {
            // R6：只有所选客户端的 wow.exe 在运行时才允许启动/复用面板，避免把面板
            // 常驻在错误的客户端上并占用 DLSS5PanelMutex；同时可选校验面板哈希。
            *result = control_center::panel_detail::StartPanel(
                exeDir, dataDir, client, control_center::PanelStartOptions{true, true});
        },
        [&app, token, result, autoRequest](const std::wstring& error) {
            if (token != app.toolRun.token) return;
            app.toolRun.running = false;
            app.toolRun.finished = true;
            app.dirty = true;
            if (!error.empty()) {
                const std::wstring message = (autoRequest ? L"面板自动拉起未执行：" : L"操作未完成：") + error;
                app.toolRun.error = error;
                app.log.Add(message);
                AppSetStatus(app, message);
                return;
            }
            if (!result->hashWarning.empty()) app.log.Add(result->hashWarning);
            if (result->status == control_center::PanelStartStatus::AlreadyRunning) {
                // 面板已经在运行且已连接所选客户端：视为“已连接”，不算失败。
                app.toolRun.ok = true;
                if (!result->logDir.empty()) app.lastPanelLog = result->logDir;
                app.log.Add(L"游戏内面板已在运行，已连接所选客户端。");
                AppSetStatus(app, L"已连接游戏内面板。");
                return;
            }
            const std::wstring failure = result->ok ? std::wstring() : result->error;
            if (!failure.empty()) {
                const std::wstring message = (autoRequest ? L"面板自动拉起未执行：" : L"操作未完成：") + failure;
                app.toolRun.error = failure;
                app.log.Add(message);
                AppSetStatus(app, message);
                return;
            }
            app.toolRun.ok = true;
            app.lastPanelLog = result->logDir;
            if (autoRequest) {
                app.log.Add(L"检测到游戏运行但面板未启动，已自动拉起。日志：" + result->logDir);
                AppSetStatus(app, L"已自动拉起游戏内面板。");
            } else {
                app.log.Add(L"已启动游戏内面板。日志：" + result->logDir);
                AppSetStatus(app, L"已启动游戏内面板。");
            }
        });
    if (!posted) {
        app.toolRun = App::ToolRun{};
        AppSetStatus(app, L"后台任务队列已满，请稍后重试。");
    }
}
void AppOpenPanelLog(App& app) {
    if (app.lastPanelLog.empty()) {
        AppSetStatus(app, L"本次窗口尚未启动面板。");
        return;
    }
    if (app.toolRun.running) return;
    app.toolRun = App::ToolRun{};
    app.toolRun.running = true;
    app.toolRun.action = L"panellog";
    app.toolRun.token = ++app.toolToken;
    const std::uint64_t token = app.toolRun.token;
    app.dirty = true;
    auto result = std::make_shared<LogTailTaskResult>();
    const std::wstring logDir = app.lastPanelLog;
    const bool posted = app.jobs.Post(
        [logDir, result]() { *result = RunLogTailTask(logDir); },
        [&app, token, result](const std::wstring&) {
            if (token != app.toolRun.token) return;
            app.toolRun.running = false;
            app.toolRun.finished = true;
            app.dirty = true;
            if (!result->ok) {
                AppSetStatus(app, L"操作未完成：" + (result->error.empty() ? std::wstring(L"读取日志失败。") : result->error));
                return;
            }
            AppShowLog(app, L"悬浮面板日志", result->lines);
        });
    if (!posted) {
        app.toolRun = App::ToolRun{};
        AppSetStatus(app, L"后台任务队列已满，请稍后重试。");
    }
}

void AppOpenCarrierLog(App& app) {
    if (app.lastWatchLog.empty()) {
        AppSetStatus(app, L"本次窗口尚未启动助手。");
        return;
    }
    if (app.toolRun.running) return;
    app.toolRun = App::ToolRun{};
    app.toolRun.running = true;
    app.toolRun.action = L"carrierlog";
    app.toolRun.token = ++app.toolToken;
    const std::uint64_t token = app.toolRun.token;
    app.dirty = true;
    auto result = std::make_shared<LogTailTaskResult>();
    const std::wstring logDir = app.lastWatchLog;
    const bool posted = app.jobs.Post(
        [logDir, result]() { *result = RunLogTailTask(logDir); },
        [&app, token, result](const std::wstring&) {
            if (token != app.toolRun.token) return;
            app.toolRun.running = false;
            app.toolRun.finished = true;
            app.dirty = true;
            if (!result->ok) {
                AppSetStatus(app, L"操作未完成：" + (result->error.empty() ? std::wstring(L"读取日志失败。") : result->error));
                return;
            }
            AppShowLog(app, L"载体日志", result->lines);
        });
    if (!posted) {
        app.toolRun = App::ToolRun{};
        AppSetStatus(app, L"后台任务队列已满，请稍后重试。");
    }
}

void AppRequestGpuStats(App& app) {
    if (app.closeRequested) return;
    if (app.gpuPolling) return;
    app.gpuPolling = true;
    auto result = std::make_shared<GpuStatsTaskResult>();
    const bool posted = app.jobs.Post(
        [result]() { *result = RunGpuStatsTask(); },
        [&app, result](const std::wstring&) {
            app.gpuPolling = false;
            app.gpuStats = result->stats;
            app.dirty = true;
        });
    if (!posted) app.gpuPolling = false;
}

void AppExportDiagnostics(App& app) {
    if (app.toolRun.running) {
        AppSetStatus(app, L"另一个工具操作正在执行，请等待完成。");
        return;
    }
    if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
        AppSetStatus(app, L"请先在「切换客户端」中选择含 wow.exe 的目录。");
        return;
    }
    if (!app.catalog.ok) {
        AppSetStatus(app, L"安装清单无效，无法导出诊断。");
        return;
    }
    const std::wstring path = PickSavePath(app.hwnd, L"导出诊断信息", L"dlss5-diagnostic.json");
    if (path.empty()) return;
    std::vector<std::wstring> devices;
    if (app.snapshot.gpu.ok && !app.snapshot.gpu.name.empty()) devices.push_back(app.snapshot.gpu.name);
    app.toolRun = App::ToolRun{};
    app.toolRun.running = true;
    app.toolRun.action = L"diagnostics";
    app.toolRun.token = ++app.toolToken;
    const std::uint64_t token = app.toolRun.token;
    app.dirty = true;
    auto result = std::make_shared<DiagnosticsTaskResult>();
    const std::wstring client = app.clientRoot;
    const CatalogResult catalog = app.catalog.catalog;
    const TelemetrySample telemetry = app.telemetry;
    const bool posted = app.jobs.Post(
        [client, catalog, path, telemetry, devices, result]() {
            const ProcessAssociation association = AssociateClientProcesses(client);
            *result = RunDiagnosticsExportTask(client, catalog, path, association.games, association.hosts,
                                              association.unidentified, telemetry, devices);
        },
        [&app, token, result](const std::wstring& error) {
            if (token != app.toolRun.token) return;
            app.toolRun.running = false;
            app.toolRun.finished = true;
            app.dirty = true;
            if (!error.empty() || !result->ok) {
                const std::wstring message = !error.empty() ? error : result->error;
                app.toolRun.error = message;
                app.log.Add(L"操作未完成：" + message);
                AppSetStatus(app, L"操作未完成：" + message);
                return;
            }
            app.toolRun.ok = true;
            app.log.Add(L"诊断已保存：" + result->path);
            AppSetStatus(app, L"诊断已保存。");
        });
    if (!posted) {
        app.toolRun = App::ToolRun{};
        AppSetStatus(app, L"后台任务队列已满，请稍后重试。");
    }
}

void AppExportManifestList(App& app) {
    if (app.toolRun.running) {
        AppSetStatus(app, L"另一个工具操作正在执行，请等待完成。");
        return;
    }
    if (!app.catalog.ok) {
        AppSetStatus(app, L"安装清单无效，无法导出。");
        return;
    }
    const std::wstring path = PickSavePath(app.hwnd, L"导出移入清单", L"DLSS5移入清单.md");
    if (path.empty()) return;
    app.toolRun = App::ToolRun{};
    app.toolRun.running = true;
    app.toolRun.action = L"manifest";
    app.toolRun.token = ++app.toolToken;
    const std::uint64_t token = app.toolRun.token;
    app.dirty = true;
    auto result = std::make_shared<ManifestListTaskResult>();
    const std::wstring client = app.clientRoot;
    const std::wstring manifestPath = app.catalog.manifestPath;
    const CatalogResult catalog = app.catalog.catalog;
    const bool posted = app.jobs.Post(
        [client, manifestPath, catalog, path, result]() {
            *result = RunManifestListTask(client, manifestPath, catalog, path);
        },
        [&app, token, result](const std::wstring& error) {
            if (token != app.toolRun.token) return;
            app.toolRun.running = false;
            app.toolRun.finished = true;
            app.dirty = true;
            if (!error.empty() || !result->ok) {
                const std::wstring message = !error.empty() ? error : result->error;
                app.toolRun.error = message;
                app.log.Add(L"操作未完成：" + message);
                AppSetStatus(app, L"操作未完成：" + message);
                return;
            }
            app.toolRun.ok = true;
            app.log.Add(L"移入清单已导出：" + result->path);
            AppSetStatus(app, L"移入清单已导出。");
        });
    if (!posted) {
        app.toolRun = App::ToolRun{};
        AppSetStatus(app, L"后台任务队列已满，请稍后重试。");
    }
}

void AppRestoreInstall(App& app) {
    if (app.toolRun.running) {
        AppSetStatus(app, L"另一个工具操作正在执行，请等待完成。");
        return;
    }
    if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
        AppSetStatus(app, L"请先在「切换客户端」中选择含 wow.exe 的目录。");
        return;
    }
    if (!app.catalog.ok) {
        AppSetStatus(app, L"安装清单无效，无法恢复安装前文件。");
        return;
    }
    app.toolRun = App::ToolRun{};
    app.toolRun.running = true;
    app.toolRun.action = L"restore";
    app.toolRun.token = ++app.toolToken;
    const std::uint64_t token = app.toolRun.token;
    app.dirty = true;
    auto result = std::make_shared<RestoreTaskResult>();
    const std::wstring client = app.clientRoot;
    const std::wstring operationsRoot = OperationsRoot(app.paths);
    const std::vector<std::wstring> protectedRoots = app.protectedRoots;
    const CatalogResult catalog = app.catalog.catalog;
    const bool posted = app.jobs.Post(
        [client, operationsRoot, protectedRoots, catalog, result]() {
            *result = RunRestoreTask(client, operationsRoot, protectedRoots, catalog);
        },
        [&app, token, result](const std::wstring& error) {
            if (token != app.toolRun.token) return;
            app.toolRun.running = false;
            app.toolRun.finished = true;
            app.dirty = true;
            if (!error.empty() || !result->ok) {
                const std::wstring message = !error.empty() ? error : result->error;
                app.toolRun.error = message;
                app.log.Add(L"操作未完成：" + message);
                AppSetStatus(app, L"操作未完成：" + message);
                return;
            }
            app.toolRun.ok = true;
            const std::wstring summary = Fmt(L"恢复状态：%ls；记录：%ls", result->status.c_str(), result->report.c_str());
            app.log.Add(L"已恢复安装前文件：" + summary);
            AppSetStatus(app, L"已恢复安装前文件。");
            AppRequestSnapshot(app, true);
            // 还原可能改写或删除 host64\ReShade.ini：重新读取高级参数，
            // 避免维护页继续显示旧档位/旧数值。
            if (!app.clientRoot.empty()) AppRequestAdvanced(app);
        });
    if (!posted) {
        app.toolRun = App::ToolRun{};
        AppSetStatus(app, L"后台任务队列已满，请稍后重试。");
    }
}

void AppStartWindowWatch(App& app) {
    if (app.toolRun.running) {
        AppSetStatus(app, L"另一个工具操作正在执行，请等待完成。");
        return;
    }
    if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
        AppSetStatus(app, L"请先选择含 wow.exe 的客户端目录。");
        return;
    }
    app.toolRun = App::ToolRun{};
    app.toolRun.running = true;
    app.toolRun.action = L"watch";
    app.toolRun.token = ++app.toolToken;
    const std::uint64_t token = app.toolRun.token;
    app.dirty = true;
    auto result = std::make_shared<WindowWatchTaskResult>();
    const std::wstring productRoot = app.paths.exeDir;
    const std::wstring operationsRoot = OperationsRoot(app.paths);
    const std::wstring client = app.clientRoot;
    const bool posted = app.jobs.Post(
        [productRoot, operationsRoot, client, result]() {
            *result = RunWindowWatchTask(productRoot, operationsRoot, client);
        },
        [&app, token, result](const std::wstring& error) {
            if (token != app.toolRun.token) return;
            app.toolRun.running = false;
            app.toolRun.finished = true;
            app.dirty = true;
            if (!error.empty() || !result->ok) {
                const std::wstring message = !error.empty() ? error : result->error;
                app.toolRun.error = message;
                app.log.Add(L"操作未完成：" + message);
                AppSetStatus(app, L"操作未完成：" + message);
                return;
            }
            app.toolRun.ok = true;
            app.lastWatchLog = result->logDir;
            app.log.Add(result->message.empty() ? L"已启动窗口助手。" : result->message);
            AppSetStatus(app, result->message.empty() ? L"已启动窗口助手。" : result->message);
        });
    if (!posted) {
        app.toolRun = App::ToolRun{};
        AppSetStatus(app, L"后台任务队列已满，请稍后重试。");
    }
}

void AppTogglePipeline(App& app) {
    if (app.toolRun.running) {
        AppSetStatus(app, L"另一个工具操作正在执行，请等待完成。");
        return;
    }
    if (app.clientRoot.empty() || !TestClientRoot(app.clientRoot)) {
        AppSetStatus(app, L"请先在「切换客户端」中选择含 wow.exe 的目录。");
        return;
    }
    app.toolRun = App::ToolRun{};
    app.toolRun.running = true;
    app.toolRun.action = L"pipeline";
    app.toolRun.token = ++app.toolToken;
    const std::uint64_t token = app.toolRun.token;
    app.dirty = true;
    auto result = std::make_shared<PipelineToggleTaskResult>();
    const std::wstring client = app.clientRoot;
    const std::wstring operationsRoot = OperationsRoot(app.paths);
    const bool posted = app.jobs.Post(
        [client, operationsRoot, result]() { *result = RunPipelineToggleTask(client, operationsRoot); },
        [&app, token, result](const std::wstring& error) {
            if (token != app.toolRun.token) return;
            app.toolRun.running = false;
            app.toolRun.finished = true;
            app.dirty = true;
            if (!error.empty() || !result->ok) {
                const std::wstring message = !error.empty() ? error : result->error;
                app.toolRun.error = message;
                app.log.Add(L"操作未完成：" + message);
                AppSetStatus(app, L"操作未完成：" + message);
                return;
            }
            app.toolRun.ok = true;
            if (result->changed) {
                const std::wstring message = result->enabled == 1 ? L"已保存：启用 AIO 管线；重启游戏后生效。"
                                                                 : L"已保存：停用 AIO 管线；重启游戏后生效。";
                app.log.Add(message);
                AppSetStatus(app, message);
            } else {
                app.log.Add(L"AIO 管线开关没有变化。");
                AppSetStatus(app, L"AIO 管线开关没有变化。");
            }
        });
    if (!posted) {
        app.toolRun = App::ToolRun{};
        AppSetStatus(app, L"后台任务队列已满，请稍后重试。");
    }
}
void AppOpenFolder(const std::wstring& path) {
    if (path.empty()) return;
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        if (gApp) AppSetStatus(*gApp, Fmt(L"打开目录失败（%lld）：%s", static_cast<long long>(reinterpret_cast<INT_PTR>(result)),
                                          path.c_str()));
    }
}

bool AppCanInstall(const App& app) {
    if (app.clientRoot.empty()) return false;
    if (!app.snapshotReady || !app.snapshot.clientValid) return false;
    if (app.snapshot.processState != control_center::ProcessState::Stopped) return false;
    if (!app.install.ready || !app.install.ok) return false;
    if (!app.install.preview.filesReady) return false;
    for (const auto& item : app.install.preview.items) {
        if (item.state == control_center::PreviewState::Invalid) return false;
    }
    return true;
}

bool AppCanUninstall(const App& app) {
    return app.uninstall.ready && app.uninstall.ok && app.uninstall.preview.valid &&
           app.uninstall.preview.canUninstall;
}

// ===========================================================================
// 文件夹选择（系统 IFileDialog，保留中文输入法与系统交互）
// ===========================================================================
std::wstring PickFolder(HWND owner, const wchar_t* title) {
    IFileOpenDialog* dialog = nullptr;
    const HRESULT created =
        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(created) || dialog == nullptr) return std::wstring();

    std::wstring result;
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(title);
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

}  // namespace cc

// ===========================================================================
// 窗口过程
// ===========================================================================
namespace {

void CreateFpsEdits(cc::App& app) {
    // 帧率输入框改为自绘（主窗口处理键盘输入），这里只保留方案名输入框。
    // 方案名输入框：允许中文与空格，不能只有数字。
    const DWORD schemeStyle = WS_CHILD | ES_AUTOHSCROLL;
    app.editSchemeName = CreateWindowExW(0, L"EDIT", L"", schemeStyle, 0, 0, 10, 10, app.hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(1003)), app.instance, nullptr);
    if (app.editSchemeName) {
        SendMessageW(app.editSchemeName, EM_SETLIMITTEXT, 24, 0);
        SendMessageW(app.editSchemeName, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
    }
}

void UpdateEditFont(cc::App& app) {
    if (app.editFont) {
        DeleteObject(app.editFont);
        app.editFont = nullptr;
    }
    const int height = -static_cast<int>(std::lround(12.0f * app.layoutScale * app.dpiScale));
    app.editFont = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               L"Microsoft YaHei UI");
    HWND controls[1] = {app.editSchemeName};
    for (HWND control : controls) {
        if (control && app.editFont) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.editFont), TRUE);
    }
}

float SystemDpiScale() {
    using GetDpiForSystemFn = UINT(WINAPI*)();
    const auto getDpi = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                                          "GetDpiForSystem"));
    if (getDpi != nullptr) {
        const UINT dpi = getDpi();
        if (dpi >= 72) return static_cast<float>(dpi) / 96.0f;
    }
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

float WindowDpiScale(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    const auto getDpi = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                                         "GetDpiForWindow"));
    if (getDpi != nullptr) {
        const UINT dpi = getDpi(hwnd);
        if (dpi >= 72) return static_cast<float>(dpi) / 96.0f;
    }
    return SystemDpiScale();
}

void EnableDpiAwareness() {
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        const auto setContext = reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext != nullptr) {
            if (setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
        }
        using SetAwareFn = BOOL(WINAPI*)();
        const auto setAware = reinterpret_cast<SetAwareFn>(GetProcAddress(user32, "SetProcessDPIAware"));
        if (setAware != nullptr) setAware();
    }
}

float DipFromPixels(const cc::App& app, int pixels) {
    const float dpi = app.dpiScale > 0.0f ? app.dpiScale : 1.0f;
    return static_cast<float>(pixels) / dpi;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NCCALCSIZE:
            // 无边框窗口：客户区覆盖整个窗口，最大化时仍受 WM_GETMINMAXINFO 的工作区限制。
            if (wParam == TRUE) return 0;
            break;
        case WM_GETMINMAXINFO: {
            if (gApp == nullptr) break;
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const float scale = (gApp->dpiScale > 0.0f ? gApp->dpiScale : 1.0f) * gApp->layoutScale;
            info->ptMinTrackSize.x = static_cast<LONG>(std::lround(cc::theme::kMinWindowWidth * scale));
            info->ptMinTrackSize.y = static_cast<LONG>(std::lround(cc::theme::kMinWindowHeight * scale));
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
                info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
                info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
                info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
            }
            return 0;
        }
        case WM_NCHITTEST: {
            if (!gReady || gApp == nullptr) break;
            POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT clientPoint = screenPoint;
            ScreenToClient(hwnd, &clientPoint);
            RECT clientRect{};
            GetClientRect(hwnd, &clientRect);
            const float dpi = gApp->dpiScale > 0.0f ? gApp->dpiScale : 1.0f;
            const int border = (std::max)(4, static_cast<int>(std::lround(4.0 * dpi)));
            if (clientPoint.y < border) return HTTOP;
            if (clientPoint.y >= clientRect.bottom - border) return HTBOTTOM;
            if (clientPoint.x < border) return HTLEFT;
            if (clientPoint.x >= clientRect.right - border) return HTRIGHT;
            const float x = DipFromPixels(*gApp, clientPoint.x);
            const float y = DipFromPixels(*gApp, clientPoint.y);
            if (y < cc::theme::kTitleBarHeight * cc::AppScale(*gApp)) {
                if (cc::AppChromeHitTest(*gApp, x, y) != 0) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_CTLCOLOREDIT: {
            if (gEditBrush == nullptr) {
                const unsigned surface = cc::theme::kSurface3;
                gEditBrush = CreateSolidBrush(RGB(Channel(surface, 16), Channel(surface, 8), Channel(surface, 0)));
            }
            HDC dc = reinterpret_cast<HDC>(wParam);
            const unsigned ink = cc::theme::kText;
            SetTextColor(dc, RGB(Channel(ink, 16), Channel(ink, 8), Channel(ink, 0)));
            SetBkColor(dc, RGB(Channel(cc::theme::kSurface3, 16), Channel(cc::theme::kSurface3, 8),
                               Channel(cc::theme::kSurface3, 0)));
            return reinterpret_cast<LRESULT>(gEditBrush);
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (gReady && gApp != nullptr) {
                gApp->gfx.Resize(LOWORD(lParam), HIWORD(lParam));
                gApp->dirty = true;
            }
            return 0;
        case WM_MOVE:
        case WM_WINDOWPOSCHANGED:
            break;
        case WM_DPICHANGED: {
            if (!gReady || gApp == nullptr) break;
            gApp->dpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
            gApp->gfx.SetDpi(gApp->dpiScale * 96.0f);
            UpdateEditFont(*gApp);
            auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            gApp->dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            if (gReady && gApp != nullptr) cc::AppRender(*gApp);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!gReady || gApp == nullptr) return 0;
            const int px = GET_X_LPARAM(lParam);
            const int py = GET_Y_LPARAM(lParam);
            gApp->ui.OnMouseMove(DipFromPixels(*gApp, px), DipFromPixels(*gApp, py));
            if (!gTrackingMouse) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                gTrackingMouse = true;
            }
            gApp->dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (gReady && gApp != nullptr) {
                gTrackingMouse = false;
                gApp->ui.OnMouseLeave();
                gApp->dirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!gReady || gApp == nullptr) return 0;
            SetFocus(hwnd);
            SetCapture(hwnd);
            const int px = GET_X_LPARAM(lParam);
            const int py = GET_Y_LPARAM(lParam);
            gApp->ui.OnMouseDown(DipFromPixels(*gApp, px), DipFromPixels(*gApp, py));
            DiagLine(Fmt(L"鼠标按下：px=%d,%d dip=%.1f,%.1f active=%s hot=%s hits=%zu page=%d", px, py,
                         DipFromPixels(*gApp, px), DipFromPixels(*gApp, py),
                         cc::Utf8ToWide(gApp->ui.active).c_str(), cc::Utf8ToWide(gApp->ui.hot).c_str(),
                         gApp->ui.HitCount(), static_cast<int>(gApp->page)));
            if (gDebugHits) {
                // 命中矩形只在页面/步骤变化后 dump 一次，避免每次点击都刷日志；
                // 需要重新测量同一页时先切换一次页面即可。
                static int dumpedPage = -1;
                static int dumpedWizard = -1;
                const int pageNow = static_cast<int>(gApp->page);
                const int wizardNow = static_cast<int>(gApp->wizardStep);
                if (pageNow != dumpedPage || wizardNow != dumpedWizard) {
                    dumpedPage = pageNow;
                    dumpedWizard = wizardNow;
                    for (const auto& hit : gApp->ui.LastHits()) {
                        DiagLine(Fmt(L"  命中 %s rect=%.1f,%.1f,%.1f,%.1f enabled=%d",
                                     cc::Utf8ToWide(hit.id).c_str(), hit.rect.left, hit.rect.top, hit.rect.right,
                                     hit.rect.bottom, hit.enabled ? 1 : 0));
                    }
                }
            }
            gApp->dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!gReady || gApp == nullptr) return 0;
            const int px = GET_X_LPARAM(lParam);
            const int py = GET_Y_LPARAM(lParam);
            DiagLine(Fmt(L"鼠标抬起：px=%d,%d dip=%.1f,%.1f active=%s hot=%s", px, py,
                         DipFromPixels(*gApp, px), DipFromPixels(*gApp, py),
                         cc::Utf8ToWide(gApp->ui.active).c_str(), cc::Utf8ToWide(gApp->ui.hot).c_str()));
            gApp->ui.OnMouseUp(DipFromPixels(*gApp, px), DipFromPixels(*gApp, py));
            if (GetCapture() == hwnd) ReleaseCapture();
            gApp->dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (gReady && gApp != nullptr) gApp->ui.OnCaptureLost();
            return 0;
        case WM_MOUSEWHEEL:
            if (gReady && gApp != nullptr) {
                cc::AppHandleWheel(*gApp, GET_WHEEL_DELTA_WPARAM(wParam));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (gReady && gApp != nullptr) {
                if (cc::AppHandleFpsKey(*gApp, wParam)) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    cc::AppHandleKey(*gApp, wParam);
                }
            }
            return 0;
        case WM_CHAR:
            if (gReady && gApp != nullptr) {
                if (cc::AppHandleFpsChar(*gApp, static_cast<wchar_t>(wParam))) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_COMMAND:
            return 0;
        case cc::WM_APP_JOB:
            if (gReady && gApp != nullptr) cc::AppDrainJobs(*gApp);
            return 0;
        case WM_TIMER:
            if (gReady && gApp != nullptr && wParam == kTickTimer) cc::AppTick(*gApp);
            return 0;
        case WM_ACTIVATE:
            if (gReady && gApp != nullptr) {
                gApp->dirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_CLOSE:
            if (gReady && gApp != nullptr) {
                if (!gApp->closeRequested) {
                    gApp->closeRequested = true;
                    cc::AppSetStatus(*gApp, L"正在退出：取消未开始的任务，等待当前任务安全结束…");
                    gApp->jobs.RequestStop();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTickTimer);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

// ===========================================================================
// 程序入口
// ===========================================================================
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    struct InstanceGuard {
        HANDLE handle = CreateMutexW(nullptr, TRUE, L"Local\\WOW335NR-ControlCenter-UI");
        DWORD error = GetLastError();
        ~InstanceGuard() { if (handle) CloseHandle(handle); }
    } instanceGuard;
    if (!instanceGuard.handle) return 1;
    if (instanceGuard.error == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    gStartTick = GetTickCount64();
    EnableDpiAwareness();
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }

    cc::App app;
    gApp = &app;
    const bool initialized = cc::AppInit(app, instance, argc, argv);
    LocalFree(argv);
    if (!initialized) {
        gApp = nullptr;
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }

    CreateDirectoryW(app.paths.dataDir.c_str(), nullptr);
    CreateDirectoryW((app.paths.dataDir + L"\\logs").c_str(), nullptr);
    gDiagPath = app.paths.logPath;
    DiagLine(Fmt(L"启动：exe=%s", app.paths.exePath.c_str()));
    DiagLine(Fmt(L"启动：客户端=%s 来源=%s", app.clientRoot.c_str(), app.stageRoot.c_str()));
    DiagLine(Fmt(L"启动：SystemDpiScale=%.3f layoutScale=%.3f 清单=%s(%s)",
                 SystemDpiScale(), app.layoutScale, app.catalog.manifestPath.c_str(),
                 app.catalog.ok ? L"ok" : L"fail"));

    app.dpiScale = SystemDpiScale();
    const float windowScale = app.dpiScale * app.layoutScale;
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int width = (std::max)(static_cast<int>(std::lround(cc::theme::kMinWindowWidth * windowScale)),
                           static_cast<int>(std::lround(cc::theme::kWindowWidth * windowScale)));
    int height = (std::max)(static_cast<int>(std::lround(cc::theme::kMinWindowHeight * windowScale)),
                            static_cast<int>(std::lround(cc::theme::kWindowHeight * windowScale)));
    const int workWidth = workArea.right - workArea.left;
    const int workHeight = workArea.bottom - workArea.top;
    width = (std::min)(width, workWidth - 16);
    height = (std::min)(height, workHeight - 16);
    int x = workArea.left + (workWidth - width) / 2;
    int y = workArea.top + (workHeight - height) / 2;
    if (y < workArea.top) y = workArea.top;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_CONTROL_CENTER_ICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_CONTROL_CENTER_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassExW(&windowClass) == 0) {
        gApp = nullptr;
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }

    const HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, kWindowClass, L"WOW-335-NR 控制台",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN, x, y, width, height, nullptr,
        nullptr, instance,
        nullptr);
    if (hwnd == nullptr) {
        gApp = nullptr;
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }
    app.hwnd = hwnd;

    // 显式设置大/小图标：任务栏、Alt-Tab 与窗口缩略图都取窗口图标，
    // 窗口类图标负责新窗口默认值，这里按当前 DPI 重新取一次以保证清晰。
    if (const HICON bigIcon = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(IDI_CONTROL_CENTER_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR))) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
    }
    if (const HICON smallIcon = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(IDI_CONTROL_CENTER_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR))) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }

    // 建窗后按真实 DPI 校正尺寸，并让客户区覆盖整个窗口。
    app.dpiScale = WindowDpiScale(hwnd);
    const float realScale = app.dpiScale * app.layoutScale;
    width = (std::min)(workWidth - 16, static_cast<int>(std::lround(cc::theme::kWindowWidth * realScale)));
    height = (std::min)(workHeight - 16, static_cast<int>(std::lround(cc::theme::kWindowHeight * realScale)));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    if (!app.gfx.Initialize(hwnd, app.dpiScale * 96.0f)) {
        MessageBoxW(hwnd, L"Direct2D 初始化失败，无法启动原生控制中心。", L"WOW-335-NR 控制台", MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        gApp = nullptr;
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }
    DiagLine(Fmt(L"窗口：dpi=%.3f 物理=%dx%d DIP=%dx%d 位置=%d,%d 设备=%s", app.dpiScale,
                 static_cast<int>(std::lround(cc::theme::kWindowWidth * app.dpiScale * app.layoutScale)),
                 static_cast<int>(std::lround(cc::theme::kWindowHeight * app.dpiScale * app.layoutScale)),
                 static_cast<int>(std::lround(app.gfx.ClientWidthDip())),
                 static_cast<int>(std::lround(app.gfx.ClientHeightDip())), x, y,
                 app.gfx.DeviceReady() ? L"ready" : L"null"));
    app.jobs.Start(hwnd);
    CreateFpsEdits(app);
    UpdateEditFont(app);

    gReady = true;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    if (!app.clientRoot.empty()) {
        cc::AppRequestSnapshot(app, false);
        cc::AppRequestAdvanced(app);
        cc::AppRequestTelemetry(app);
    } else {
        cc::AppRequestClientScan(app);
    }
    SetTimer(hwnd, kTickTimer, 33, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    gReady = false;
    KillTimer(hwnd, kTickTimer);
    cc::AppShutdown(app);
    gApp = nullptr;
    if (gEditBrush) {
        DeleteObject(gEditBrush);
        gEditBrush = nullptr;
    }
    if (SUCCEEDED(comResult)) CoUninitialize();
    return static_cast<int>(message.wParam);
}
