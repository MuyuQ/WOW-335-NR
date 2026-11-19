#pragma once
// Native WTF/Config.wtf reader and FPS cap writer for the control center.
//
// Behaviour is byte oriented on purpose: the original file is only spliced on the
// digit spans of the matching "SET maxFPS"/"SET maxFPSBK" lines, so every other
// byte (ANSI comments, line endings, unknown keys) is preserved exactly as it was.
//
// Differences from the previous PowerShell implementation, all intentional:
//   * the file keeps its original encoding; it is never silently rewritten to UTF-8
//   * the temporary file name is unique per write instead of a fixed ".nr-tmp"
//   * backup names fall back to a sequence suffix instead of failing within one second
//   * the write is verified by re-reading the file and rolled back from the backup
//     when verification fails
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "client_identity.hpp"

namespace control_center {

constexpr size_t kConfigWtfMaxBytes = 1u << 20;  // 1 MiB is far above any real Config.wtf

struct ConfigWtfCap {
    bool exists = false;
    bool readable = false;
    int front = -1;  // -1: key not present or value unparsable
    int back = -1;
    DWORD win32Error = 0;
    std::string error;
};

enum class ConfigWtfWriteError {
    None,
    InvalidRange,
    MissingFile,
    GameRunning,
    Read,
    Backup,
    Write,
    VerifyFront,
    VerifyBack,
};

struct ConfigWtfWriteResult {
    ConfigWtfWriteError error = ConfigWtfWriteError::None;
    DWORD win32Error = 0;
    std::wstring backupPath;
    // 备份内容对应第几轮读取（从 1 起）；0 表示没有可恢复的备份。
    int backupAttempt = 0;
    std::string detail;

