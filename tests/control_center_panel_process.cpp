#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

#include "../native/control_center/panel_process.hpp"

// 面板进程回归：夹具进程是本测试 EXE 自身的副本（改名为 dlss5-panel.exe）。
// 它以“假面板”模式运行：创建 StandaloneDLSSNRNativeOutput 属主窗 + 带 owner 的
// Dlss5PanelWindow 面板窗（与真实面板结构一致），收到 WM_CLOSE 先写待保存参数
// 再退出，从而覆盖“owned window 识别 / 归属核验 / 优雅交接 / 超时终止”等路径。
// 不会创建 DLSS5PanelMutex，也不会触碰用户桌面上真实的面板或游戏进程。

namespace {

int gFailures = 0;
const wchar_t* kPanelClass = L"Dlss5PanelWindow";
const wchar_t* kOutputClass = L"StandaloneDLSSNRNativeOutput";

void Check(bool condition, const char* label) {
    if (!condition) {
        ++gFailures;
        std::printf("FAIL %s\n", label);
    }
}

void CheckWide(bool condition, const char* label, const std::wstring& actual) {
    if (!condition) {
        ++gFailures;
        std::printf("FAIL %s\n", label);
        std::printf("  actual: ");
        std::wprintf(L"%ls\n", actual.c_str());
    }
}

std::wstring Join(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (left.back() == L'\\' || left.back() == L'/') return left + right;
    return left + L"\\" + right;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        if (!EnsureDirectory(path.substr(0, slash))) return false;
    }
    return CreateDirectoryW(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteBytes(const std::wstring& path, const std::string& bytes) {
    EnsureDirectory(path.substr(0, path.find_last_of(L'\\')));
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
                    written == bytes.size();
    CloseHandle(file);
    return ok;
}

bool RemoveTree(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return DeleteFileW(path.c_str()) != 0;
    WIN32_FIND_DATAW find{};
    const HANDLE handle = FindFirstFileW(Join(path, L"*").c_str(), &find);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = find.cFileName;
            if (name == L"." || name == L"..") continue;
            RemoveTree(Join(path, name));
        } while (FindNextFileW(handle, &find));
        FindClose(handle);
    }
    return RemoveDirectoryW(path.c_str()) != 0;
}

std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH * 2, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return std::wstring();
        if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

// 把本测试 EXE 复制成假面板（保留自身逻辑，用于 --fake-panel 模式）。
bool CopySelfExe(const std::wstring& target) {
    if (!EnsureDirectory(target.substr(0, target.find_last_of(L'\\')))) return false;
    return CopyFileW(ExecutablePath().c_str(), target.c_str(), FALSE) != 0;
}

// 把 cmd.exe 复制成 wow.exe：游戏的“运行中”只按映像路径判定，不要求真实游戏。
bool CopySystemImage(const std::wstring& target, const wchar_t* name) {
    if (!EnsureDirectory(target.substr(0, target.find_last_of(L'\\')))) return false;
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    return CopyFileW(Join(systemDirectory, name).c_str(), target.c_str(), FALSE) != 0;
}

bool StartImage(const std::wstring& image, const std::wstring& arguments, const std::wstring& cwd, DWORD& pid) {
    std::wstring command = L"\"" + image + L"\"";
    if (!arguments.empty()) command += L" " + arguments;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(image.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &startup, &process)) {
        return false;
    }
    pid = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool TerminatePid(DWORD pid) {
    if (pid == 0) return true;
    const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) return true;
    TerminateProcess(process, 1);
    WaitForSingleObject(process, 2000);
    CloseHandle(process);
    return true;
}

bool WaitPidState(DWORD pid, bool expected, ULONGLONG timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeoutMs) {
        if (control_center::panel_detail::ProcessAlive(pid) == expected) return true;
        Sleep(25);
    }
    return control_center::panel_detail::ProcessAlive(pid) == expected;
}

bool WaitWindow(control_center::panel_detail::PanelWindowInfo& window, DWORD pid, bool visible,
                ULONGLONG timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        window = control_center::panel_detail::FindPanelWindowForPid(pid);
        if (window.found && window.visible == visible) return true;
        if (GetTickCount64() - start >= timeoutMs) return false;
        Sleep(25);
    }
}

std::wstring FakePanelArgs(const std::wstring& game, bool hidden = false, DWORD slowCloseMs = 0,
                           const std::wstring& pendingSave = std::wstring()) {
    std::wstring args = L"--fake-panel --game \"" + game + L"\"";
    if (hidden) args += L" --hidden";
    if (slowCloseMs) args += L" --slow-close " + std::to_wstring(slowCloseMs);
    if (!pendingSave.empty()) args += L" --pending-save \"" + pendingSave + L"\"";
    return args;
}

// 为夹具面板写一份“已核验”的 v2 归属标记（创建时间取自真实进程）。
bool WriteVerifiedMarker(const std::wstring& dataDir, DWORD pid, const std::wstring& panelExe,
                         const std::wstring& game) {
    (void)panelExe;  // 身份核验以进程真实映像路径为准，避免长短路径格式差异。
    std::wstring image;
    std::uint64_t created = 0;
    DWORD error = ERROR_SUCCESS;
    if (!control_center::panel_detail::QueryProcessIdentity(pid, image, created, error)) return false;
    return control_center::panel_detail::WritePanelInstanceMarker(dataDir, pid, created, image, game);
}

// ---------- 假面板进程 ----------
std::wstring g_pendingSave;
std::wstring g_messageLog;
std::wstring g_readyFile;
DWORD g_slowCloseMs = 0;

