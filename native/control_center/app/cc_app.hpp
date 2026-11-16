// WOW-335-NR 原生控制中心：应用状态与页面入口。
//
// 所有界面状态都只在 UI 线程读写；后台线程只处理装进 shared_ptr 的结果，
// 再由 JobQueue::DrainApply 在 UI 线程 apply，apply 内校验 generation/客户端。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cc_gfx.hpp"
#include "cc_jobs.hpp"
#include "cc_model.hpp"
#include "cc_tasks.hpp"
#include "cc_theme.hpp"
#include "cc_ui.hpp"
#include "../telemetry.hpp"
#include "install_preview.hpp"
#include "uninstall_preview.hpp"

namespace cc {

// 页面直接使用核心只读类型（InstallPreview / UninstallPreview / MemberState 等），
// 避免每处都写 control_center:: 前缀；写事务仍由主程序显式接线。
using namespace control_center;

enum class Page { Home = 0, Wizard = 1, Maintenance = 2, Uninstall = 3 };
enum class WizardStep { Client = 0, Checks = 1, Files = 2, Install = 3, Done = 4 };

struct InstallSummary {
    bool ok = false;
    bool ready = false;
    std::wstring error;
    std::wstring stageRoot;
    std::wstring templateRoot;
    InstallPreview preview;
};

struct UninstallSummary {
    bool ok = false;
    bool ready = false;
    bool hasRecord = false;
    std::wstring error;
    std::wstring clientRoot;
    UninstallPreview preview;
};

// 维护页“高级工具”：ReShade.ini 档位/参数与方案。
struct AdvancedSummary {
    bool loading = false;        // 后台读取当前参数
    bool loaded = false;
    bool saving = false;         // 后台写入参数
    bool schemesLoading = false; // 后台扫描方案目录
    bool schemesLoaded = false;
    bool schemeSaving = false;   // 后台保存/应用方案
    std::wstring error;          // 最近一次读取/写入错误
    std::wstring status;         // 最近一次成功提示
    Ds5AdvancedSettings settings;
    std::wstring tierName = L"自定义";
    std::wstring modelName = L"默认";
    // 编辑副本；读取成功或点“填入当前值”时刷新。
    Ds5Values edit;
    bool editDetached = true;
    bool editOpticalFlow = false;
    std::wstring schemeName;     // 方案名输入框（系统 EDIT）
    std::vector<Ds5Scheme> schemes;
    std::wstring schemesError;
    // 后台请求令牌：切换客户端或发起新请求后，旧回调直接丢弃。
    std::uint64_t token = 0;
    // 动作令牌：保存/应用等写操作的请求，不能被并发的加载请求作废。
    std::uint64_t actionToken = 0;
};

struct App {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;

    AppPaths paths;
    Settings settings;
    CatalogBundle catalog;
    std::vector<std::wstring> protectedRoots;
    Log log;
    JobQueue jobs;
    Gfx gfx;
    Ui ui;

    float dpiScale = 1.0f;    // 窗口实际 DPI / 96
    float layoutScale = 1.0f; // 开发用模拟缩放（--dpi-scale），1 表示用实际 DPI

    Page page = Page::Home;
    WizardStep wizardStep = WizardStep::Client;
    // 切页过渡：新页面从一侧滑入（方向 +1 前进 / -1 后退），见 AppRender。
    bool pageAnimating = false;
    ULONGLONG pageAnimStart = 0;
    float pageAnimDir = 1.0f;
    int pageTransitionStyle = 0;  // 0 顺滑滑入 / 1 缩放淡入 / 2 弹性滑入 / 3 长距滑入
    float pageAnimOffsetX = 0.0f;  // 当前帧页面横向偏移（DIP），供系统 EDIT 控件跟随
    float pageAnimScale = 1.0f;    // 当前帧页面缩放；EDIT 仅在纯平移过渡时跟随
    float pageAnimOpacity = 1.0f;  // 当前帧页面不透明度
    std::wstring clientRoot;
    std::wstring stageRoot;
    std::wstring templateRoot;
    std::uint64_t generation = 1;