    explicit operator bool() const { return error == ConfigWtfWriteError::None; }
};

inline std::wstring ConfigWtfPath(const std::wstring& clientRoot) {
    if (clientRoot.empty()) return std::wstring();
    std::wstring path = clientRoot;
    if (path.back() != L'\\' && path.back() != L'/') path.push_back(L'\\');
    path += L"WTF\\Config.wtf";
    return path;
}

namespace config_wtf_detail {

inline char AsciiLower(char value) {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

inline bool EqualsAsciiNoCase(const char* text, size_t length, const char* literal) {
    const size_t literalLength = std::strlen(literal);
    if (length != literalLength) return false;
    for (size_t i = 0; i < length; ++i) {
        if (AsciiLower(text[i]) != AsciiLower(literal[i])) return false;
    }
    return true;
}

// Parses one config line (without the trailing CR/LF) as
//   [ \t]* SET [ \t]+ <key> [ \t]+ "?<digits>"? [ \t]*
// and returns the digit span inside the line. This mirrors the PowerShell regex
// (?im)^[ \t]*SET[ \t]+<key>[ \t]+"?(\d+)"?[ \t]*\r?$ including its case-insensitive
// key comparison and its requirement that nothing but whitespace follows the value.
inline bool MatchFpsSetLine(const char* line, size_t length, const char* key, size_t& valueBegin,
                            size_t& valueEnd) {
    size_t i = 0;
    while (i < length && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i + 3 > length || !EqualsAsciiNoCase(line + i, 3, "SET")) return false;
    i += 3;
    size_t whitespace = i;
    while (i < length && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i == whitespace) return false;
    const size_t keyLength = std::strlen(key);
    if (i + keyLength > length || !EqualsAsciiNoCase(line + i, keyLength, key)) return false;
    i += keyLength;
    whitespace = i;
    while (i < length && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i == whitespace) return false;
    if (i < length && line[i] == '"') ++i;
    valueBegin = i;
    while (i < length && line[i] >= '0' && line[i] <= '9') ++i;
    valueEnd = i;
    if (valueBegin == valueEnd) return false;
    if (i < length && line[i] == '"') ++i;
    while (i < length && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i == length;
}

inline bool ParseDigits(const char* text, size_t length, int& value) {
    if (length == 0 || length > 9) return false;  // 9 digits still fits int
    long long parsed = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        parsed = parsed * 10 + (text[i] - '0');
    }
    if (parsed > 2147483647LL) return false;
    value = static_cast<int>(parsed);
    return true;
}

struct SetLineMatch {
    size_t valueBegin = 0;
    size_t valueEnd = 0;
    int value = 0;
    bool valid = false;
};

// Scans the body (BOM already removed) and collects every matching key occurrence.
inline void CollectMatches(const std::string& body, const char* key, std::vector<SetLineMatch>& matches) {
    size_t position = 0;
    while (position <= body.size()) {
        const size_t newline = body.find('\n', position);
        const size_t lineEnd = newline == std::string::npos ? body.size() : newline;
        size_t contentEnd = lineEnd;
        if (contentEnd > position && body[contentEnd - 1] == '\r') --contentEnd;
        size_t valueBegin = 0;
        size_t valueEnd = 0;
        if (MatchFpsSetLine(body.data() + position, contentEnd - position, key, valueBegin, valueEnd)) {
            SetLineMatch match;
            match.valueBegin = position + valueBegin;
            match.valueEnd = position + valueEnd;
            match.valid = ParseDigits(body.data() + match.valueBegin, match.valueEnd - match.valueBegin,
                                      match.value);
            matches.push_back(match);
        }
        if (newline == std::string::npos) break;
        position = newline + 1;
    }
}

inline bool ReadWholeBytes(const std::wstring& path, std::string& bytes, DWORD& win32Error) {
    bytes.clear();
    win32Error = ERROR_SUCCESS;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kConfigWtfMaxBytes) {
        win32Error = GetLastError();
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(bytes.size() - offset);
        if (!ReadFile(file, bytes.data() + offset, chunk, &read, nullptr) || read == 0) {
            win32Error = GetLastError();
            CloseHandle(file);
            bytes.clear();
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

inline bool FileExistsNoReparse(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool WriteAllBytes(const std::wstring& path, const std::string& bytes, DWORD creation, DWORD& win32Error) {
    win32Error = ERROR_SUCCESS;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, creation,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(bytes.size() - offset);
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
            win32Error = GetLastError();
            CloseHandle(file);
            return false;
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        win32Error = GetLastError();
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    return true;
}

inline bool ReadCapFromBytes(const std::string& bytes, int& front, int& back) {
    size_t bodyStart = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bodyStart = 3;
    }
    const std::string body = bytes.substr(bodyStart);
    std::vector<SetLineMatch> matches;
    CollectMatches(body, "maxFPS", matches);
    front = -1;
    for (const auto& match : matches) {
        if (match.valid) {
            front = match.value;
            break;
        }
    }
    matches.clear();
    CollectMatches(body, "maxFPSBK", matches);
    back = -1;
    for (const auto& match : matches) {
        if (match.valid) {
            back = match.value;
            break;
        }
    }
    return true;
}

inline std::wstring TimestampSuffix() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[32] = {};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%04u%02u%02u-%02u%02u%02u",
                  static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
                  static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
                  static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond));
    return std::wstring(buffer);
}

inline std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

}  // namespace config_wtf_detail

// Reads the FPS caps of <clientRoot>\WTF\Config.wtf. A missing file is not an error.
inline ConfigWtfCap ReadConfigFpsCap(const std::wstring& clientRoot) {
    ConfigWtfCap cap;
    const std::wstring path = ConfigWtfPath(clientRoot);
    if (path.empty()) return cap;
    DWORD win32Error = ERROR_SUCCESS;
    std::string bytes;
    if (!config_wtf_detail::ReadWholeBytes(path, bytes, win32Error)) {
        cap.win32Error = win32Error;
        if (win32Error == ERROR_FILE_NOT_FOUND || win32Error == ERROR_PATH_NOT_FOUND) return cap;
        cap.error = "config file unreadable";
        return cap;
    }
    cap.exists = true;
    cap.readable = config_wtf_detail::ReadCapFromBytes(bytes, cap.front, cap.back);
    if (!cap.readable) cap.error = "config file unreadable";
    return cap;
}

// True only when a process whose executable path equals <clientRoot>\wow.exe is running.
// This mirrors the old Test-GameRunning semantics (same client, not just the same name).
inline bool IsClientGameRunning(const std::wstring& clientRoot) {
    if (clientRoot.empty()) return false;
    const std::wstring gamePath = clientRoot + (clientRoot.back() == L'\\' ? L"" : L"\\") + L"wow.exe";
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"wow.exe") != 0) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr) continue;
            wchar_t imagePath[1024] = {};
            DWORD length = static_cast<DWORD>(sizeof(imagePath) / sizeof(imagePath[0]));
            if (QueryFullProcessImageNameW(process, 0, imagePath, &length) &&
                _wcsicmp(imagePath, gamePath.c_str()) == 0) {
                running = true;
            }
            CloseHandle(process);
            if (running) break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

// 写入过程的测试注入点。两个回调都只在测试中设置，生产调用方使用默认值（nullptr）。
struct ConfigWtfWriteHooks {
    // 替换前：临时文件已写好、ReplaceFileW 尚未执行时调用，用于确定性地模拟
    // “另一写者在我们读完原文之后、替换之前”提交内容的时序。
    void (*beforeCommit)(const std::wstring& path, int attempt, void* context) = nullptr;
    // 替换后：在原子替换完成、回读校验之前调用，用于确定性地模拟并发写者。
    void (*afterWrite)(const std::wstring& path, int attempt, void* context) = nullptr;
    void* context = nullptr;
};

