// WOW-335-NR 原生控制中心：鼠标助手（window watch）启动。
//
// 对应旧实现 Start-Dlss5WindowWatch（Core:1054-1095），保留同一组门槛与文案：
//   1. 客户端必须含 wow.exe；
//   2. ReShade.ini / dlss5-aio-x86.cfg 的组合必须是“独立输出候选”
//      （AIO 管线开、CPU 桥开、输入缩放开、FG 关）；
//   3. 助手程序只允许受控构建（<产品目录>\bin\window_watch.exe）或 _scratch 里的候选；
//   4. watcher-lock.json 的三份哈希必须与助手、window_watch.cpp、window_layer.hpp 一致；
//   5. 同一助手路径不允许重复启动；
//   6. 日志写入用户数据目录下的 watch-<token>，不向游戏目录写运行时日志。
//
// 相对旧实现的差异：日志目录改为 <operationsRoot>\watch-<token>（原生事务目录），
// 进程互斥按完整映像路径比较（旧实现用 WMI，见 spec §4.4 的验收要求）。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "client_identity.hpp"    // WithinRoot
#include "diagnostics_export.hpp" // diagnostics_detail::{ReadWholeBytes, Widen}, HexSha256
#include "ds5_settings.hpp"       // ds5_detail::*
#include "file_identity.hpp"      // HashFile
#include "panel_process.hpp"      // panel_detail::{EnumerateProcesses, Join, OpenAppendHandle}
#include "strict_json.hpp"        // ParseJsonObject

namespace control_center {

constexpr size_t kWindowWatchLockMaxBytes = 1u << 20;

struct WindowWatchStartResult {
    bool ok = false;
    DWORD win32Error = ERROR_SUCCESS;
    std::wstring error;
    std::wstring logDir;
    std::wstring message;
    std::uint32_t pid = 0;
};

namespace window_watch_detail {

struct WatchConfig {
    bool ok = false;
    std::string frameGeneration;
    std::string detachedPresentation;
    std::string windowedInputScaling;
    std::string pipelineEnabled;
    std::string classicBridge;
};

// Collects every matching key value in the whole cfg file (any section).
inline void CollectCfgValues(const std::string& text, const char* key, std::vector<std::string>& values) {
    values.clear();
    size_t position = 0;
    while (position < text.size()) {
        ds5_detail::IniLine line;
        if (!ds5_detail::ReadLine(text, position, text.size(), line)) break;
        size_t valueBegin = 0;
        size_t valueEnd = 0;
        if (ds5_detail::MatchKeyLine(text, line, key, valueBegin, valueEnd)) {
            values.push_back(ds5_detail::TrimBlanks(text.substr(valueBegin, valueEnd - valueBegin)));
        }
        if (line.last) break;
        position = line.nextPosition;
    }
}

// Reads the four ReShade.ini keys and the two cfg keys used by the gate.
inline bool ReadWatchConfig(const std::wstring& clientRoot, WatchConfig& config, std::wstring& error) {
    using namespace diagnostics_detail;
    std::string ini;
    DWORD readError = ERROR_SUCCESS;
    const std::wstring iniPath = Join(Join(clientRoot, L"host64"), L"ReShade.ini");
    if (!ReadWholeBytes(iniPath, ini, readError, kDiagnosticsMaxConfigBytes)) {
        error = L"缺少 host64\\ReShade.ini。";
        return false;
    }
    size_t bodyStart = 0;
    if (ds5_detail::HasUtf8Bom(ini, bodyStart)) ini.erase(0, bodyStart);
    if (!ds5_detail::IsValidUtf8(ini)) {
        error = L"host64\\ReShade.ini 不是合法 UTF-8。";
        return false;
    }
    ds5_detail::SectionRange range;
    std::string sectionError;
    if (!ds5_detail::FindSection(ini, kDs5SectionName, range, sectionError)) {
        error = L"配置节不存在或重复：Standalone.DLSSNR。";
        return false;
    }
    auto readKey = [&](const char* key, std::string& value) {
        bool present = false;
        std::string valueError;
        if (!ds5_detail::GetIniValue(ini, range, key, value, present, valueError)) {
            error = L"ReShade.ini 存在重复配置键。";
            return false;
        }
        return true;
    };
    if (!readKey("FrameGeneration", config.frameGeneration)) return false;
    if (!readKey("DetachedPresentation", config.detachedPresentation)) return false;
    if (!readKey("WindowedInputScaling", config.windowedInputScaling)) return false;

    std::string cfg;
    if (!ReadWholeBytes(Join(clientRoot, L"dlss5-aio-x86.cfg"), cfg, readError, kDiagnosticsMaxConfigBytes)) {
        error = L"缺少 dlss5-aio-x86.cfg。";
        return false;
    }
    bodyStart = 0;
    if (ds5_detail::HasUtf8Bom(cfg, bodyStart)) cfg.erase(0, bodyStart);
    if (!ds5_detail::IsValidUtf8(cfg)) {
        error = L"dlss5-aio-x86.cfg 不是合法 UTF-8。";
        return false;
    }
    std::vector<std::string> pipeline;
    CollectCfgValues(cfg, "enabled", pipeline);
    std::vector<std::string> bridge;
    CollectCfgValues(cfg, "classic_d3d9_cpu_fallback", bridge);
    if (pipeline.size() != 1 || bridge.size() != 1) {
        error = L"AIO 管线开关缺失、重复或数值无效。";
        return false;
    }
    config.pipelineEnabled = pipeline[0];
    config.classicBridge = bridge[0];
    config.ok = true;
    return true;
}

// 独立输出候选：AIO 管线开、CPU 桥开、输入缩放开、FG 关。
inline bool ConfigAllowsIndependentOutput(const WatchConfig& config) {
    return config.ok && config.pipelineEnabled == "1" && config.classicBridge == "1" &&
           config.detachedPresentation == "1" && config.windowedInputScaling == "1" &&
           config.frameGeneration == "0";
}

struct WatchLock {
    std::string exeSha256;
    std::string sourceSha256;
    std::string layerSha256;
};

inline std::string LowerHex(std::string text) {
    for (char& value : text) {
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
    }
    return text;
}

inline bool Sha256MatchesFile(const std::wstring& path, const std::string& expected) {
    const HashResult hashed = HashFile(path);
    if (!hashed) return false;
    return LowerHex(HexSha256(hashed.identity.sha256)) == LowerHex(expected);
}

inline bool ReadWatchLock(const std::wstring& productRoot, WatchLock& lock, std::wstring& error) {
    std::string text;
    DWORD readError = ERROR_SUCCESS;
    if (!diagnostics_detail::ReadWholeBytes(panel_detail::Join(productRoot, L"watcher-lock.json"), text, readError,
                                            kWindowWatchLockMaxBytes)) {
        error = L"缺少 watcher-lock.json。";
        return false;
    }
    const JsonResult parsed = ParseJsonObject(text);
    if (!parsed) {
        error = L"watcher-lock.json 不是合法 JSON。";
        return false;
    }
    const Json& root = parsed.value;
    if (!root.contains("schema") || !root["schema"].is_number_integer() || root["schema"].get<int>() != 1) {
        error = L"watcher-lock.json 的 schema 不是 1。";
        return false;
    }
    if (!root.contains("files") || !root["files"].is_object()) {
        error = L"watcher-lock.json 缺少 files。";
        return false;
    }
    const Json& files = root["files"];
    auto readHash = [&](const char* name, std::string& value) {
        if (!files.contains(name) || !files[name].is_string()) {
            error = L"watcher-lock.json 缺少文件哈希。";
            return false;
        }
        value = files[name].get<std::string>();
        return true;
    };
    return readHash("window_watch.exe", lock.exeSha256) && readHash("window_watch.cpp", lock.sourceSha256) &&
           readHash("window_layer.hpp", lock.layerSha256);
}

inline bool ProcessImagePath(std::uint32_t pid, std::wstring& path) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return false;
    wchar_t buffer[32768];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer, &size);
    CloseHandle(process);
    if (!ok) return false;
    path.assign(buffer, size);
    return true;
}

