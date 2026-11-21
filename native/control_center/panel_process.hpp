// WOW-335-NR 原生控制中心：启动游戏、游戏内面板进程与日志尾读。
//
// 与旧实现（CC:943-954 / CC:1362-1374）一致：面板位于 <产品目录>\bin\dlss5-panel.exe，
// 以隐藏窗口+重定向 stdio 启动，参数 --game "<client>\wow.exe>；日志只保留“本次进程
// 最后一次启动”的目录。日志目录改为用户数据目录（<dataDir>\panel-logs\<token>），
// 避免向游戏目录写运行时日志。
#pragma once

#include <windows.h>

#include <tlhelp32.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "file_identity.hpp"  // 哈希/文件身份类型
#include "ds5_settings.hpp"   // ds5_detail：FileExistsNoReparse / UniqueToken / CreateDirectoryTree
#include "strict_json.hpp"    // panel-lock.json（可选哈希校验）

namespace control_center {

constexpr size_t kLogTailMaxBytes = 256u * 1024;
constexpr size_t kLogTailMaxLines = 40;

// 面板启动结果分类：调用方据此区分“已连接/路径不符/启动失败”，而不是只靠错误文案。
enum class PanelStartStatus {
    None = 0,
    PanelMissing,
    AlreadyRunning,           // 面板在运行，且（严格模式）属于所选客户端
    AlreadyRunningOtherClient,  // 面板在运行，但所选客户端没有运行中的游戏
    ClientGameNotRunning,     // 所选客户端没有运行中的 wow.exe
    ClientPathMismatch,       // 有其它客户端/未知路径的 wow.exe 在运行
    ClientStateUnknown,       // 无法读取进程映像路径
    LogDirectoryFailed,
    LaunchFailed,
    PanelUnverified,          // 存在无法确认归属的面板实例（保留进程）
    PanelHiddenOwn,           // 属于所选客户端但窗口不可见（保留进程）
    PanelGameStateUnknown,    // 无法判断旧面板的目标游戏是否已退出（保留进程）
    TakeoverFailed,           // 等待态实例无法正常退出/结束
    LaunchExitedImmediately,  // 新实例启动后立即退出（通常是已有其它面板占着）
};

struct PanelStartResult {
    bool ok = false;
    DWORD win32Error = ERROR_SUCCESS;
    PanelStartStatus status = PanelStartStatus::None;
    std::wstring error;
    std::wstring logDir;
    // 可选的 panel-lock.json 校验提示：校验不通过只记录，不阻断启动。
    std::wstring hashWarning;
    std::uint32_t pid = 0;
    // 启动成功但未能写入归属标记等非致命问题；只提示，不改变 ok。
    std::wstring warning;
    // 本次为了保证归属正确而结束的旧面板 PID（可能为空）。
    std::vector<std::uint32_t> terminatedPids;
};

// 启动选项：
// - requireSelectedClientRunning：只允许在“所选客户端”的 wow.exe 正在运行时打开面板，
//   并拒绝在其它客户端的游戏进程下复用面板（R6）。为兼容既有调用/测试默认关闭。
// - verifyPanelHash：额外比对 panel-lock.json 的 dlss5-panel.exe 哈希，失败只警告。
struct PanelStartOptions {
    bool requireSelectedClientRunning = false;
    bool verifyPanelHash = false;
    // 用户明确要求“重新打开面板”：即使已有属于所选客户端的面板实例，
    // 也先结束它再启动一个新的（用于窗口卡死/不可见等恢复场景）。
    bool forceRestart = false;
};

// 一个正在运行的 dlss5-panel.exe 实例。控制中心启动面板时会在
// <dataDir>\panel-instances\<pid>.txt 记录“PID + 进程创建时间 + 面板映像路径 +
// 目标 wow.exe 路径”。读取方必须逐项核对 PID/创建时间/映像路径，防止 PID 复用；
// 旧版标记（只有 PID + game 路径）视为“身份无法确认”，只保留进程不接管。
//
// 目标游戏的存活状态必须三态：读不到任何 wow.exe 映像路径时是 Unknown，绝不能
// 当成“游戏已退出”——否则会把正在服务另一个客户端的面板错误地判为等待实例。
enum class GameProcessState { NotRunning, Running, Unknown };

struct PanelInstance {
    std::uint32_t pid = 0;
    std::wstring imagePath;    // 实际映像完整路径
    std::uint64_t creationTime = 0;  // 100ns since 1601（GetProcessTimes）
    std::wstring gamePath;     // 标记里的目标 wow.exe；空 = 未登记/旧标记
    bool alive = true;
    bool windowFound = false;  // 找到本工具面板窗口类（含 owned window）
    bool visible = false;      // 该窗口 IsWindowVisible
    bool windowOwned = false;  // 面板窗口带 AIO 输出窗 owner（正常结构）
    GameProcessState gameState = GameProcessState::Unknown;  // 目标游戏：运行/已退出/无法判断
    bool identityVerified = false;  // 标记与 PID/创建时间/映像路径完全一致
    bool markerLegacy = false;      // 只有旧版标记（无创建时间），不能排除 PID 复用
    bool foreign = false;      // 映像路径不是本产品目录的 bin\dlss5-panel.exe
};

enum class PanelInstanceAction {
    Launch,                       // 没有相关实例，直接启动
    Reuse,                        // 实例属于所选客户端且窗口可见，直接复用
    TakeoverWaiting,              // 身份已证明、原游戏确认退出的等待实例：正常关闭后重启
    LaunchAfterTerminate,         // 显式重开：只结束身份已证明属于所选客户端的实例
    RefuseOtherClientRunning,     // 实例正在为另一个运行中的客户端服务
    RefuseOtherClientWaiting,     // 实例属于其它客户端但该客户端的游戏当前未运行：保留等待实例
    RefuseUnidentified,           // 无标记/旧标记/身份不符：保留进程并说明
    RefuseHiddenOwn,              // 属于所选客户端但窗口不可见：保留进程，提示用“重新打开”
    RefuseGameStateUnknown,       // 无法确认旧面板目标游戏是否已退出：保留进程
};

struct PanelInstanceDecision {
    PanelInstanceAction action = PanelInstanceAction::Launch;
    std::uint32_t instancePid = 0;  // 触发判定的实例（可为 0）
    std::wstring otherGamePath;     // RefuseOtherClientRunning 时用于提示
    std::wstring imagePath;         // RefuseUnidentified 时用于提示
};

// 所选客户端的 wow.exe 进程状态（按 QueryFullProcessImageNameW 的完整映像路径判定）。
enum class ClientGameState { NotRunning, Running, Unknown };

struct ClientGameCheck {
    ClientGameState state = ClientGameState::NotRunning;
    DWORD win32Error = ERROR_SUCCESS;
    std::uint32_t pid = 0;
    std::uint32_t matching = 0;  // 映像路径属于所选 clientRoot 的 wow.exe 数量
    std::uint32_t otherWow = 0;  // 名称是 wow.exe、但路径不属于所选 clientRoot 的数量
    std::uint32_t unreadable = 0;
    std::wstring imagePath;
};

struct GameStartResult {
    bool ok = false;
    DWORD win32Error = ERROR_SUCCESS;
    std::wstring error;
    std::uint32_t pid = 0;
};

struct LogTailResult {
    bool ok = false;
    DWORD win32Error = ERROR_SUCCESS;
    std::wstring error;
    std::vector<std::wstring> lines;
};

namespace panel_detail {

struct ProcessEntry {
    std::uint32_t pid = 0;
    std::wstring name;
};

inline std::vector<ProcessEntry> EnumerateProcesses() {
    std::vector<ProcessEntry> processes;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return processes;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            processes.push_back({entry.th32ProcessID, entry.szExeFile});
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return processes;
}

inline bool NameEquals(const std::wstring& name, const wchar_t* expected) {
    return _wcsicmp(name.c_str(), expected) == 0;
}

inline bool ProcessRunning(const wchar_t* imageName) {
    for (const auto& process : EnumerateProcesses()) {
        if (NameEquals(process.name, imageName)) return true;
    }
    return false;
}

inline std::wstring Join(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (!left.empty() && (left.back() == L'\\' || left.back() == L'/')) return left + right;
    return left + L"\\" + right;
}

// 去掉 Win32 长路径前缀，便于按完整路径比较映像路径。
inline std::wstring StripLongPathPrefix(const std::wstring& path) {
    if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\') {
        return path.substr(4);
    }
    return path;
}

inline bool SameImagePath(const std::wstring& actual, const std::wstring& expected) {
    const std::wstring left = StripLongPathPrefix(actual);
    const std::wstring right = StripLongPathPrefix(expected);
    if (left.empty() || right.empty()) return false;
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// 读取一个进程的完整映像路径；失败时返回 Win32 错误（无权限/进程已退出等）。
inline bool QueryImagePath(std::uint32_t pid, std::wstring& path, DWORD& win32Error) {
    path.clear();
    win32Error = ERROR_SUCCESS;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr || process == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    std::vector<wchar_t> buffer(1024, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &size);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    if (!ok) {
        win32Error = error;
        return false;
    }
    path.assign(buffer.data(), size);
    return true;
}

// 判断所选客户端的 wow.exe 是否正在运行（逐进程核对完整映像路径）。
inline ClientGameCheck CheckClientGameProcess(const std::wstring& clientRoot) {
    ClientGameCheck check;
    const std::wstring expected = Join(clientRoot, L"wow.exe");
    bool unreadable = false;
    for (const auto& process : EnumerateProcesses()) {
        if (!NameEquals(process.name, L"wow.exe")) continue;
        std::wstring image;
        DWORD error = ERROR_SUCCESS;
        if (!QueryImagePath(process.pid, image, error)) {
            ++check.unreadable;
            unreadable = true;
            if (check.win32Error == ERROR_SUCCESS) check.win32Error = error;
            continue;
        }
        if (SameImagePath(image, expected)) {
            ++check.matching;
            if (check.pid == 0) {
                check.pid = process.pid;
                check.imagePath = image;
            }
        } else {
            ++check.otherWow;
        }
    }
    if (check.matching > 0) {
        check.state = ClientGameState::Running;
    } else if (check.otherWow > 0) {
        // 有 wow.exe 在运行，但路径不属于所选客户端：按“路径不符”处理。
        check.state = ClientGameState::NotRunning;
    } else if (unreadable) {
        check.state = ClientGameState::Unknown;
    } else {
        check.state = ClientGameState::NotRunning;
    }
    return check;
}

// ---------- 面板实例归属：探测与判定 ----------
// 只按映像名 dlss5-panel.exe 判断会把其它客户端遗留的“等待中”实例当成
// 已连接（用户现场：面板进程在跑但窗口不可见，控制中心却显示已连接）。
// 因此启动面板时记录归属标记，并在此按标记/窗口可见性决定复用或接管。

inline std::string Narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string bytes(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), needed, nullptr,
                        nullptr);
    return bytes;
}