    ClientSnapshot snapshot;
    bool snapshotReady = false;
    bool snapshotLoading = false;
    // 快速快照在途时又请求了哈希校验：等这一轮结束后补一次校验（只读，可丢弃）。
    bool verifyPending = false;

    std::vector<ClientCandidate> candidates;
    bool candidatesReady = false;
    bool scanLoading = false;

    InstallSummary install;
    bool installLoading = false;
    std::uint64_t installToken = 0;

    UninstallSummary uninstall;
    bool uninstallLoading = false;
    std::uint64_t uninstallToken = 0;

    AdvancedSummary advanced;

    // 阶段二工具操作（诊断/清单/恢复/面板/助手/管线开关）：一次只跑一个。
    struct ToolRun {
        bool running = false;
        bool finished = false;
        bool ok = false;
        std::wstring action;  // 触发的动作 id，apply 时用于选择提示文案
        std::wstring error;
        std::wstring detail;
        std::uint64_t token = 0;
    };
    ToolRun toolRun;
    bool restoreArmed = false;  // “恢复安装前文件”需要二次点击确认

    // 游戏内面板：本进程最近一次启动的日志目录；自动拉起的守门状态。
    std::wstring lastPanelLog;
    std::wstring lastWatchLog;
    bool panelAutoLaunchTried = false;
    bool panelAutoRequest = false;  // 自动拉起的请求标记：用于区分日志文案
    ULONGLONG lastPanelCheck = 0;

    // GPU 统计（系统合计；核心内部 5 秒缓存，主页运行时刷新）。
    control_center::GpuStats gpuStats;
    bool gpuPolling = false;

    // 日志查看器（悬浮面板 / 载体日志的尾部内容）。
    struct LogViewer {
        bool visible = false;
        std::wstring title;
        std::vector<std::wstring> lines;
    };
    LogViewer logViewer;

    // 运行时遥测（主页实时数值）：后台线程读取，apply 后 UI 线程只读。
    bool telemetryPolling = false;
    bool gameRunning = false;  // 当前客户端 wow.exe 是否在运行（进程关联）
    int gameProcessCount = 0;
    int hostProcessCount = 0;
    int unidentifiedProcesses = 0;
    std::wstring telemetryNote;  // 无遥测/载体未运行时的说明
    TelemetrySample telemetry;
    std::uint32_t telemetryHostPid = 0;
    double fpsSource = -1.0;  // 源帧率（帧/秒），未知为 -1
    double fpsOutput = -1.0;  // 输出帧率（帧/秒），未知为 -1
    double sampleAt = -1.0;   // 上次采样时刻（遥测时钟秒）
    double sampleNr = 0.0;
    double sampleFrames = 0.0;
    std::uint64_t runtimeToken = 0;

    // 写事务执行状态：后台线程更新 progress 原子量，UI 线程只读。
    struct OperationRun {
        bool running = false;
        bool finished = false;
        bool ok = false;
        std::wstring error;
        std::wstring detail;
        std::shared_ptr<TaskProgress> progress;
        std::uint64_t token = 0;
        std::uint64_t actionToken = 0;
    };
    OperationRun installRun;
    OperationRun repairRun;
    OperationRun uninstallRun;

    // Config.wtf 帧率输入：自绘输入框（主窗口处理键盘输入），避免原生 EDIT
    // 子窗口在切页时出现合成残影或跟随滞后。
    std::wstring fpsFrontText;
    std::wstring fpsBackText;
    int fpsEditFocus = 0;  // 0 未聚焦，1 前台帧率，2 后台帧率
    bool fpsCaretOn = true;
    ULONGLONG fpsCaretTick = 0;
    bool fpsSaving = false;
    // 输入框最近一次由快照同步的值：只有快照值变化时才覆盖，避免玩家输入被每帧重置。
    int syncedFpsFront = -1;
    int syncedFpsBack = -1;
    Rect editFrontRect;
    Rect editBackRect;
    HFONT editFont = nullptr;

    // 维护页方案名输入框（系统 EDIT，只有高级工具可见时显示）。
    HWND editSchemeName = nullptr;
    bool schemeNameVisible = false;
    Rect editSchemeNameRect;