// 最多写入尝试次数。回读字节与本轮期望不一致时说明有并发写者（例如用户另开了
// 配置工具或游戏退出时回写 Config.wtf），重新读取最新内容并把两个帧率值重新应用。
constexpr int kConfigWtfWriteAttempts = 3;

namespace config_wtf_detail {

// 只替换 maxFPS / maxFPSBK 的数字区段，BOM 与其余字节原样保留。
inline std::string BuildUpdatedConfigBytes(const std::string& original, int front, int back) {
    bool bom = false;
    const std::string body = [&original, &bom]() {
        if (original.size() >= 3 && static_cast<unsigned char>(original[0]) == 0xEF &&
            static_cast<unsigned char>(original[1]) == 0xBB && static_cast<unsigned char>(original[2]) == 0xBF) {
            bom = true;
            return original.substr(3);
        }
        return original;
    }();

    std::vector<SetLineMatch> frontMatches;
    std::vector<SetLineMatch> backMatches;
    CollectMatches(body, "maxFPS", frontMatches);
    CollectMatches(body, "maxFPSBK", backMatches);

    const std::string frontText = std::to_string(front);
    const std::string backText = std::to_string(back);
    // Apply replacements from the end of the body so earlier offsets stay valid.
    struct Splice {
        size_t begin;
        size_t end;
        const std::string* text;
    };
    std::vector<Splice> splices;
    for (const auto& match : frontMatches) splices.push_back({match.valueBegin, match.valueEnd, &frontText});
    for (const auto& match : backMatches) splices.push_back({match.valueBegin, match.valueEnd, &backText});
    std::sort(splices.begin(), splices.end(), [](const Splice& a, const Splice& b) { return a.begin < b.begin; });
    std::string updated = body;
    for (size_t index = splices.size(); index > 0; --index) {
        const Splice& splice = splices[index - 1];
        updated.replace(splice.begin, splice.end - splice.begin, *splice.text);
    }
    if (frontMatches.empty()) {
        if (!updated.empty() && updated.back() != '\n') updated += "\r\n";
        updated += "SET maxFPS \"";
        updated += frontText;
        updated += "\"\r\n";
    }
    if (backMatches.empty()) {
        if (!updated.empty() && updated.back() != '\n') updated += "\r\n";
        updated += "SET maxFPSBK \"";
        updated += backText;
        updated += "\"\r\n";
    }
    std::string finalBytes;
    finalBytes.reserve(updated.size() + 3);
    if (bom) finalBytes.append("\xEF\xBB\xBF", 3);
    finalBytes += updated;
    return finalBytes;
}

// 在 path 旁创建一个不冲突的备份文件，内容为 bytes；成功时返回备份路径。
inline bool CreateBackupFile(const std::wstring& path, const std::string& bytes, std::wstring& backup,
                             DWORD& win32Error) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::wstring candidate = attempt == 0
                                     ? path + L".nr-" + TimestampSuffix() + L".bak"
                                     : path + L".nr-" + TimestampSuffix() + L"-" + std::to_wstring(attempt) + L".bak";
        if (!WriteAllBytes(candidate, bytes, CREATE_NEW, win32Error)) {
            if (win32Error == ERROR_FILE_EXISTS || win32Error == ERROR_ALREADY_EXISTS) continue;
            return false;
        }
        backup = candidate;
        return true;
    }
    win32Error = ERROR_ALREADY_EXISTS;
    return false;
}

}  // namespace config_wtf_detail