LRESULT CALLBACK FakePanelProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CLOSE:
            if (!g_pendingSave.empty()) {
                WriteBytes(g_pendingSave, std::string("pending-save"));
            }
            if (g_slowCloseMs) Sleep(g_slowCloseMs);
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

std::string NarrowAscii(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t ch : text) result.push_back(static_cast<char>(ch));
    return result;
}

// 记录图层按钮消息（WM_APP+0x5A）：用于验证“只有目标客户端的载具收到消息”。
LRESULT CALLBACK FakeOutputProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_APP + 0x5A) {
        if (!g_messageLog.empty()) {
            std::ofstream log(NarrowAscii(g_messageLog), std::ios::app);
            if (log) log << (wparam != 0 ? 1 : 0) << "\n";
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int RunFakePanel(const std::vector<std::wstring>& args) {
    std::wstring game;
    bool hidden = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--game" && i + 1 < args.size()) game = args[++i];
        else if (args[i] == L"--hidden") hidden = true;
        else if (args[i] == L"--slow-close" && i + 1 < args.size()) g_slowCloseMs = static_cast<DWORD>(_wtoi(args[++i].c_str()));
        else if (args[i] == L"--pending-save" && i + 1 < args.size()) g_pendingSave = args[++i];
        else if (args[i] == L"--message-log" && i + 1 < args.size()) g_messageLog = args[++i];
        else if (args[i] == L"--ready" && i + 1 < args.size()) g_readyFile = args[++i];
    }
    if (game.empty()) return 2;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW outputClass{};
    outputClass.cbSize = sizeof(outputClass);
    outputClass.lpfnWndProc = FakeOutputProc;
    outputClass.hInstance = instance;
    outputClass.lpszClassName = kOutputClass;
    RegisterClassExW(&outputClass);
    WNDCLASSEXW panelClass{};
    panelClass.cbSize = sizeof(panelClass);
    panelClass.lpfnWndProc = FakePanelProc;
    panelClass.hInstance = instance;
    panelClass.lpszClassName = kPanelClass;
    RegisterClassExW(&panelClass);
    // 属主窗（模拟 AIO 输出窗）必须可见，否则 owned 面板的 IsWindowVisible 为假。
    HWND output = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOutputClass, L"fake aio output",
                                  WS_POPUP, -4000, -4000, 64, 64, nullptr, nullptr, instance, nullptr);
    if (!output) return 3;
    ShowWindow(output, SW_SHOWNOACTIVATE);
    HWND panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kPanelClass, L"fake panel",
                                 WS_POPUP, 100, 100, 480, 420, output, nullptr, instance, nullptr);
    if (!panel) return 4;
    if (!hidden) ShowWindow(panel, SW_SHOWNOACTIVATE);
    if (!g_readyFile.empty()) WriteBytes(g_readyFile, std::string("ready"));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

