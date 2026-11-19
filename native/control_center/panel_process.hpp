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
};

// 启动选项：
// - requireSelectedClientRunning：只允许在“所选客户端”的 wow.exe 正在运行时打开面板，
//   并拒绝在其它客户端的游戏进程下复用面板（R6）。为兼容既有调用/测试默认关闭。
// - verifyPanelHash：额外比对 panel-lock.json 的 dlss5-panel.exe 哈希，失败只警告。
struct PanelStartOptions {
    bool requireSelectedClientRunning = false;
    bool verifyPanelHash = false;
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
        const bool panelRunning = ProcessRunning(L"dlss5-panel.exe");
        if (check.matching == 0 && check.state == ClientGameState::Unknown) {
            result.status = PanelStartStatus::ClientStateUnknown;
            result.error = L"无法确认所选客户端的游戏进程状态，请稍后重试。";
            result.win32Error = check.win32Error;
            return result;
        }
        if (check.matching == 0) {
            if (panelRunning) {
                result.status = PanelStartStatus::AlreadyRunningOtherClient;
                result.error = L"游戏内面板已在运行，但所选客户端未运行；它可能属于其他客户端。请先结束该面板进程，或启动所选客户端的游戏。";
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
            result.status = PanelStartStatus::AlreadyRunning;
            result.error = L"游戏内面板已在运行（已连接所选客户端）。";
            return result;
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
    CloseHandle(process.hProcess);
    result.ok = true;
    result.status = PanelStartStatus::None;
    result.logDir = logDir;
    result.pid = process.dwProcessId;
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