inline bool ProcessAlive(std::uint32_t pid) {
    if (pid == 0) return false;
    const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr || process == INVALID_HANDLE_VALUE) return false;
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
}

inline std::wstring ImageBaseName(const std::wstring& path) {
    const std::wstring clean = StripLongPathPrefix(path);
    const size_t slash = clean.find_last_of(L"\\/");
    return slash == std::wstring::npos ? clean : clean.substr(slash + 1);
}

// ---------- 进程快照与面板窗口身份 ----------
// 归属检查每次只取一次进程快照；窗口/映像查询都基于同一份快照，避免在 UI 线程
// 反复枚举进程。快照通常在后台任务里生成，UI 只消费结果。

struct ProcessSnapshot {
    std::vector<ProcessEntry> entries;
};

inline ProcessSnapshot TakeProcessSnapshot() {
    ProcessSnapshot snapshot;
    snapshot.entries = EnumerateProcesses();
    return snapshot;
}

inline bool SnapshotHasImage(const ProcessSnapshot& snapshot, const wchar_t* imageName) {
    for (const auto& process : snapshot.entries) {
        if (NameEquals(process.name, imageName)) return true;
    }
    return false;
}

// 指定完整映像路径的程序是否在运行（在同一份进程快照内判断）。
inline bool IsImageRunningByPathIn(const std::wstring& expected, const ProcessSnapshot& snapshot) {
    if (expected.empty()) return false;
    const std::wstring base = ImageBaseName(expected);
    for (const auto& process : snapshot.entries) {
        if (!NameEquals(process.name, base.c_str())) continue;
        std::wstring image;
        DWORD error = ERROR_SUCCESS;
        if (!QueryImagePath(process.pid, image, error)) continue;
        if (SameImagePath(image, expected)) return true;
    }
    return false;
}

inline bool IsImageRunningByPath(const std::wstring& expected) {
    return IsImageRunningByPathIn(expected, TakeProcessSnapshot());
}

// 由证据判定目标游戏状态：任一相关进程的映像路径读不到时，不能声称“已退出”。
inline GameProcessState ClassifyGameProcessState(bool matched, std::uint32_t unreadable) {
    if (matched) return GameProcessState::Running;
    return unreadable > 0 ? GameProcessState::Unknown : GameProcessState::NotRunning;
}

// 指定完整映像路径的游戏进程状态（在调用方提供的同一份进程快照内判断）。
// 只有“同名的相关进程全部可读、且没有一条路径匹配”才返回 NotRunning。
inline GameProcessState GameStateByImagePathIn(const std::wstring& expected, const ProcessSnapshot& snapshot) {
    if (expected.empty()) return GameProcessState::Unknown;
    const std::wstring base = ImageBaseName(expected);
    if (base.empty()) return GameProcessState::Unknown;
    std::uint32_t unreadable = 0;
    for (const auto& process : snapshot.entries) {
        if (!NameEquals(process.name, base.c_str())) continue;
        std::wstring image;
        DWORD error = ERROR_SUCCESS;
        if (!QueryImagePath(process.pid, image, error)) {
            ++unreadable;
            continue;
        }
        if (SameImagePath(image, expected)) return GameProcessState::Running;
    }
    return ClassifyGameProcessState(false, unreadable);
}

// 面板主窗口的窗口类（native/panel.cpp 注册）。按 PID + 类名识别，不按窗口标题，
// 也不过滤 owner：面板正常情况下以 AIO 输出窗为 owner（GWLP_HWNDPARENT），旧实现
// 过滤 owner 会把真实的已连接面板误判成“不可见”，进而错误地走强制终止分支。
constexpr wchar_t kPanelWindowClass[] = L"Dlss5PanelWindow";

struct PanelWindowInfo {
    HWND window = nullptr;
    bool found = false;
    bool visible = false;
    bool owned = false;  // 窗口带 owner（正常的面板/AIO 结构）
};

struct PanelWindowLookup {
    std::uint32_t pid = 0;
    PanelWindowInfo info;
};

inline BOOL CALLBACK FindPanelWindowCallback(HWND window, LPARAM parameter) {
    auto* lookup = reinterpret_cast<PanelWindowLookup*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != lookup->pid) return TRUE;
    wchar_t className[64] = {};
    if (GetClassNameW(window, className, 64) == 0) return TRUE;
    if (_wcsicmp(className, kPanelWindowClass) != 0) return TRUE;
    PanelWindowInfo info;
    info.found = true;
    info.window = window;
    info.visible = IsWindowVisible(window) != FALSE;
    info.owned = GetWindow(window, GW_OWNER) != nullptr;
    lookup->info = info;
    return FALSE;  // 每个面板进程只认一个主窗口
}