inline bool WatcherInstanceRunning(const std::wstring& watcherFull) {
    for (const auto& process : panel_detail::EnumerateProcesses()) {
        if (!panel_detail::NameEquals(process.name, L"window_watch.exe")) continue;
        std::wstring image;
        if (!ProcessImagePath(process.pid, image)) continue;
        if (CompareStringOrdinal(image.c_str(), -1, watcherFull.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
    }
    return false;
}

inline std::wstring ManagedWatcherPath(const std::wstring& productRoot) {
    return panel_detail::Join(panel_detail::Join(productRoot, L"bin"), L"window_watch.exe");
}

}  // namespace window_watch_detail

// 启动鼠标助手。watcherPath 为空时使用受控构建 <productRoot>\bin\window_watch.exe。
inline WindowWatchStartResult StartWindowWatch(const std::wstring& productRoot, const std::wstring& operationsRoot,
                                               const std::wstring& clientRoot,
                                               const std::wstring& watcherPath = std::wstring()) {
    using namespace window_watch_detail;
    WindowWatchStartResult result;
    const std::wstring gameExe = panel_detail::Join(clientRoot, L"wow.exe");
    if (clientRoot.empty() || !ds5_detail::FileExistsNoReparse(gameExe)) {
        result.error = L"请先选择含 wow.exe 的客户端目录。";
        return result;
    }
    WatchConfig config;
    std::wstring configError;
    if (!ReadWatchConfig(clientRoot, config, configError) || !ConfigAllowsIndependentOutput(config)) {
        result.error = L"当前配置不符合独立输出候选：请检查 AIO 管线、CPU 桥、输入缩放及 FG 状态。";
        return result;
    }
    const std::wstring requested = watcherPath.empty() ? ManagedWatcherPath(productRoot) : watcherPath;
    std::wstring watcherFull;
    if (!CanonicalLocalPath(requested, watcherFull)) {
        result.error = L"窗口助手路径无效。";
        result.win32Error = ERROR_INVALID_NAME;
        return result;
    }
    std::wstring managedFull;
    if (!CanonicalLocalPath(ManagedWatcherPath(productRoot), managedFull)) managedFull = ManagedWatcherPath(productRoot);
    std::wstring scratchFull;
    CanonicalLocalPath(panel_detail::Join(productRoot, L"_scratch"), scratchFull);
    const bool isManaged =
        CompareStringOrdinal(watcherFull.c_str(), -1, managedFull.c_str(), -1, TRUE) == CSTR_EQUAL;
    const bool inScratch = !scratchFull.empty() && WithinRoot(watcherFull, scratchFull);
    if (!isManaged && !inScratch) {
        result.error = L"窗口助手须为受控构建或 _scratch 中的候选。";
        return result;
    }
    if (!ds5_detail::FileExistsNoReparse(watcherFull)) {
        result.error = L"窗口助手程序缺失。";
        result.win32Error = ERROR_FILE_NOT_FOUND;
        return result;
    }
    WatchLock lock;
    std::wstring lockError;
    if (!ReadWatchLock(productRoot, lock, lockError)) {
        result.error = lockError;
        return result;
    }
    if (!Sha256MatchesFile(watcherFull, lock.exeSha256)) {
        result.error = L"窗口助手或源码身份不符。";
        return result;
    }
    // 开发目录同时核对源码哈希；便携包不带源码，两份源码都不存在时只信任锁中的程序哈希。
    const std::wstring sourcePath = panel_detail::Join(panel_detail::Join(productRoot, L"native"), L"window_watch.cpp");
    const std::wstring layerPath = panel_detail::Join(panel_detail::Join(productRoot, L"native"), L"window_layer.hpp");
    const bool sourcePresent = ds5_detail::FileExistsNoReparse(sourcePath);
    const bool layerPresent = ds5_detail::FileExistsNoReparse(layerPath);
    if (sourcePresent || layerPresent) {
        if (!sourcePresent || !layerPresent || !Sha256MatchesFile(sourcePath, lock.sourceSha256) ||
            !Sha256MatchesFile(layerPath, lock.layerSha256)) {
            result.error = L"窗口助手或源码身份不符。";
            return result;
        }
    }
    if (WatcherInstanceRunning(watcherFull)) {
        result.error = L"窗口助手候选已在运行。";
        return result;
    }
    if (operationsRoot.empty()) {
        result.error = L"无法确定助手日志目录。";
        result.win32Error = ERROR_PATH_NOT_FOUND;
        return result;
    }
    DWORD directoryError = ERROR_SUCCESS;
    if (!ds5_detail::CreateDirectoryTree(operationsRoot, directoryError)) {
        result.error = L"无法创建助手日志目录。";
        result.win32Error = directoryError;
        return result;
    }
    const std::wstring token = ds5_detail::Widen(ds5_detail::UniqueToken());
    const std::wstring logDir = panel_detail::Join(operationsRoot, L"watch-" + token);
    if (!ds5_detail::CreateDirectoryTree(logDir, directoryError)) {
        result.error = L"助手日志目录已存在。";
        result.win32Error = directoryError;
        return result;
    }
    const HANDLE stdoutHandle = panel_detail::OpenAppendHandle(panel_detail::Join(logDir, L"stdout.log"));
    if (stdoutHandle == INVALID_HANDLE_VALUE) {
        result.error = L"无法创建助手标准输出日志。";
        result.win32Error = GetLastError();
        return result;
    }
    const HANDLE stderrHandle = panel_detail::OpenAppendHandle(panel_detail::Join(logDir, L"stderr.log"));
    if (stderrHandle == INVALID_HANDLE_VALUE) {
        result.win32Error = GetLastError();
        CloseHandle(stdoutHandle);
        result.error = L"无法创建助手标准错误日志。";
        return result;
    }
    const HANDLE nilInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nilInput != INVALID_HANDLE_VALUE) SetHandleInformation(nilInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    std::wstring command = L"\"" + watcherFull + L"\" --game \"" + gameExe + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nilInput == INVALID_HANDLE_VALUE ? GetStdHandle(STD_INPUT_HANDLE) : nilInput;
    startup.hStdOutput = stdoutHandle;
    startup.hStdError = stderrHandle;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(watcherFull.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, productRoot.c_str(), &startup, &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    if (nilInput != INVALID_HANDLE_VALUE) CloseHandle(nilInput);
    CloseHandle(stdoutHandle);
    CloseHandle(stderrHandle);
    if (!created) {
        result.error = L"启动窗口助手失败。";
        result.win32Error = createError;
        return result;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result.ok = true;
    result.pid = process.dwProcessId;
    result.logDir = logDir;
    result.message = L"助手已启动；是否处理成功须查看日志并进行游戏验收。";
    return result;
}

}  // namespace control_center