    // 自绘标题栏按钮。
    int chromePressed = 0;  // 1 最小化 2 最大化/还原 3 关闭
    int chromeHot = 0;

    // 页面滚动（文件清单、卸载清单可能超出窗口）。
    float scroll = 0.0f;
    float scrollMax = 0.0f;

    // 最近一次真实状态提示（后台错误、操作结果），显示在日志区。
    std::wstring statusNote;

    // 性能自检：--debug 时在标题栏右侧显示帧时间与后台任务数。
    bool debugOverlay = false;
    double frameMs = 0.0;

    ULONGLONG lastTick = 0;
    bool dirty = true;
};

// 生命周期。
bool AppInit(App& app, HINSTANCE instance, int argc, wchar_t** argv);
void AppShutdown(App& app);
void AppRender(App& app);
void AppHandleClick(App& app);
void AppHandleKey(App& app, WPARAM key);
// 帧率输入框聚焦时的键盘处理：返回 true 表示按键已被输入框消费。
bool AppHandleFpsKey(App& app, WPARAM key);
bool AppHandleFpsChar(App& app, wchar_t ch);
void AppBlurFpsEdit(App& app);
void AppHandleWheel(App& app, int wheelDelta);
void AppTick(App& app);

// 页面导航与数据请求。
void AppGotoPage(App& app, Page page);
void AppGotoWizard(App& app, WizardStep step);
void AppSelectClient(App& app, const std::wstring& root, bool remember);
void AppBumpGeneration(App& app);
void AppRequestSnapshot(App& app, bool verifyHashes);
void AppRequestClientScan(App& app);
void AppRequestInstallPreview(App& app);
void AppRequestUninstallPreview(App& app);
void AppRequestTelemetry(App& app);
void AppRequestInstallPreview(App& app);
void AppStartInstall(App& app);
void AppStartRepair(App& app);
void AppStartUninstall(App& app);
void AppDrainJobs(App& app);

// 动作。
void AppBrowseClient(App& app);
void AppBrowseStage(App& app);
void AppLaunchGame(App& app);
void AppSaveFpsCap(App& app);
void AppRequestAdvanced(App& app);
void AppRequestSchemes(App& app);
void AppSaveAdvanced(App& app);
void AppSaveScheme(App& app);
void AppApplyScheme(App& app, const std::wstring& path);
void AppRestoreCandidateDefaults(App& app);
void AppOpenFolder(const std::wstring& path);
void AppLaunchOrPanel(App& app);
void AppStartPanel(App& app);
void AppOpenPanelLog(App& app);
void AppOpenCarrierLog(App& app);
void AppShowLog(App& app, const std::wstring& title, const std::vector<std::wstring>& lines);
void AppCloseLogViewer(App& app);
void AppRequestGpuStats(App& app);
void AppExportDiagnostics(App& app);
void AppExportManifestList(App& app);
void AppRestoreInstall(App& app);
void AppStartWindowWatch(App& app);
void AppTogglePipeline(App& app);
bool AppCanInstall(const App& app);
bool AppCanUninstall(const App& app);

// 布局辅助。
float AppContentWidth(const App& app);
Rect AppBodyRect(const App& app);
Rect AppFooterRect(const App& app);
Rect AppChromeButtonRect(const App& app, int index);
int AppChromeHitTest(const App& app, float x, float y);
float AppScale(const App& app);
void AppSetStatus(App& app, const std::wstring& text);
// 诊断日志（写当前日志文件，仅用于开发排查）。
void AppDiagLog(const std::wstring& text);

// 页面绘制（cc_pages.cpp）。
void DrawChrome(App& app);
void DrawFooter(App& app);
void DrawHome(App& app);
void DrawWizard(App& app);
void DrawMaintenance(App& app);
void DrawUninstall(App& app);
void DrawLogViewer(App& app);

// 帧率输入框同步（只有主页显示）。
void AppSyncFpsText(App& app);
// 维护页方案名输入框同步（只有高级工具可见时显示）。
void AppSyncSchemeNameEdit(App& app);

}  // namespace cc