inline PanelWindowInfo FindPanelWindowForPid(std::uint32_t pid) {
    PanelWindowLookup lookup;
    lookup.pid = pid;
    if (pid != 0) EnumWindows(FindPanelWindowCallback, reinterpret_cast<LPARAM>(&lookup));
    return lookup.info;
}

inline bool PanelWindowVisible(std::uint32_t pid) {
    return FindPanelWindowForPid(pid).visible;
}

struct ProcessIdentity {
    HANDLE process = nullptr;  // 调用方负责 CloseHandle
    std::wstring imagePath;
    std::uint64_t creationTime = 0;  // 100ns since 1601（GetProcessTimes）
};

inline std::uint64_t FileTimeToUint64(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// 打开进程并读取“映像完整路径 + 创建时间”，用于核对 PID 复用与旧标记缺字段。
// 需要结束进程时由调用方把 access 加上 PROCESS_TERMINATE。
inline bool OpenProcessIdentity(std::uint32_t pid, DWORD access, ProcessIdentity& identity, DWORD& win32Error) {
    identity = ProcessIdentity();
    win32Error = ERROR_SUCCESS;
    if (pid == 0) {
        win32Error = ERROR_INVALID_PARAMETER;
        return false;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | access, FALSE, pid);
    if (process == nullptr || process == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    std::vector<wchar_t> buffer(1024, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
        win32Error = GetLastError();
        CloseHandle(process);
        return false;
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        win32Error = GetLastError();
        CloseHandle(process);
        return false;
    }
    identity.process = process;
    identity.imagePath.assign(buffer.data(), size);
    identity.creationTime = FileTimeToUint64(creation);
    return true;
}

// 不持有句柄的轻量身份核验（供收集实例使用）。
inline bool QueryProcessIdentity(std::uint32_t pid, std::wstring& imagePath, std::uint64_t& creationTime,
                                 DWORD& win32Error) {
    ProcessIdentity identity;
    const bool ok = OpenProcessIdentity(pid, 0, identity, win32Error);
    if (ok) {
        imagePath = identity.imagePath;
        creationTime = identity.creationTime;
        CloseHandle(identity.process);
    }
    return ok;
}

// 纯决策函数：只依据实例快照决定复用/接管/拒绝，便于单测覆盖。
// 顺序：其它运行中客户端 > 本客户端实例 > 已登记等待实例 > 可见但归属不明 > 其它残留实例 > 直接启动。
// 关键原则：
//  * 普通打开/自动拉起不结束身份无法证明的进程；显式重开也只结束“身份已证明”
//    （v2 标记：PID + 创建时间 + 映像路径一致）的实例。
//  * 旧版标记（v1，无创建时间）不能证明进程身份：只复用可见窗口，绝不自动交接。
//  * 目标游戏状态为 Unknown（路径读不到）时不做任何接管，保留原进程。
inline PanelInstanceDecision DecidePanelInstance(const std::vector<PanelInstance>& instances,
                                                 const std::wstring& selectedGame,
                                                 bool selectedGameRunning,
                                                 bool forceRestart) {
    PanelInstanceDecision decision;
    // 1) 正在为另一个“运行中的客户端”服务的面板：任何入口都不抢占、不结束。
    //    另一个客户端状态为 Unknown 时也不结束（没有 request 会走到这里，保守起见跳过）。
    for (const auto& instance : instances) {
        if (!instance.alive || instance.pid == 0 || instance.foreign) continue;
        if (instance.gamePath.empty()) continue;
        if (SameImagePath(instance.gamePath, selectedGame)) continue;
        if (instance.gameState != GameProcessState::Running) continue;
        decision.action = PanelInstanceAction::RefuseOtherClientRunning;
        decision.instancePid = instance.pid;
        decision.otherGamePath = instance.gamePath;
        return decision;
    }
    // 1b) 属于其它客户端、但其游戏当前未在运行（等待中或游戏已退出）：保留该实例。
    //     不能因为“游戏已退出”就把它当成可交接的等待实例——它属于另一个客户端。
    for (const auto& instance : instances) {
        if (!instance.alive || instance.pid == 0 || instance.foreign) continue;
        if (instance.gamePath.empty()) continue;
        if (SameImagePath(instance.gamePath, selectedGame)) continue;
        decision.action = PanelInstanceAction::RefuseOtherClientWaiting;
        decision.instancePid = instance.pid;
        decision.otherGamePath = instance.gamePath;
        return decision;
    }
    // 2) 属于所选客户端自己的面板：可见→复用；隐藏时只有“身份已证明 + 游戏确认退出”
    //    才允许交接；旧标记/游戏状态未知一律保留。
    for (const auto& instance : instances) {
        if (!instance.alive || instance.pid == 0 || instance.foreign) continue;
        if (instance.gamePath.empty() || !SameImagePath(instance.gamePath, selectedGame)) continue;
        const bool trusted = instance.identityVerified || instance.markerLegacy;
        if (!trusted) continue;
        decision.instancePid = instance.pid;
        decision.imagePath = instance.imagePath;
        if (forceRestart) {
            // 显式重开必须能证明“这就是登记时的进程”，否则不结束它。
            decision.action = instance.identityVerified ? PanelInstanceAction::LaunchAfterTerminate
                                                        : PanelInstanceAction::RefuseUnidentified;
            return decision;
        }
        if (instance.visible) {
            decision.action = PanelInstanceAction::Reuse;
            return decision;
        }
        if (instance.gameState == GameProcessState::Running || selectedGameRunning) {
            decision.action = PanelInstanceAction::RefuseHiddenOwn;
            return decision;
        }
        if (!instance.identityVerified) {
            // 旧版标记没有创建时间，无法排除 PID 复用：保留进程，请用户手动确认。
            decision.action = PanelInstanceAction::RefuseUnidentified;
            return decision;
        }
        if (instance.gameState == GameProcessState::Unknown) {
            decision.action = PanelInstanceAction::RefuseGameStateUnknown;
            return decision;
        }
        decision.action = PanelInstanceAction::TakeoverWaiting;
        return decision;
    }
    // 3) 已登记目标游戏、且能证明同一进程（v2 身份核验）并确认游戏已退出的等待实例：
    //    普通打开/自动拉起允许正常交接（先 WM_CLOSE 落盘，再启动新实例）。
    for (const auto& instance : instances) {
        if (!instance.alive || instance.pid == 0 || instance.foreign) continue;
        if (instance.gamePath.empty()) continue;
        if (instance.gameState == GameProcessState::Running) continue;
        if (!instance.identityVerified) continue;
        if (instance.gameState == GameProcessState::Unknown) {
            decision.action = PanelInstanceAction::RefuseGameStateUnknown;
            decision.instancePid = instance.pid;
            decision.imagePath = instance.imagePath;
            return decision;
        }
        decision.instancePid = instance.pid;
        decision.imagePath = instance.imagePath;
        decision.action = forceRestart ? PanelInstanceAction::LaunchAfterTerminate
                                       : PanelInstanceAction::TakeoverWaiting;
        return decision;
    }
    // 4) 可见但归属无法确认（无标记/旧版标记无登记/PID 复用后 v2 不符）：保留进程。
    for (const auto& instance : instances) {
        if (!instance.alive || instance.pid == 0 || instance.foreign) continue;
        if (!instance.visible) continue;
        decision.action = PanelInstanceAction::RefuseUnidentified;
        decision.instancePid = instance.pid;
        decision.imagePath = instance.imagePath;
        return decision;
    }
    // 5) 存在隐藏但无法安全接管的残留实例（旧标记/无标记/身份无法证明）：保留进程并说明，
    //    不再直接启动（新实例会被互斥挡住，且用户需要知道还有面板在运行）。
    for (const auto& instance : instances) {
        if (!instance.alive || instance.pid == 0 || instance.foreign) continue;
        decision.action = PanelInstanceAction::RefuseUnidentified;
        decision.instancePid = instance.pid;
        decision.imagePath = instance.imagePath;
        return decision;
    }
    // 6) 没有任何本产品面板实例：直接启动。
    decision.action = PanelInstanceAction::Launch;
    return decision;
}

// Decodes one byte buffer as UTF-8 (invalid sequences become '?'); strips BOM.
inline std::wstring DecodeLog(const std::string& bytes) {
    size_t start = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        start = 3;
    }
    if (start >= bytes.size()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + start, static_cast<int>(bytes.size() - start),
                                          nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data() + start, static_cast<int>(bytes.size() - start), wide.data(), needed);
    return wide;
}

// Reads the last maxLines lines of one log file (not the whole file).
inline bool ReadTailLines(const std::wstring& path, size_t maxLines, std::vector<std::wstring>& lines,
                          DWORD& win32Error) {
    win32Error = ERROR_SUCCESS;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        win32Error = GetLastError();
        CloseHandle(file);
        return false;
    }
    if (size.QuadPart <= 0) {
        CloseHandle(file);
        return true;
    }
    const unsigned long long fileSize = static_cast<unsigned long long>(size.QuadPart);
    const unsigned long long readSize = fileSize < kLogTailMaxBytes ? fileSize : kLogTailMaxBytes;
    LARGE_INTEGER start{};
    start.QuadPart = static_cast<LONGLONG>(fileSize - readSize);
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) {
        win32Error = GetLastError();
        CloseHandle(file);
        return false;
    }
    std::string bytes(static_cast<size_t>(readSize), '\0');
    DWORD total = 0;
    while (total < static_cast<DWORD>(bytes.size())) {
        DWORD got = 0;
        if (!ReadFile(file, bytes.data() + total, static_cast<DWORD>(bytes.size()) - total, &got, nullptr)) {
            win32Error = GetLastError();
            CloseHandle(file);
            return false;
        }
        if (got == 0) break;
        total += got;
    }
    CloseHandle(file);
    bytes.resize(total);
    // If we started mid-file, drop the partial first line.
    if (readSize < fileSize) {
        const size_t newline = bytes.find('\n');
        if (newline == std::string::npos) {
            lines.clear();
            return true;
        }
        bytes.erase(0, newline + 1);
    }
    std::vector<std::string> rawLines;
    size_t position = 0;
    while (position < bytes.size()) {
        const size_t newline = bytes.find('\n', position);
        const size_t end = newline == std::string::npos ? bytes.size() : newline;
        std::string line = bytes.substr(position, end - position);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        rawLines.push_back(std::move(line));
        if (newline == std::string::npos) break;
        position = newline + 1;
    }
    // A file ending with a newline yields a trailing empty element; drop it.
    if (!rawLines.empty() && rawLines.back().empty() && !bytes.empty() && bytes.back() == '\n') rawLines.pop_back();
    const size_t begin = rawLines.size() > maxLines ? rawLines.size() - maxLines : 0;
    lines.clear();
    for (size_t i = begin; i < rawLines.size(); ++i) lines.push_back(DecodeLog(rawLines[i]));
    return true;
}