// ---------- 纯决策 ----------
void TestPanelInstanceDecisions() {
    using control_center::GameProcessState;
    using control_center::PanelInstance;
    using control_center::PanelInstanceAction;
    using control_center::panel_detail::DecidePanelInstance;

    const std::wstring selected = L"C:\\clients\\selected\\wow.exe";
    const std::wstring other = L"C:\\clients\\other\\wow.exe";

    {
        const auto decision = DecidePanelInstance({}, selected, true, false);
        Check(decision.action == PanelInstanceAction::Launch, "no instance launches");
        Check(decision.instancePid == 0, "no instance has pid 0");
    }
    {
        PanelInstance instance;
        instance.pid = 41;
        instance.gamePath = selected;
        instance.visible = true;
        instance.gameState = GameProcessState::Running;
        instance.identityVerified = true;
        const auto decision = DecidePanelInstance({instance}, selected, true, false);
        Check(decision.action == PanelInstanceAction::Reuse, "visible own instance is reused");
        Check(decision.instancePid == 41, "reused instance pid reported");
    }
    {
        // 旧版标记（无创建时间）驱动的可见窗口仍可复用，但不会被当作“身份已核验”。
        PanelInstance instance;
        instance.pid = 43;
        instance.gamePath = selected;
        instance.visible = true;
        instance.gameState = GameProcessState::Running;
        instance.markerLegacy = true;
        const auto decision = DecidePanelInstance({instance}, selected, true, false);
        Check(decision.action == PanelInstanceAction::Reuse, "visible legacy instance is reused");
    }
    {
        PanelInstance instance;
        instance.pid = 44;
        instance.gamePath = selected;
        instance.visible = false;
        instance.gameState = GameProcessState::Running;
        instance.identityVerified = true;
        const auto decision = DecidePanelInstance({instance}, selected, true, false);
        Check(decision.action == PanelInstanceAction::RefuseHiddenOwn,
              "hidden own instance while the game runs is kept");
        Check(decision.instancePid == 44, "hidden own instance pid reported");
    }
    {
        PanelInstance instance;
        instance.pid = 45;
        instance.gamePath = selected;
        instance.visible = false;
        instance.gameState = GameProcessState::NotRunning;
        instance.identityVerified = true;
        const auto decision = DecidePanelInstance({instance}, selected, false, false);
        Check(decision.action == PanelInstanceAction::TakeoverWaiting,
              "verified waiting instance whose game exited is handed over");
    }
    {
        // P1：读不到游戏映像路径 → Unknown，不得当作“游戏已退出”而接管。
        PanelInstance instance;
        instance.pid = 46;
        instance.gamePath = selected;
        instance.visible = false;
        instance.gameState = GameProcessState::Unknown;
        instance.identityVerified = true;
        const auto decision = DecidePanelInstance({instance}, selected, false, false);
        Check(decision.action == PanelInstanceAction::RefuseGameStateUnknown,
              "unknown game state is never handed over");
        Check(decision.instancePid == 46, "unknown game state pid reported");
        const auto forced = DecidePanelInstance({instance}, selected, false, true);
        Check(forced.action == PanelInstanceAction::LaunchAfterTerminate,
              "explicit restart may take over a verified instance even when the game state is unknown");
    }
    {
        PanelInstance instance;
        instance.pid = 47;
        instance.gamePath = selected;
        instance.visible = true;
        instance.gameState = GameProcessState::Running;
        instance.identityVerified = true;
        const auto decision = DecidePanelInstance({instance}, selected, true, true);
        Check(decision.action == PanelInstanceAction::LaunchAfterTerminate,
              "explicit restart takes over the verified own instance");
    }
    {
        // P1：旧版标记无法证明当前进程就是登记进程；显式重开也不得结束它。
        PanelInstance instance;
        instance.pid = 48;
        instance.gamePath = selected;
        instance.visible = true;
        instance.gameState = GameProcessState::Running;
        instance.markerLegacy = true;
        const auto decision = DecidePanelInstance({instance}, selected, true, true);
        Check(decision.action == PanelInstanceAction::RefuseUnidentified,
              "explicit restart refuses a legacy (unproven) instance");
        const auto waiting = DecidePanelInstance({instance}, selected, false, false);
        Check(waiting.action == PanelInstanceAction::Reuse, "visible legacy instance is reused on normal open");
        instance.visible = false;
        const auto hiddenLegacyRunning = DecidePanelInstance({instance}, selected, false, false);
        Check(hiddenLegacyRunning.action == PanelInstanceAction::RefuseHiddenOwn,
              "hidden legacy instance with a running game is kept");
        // 游戏确认退出后，旧标记仍然无法证明进程身份 → 保留，不交接。
        instance.gameState = GameProcessState::NotRunning;
        const auto hiddenLegacy = DecidePanelInstance({instance}, selected, false, false);
        Check(hiddenLegacy.action == PanelInstanceAction::RefuseUnidentified,
              "hidden legacy instance is not handed over even after the game exited");
    }
    {
        PanelInstance instance;
        instance.pid = 49;
        instance.gamePath = other;
        instance.visible = true;
        instance.gameState = GameProcessState::Running;
        instance.identityVerified = true;
        const auto decision = DecidePanelInstance({instance}, selected, true, false);
        Check(decision.action == PanelInstanceAction::RefuseOtherClientRunning,
              "instance serving another running client is refused");
        Check(decision.instancePid == 49, "refused instance pid reported");
        CheckWide(decision.otherGamePath == other, "refused instance path reported", decision.otherGamePath);
    }
    {
        // 其它客户端游戏状态 Unknown：不触发“正在服务其它客户端”，但也不结束该进程。
        PanelInstance instance;
        instance.pid = 50;
        instance.gamePath = other;
        instance.visible = false;
        instance.gameState = GameProcessState::Unknown;
        instance.identityVerified = true;
        const auto decision = DecidePanelInstance({instance}, selected, true, false);
        Check(decision.action == PanelInstanceAction::RefuseOtherClientWaiting,
              "other client with unknown game state is kept, not terminated");
    }
    {
        PanelInstance instance;
        instance.pid = 51;
        instance.visible = true;
        const auto decision = DecidePanelInstance({instance}, selected, true, false);
        Check(decision.action == PanelInstanceAction::RefuseUnidentified, "visible unidentified instance is refused");
    }
    {
        PanelInstance instance;
        instance.pid = 52;
        instance.visible = false;
        const auto decision = DecidePanelInstance({instance}, selected, true, false);
        Check(decision.action == PanelInstanceAction::RefuseUnidentified,
              "hidden unidentified instance is kept, never terminated by a normal open");
    }
    {
        PanelInstance instance;
        instance.pid = 53;
        instance.gamePath = selected;
        instance.visible = true;
        instance.gameState = GameProcessState::Running;
        instance.foreign = true;
        const auto decision = DecidePanelInstance({instance}, selected, true, true);
        Check(decision.action == PanelInstanceAction::Launch, "foreign instance never participates in takeover");
    }
}

// ---------- 游戏进程三态分类（P1：“读不到路径”不得当成“已退出”） ----------
void TestGameStateClassification() {
    using control_center::GameProcessState;
    using control_center::panel_detail::ClassifyGameProcessState;
    Check(ClassifyGameProcessState(true, 0) == GameProcessState::Running, "matched process is running");
    Check(ClassifyGameProcessState(true, 3) == GameProcessState::Running, "matched process wins over unreadable");
    Check(ClassifyGameProcessState(false, 0) == GameProcessState::NotRunning, "no process and no errors is not running");
    Check(ClassifyGameProcessState(false, 1) == GameProcessState::Unknown,
          "an unreadable wow.exe image path makes the game state unknown, never 'exited'");
    Check(ClassifyGameProcessState(false, 5) == GameProcessState::Unknown, "multiple unreadable processes stay unknown");
}

