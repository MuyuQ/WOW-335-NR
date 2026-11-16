// WOW-335-NR 原生控制中心：启动游戏、游戏内面板进程与日志尾读。
//
// 与旧实现（CC:943-954 / CC:1362-1374）一致：面板位于 <产品目录>\bin\dlss5-panel.exe，
// 以隐藏窗口+重定向 stdio 启动，参数 --game "<client>\wow.exe>；日志只保留“本次进程
// 最后一次启动”的目录。日志目录改为用户数据目录（<dataDir>\panel-logs\<token>），
// 避免向游戏目录写运行时日志。
#pragma once

#include <windows.h>

#include <tlhelp32.h>

#include <cstdint>
#include <string>
#include <vector>

#include "file_identity.hpp"  // 哈希/文件身份类型
#include "ds5_settings.hpp"   // ds5_detail：FileExistsNoReparse / UniqueToken / CreateDirectoryTree

namespace control_center {

constexpr size_t kLogTailMaxBytes = 256u * 1024;
constexpr size_t kLogTailMaxLines = 40;

struct PanelStartResult {
    bool ok = false;
    DWORD win32Error = ERROR_SUCCESS;
    std::wstring error;
    std::wstring logDir;
    std::uint32_t pid = 0;
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

inline PanelStartResult StartPanel(const std::wstring& exeDir, const std::wstring& dataDir,
                                   const std::wstring& clientRoot) {
    PanelStartResult result;
    const std::wstring panelExe = Join(Join(exeDir, L"bin"), L"dlss5-panel.exe");
    if (!ds5_detail::FileExistsNoReparse(panelExe)) {
        result.error = L"面板程序缺失：bin/dlss5-panel.exe";
        return result;
    }
    if (ProcessRunning(L"dlss5-panel.exe")) {
        result.error = L"游戏内面板已在运行。";
        return result;
    }
    const std::wstring gameExe = Join(clientRoot, L"wow.exe");
    if (!ds5_detail::FileExistsNoReparse(gameExe)) {
        result.error = L"请先选择含 wow.exe 的客户端目录。";
        return result;
    }
    if (dataDir.empty()) {
        result.error = L"无法确定日志目录。";
        result.win32Error = ERROR_PATH_NOT_FOUND;
        return result;
    }
    const std::wstring token = ds5_detail::Widen(ds5_detail::UniqueToken());
    const std::wstring logDir = Join(Join(dataDir, L"panel-logs"), token);
    DWORD directoryError = ERROR_SUCCESS;
    if (!ds5_detail::CreateDirectoryTree(logDir, directoryError)) {
        result.error = L"无法创建面板日志目录。";
        result.win32Error = directoryError;
        return result;
    }
    const std::wstring stdoutPath = Join(logDir, L"stdout.log");
    const std::wstring stderrPath = Join(logDir, L"stderr.log");
    const HANDLE stdoutHandle = OpenAppendHandle(stdoutPath);
    if (stdoutHandle == INVALID_HANDLE_VALUE) {
        result.error = L"无法创建面板标准输出日志。";
        result.win32Error = GetLastError();
        return result;
    }
    const HANDLE stderrHandle = OpenAppendHandle(stderrPath);
    if (stderrHandle == INVALID_HANDLE_VALUE) {
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
        result.error = L"启动游戏内面板失败。";
        result.win32Error = createError;
        return result;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result.ok = true;
    result.logDir = logDir;
    result.pid = process.dwProcessId;
    return result;
}

}  // namespace panel_detail

// 面板进程是否已在运行（按映像名 dlss5-panel.exe，不区分大小写）。
inline bool PanelProcessRunning() {
    return panel_detail::ProcessRunning(L"dlss5-panel.exe");
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