inline HANDLE OpenAppendHandle(const std::wstring& path) {
    const HANDLE handle = CreateFileW(path.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    return handle;
}

// ---------- 可选的 panel-lock.json 哈希校验 ----------
// 只读、只警告，不改变启动结果；锁文件缺失/非法时返回说明文本，绝不阻断启动。

inline bool ReadSmallTextFile(const std::wstring& path, std::string& bytes, DWORD& win32Error) {
    bytes.clear();
    win32Error = ERROR_SUCCESS;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        win32Error = GetLastError();
        CloseHandle(file);
        return false;
    }
    if (size.QuadPart < 0 || size.QuadPart > 64 * 1024) {
        win32Error = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD got = 0;
        const DWORD chunk = static_cast<DWORD>(bytes.size() - offset);
        if (!ReadFile(file, bytes.data() + offset, chunk, &got, nullptr)) {
            win32Error = GetLastError();
            CloseHandle(file);
            bytes.clear();
            return false;
        }
        if (got == 0) break;
        offset += got;
    }
    CloseHandle(file);
    bytes.resize(offset);
    return true;
}

// ---------- 面板归属标记（<dataDir>\panel-instances\<pid>.txt） ----------
// v2 文本格式（UTF-8）：
//   dlss5-panel-instance 2
//   pid=<十进制>
//   created=<100ns since 1601>
//   image=<面板映像完整路径>
//   game=<目标 wow.exe 完整路径>
// v1（旧版）只有两行：PID、game 路径。读到 v1 时只把 game 路径当提示，
// 不得据此结束进程（缺创建时间，无法排除 PID 复用）。标记始终只是提示，
// 读取方必须再核对活进程的映像路径/创建时间。

constexpr wchar_t kPanelInstanceMarkerHeader[] = L"dlss5-panel-instance 2";

struct PanelInstanceMarker {
    bool version2 = false;
    std::uint32_t pid = 0;
    std::uint64_t creationTime = 0;
    std::wstring imagePath;
    std::wstring gamePath;
};

inline std::wstring PanelInstanceDirectory(const std::wstring& dataDir) {
    return Join(dataDir, L"panel-instances");
}

inline std::wstring PanelInstanceMarkerPath(const std::wstring& dataDir, std::uint32_t pid) {
    return Join(PanelInstanceDirectory(dataDir), std::to_wstring(pid) + L".txt");
}