// ---------- 旧版标记 + PID 复用：显式重开也必须保留原进程（P1） ----------
void TestLegacyMarkerPidReuseRetained(const std::wstring& root) {
    using control_center::CollectPanelOwnership;
    using control_center::PanelInstance;
    using control_center::PanelInstanceAction;
    using namespace control_center::panel_detail;
    const std::wstring product = Join(root, L"legacy-product");
    const std::wstring panelExe = Join(product, L"bin\\dlss5-panel.exe");
    const std::wstring client = Join(root, L"legacy-client");
    const std::wstring game = Join(client, L"wow.exe");
    const std::wstring dataDir = Join(root, L"legacy-data");
    EnsureDirectory(dataDir);
    EnsureDirectory(client);
    Check(CopySelfExe(panelExe), "legacy panel fixture copied");
    Check(CopySystemImage(game, L"cmd.exe"), "legacy game fixture copied");

    DWORD gamePid = 0;
    Check(StartImage(game, L"/c ping -n 300 127.0.0.1 > nul", client, gamePid), "legacy game fixture started");
    DWORD panelPid = 0;
    // 同一映像路径启动的新面板进程；v1 标记只有 PID+game，模拟“PID 被同路径新面板复用”。
    Check(StartImage(panelExe, FakePanelArgs(game, true), product, panelPid), "legacy hidden panel started");
    PanelWindowInfo window;
    Check(WaitWindow(window, panelPid, false, 5000), "legacy hidden panel window found");
    Check(WritePanelInstanceMarker(dataDir, panelPid, game), "v1 ownership marker written");

    const auto collected = CollectPanelInstances(dataDir, panelExe);
    bool legacy = false;
    for (const auto& instance : collected) {
        if (instance.pid != panelPid) continue;
        legacy = instance.markerLegacy && !instance.identityVerified;
        Check(instance.gameState == control_center::GameProcessState::Running,
              "legacy marker target game is detected as running");
    }
    Check(legacy, "v1 marker is treated as legacy (unproven identity)");

    // 决策层拒绝自动交接。
    const auto decision = DecidePanelInstance(collected, game, true, false);
    Check(decision.action == PanelInstanceAction::RefuseUnidentified ||
              decision.action == PanelInstanceAction::RefuseHiddenOwn,
          "legacy hidden instance is not handed over");
    Check(decision.instancePid == panelPid, "decision reports the legacy pid");

    // 显式重开：StartPanel 必须返回 PanelUnverified 且不结束旧进程。
    const auto restartOptions = control_center::PanelStartOptions{true, true, true};
    const auto restarted = StartPanel(product, dataDir, client, restartOptions);
    Check(!restarted.ok, "explicit restart refuses a legacy (unproven) waiting instance");
    Check(restarted.status == control_center::PanelStartStatus::PanelUnverified, "legacy refusal status reported");
    Check(restarted.error.find(L"无法证明") != std::wstring::npos, "legacy refusal explains the missing identity");
    Check(WaitPidState(panelPid, true, 200), "legacy panel survives an explicit restart");

    // 直接调用接管函数同样拒绝，进程保持存活。
    PanelInstance legacyInstance;
    for (const auto& instance : collected) {
        if (instance.pid == panelPid) legacyInstance = instance;
    }
    const auto takeover = TakeOverPanelInstance(legacyInstance);
    Check(!takeover.ok, "TakeOverPanelInstance refuses an unproven legacy instance");
    Check(takeover.error.find(L"无法证明") != std::wstring::npos, "takeover refusal explains the missing identity");
    Check(WaitPidState(panelPid, true, 200), "legacy panel survives a direct takeover attempt");

    // 归属快照也不把旧标记窗口算作已连接，而是提示无法确认。
    const auto ownership = CollectPanelOwnership(dataDir, client, panelExe);
    Check(ownership.valid && ownership.anyRunning, "ownership snapshot sees the legacy instance");
    Check(!ownership.connected, "legacy marker is not treated as connected");
    Check(ownership.blockedByUnverified, "legacy marker is reported as unverified");

    TerminatePid(panelPid);
    TerminatePid(gamePid);
}


// ---------- 标记格式 ----------
void TestPanelInstanceMarkers(const std::wstring& root) {
    using control_center::panel_detail::CollectPanelInstances;
    using control_center::panel_detail::PanelInstanceMarker;
    using control_center::panel_detail::PanelInstanceMarkerPath;
    using control_center::panel_detail::ReadPanelInstanceMarker;
    using control_center::panel_detail::WritePanelInstanceMarker;

    const std::wstring dataDir = Join(root, L"marker-data");
    Check(!WritePanelInstanceMarker(std::wstring(), 1, 1, L"C:\\p\\dlss5-panel.exe", L"C:\\x\\wow.exe"),
          "v2 marker rejects empty data dir");
    Check(!WritePanelInstanceMarker(dataDir, 0, 1, L"C:\\p\\dlss5-panel.exe", L"C:\\x\\wow.exe"),
          "v2 marker rejects pid 0");

    const DWORD current = GetCurrentProcessId();
    const std::uint64_t created = 123456789ull;
    Check(WritePanelInstanceMarker(dataDir, current, created, L"C:\\p\\dlss5-panel.exe", L"C:\\fixture\\wow.exe"),
          "v2 marker written");
    PanelInstanceMarker marker;
    Check(ReadPanelInstanceMarker(PanelInstanceMarkerPath(dataDir, current), marker), "v2 marker read back");
    Check(marker.version2, "v2 marker reports its version");
    Check(marker.pid == current, "v2 marker keeps pid");
    Check(marker.creationTime == created, "v2 marker keeps creation time");
    CheckWide(marker.imagePath == L"C:\\p\\dlss5-panel.exe", "v2 marker keeps image path", marker.imagePath);
    CheckWide(marker.gamePath == L"C:\\fixture\\wow.exe", "v2 marker keeps game path", marker.gamePath);
    DeleteFileW(PanelInstanceMarkerPath(dataDir, current).c_str());

    Check(WritePanelInstanceMarker(dataDir, current, L"C:\\fixture\\wow.exe"), "v1 marker written");
    Check(ReadPanelInstanceMarker(PanelInstanceMarkerPath(dataDir, current), marker), "v1 marker read back");
    Check(!marker.version2, "v1 marker is not v2");
    Check(marker.pid == current, "v1 marker keeps pid");
    CheckWide(marker.gamePath == L"C:\\fixture\\wow.exe", "v1 marker keeps game path", marker.gamePath);

    // 当前进程不是 dlss5-panel.exe：收集时标记应作为陈旧记录清理。
    const auto instances = CollectPanelInstances(dataDir);
    bool reported = false;
    for (const auto& instance : instances) {
        if (instance.pid == current) reported = true;
    }
    Check(!reported, "marker for a non-panel process is not reported");
    Check(!FileExists(PanelInstanceMarkerPath(dataDir, current)), "stale marker is pruned");

    const std::wstring garbage = Join(dataDir, L"99999.txt");
    WriteBytes(garbage, std::string("not-a-marker"));
    PanelInstanceMarker unreadable;
    Check(!ReadPanelInstanceMarker(garbage, unreadable), "garbage marker rejected");
    DeleteFileW(garbage.c_str());
}