// Replaces the maxFPS / maxFPSBK values while keeping every other byte of the file.
// Returns the full path of the backup file on success.
//
// 并发保护（R10）：每一轮“读取快照 -> 计算期望字节 -> 备份 -> 写临时文件 -> 替换前复核
// -> 原子替换 -> 逐字节回读”，最多 3 轮。替换前复核会再次读取目标文件并与本轮快照逐
// 字节比较：若另一写者已在我们读取之后提交了内容，本轮放弃替换，下一轮以磁盘最新内容
// 为准重新应用两个帧率值，因此对方提交的无关设置不会被旧快照覆盖。3 轮都检测到并发写
// 者时不再回填旧快照/旧备份：目标文件保持其他写者的最新内容，只保留一份对应最后一轮
// 读取的备份供人工恢复，并返回校验错误（detail 指明并发写者）。
inline ConfigWtfWriteResult SaveConfigFpsCap(const std::wstring& clientRoot, int front, int back,
                                             const ConfigWtfWriteHooks& hooks = ConfigWtfWriteHooks()) {
    using namespace config_wtf_detail;
    ConfigWtfWriteResult result;
    const auto fail = [&result](ConfigWtfWriteError error, DWORD win32Error) {
        result.error = error;
        result.win32Error = win32Error;
        return result;
    };
    if (front < 0 || front > 999 || back < 0 || back > 999) return fail(ConfigWtfWriteError::InvalidRange, 0);
    const std::wstring path = ConfigWtfPath(clientRoot);
    if (path.empty() || !FileExistsNoReparse(path)) return fail(ConfigWtfWriteError::MissingFile, 0);
    if (IsClientGameRunning(clientRoot)) return fail(ConfigWtfWriteError::GameRunning, 0);

    const std::wstring directory = DirectoryOf(path);
    DWORD win32Error = ERROR_SUCCESS;
    std::wstring lastBackup;      // 最后一轮读取时的快照，仅供人工恢复。
    int lastBackupAttempt = 0;    // 该备份对应的轮次（从 1 起，0 表示没有）。
    bool concurrentDetected = false;
    for (int attempt = 1; attempt <= kConfigWtfWriteAttempts; ++attempt) {
        // 1) 每一轮都以磁盘上的最新快照为准，避免覆盖并发写者已提交的内容。
        std::string original;
        if (!ReadWholeBytes(path, original, win32Error)) return fail(ConfigWtfWriteError::Read, win32Error);
        const std::string expected = BuildUpdatedConfigBytes(original, front, back);

        // 2) 备份本轮读取到的字节；并发失败时它就是留给人工恢复的那一份。
        std::wstring backup;
        if (!CreateBackupFile(path, original, backup, win32Error)) {
            return fail(ConfigWtfWriteError::Backup, win32Error);
        }
        lastBackup = backup;
        lastBackupAttempt = attempt;

        // 3) 同目录唯一临时文件，替换前先写好。
        wchar_t temporaryName[96] = {};
        std::swprintf(temporaryName, sizeof(temporaryName) / sizeof(temporaryName[0]),
                      L"Config.wtf.nr-%lu-%llu-%d.tmp", static_cast<unsigned long>(GetCurrentProcessId()),
                      static_cast<unsigned long long>(GetTickCount64()), attempt);
        const std::wstring temporary =
            directory.empty() ? std::wstring(temporaryName) : directory + L"\\" + temporaryName;
        if (!WriteAllBytes(temporary, expected, CREATE_NEW, win32Error)) {
            DeleteFileW(backup.c_str());
            return fail(ConfigWtfWriteError::Write, win32Error);
        }

        // 4) 替换前复核：目标文件必须仍等于本轮读取时的快照。否则说明另一写者在我们
        //    读完原文之后、ReplaceFileW 之前提交了新内容，用旧快照替换会丢掉对方设置，
        //    因此删除临时文件并以磁盘最新内容进入下一轮。
        if (hooks.beforeCommit != nullptr) hooks.beforeCommit(path, attempt, hooks.context);
        std::string current;
        const bool currentRead = ReadWholeBytes(path, current, win32Error);
        if (!currentRead || current != original) {
            DeleteFileW(temporary.c_str());
            concurrentDetected = true;
            if (attempt < kConfigWtfWriteAttempts) Sleep(10);  // 给并发写者短暂让路
            continue;
        }

        // 5) 原子替换。
        if (!ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
            win32Error = GetLastError();
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(temporary.c_str());
                DeleteFileW(backup.c_str());
                return fail(ConfigWtfWriteError::Write, win32Error);
            }
        }

        if (hooks.afterWrite != nullptr) hooks.afterWrite(path, attempt, hooks.context);

        // 6) 逐字节回读：与期望一致才算成功；否则视为并发写入，重新读取后重试。
        std::string actual;
        if (ReadWholeBytes(path, actual, win32Error) && actual == expected) {
            result.backupPath = backup;
            result.backupAttempt = attempt;
            return result;
        }
        concurrentDetected = true;
        if (attempt < kConfigWtfWriteAttempts) Sleep(10);  // 给并发写者短暂让路
    }

    // 7) 持续冲突：绝不回填旧快照或旧备份，目标文件保持其他写者最后提交的内容；
    //    仅报告一份对应最后一轮读取的备份供人工恢复。
    result.backupPath = lastBackup;
    result.backupAttempt = lastBackupAttempt;
    result.detail = concurrentDetected ? "concurrent writer detected" : "config write could not be verified";
    return fail(ConfigWtfWriteError::VerifyFront, win32Error);
}

}  // namespace control_center