inline bool WritePanelInstanceText(const std::wstring& dataDir, std::uint32_t pid, const std::wstring& content) {
    if (dataDir.empty() || pid == 0) return false;
    const std::wstring directory = PanelInstanceDirectory(dataDir);
    DWORD directoryError = ERROR_SUCCESS;
    if (!ds5_detail::CreateDirectoryTree(directory, directoryError)) return false;
    const std::string bytes = Narrow(content);
    const std::wstring path = PanelInstanceMarkerPath(dataDir, pid);
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
                    written == bytes.size() && FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// 写入 v2 标记：PID + 创建时间 + 映像路径 + 目标游戏，用于排除 PID 复用。
inline bool WritePanelInstanceMarker(const std::wstring& dataDir, std::uint32_t pid, std::uint64_t creationTime,
                                     const std::wstring& imagePath, const std::wstring& gamePath) {
    std::wstring content = kPanelInstanceMarkerHeader;
    content += L"\npid=" + std::to_wstring(pid);
    content += L"\ncreated=" + std::to_wstring(creationTime);
    content += L"\nimage=" + imagePath;
    content += L"\ngame=" + gamePath;
    content += L"\n";
    return WritePanelInstanceText(dataDir, pid, content);
}

// 兼容旧调用/测试：写 v1 标记（只有 PID + game）。
inline bool WritePanelInstanceMarker(const std::wstring& dataDir, std::uint32_t pid, const std::wstring& gamePath) {
    return WritePanelInstanceText(dataDir, pid, std::to_wstring(pid) + L"\n" + gamePath + L"\n");
}

inline std::wstring TrimMarkerValue(const std::wstring& line, const wchar_t* key) {
    const size_t keyLength = wcslen(key);
    if (line.size() < keyLength || line.compare(0, keyLength, key) != 0) return std::wstring();
    std::wstring value = line.substr(keyLength);
    while (!value.empty() && (value.back() == L'\r' || value.back() == L' ')) value.pop_back();
    return value;
}

inline bool ReadPanelInstanceMarker(const std::wstring& path, PanelInstanceMarker& marker) {
    marker = PanelInstanceMarker();
    std::string bytes;
    DWORD error = ERROR_SUCCESS;
    if (!ReadSmallTextFile(path, bytes, error)) return false;
    const std::wstring text = DecodeLog(bytes);
    const size_t firstBreak = text.find(L'\n');
    if (firstBreak == std::wstring::npos) return false;
    const std::wstring firstLine = text.substr(0, firstBreak);
    if (firstLine == kPanelInstanceMarkerHeader) {
        marker.version2 = true;
        size_t position = firstBreak + 1;
        while (position < text.size()) {
            size_t end = text.find(L'\n', position);
            if (end == std::wstring::npos) end = text.size();
            const std::wstring line = text.substr(position, end - position);
            position = end + 1;
            if (line.empty()) continue;
            if (const std::wstring pidValue = TrimMarkerValue(line, L"pid="); !pidValue.empty()) {
                try {
                    const unsigned long pid = std::stoul(pidValue);
                    if (pid != 0 && pid <= 0xFFFFFFFFul) marker.pid = static_cast<std::uint32_t>(pid);
                } catch (...) {
                    return false;
                }
            } else if (const std::wstring createdValue = TrimMarkerValue(line, L"created="); !createdValue.empty()) {
                try {
                    marker.creationTime = std::stoull(createdValue);
                } catch (...) {
                    return false;
                }
            } else if (const std::wstring imageValue = TrimMarkerValue(line, L"image="); !imageValue.empty()) {
                marker.imagePath = imageValue;
            } else if (const std::wstring gameValue = TrimMarkerValue(line, L"game="); !gameValue.empty()) {
                marker.gamePath = gameValue;
            }
        }
        return marker.pid != 0 && marker.creationTime != 0 && !marker.imagePath.empty() &&
               !marker.gamePath.empty();
    }
    // v1：pid 行 + game 行。
    try {
        const unsigned long pid = std::stoul(firstLine);
        if (pid == 0 || pid > 0xFFFFFFFFul) return false;
        marker.pid = static_cast<std::uint32_t>(pid);
    } catch (...) {
        return false;
    }
    size_t secondStart = firstBreak + 1;
    size_t secondEnd = text.find(L'\n', secondStart);
    if (secondEnd == std::wstring::npos) secondEnd = text.size();
    marker.gamePath = text.substr(secondStart, secondEnd - secondStart);
    return !marker.gamePath.empty();
}

// 收集本机所有 dlss5-panel.exe 实例：PID/创建时间/映像路径逐项核对标记，
// 再叠加面板窗口身份（含带 owner 的正常结构）与目标游戏存活状态。
// expectedImage 非空时，映像路径不是本产品面板的实例标记为 foreign：不参与
// 接管决策，也绝不结束。同一份进程快照同时用于“目标游戏是否在跑”的判断。
inline std::vector<PanelInstance> CollectPanelInstances(const std::wstring& dataDir,
                                                        const std::wstring& expectedImage = std::wstring()) {
    std::vector<PanelInstanceMarker> markers;
    std::vector<std::wstring> markerPaths;
    if (!dataDir.empty()) {
        const std::wstring directory = PanelInstanceDirectory(dataDir);
        WIN32_FIND_DATAW find{};
        const HANDLE handle = FindFirstFileW(Join(directory, L"*.txt").c_str(), &find);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = find.cFileName;
                if (name == L"." || name == L"..") continue;
                const std::wstring path = Join(directory, name);
                PanelInstanceMarker marker;
                if (ReadPanelInstanceMarker(path, marker)) {
                    markers.push_back(marker);
                    markerPaths.push_back(path);
                } else {
                    DeleteFileW(path.c_str());
                }
            } while (FindNextFileW(handle, &find));
            FindClose(handle);
        }
    }
    const ProcessSnapshot snapshot = TakeProcessSnapshot();
    std::vector<bool> markerUsed(markers.size(), false);
    std::vector<bool> markerVerified(markers.size(), false);
    std::vector<PanelInstance> instances;
    for (const auto& process : snapshot.entries) {
        if (!NameEquals(process.name, L"dlss5-panel.exe")) continue;
        PanelInstance instance;
        instance.pid = process.pid;
        instance.alive = true;
        std::wstring image;
        std::uint64_t creationTime = 0;
        DWORD identityError = ERROR_SUCCESS;
        const bool identityRead = QueryProcessIdentity(process.pid, image, creationTime, identityError);
        instance.imagePath = image;
        instance.creationTime = creationTime;
        for (size_t i = 0; i < markers.size(); ++i) {
            if (markerUsed[i] || markers[i].pid != process.pid) continue;
            markerUsed[i] = true;
            instance.gamePath = markers[i].gamePath;
            if (markers[i].version2 && identityRead && markers[i].creationTime == creationTime &&
                SameImagePath(markers[i].imagePath, image)) {
                instance.identityVerified = true;
                markerVerified[i] = true;
            } else if (!markers[i].version2) {
                instance.markerLegacy = true;
            }
            break;
        }
        instance.foreign = !expectedImage.empty() && identityRead && !SameImagePath(image, expectedImage);
        instance.gameState = GameStateByImagePathIn(instance.gamePath, snapshot);
        const PanelWindowInfo window = FindPanelWindowForPid(instance.pid);
        instance.windowFound = window.found;
        instance.visible = window.visible;
        instance.windowOwned = window.owned;
        instances.push_back(instance);
    }
    // 清理陈旧标记：进程已退出/已不再是面板，或 v2 标记与活进程身份不符（PID 复用）。
    for (size_t i = 0; i < markers.size(); ++i) {
        if (markerUsed[i] && (markerVerified[i] || !markers[i].version2)) continue;
        DeleteFileW(markerPaths[i].c_str());
    }
    return instances;
}

// ---------- 实例接管：先核验身份，再请求正常关闭，超时才终止 ----------
struct PanelTakeoverResult {
    bool ok = false;
    bool graceful = false;
    bool terminated = false;
    DWORD win32Error = ERROR_SUCCESS;
    std::wstring error;
};

// 结束一个已收集的实例。所有步骤绑定同一个进程句柄，避免“核验后 PID 被复用”。
// 只接受 v2 身份核验过的实例：旧版标记没有创建时间，无法证明当前进程就是登记
// 时的进程（PID 可能被同一路径的新面板复用），一律拒绝并保留。
inline PanelTakeoverResult TakeOverPanelInstance(const PanelInstance& instance, DWORD gracefulTimeoutMs = 6000,
                                                 DWORD terminateTimeoutMs = 3000) {
    PanelTakeoverResult result;
    if (instance.pid == 0) {
        result.ok = true;
        result.graceful = true;
        return result;
    }
    if (!instance.identityVerified) {
        result.error = L"无法证明旧面板实例与登记一致（旧版本标记缺少创建时间，或 PID 已被复用），已保留该进程。";
        result.win32Error = ERROR_INVALID_DATA;
        return result;
    }
    ProcessIdentity identity;
    DWORD error = ERROR_SUCCESS;
    if (!OpenProcessIdentity(instance.pid, PROCESS_TERMINATE, identity, error)) {
        result.win32Error = error;
        // 进程已经退出：视为已完成交接。
        if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND) {
            result.ok = true;
            result.graceful = true;
            result.terminated = false;
            return result;
        }
        result.error = L"无法打开旧面板进程（Win32 错误 " + std::to_wstring(error) + L"）。";
        return result;
    }
    // 同一句柄上复核身份：映像路径与创建时间都必须与登记一致。
    const bool identityMatches = SameImagePath(identity.imagePath, instance.imagePath) &&
                                 identity.creationTime == instance.creationTime;
    if (!identityMatches) {
        CloseHandle(identity.process);
        result.error = L"旧面板进程身份与登记不符（PID 可能已被复用），已保留该进程。";
        return result;
    }
    const PanelWindowInfo window = FindPanelWindowForPid(instance.pid);
    if (window.found) {
        // 请求正常关闭：面板的 WM_CLOSE → WM_DESTROY 会先刷新待保存参数。
        PostMessageW(window.window, WM_CLOSE, 0, 0);
    }
    DWORD wait = WaitForSingleObject(identity.process, gracefulTimeoutMs);
    if (wait == WAIT_OBJECT_0) {
        result.ok = true;
        result.graceful = true;
        CloseHandle(identity.process);
        return result;
    }
    if (!TerminateProcess(identity.process, 1)) {
        result.win32Error = GetLastError();
        CloseHandle(identity.process);
        result.error = L"旧面板进程未在时限内退出，且无法结束（Win32 错误 " +
                       std::to_wstring(result.win32Error) + L"）。";
        return result;
    }
    result.terminated = true;
    wait = WaitForSingleObject(identity.process, terminateTimeoutMs);
    result.win32Error = wait == WAIT_OBJECT_0 ? ERROR_SUCCESS : WAIT_TIMEOUT;
    CloseHandle(identity.process);
    if (wait != WAIT_OBJECT_0) {
        result.error = L"旧面板进程在结束请求后仍未退出。";
        return result;
    }
    result.ok = true;
    return result;
}