// ---------- owned window 可见性 ----------
void TestOwnedPanelWindow(const std::wstring& root) {
    using namespace control_center::panel_detail;
    const std::wstring product = Join(root, L"product");
    const std::wstring panelExe = Join(product, L"bin\\dlss5-panel.exe");
    const std::wstring client = Join(root, L"client");
    const std::wstring game = Join(client, L"wow.exe");
    const std::wstring dataDir = Join(root, L"owned-data");
    EnsureDirectory(dataDir);
    Check(CopySelfExe(panelExe), "fake panel fixture copied");
    Check(CopySystemImage(game, L"cmd.exe"), "fake game fixture copied");

    DWORD pid = 0;
    Check(StartImage(panelExe, FakePanelArgs(game), product, pid), "owned panel started");
    PanelWindowInfo window;
    Check(WaitWindow(window, pid, true, 5000), "owned panel window becomes visible");
    Check(window.found, "owned panel window found by PID and class");
    Check(window.owned, "panel window keeps its AIO owner");
    Check(PanelWindowVisible(pid), "owned panel is reported visible");
    TerminatePid(pid);
    Check(WaitPidState(pid, false, 3000), "owned panel terminated");
}

// ---------- 普通打开复用可见实例 ----------
void TestNormalOpenReusesPanel(const std::wstring& root) {
    using namespace control_center::panel_detail;
    const std::wstring product = Join(root, L"product");
    const std::wstring client = Join(root, L"client");
    const std::wstring game = Join(client, L"wow.exe");
    const std::wstring dataDir = Join(root, L"reuse-data");
    EnsureDirectory(dataDir);

    DWORD gamePid = 0;
    Check(StartImage(game, L"/c ping -n 300 127.0.0.1 > nul", client, gamePid), "game fixture started");
    const auto options = control_center::PanelStartOptions{true, false, false};
    const auto first = StartPanel(product, dataDir, client, options);
    Check(first.ok, "normal open starts the panel");
    Check(first.pid != 0, "started panel reports pid");
    PanelWindowInfo window;
    Check(WaitWindow(window, first.pid, true, 5000), "started panel is visible");

    const auto second = StartPanel(product, dataDir, client, options);
    Check(second.ok, "second normal open succeeds");
    Check(second.pid == first.pid, "second normal open reuses the same pid");
    Check(second.status == control_center::PanelStartStatus::AlreadyRunning, "reuse reports already running");
    CheckWide(second.error == L"游戏内面板已在运行（已连接所选客户端）。", "reuse message", second.error);

    const auto marker = CollectPanelInstances(dataDir, Join(product, L"bin\\dlss5-panel.exe"));
    bool verified = false;
    for (const auto& instance : marker) {
        if (instance.pid != first.pid) continue;
        verified = instance.identityVerified;
    }
    Check(verified, "started panel has a verified v2 ownership marker");

    TerminatePid(first.pid);
    TerminatePid(gamePid);
    Check(WaitPidState(first.pid, false, 3000), "panel cleaned up");
    Check(WaitPidState(gamePid, false, 3000), "game cleaned up");
}

// ---------- 等待态实例交接（待保存参数落盘） ----------
void TestTakeoverFlushesPendingState(const std::wstring& root) {
    using namespace control_center::panel_detail;
    const std::wstring product = Join(root, L"product");
    const std::wstring panelExe = Join(product, L"bin\\dlss5-panel.exe");
    const std::wstring client = Join(root, L"client");
    const std::wstring game = Join(client, L"wow.exe");
    const std::wstring dataDir = Join(root, L"takeover-data");
    const std::wstring pending = Join(dataDir, L"pending-save.txt");
    EnsureDirectory(dataDir);
    DeleteFileW(pending.c_str());

    DWORD gamePid = 0;
    Check(StartImage(game, L"/c ping -n 300 127.0.0.1 > nul", client, gamePid), "game fixture started");
    DWORD oldPid = 0;
    Check(StartImage(panelExe, FakePanelArgs(game, true, 0, pending), product, oldPid), "hidden waiting panel started");
    PanelWindowInfo window;
    Check(WaitWindow(window, oldPid, false, 5000), "hidden panel window found and invisible");
    Check(WriteVerifiedMarker(dataDir, oldPid, panelExe, game), "verified marker written for waiting panel");

    const auto options = control_center::PanelStartOptions{true, false, false};
    const auto hidden = StartPanel(product, dataDir, client, options);
    Check(!hidden.ok, "normal open refuses to kill the hidden own instance");
    Check(hidden.status == control_center::PanelStartStatus::PanelHiddenOwn, "hidden own status reported");
    Check(hidden.pid == oldPid, "hidden own pid reported");
    Check(WaitPidState(oldPid, true, 100), "hidden own instance survives a normal open");

    const auto restartOptions = control_center::PanelStartOptions{true, false, true};
    const auto restarted = StartPanel(product, dataDir, client, restartOptions);
    Check(restarted.ok, "explicit restart takes over the waiting instance");
    Check(restarted.pid != 0 && restarted.pid != oldPid, "restart reports a new pid");
    Check(WaitPidState(oldPid, false, 3000), "old waiting instance exited");
    Check(FileExists(pending), "pending state was flushed before the old instance exited");
    bool terminatedRecorded = false;
    for (const auto pid : restarted.terminatedPids) {
        if (pid == oldPid) terminatedRecorded = true;
    }
    Check(terminatedRecorded, "restart reports the terminated pid");

    TerminatePid(restarted.pid);
    TerminatePid(gamePid);
}

// ---------- 退出超时：终止兜底后仍正确登记新实例 ----------
void TestTakeoverTimeoutFallsBack(const std::wstring& root) {
    using namespace control_center::panel_detail;
    const std::wstring product = Join(root, L"product");
    const std::wstring panelExe = Join(product, L"bin\\dlss5-panel.exe");
    const std::wstring client = Join(root, L"client");
    const std::wstring game = Join(client, L"wow.exe");
    const std::wstring dataDir = Join(root, L"timeout-data");
    const std::wstring pending = Join(dataDir, L"slow-pending.txt");
    EnsureDirectory(dataDir);
    DeleteFileW(pending.c_str());

    DWORD gamePid = 0;
    Check(StartImage(game, L"/c ping -n 300 127.0.0.1 > nul", client, gamePid), "game fixture started");
    DWORD oldPid = 0;
    Check(StartImage(panelExe, FakePanelArgs(game, true, 9000, pending), product, oldPid), "slow-close panel started");
    PanelWindowInfo window;
    Check(WaitWindow(window, oldPid, false, 5000), "slow panel window found");
    Check(WriteVerifiedMarker(dataDir, oldPid, panelExe, game), "verified marker written for slow panel");

    const auto restartOptions = control_center::PanelStartOptions{true, false, true};
    const auto restarted = StartPanel(product, dataDir, client, restartOptions);
    Check(restarted.ok, "restart succeeds after the graceful timeout");
    Check(restarted.pid != 0 && restarted.pid != oldPid, "restart reports a new pid after termination");
    Check(WaitPidState(oldPid, false, 5000), "slow old instance was terminated");
    Check(FileExists(pending), "pending state was written before the slow close");
    TerminatePid(restarted.pid);
    TerminatePid(gamePid);
}

// ---------- 其它客户端/其它目录同名进程：普通打开不结束 ----------
void TestForeignProcessesSurvive(const std::wstring& root) {
    using namespace control_center::panel_detail;
    const std::wstring product = Join(root, L"product");
    const std::wstring client = Join(root, L"client");
    const std::wstring game = Join(client, L"wow.exe");
    const std::wstring dataDir = Join(root, L"foreign-data");
    const std::wstring otherProduct = Join(root, L"other-product");
    const std::wstring otherPanel = Join(otherProduct, L"bin\\dlss5-panel.exe");
    const std::wstring otherClient = Join(root, L"other-client");
    const std::wstring otherGame = Join(otherClient, L"wow.exe");
    EnsureDirectory(dataDir);
    Check(CopySelfExe(otherPanel), "foreign panel fixture copied");
    Check(CopySystemImage(otherGame, L"cmd.exe"), "other client game fixture copied");

    DWORD gamePid = 0;
    Check(StartImage(game, L"/c ping -n 300 127.0.0.1 > nul", client, gamePid), "game fixture started");
    DWORD otherGamePid = 0;
    Check(StartImage(otherGame, L"/c ping -n 300 127.0.0.1 > nul", otherClient, otherGamePid),
          "other client game fixture started");

    // 另一目录的同名隐藏进程：映像路径不属于本产品，绝不能结束。
    DWORD foreignPid = 0;
    Check(StartImage(otherPanel, FakePanelArgs(otherGame, true), otherProduct, foreignPid),
          "foreign hidden panel started");
    PanelWindowInfo window;
    Check(WaitWindow(window, foreignPid, false, 5000), "foreign panel window found");

    const auto options = control_center::PanelStartOptions{true, false, false};
    const auto started = StartPanel(product, dataDir, client, options);
    Check(started.ok, "normal open starts a panel despite a foreign same-name process");
    Check(WaitPidState(foreignPid, true, 100), "foreign same-name process survives");

    // 另一客户端的面板在服务运行中的游戏：普通打开必须拒绝并保留进程。
    const std::wstring otherData = Join(root, L"other-data");
    EnsureDirectory(otherData);
    DWORD otherPanelPid = 0;
    Check(StartImage(Join(product, L"bin\\dlss5-panel.exe"), FakePanelArgs(otherGame), product, otherPanelPid),
          "other-client panel started");
    Check(WaitWindow(window, otherPanelPid, true, 5000), "other-client panel is visible");
    Check(WriteVerifiedMarker(dataDir, otherPanelPid, Join(product, L"bin\\dlss5-panel.exe"), otherGame),
          "verified marker written for the other-client panel");
    const auto refused = StartPanel(product, dataDir, client, options);
    Check(!refused.ok, "normal open refuses an instance serving another running client");
    Check(refused.status == control_center::PanelStartStatus::AlreadyRunningOtherClient,
          "other-client-running status reported");
    CheckWide(refused.error.find(otherGame) != std::wstring::npos, "refusal names the other game", refused.error);
    Check(WaitPidState(otherPanelPid, true, 100), "other-client panel survives");

    TerminatePid(started.pid);
    TerminatePid(otherPanelPid);
    TerminatePid(foreignPid);
    TerminatePid(gamePid);
    TerminatePid(otherGamePid);
}