inline bool TerminatePanelInstance(std::uint32_t pid, DWORD& win32Error) {
    win32Error = ERROR_SUCCESS;
    if (pid == 0) return true;
    const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (process == nullptr || process == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    const bool terminated = TerminateProcess(process, 1) != 0;
    if (!terminated) win32Error = GetLastError();
    WaitForSingleObject(process, 3000);
    CloseHandle(process);
    return terminated;
}

inline bool IsSha256Hex(const std::string& text) {
    if (text.size() != 64) return false;
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper) return false;
    }
    return true;
}

inline std::string LowerHexText(const std::string& text) {
    std::string lower = text;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return lower;
}

inline std::string Sha256HexText(const std::array<std::uint8_t, 32>& hash) {
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (const std::uint8_t b : hash) {
        text.push_back(hex[b >> 4]);
        text.push_back(hex[b & 15]);
    }
    return text;
}

// 校验 <exeDir>\panel-lock.json 中的 dlss5-panel.exe 哈希；任何异常都降级为提示文本。
inline std::wstring VerifyPanelLockHash(const std::wstring& exeDir, const std::wstring& panelExe) {
    const std::wstring lockPath = Join(exeDir, L"panel-lock.json");
    std::string text;
    DWORD readError = ERROR_SUCCESS;
    if (!ReadSmallTextFile(lockPath, text, readError)) {
        if (readError == ERROR_FILE_NOT_FOUND || readError == ERROR_PATH_NOT_FOUND) {
            return L"面板哈希校验：未找到 panel-lock.json，已跳过。";
        }
        return L"面板哈希校验：无法读取 panel-lock.json，已跳过。";
    }
    const auto parsed = ParseJsonObject(text);
    if (!parsed || !parsed.value.is_object()) {
        return L"面板哈希校验：panel-lock.json 无法解析，已跳过。";
    }
    try {
        const auto schema = parsed.value.find("schema");
        if (schema == parsed.value.end() || !schema->is_number_integer() || schema->get<long long>() != 1) {
            return L"面板哈希校验：panel-lock.json schema 无效，已跳过。";
        }
        const auto files = parsed.value.find("files");
        if (files == parsed.value.end() || !files->is_object()) {
            return L"面板哈希校验：panel-lock.json 缺少 files，已跳过。";
        }
        const auto hash = files->find("dlss5-panel.exe");
        if (hash == files->end() || !hash->is_string()) {
            return L"面板哈希校验：panel-lock.json 缺少 dlss5-panel.exe 哈希，已跳过。";
        }
        const std::string locked = LowerHexText(hash->get<std::string>());
        if (!IsSha256Hex(locked)) {
            return L"面板哈希校验：panel-lock.json 的 dlss5-panel.exe 哈希格式无效，已跳过。";
        }
        const HashResult hashed = HashFile(panelExe);
        if (!hashed) {
            return L"面板哈希校验：无法读取 bin/dlss5-panel.exe，已跳过。";
        }
        if (LowerHexText(Sha256HexText(hashed.identity.sha256)) != locked) {
            return L"面板哈希校验：bin/dlss5-panel.exe 与 panel-lock.json 记录不一致，可能已被替换。";
        }
        return std::wstring();
    } catch (...) {
        // 结构异常的数据不应影响启动，只降级为提示。
        return L"面板哈希校验：panel-lock.json 结构异常，已跳过。";
    }
}