// ---------- 身份不符（PID 复用/旧标记）保留进程 ----------
void TestIdentityMismatchIsRetained(const std::wstring& root) {
    using namespace control_center::panel_detail;
    const std::wstring product = Join(root, L"product");
    const std::wstring panelExe = Join(product, L"bin\\dlss5-panel.exe");
    const std::wstring client = Join(root, L"client");
    const std::wstring game = Join(client, L"wow.exe");
    const std::wstring dataDir = Join(root, L"mismatch-data");
    EnsureDirectory(dataDir);

    DWORD gamePid = 0;
    Check(StartImage(game, L"/c ping -n 300 127.0.0.1 > nul", client, gamePid), "game fixture started");
    DWORD panelPid = 0;
    Check(StartImage(panelExe, FakePanelArgs(game), product, panelPid), "visible panel started");
    PanelWindowInfo window;
    Check(WaitWindow(window, panelPid, true, 5000), "visible panel found");

    std::wstring image;
    std::uint64_t created = 0;
    DWORD error = ERROR_SUCCESS;
    Check(QueryProcessIdentity(panelPid, image, created, error), "panel identity read");
    Check(WritePanelInstanceMarker(dataDir, panelPid, created + 12345, image, game),
          "mismatched marker written (simulated pid reuse)");

    const auto collected = CollectPanelInstances(dataDir, panelExe);
    bool unverified = false;
    for (const auto& instance : collected) {
        if (instance.pid != panelPid) continue;
        unverified = !instance.identityVerified && !instance.markerLegacy;
    }
    Check(unverified, "mismatched marker is not trusted");
    Check(!FileExists(PanelInstanceMarkerPath(dataDir, panelPid)), "mismatched v2 marker is pruned");

    const auto options = control_center::PanelStartOptions{true, false, false};
    const auto refused = StartPanel(product, dataDir, client, options);
    Check(!refused.ok, "normal open refuses a visible unidentified window");
    Check(refused.status == control_center::PanelStartStatus::PanelUnverified, "unverified status reported");
    Check(WaitPidState(panelPid, true, 100), "unidentified panel survives the normal open");

    // 显式重新打开也不得杀死身份无法确认的实例。
    const auto restartOptions = control_center::PanelStartOptions{true, false, true};
    const auto restartRefused = StartPanel(product, dataDir, client, restartOptions);
    Check(!restartRefused.ok || restartRefused.pid != panelPid, "explicit restart does not reuse a mismatched pid");
    Check(WaitPidState(panelPid, true, 100), "unidentified panel survives an explicit restart attempt");

    TerminatePid(panelPid);
    TerminatePid(gamePid);
}

// ---------- 既有防护 ----------
void TestPanelGuards(const std::wstring& root) {
    const std::wstring emptyProduct = Join(root, L"empty-product");
    const std::wstring client = Join(root, L"client");
    const std::wstring dataDir = Join(root, L"guard-data");
    EnsureDirectory(emptyProduct);
    EnsureDirectory(client);
    EnsureDirectory(dataDir);
    CopySystemImage(Join(client, L"wow.exe"), L"cmd.exe");

    auto missing = control_center::panel_detail::StartPanel(emptyProduct, dataDir, client);
    Check(!missing.ok, "start panel rejects a missing program");
    CheckWide(missing.error == L"面板程序缺失：bin/dlss5-panel.exe", "start panel missing program message",
              missing.error);

    const std::wstring product = Join(root, L"product");
    Check(CopySelfExe(Join(product, L"bin\\dlss5-panel.exe")), "panel fixture copied");
    const std::wstring emptyClient = Join(root, L"empty-client");
    EnsureDirectory(emptyClient);
    const auto options = control_center::PanelStartOptions{true, false, false};

    auto noGame = control_center::panel_detail::StartPanel(product, dataDir, emptyClient, options);
    Check(!noGame.ok, "start panel rejects a client without wow.exe");
    CheckWide(noGame.error == L"请先选择含 wow.exe 的客户端目录。", "start panel missing client message",
              noGame.error);

    // 本机可能还有其它客户端的 wow.exe 在运行，此时严格模式会先报“路径不符”，
    // 两种拒绝都算通过；关键是不能启动面板。
    auto notRunning = control_center::panel_detail::StartPanel(product, dataDir, client, options);
    Check(!notRunning.ok, "start panel rejects a client whose game is not running");
    Check(notRunning.status == control_center::PanelStartStatus::ClientGameNotRunning ||
              notRunning.status == control_center::PanelStartStatus::ClientPathMismatch,
          "start panel reports a not-running game");

    // 让所选客户端的游戏运行，再验证“空日志目录”分支。
    DWORD guardGamePid = 0;
    Check(StartImage(Join(client, L"wow.exe"), L"/c ping -n 300 127.0.0.1 > nul", client, guardGamePid),
          "guard game fixture started");
    auto noData = control_center::panel_detail::StartPanel(product, std::wstring(), client, options);
    Check(!noData.ok, "start panel rejects an empty data directory");
    Check(noData.status == control_center::PanelStartStatus::LogDirectoryFailed,
          "start panel reports a missing log directory");
    CheckWide(noData.error == L"无法确定日志目录。", "start panel empty data directory message", noData.error);
    TerminatePid(guardGamePid);

    auto game = control_center::StartClientGame(emptyClient);
    Check(!game.ok, "start game rejects a client without wow.exe");
    CheckWide(game.error == L"请先在「切换客户端」中选择含 wow.exe 的目录。", "start game missing client message",
              game.error);
    auto gameEmpty = control_center::StartClientGame(std::wstring());
    Check(!gameEmpty.ok, "start game rejects an empty client root");
}

void TestReadTailLines(const std::wstring& root) {
    using control_center::panel_detail::ReadTailLines;
    const std::wstring logs = Join(root, L"logs");
    EnsureDirectory(logs);

    std::vector<std::wstring> lines;
    DWORD error = ERROR_SUCCESS;
    Check(!ReadTailLines(Join(logs, L"missing.log"), 40, lines, error), "tail missing file rejected");
    Check(error == ERROR_FILE_NOT_FOUND, "tail missing file reports not found");

    const std::wstring emptyPath = Join(logs, L"empty.log");
    WriteBytes(emptyPath, std::string());
    lines.clear();
    error = ERROR_SUCCESS;
    Check(ReadTailLines(emptyPath, 40, lines, error), "tail empty file accepted");
    Check(lines.empty(), "tail empty file yields no lines");

    std::string crlf;
    for (int i = 0; i < 60; ++i) {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "line %d\r\n", i);
        crlf += buffer;
    }
    const std::wstring crlfPath = Join(logs, L"stdout.log");
    WriteBytes(crlfPath, crlf);
    lines.clear();
    Check(ReadTailLines(crlfPath, 40, lines, error), "tail crlf file accepted");
    Check(lines.size() == 40, "tail keeps only the last 40 lines");
    if (lines.size() == 40) {
        CheckWide(lines.front() == L"line 20", "tail starts at the fortieth-from-last line", lines.front());
        CheckWide(lines.back() == L"line 59", "tail ends at the last line", lines.back());
    }

    const std::wstring bomPath = Join(logs, L"bom.log");
    WriteBytes(bomPath, std::string("\xEF\xBB\xBF") + "first\r\nsecond");
    lines.clear();
    Check(ReadTailLines(bomPath, 40, lines, error), "tail bom file accepted");
    Check(lines.size() == 2, "tail bom file has two lines");
    if (lines.size() == 2) {
        CheckWide(lines.front() == L"first", "tail strips the utf-8 bom", lines.front());
        CheckWide(lines.back() == L"second", "tail keeps a last line without newline", lines.back());
    }
}

void TestReadLogTail(const std::wstring& root) {
    using control_center::ReadLogTail;
    auto empty = ReadLogTail(std::wstring());
    Check(!empty.ok, "log tail rejects an empty directory");
    CheckWide(empty.error == L"日志目录为空。", "log tail empty directory message", empty.error);

    const std::wstring logDir = Join(root, L"panel-logs\\run-1");
    EnsureDirectory(logDir);
    std::string stdoutText;
    for (int i = 0; i < 50; ++i) {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "stdout-%d\n", i);
        stdoutText += buffer;
    }
    WriteBytes(Join(logDir, L"stdout.log"), stdoutText);
    WriteBytes(Join(logDir, L"stderr.log"), std::string("warn-1\r\nwarn-2\r\n"));
    const auto tail = ReadLogTail(logDir);
    Check(tail.ok, "log tail accepts a log directory");
    Check(tail.lines.size() == 42, "log tail concatenates stdout and stderr windows");
    if (tail.lines.size() == 42) {
        CheckWide(tail.lines.front() == L"stdout-10", "log tail starts at the last 40 stdout lines", tail.lines.front());
        CheckWide(tail.lines.back() == L"warn-2", "log tail ends with the last stderr line", tail.lines.back());
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 假面板模式：本 EXE 被复制成 bin\dlss5-panel.exe 后，由 StartPanel 以
    // `--game <wow.exe>` 拉起（或由测试直接以 --fake-panel 启动）。
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    std::wstring self = ExecutablePath();
    const size_t slash = self.find_last_of(L"\\/");
    const std::wstring base = slash == std::wstring::npos ? self : self.substr(slash + 1);
    bool fakeMode = false;
    for (const auto& arg : args) {
        if (arg == L"--fake-panel") fakeMode = true;
    }
    if (fakeMode || _wcsicmp(base.c_str(), L"dlss5-panel.exe") == 0) return RunFakePanel(args);

    if (argc < 2) {
        std::printf("usage: panel-process.exe <scratch-root>\n");
        return 2;
    }
    const std::wstring root = Join(std::wstring(argv[1]), L"panel-process");
    RemoveTree(root);
    if (!EnsureDirectory(root)) {
        std::printf("FAIL create scratch root\n");
        return 1;
    }
    TestReadTailLines(root);
    TestReadLogTail(root);
    TestGameStateClassification();
    TestPanelInstanceDecisions();
    TestPanelInstanceMarkers(root);
    TestOwnedPanelWindow(root);
    TestNormalOpenReusesPanel(root);
    TestTakeoverFlushesPendingState(root);
    TestTakeoverTimeoutFallsBack(root);
    TestForeignProcessesSurvive(root);
    TestIdentityMismatchIsRetained(root);
    TestLegacyMarkerPidReuseRetained(root);
    TestPanelGuards(root);
    if (gFailures == 0) {
        std::printf("panel process: owned windows, ownership markers, takeover, reuse and guards passed\n");
        return 0;
    }
    std::printf("panel process: %d failure(s)\n", gFailures);
    return 1;
}