inline PanelStartResult StartPanel(const std::wstring& exeDir, const std::wstring& dataDir,
                                   const std::wstring& clientRoot,
                                   const PanelStartOptions& options = PanelStartOptions()) {
    PanelStartResult result;
    const std::wstring panelExe = Join(Join(exeDir, L"bin"), L"dlss5-panel.exe");
    if (!ds5_detail::FileExistsNoReparse(panelExe)) {
        result.status = PanelStartStatus::PanelMissing;
        result.error = L"面板程序缺失：bin/dlss5-panel.exe";
        return result;
    }
    if (options.verifyPanelHash) result.hashWarning = VerifyPanelLockHash(exeDir, panelExe);

    const std::wstring gameExe = Join(clientRoot, L"wow.exe");
    if (options.requireSelectedClientRunning) {
        // R6：面板常驻并占用 DLSS5PanelMutex，只有“所选客户端”的游戏在运行时才允许复用/启动。
        if (!ds5_detail::FileExistsNoReparse(gameExe)) {
            result.status = PanelStartStatus::ClientGameNotRunning;
            result.error = L"请先选择含 wow.exe 的客户端目录。";
            return result;
        }
        const ClientGameCheck check = CheckClientGameProcess(clientRoot);
        // 归属核对以本产品 bin\dlss5-panel.exe 的完整路径为基准（另一目录的同名
        // 程序不算本产品实例）。窗口识别按 PID + 面板窗口类，兼容 AIO owner 结构。
        const std::wstring expectedImage = panelExe;
        const std::vector<PanelInstance> instances = CollectPanelInstances(dataDir, expectedImage);
        // 只有本产品路径的面板进程参与“是否已在运行”的判断；另一目录的同名程序
        // （foreign）既不算已连接，也不会被结束。
        bool panelRunning = false;
        for (const auto& instance : instances) {
            if (!instance.foreign) panelRunning = true;
        }
        if (check.matching == 0 && check.state == ClientGameState::Unknown) {
            result.status = PanelStartStatus::ClientStateUnknown;
            result.error = L"无法确认所选客户端的游戏进程状态，请稍后重试。";
            result.win32Error = check.win32Error;
            return result;
        }
        if (check.matching == 0) {
            if (panelRunning) {
                // 面板在跑但所选客户端游戏未运行：不结束任何实例（可能属于其它
                // 客户端、也可能是可交接的等待实例，交给显式“重新打开”处理）。
                result.status = PanelStartStatus::AlreadyRunningOtherClient;
                result.error = L"游戏内面板已在运行，但所选客户端未运行；它可能属于其他客户端。请先启动所选客户端的游戏，再打开面板。";
            } else if (check.otherWow > 0) {
                result.status = PanelStartStatus::ClientPathMismatch;
                result.error = L"检测到其他客户端的 wow.exe 正在运行；请先启动所选客户端的游戏。";
            } else {
                result.status = PanelStartStatus::ClientGameNotRunning;
                result.error = L"所选客户端未运行：请先启动游戏，再打开游戏内面板。";
            }
            return result;
        }
        if (panelRunning) {
            const PanelInstanceDecision decision =
                DecidePanelInstance(instances, gameExe, true, options.forceRestart);
            if (decision.action == PanelInstanceAction::Reuse) {
                result.ok = true;
                result.status = PanelStartStatus::AlreadyRunning;
                result.error = L"游戏内面板已在运行（已连接所选客户端）。";
                result.pid = decision.instancePid;
                return result;
            }
            if (decision.action == PanelInstanceAction::RefuseOtherClientRunning) {
                result.status = PanelStartStatus::AlreadyRunningOtherClient;
                result.error = L"游戏内面板正在为另一个客户端服务：" + decision.otherGamePath +
                               L"。请先退出该客户端的游戏，或结束该面板进程后重试。";
                result.pid = decision.instancePid;
                return result;
            }
            if (decision.action == PanelInstanceAction::RefuseOtherClientWaiting) {
                result.status = PanelStartStatus::AlreadyRunningOtherClient;
                result.error = L"另一个客户端的面板实例仍在等待（PID " +
                               std::to_wstring(decision.instancePid) + L"，客户端：" +
                               decision.otherGamePath +
                               L"）；为避免抢占，已保留该进程。请关闭该面板，或切换到该客户端后重试。";
                result.pid = decision.instancePid;
                return result;
            }
            if (decision.action == PanelInstanceAction::RefuseUnidentified) {
                result.status = PanelStartStatus::PanelUnverified;
                result.error = L"检测到一个无法证明归属的游戏内面板进程（PID " +
                               std::to_wstring(decision.instancePid) +
                               L"）；已保留该进程。旧版本归属标记缺少创建时间，无法排除 PID 复用；"
                               L"请确认后手动结束它，或选择该面板所属的客户端。";
                result.pid = decision.instancePid;
                return result;
            }
            if (decision.action == PanelInstanceAction::RefuseGameStateUnknown) {
                result.status = PanelStartStatus::PanelGameStateUnknown;
                result.error = L"无法确认旧游戏内面板（PID " + std::to_wstring(decision.instancePid) +
                               L"）所属的游戏是否已经退出（有游戏进程的映像路径不可读），已保留该进程。"
                               L"请先确认该客户端未在运行，再手动结束面板后重试。";
                result.pid = decision.instancePid;
                result.win32Error = ERROR_ACCESS_DENIED;
                return result;
            }
            if (decision.action == PanelInstanceAction::RefuseHiddenOwn) {
                result.status = PanelStartStatus::PanelHiddenOwn;
                result.error = L"所选客户端的面板进程在运行，但窗口不可见（PID " +
                               std::to_wstring(decision.instancePid) +
                               L"）。请使用「重新打开面板」接管，或切换到游戏窗口后重试。";
                result.pid = decision.instancePid;
                return result;
            }
            if (dataDir.empty()) {
                result.status = PanelStartStatus::LogDirectoryFailed;
                result.error = L"无法确定日志目录。";
                result.win32Error = ERROR_PATH_NOT_FOUND;
                return result;
            }
            // 交接：只结束决策允许的实例——同一进程句柄上复核身份（v2 创建时间必须一致），
            // 先请求 WM_CLOSE 让面板刷新待保存参数，超时才终止。旧版标记无法证明进程
            // 身份（PID 可能被同一路径的新面板复用），绝不在此结束。
            for (const auto& instance : instances) {
                if (!instance.alive || instance.pid == 0 || instance.foreign) continue;
                bool shouldTerminate = false;
                if (decision.instancePid != 0 && instance.pid == decision.instancePid) {
                    shouldTerminate = true;
                } else if (decision.action == PanelInstanceAction::LaunchAfterTerminate) {
                    // 显式重开：只结束决策选中的实例，避免误杀其它等待实例。
                    shouldTerminate = false;
                } else if (instance.identityVerified && !instance.gamePath.empty() &&
                           instance.gameState == GameProcessState::NotRunning) {
                    // 普通打开/自动拉起：可清理“身份已证明、目标游戏确认已退出”的等待实例，
                    // 它们会占用 DLSS5PanelMutex 导致新实例立即退出。Unknown 不动。
                    shouldTerminate = true;
                }
                if (!shouldTerminate) continue;
                if (!instance.identityVerified) {
                    // 决策只会在已核验的实例上给出交接；这里再兜底一次，防止将来误改。
                    result.status = PanelStartStatus::PanelUnverified;
                    result.error = L"无法证明旧面板实例与登记一致，已保留该进程（PID " +
                                   std::to_wstring(instance.pid) + L"）。";
                    result.pid = instance.pid;
                    return result;
                }
                if (!shouldTerminate) continue;
                const PanelTakeoverResult takeover = TakeOverPanelInstance(instance);
                if (!takeover.ok) {
                    result.status = PanelStartStatus::TakeoverFailed;
                    result.error = takeover.error.empty()
                                       ? L"无法结束旧的游戏内面板进程（PID " +
                                             std::to_wstring(instance.pid) + L"）。"
                                       : takeover.error;
                    result.win32Error = takeover.win32Error;
                    result.pid = instance.pid;
                    return result;
                }
                result.terminatedPids.push_back(instance.pid);
                DeleteFileW(PanelInstanceMarkerPath(dataDir, instance.pid).c_str());
            }
            Sleep(200);
        }
    } else {
        if (ProcessRunning(L"dlss5-panel.exe")) {
            result.status = PanelStartStatus::AlreadyRunning;
            result.error = L"游戏内面板已在运行。";
            return result;
        }
        if (!ds5_detail::FileExistsNoReparse(gameExe)) {
            result.status = PanelStartStatus::ClientGameNotRunning;
            result.error = L"请先选择含 wow.exe 的客户端目录。";
            return result;
        }
    }
    if (dataDir.empty()) {
        result.status = PanelStartStatus::LogDirectoryFailed;
        result.error = L"无法确定日志目录。";
        result.win32Error = ERROR_PATH_NOT_FOUND;
        return result;
    }
    const std::wstring token = ds5_detail::Widen(ds5_detail::UniqueToken());
    const std::wstring logDir = Join(Join(dataDir, L"panel-logs"), token);
    DWORD directoryError = ERROR_SUCCESS;
    if (!ds5_detail::CreateDirectoryTree(logDir, directoryError)) {
        result.status = PanelStartStatus::LogDirectoryFailed;
        result.error = L"无法创建面板日志目录。";
        result.win32Error = directoryError;
        return result;
    }
    const std::wstring stdoutPath = Join(logDir, L"stdout.log");
    const std::wstring stderrPath = Join(logDir, L"stderr.log");
    const HANDLE stdoutHandle = OpenAppendHandle(stdoutPath);
    if (stdoutHandle == INVALID_HANDLE_VALUE) {
        result.status = PanelStartStatus::LogDirectoryFailed;
        result.error = L"无法创建面板标准输出日志。";
        result.win32Error = GetLastError();
        return result;
    }
    const HANDLE stderrHandle = OpenAppendHandle(stderrPath);
    if (stderrHandle == INVALID_HANDLE_VALUE) {
        result.status = PanelStartStatus::LogDirectoryFailed;
        result.win32Error = GetLastError();
        CloseHandle(stdoutHandle);
        result.error = L"无法创建面板标准错误日志。";
        return result;
    }
    const HANDLE nilInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nilInput != INVALID_HANDLE_VALUE) SetHandleInformation(nilInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    std::wstring command = L"\"" + panelExe + L"\" --game \"" + gameExe + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nilInput == INVALID_HANDLE_VALUE ? GetStdHandle(STD_INPUT_HANDLE) : nilInput;
    startup.hStdOutput = stdoutHandle;
    startup.hStdError = stderrHandle;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(panelExe.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, exeDir.c_str(), &startup, &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    if (nilInput != INVALID_HANDLE_VALUE) CloseHandle(nilInput);
    CloseHandle(stdoutHandle);
    CloseHandle(stderrHandle);
    if (!created) {
        result.status = PanelStartStatus::LaunchFailed;
        result.error = L"启动游戏内面板失败。";
        result.win32Error = createError;
        return result;
    }
    CloseHandle(process.hThread);
    // 启动后短暂观察：已有实例占用互斥或面板立即崩溃时，不要误报“已连接”。
    const DWORD startupWait = WaitForSingleObject(process.hProcess, 300);
    std::uint64_t creationTime = 0;
    FILETIME creationFileTime{};
    FILETIME exitFileTime{};
    FILETIME kernelFileTime{};
    FILETIME userFileTime{};
    if (GetProcessTimes(process.hProcess, &creationFileTime, &exitFileTime, &kernelFileTime, &userFileTime)) {
        creationTime = FileTimeToUint64(creationFileTime);
    }
    if (startupWait == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        result.status = PanelStartStatus::LaunchExitedImmediately;
        result.error = L"游戏内面板启动后立即退出（退出码 " + std::to_wstring(exitCode) +
                       L"）；可能已有另一个面板实例占用，或面板启动失败。请查看面板日志或使用「重新打开面板」。";
        return result;
    }
    CloseHandle(process.hProcess);
    result.ok = true;
    result.status = PanelStartStatus::None;
    result.logDir = logDir;
    result.pid = process.dwProcessId;
    // 记录面板归属（PID + 创建时间 + 映像路径 + 目标游戏），供后续复用/交接判定；
    // 写失败只降级为提示，不能影响已经启动成功的面板。
    const bool markerWritten = creationTime != 0 &&
                               WritePanelInstanceMarker(dataDir, result.pid, creationTime, panelExe, gameExe);
    if (!markerWritten) {
        WritePanelInstanceMarker(dataDir, result.pid, gameExe);  // 旧格式兜底
        result.warning = L"面板已启动，但未能记录完整归属标记；若本机有多个客户端，下次可能误判面板归属。";
    }
    return result;
}

}  // namespace panel_detail

// 面板进程是否已在运行（按映像名 dlss5-panel.exe，不区分大小写）。
inline bool PanelProcessRunning() {
    return panel_detail::ProcessRunning(L"dlss5-panel.exe");
}

// 所选客户端的 wow.exe 是否在运行（按完整映像路径判定）。
inline ClientGameCheck CheckClientGameRunning(const std::wstring& clientRoot) {
    return panel_detail::CheckClientGameProcess(clientRoot);
}

// 当前运行的面板实例是否属于所选客户端（v2 标记身份核验 + 窗口可见性）。
// 没有标记/旧版标记/身份不符的实例一律不算“已连接”：旧标记缺少创建时间，
// 无法排除 PID 被同一路径的新面板复用，只能提示无法确认归属。
inline bool PanelInstanceMatchesClient(const std::wstring& dataDir, const std::wstring& clientRoot,
                                       std::uint32_t* pid = nullptr) {
    if (clientRoot.empty()) return false;
    const std::wstring gameExe = panel_detail::Join(clientRoot, L"wow.exe");
    for (const auto& instance : panel_detail::CollectPanelInstances(dataDir)) {
        if (!instance.alive || instance.pid == 0 || instance.foreign) continue;
        if (!instance.identityVerified) continue;
        if (instance.gamePath.empty() || !panel_detail::SameImagePath(instance.gamePath, gameExe)) continue;
        if (!instance.visible) continue;
        if (pid != nullptr) *pid = instance.pid;
        return true;
    }
    return false;
}

// UI 用的面板归属快照。每秒在后台任务里计算，UI 线程只读取结果。
struct PanelOwnershipSnapshot {
    bool valid = false;                // 是否成功完成一次检查
    bool anyRunning = false;           // 是否有本产品面板进程（不含 foreign）
    bool connected = false;            // 所选客户端的面板可见且身份已核验（v2）
    bool hiddenOwn = false;            // 所选客户端面板隐藏，且目标游戏仍在运行
    bool hiddenOwnExited = false;      // 所选客户端面板隐藏，目标游戏已确认退出（可交接）
    bool hiddenOwnUncertain = false;   // 所选客户端面板隐藏，但游戏状态无法判断（保留进程）
    bool blockedByOther = false;       // 其它客户端的游戏仍在运行，面板正在为其服务
    bool blockedByUnverified = false;  // 存在无法证明归属的面板进程（旧标记/无标记/PID 复用）
    std::uint32_t connectedPid = 0;
    std::uint32_t hiddenOwnPid = 0;
    std::size_t count = 0;
};

inline PanelOwnershipSnapshot CollectPanelOwnership(const std::wstring& dataDir, const std::wstring& clientRoot,
                                                    const std::wstring& expectedImage = std::wstring()) {
    PanelOwnershipSnapshot snapshot;
    if (dataDir.empty() || clientRoot.empty()) return snapshot;
    const std::wstring gameExe = panel_detail::Join(clientRoot, L"wow.exe");
    const std::vector<PanelInstance> instances = panel_detail::CollectPanelInstances(dataDir, expectedImage);
    snapshot.valid = true;
    snapshot.count = instances.size();
    for (const auto& instance : instances) {
        if (!instance.alive || instance.pid == 0 || instance.foreign) continue;
        snapshot.anyRunning = true;
        const bool sameGame =
            !instance.gamePath.empty() && panel_detail::SameImagePath(instance.gamePath, gameExe);
        const bool trusted = instance.identityVerified;
        if (sameGame && trusted && instance.visible) {
            snapshot.connected = true;
            snapshot.connectedPid = instance.pid;
        } else if (sameGame && trusted && !instance.visible) {
            if (instance.gameState == GameProcessState::Running) {
                snapshot.hiddenOwn = true;
            } else if (instance.gameState == GameProcessState::NotRunning) {
                snapshot.hiddenOwnExited = true;
            } else {
                snapshot.hiddenOwnUncertain = true;
            }
            if (snapshot.hiddenOwnPid == 0) snapshot.hiddenOwnPid = instance.pid;
        } else if (!sameGame && instance.gameState == GameProcessState::Running) {
            snapshot.blockedByOther = true;
        } else {
            // 旧版/无标记/PID 复用后的遗留面板：可见或隐藏都不认作已连接，
            // 也不在 UI 上建议自动接管；只能提示用户手动确认。
            snapshot.blockedByUnverified = true;
        }
    }
    return snapshot;
}

// 读取一段日志目录：依次读 stdout.log、stderr.log，各自取最后 40 行并拼接。
inline LogTailResult ReadLogTail(const std::wstring& logDir) {
    using namespace panel_detail;
    LogTailResult result;
    if (logDir.empty()) {
        result.error = L"日志目录为空。";
        return result;
    }
    for (const wchar_t* name : {L"stdout.log", L"stderr.log"}) {
        const std::wstring path = Join(logDir, name);
        std::vector<std::wstring> lines;
        DWORD error = ERROR_SUCCESS;
        if (!ReadTailLines(path, kLogTailMaxLines, lines, error)) {
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            result.win32Error = error;
            result.error = L"读取日志失败。";
            return result;
        }
        result.lines.insert(result.lines.end(), lines.begin(), lines.end());
    }
    result.ok = true;
    return result;
}

// 启动客户端 wow.exe（不等待退出、不提权、不注入）。
inline GameStartResult StartClientGame(const std::wstring& clientRoot) {
    GameStartResult result;
    if (clientRoot.empty()) {
        result.error = L"请先在「切换客户端」中选择含 wow.exe 的目录。";
        return result;
    }
    const std::wstring exe = panel_detail::Join(clientRoot, L"wow.exe");
    if (!ds5_detail::FileExistsNoReparse(exe)) {
        result.error = L"请先在「切换客户端」中选择含 wow.exe 的目录。";
        return result;
    }
    std::wstring command = L"\"" + exe + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, clientRoot.c_str(),
                        &startup, &process)) {
        result.win32Error = GetLastError();
        result.error = L"启动游戏失败。";
        return result;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result.ok = true;
    result.pid = process.dwProcessId;
    return result;
}

}  // namespace control_center
