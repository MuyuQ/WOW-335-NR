// WOW-335-NR 原生控制中心：AIO 管线开关（dlss5-aio-x86.cfg 的 enabled 键）。
//
// 旧实现（Core:840-858）用整文件正则定位唯一的 enabled=，只替换值组；这里按字节
// 逐行扫描，保留注释、节顺序、行尾与编码。写入走与 ReShade.ini 相同的受控写入：
// 先备份（operations\config-<token>\before.cfg）、再写同目录唯一临时文件、原子替换、
// 回读校验；任一步失败都保留备份并尽量复原。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ds5_settings.hpp"  // 复用 INI 行扫描与进程门等助手
#include "file_identity.hpp"
#include "guarded_path.hpp"
#include "journal.hpp"        // ReadJournalBytes
#include "write_new.hpp"

namespace control_center {

constexpr size_t kPipelineCfgMaxBytes = 1u << 20;

inline std::wstring PipelineCfgPath(const std::wstring& clientRoot) {
    return clientRoot + L"\\dlss5-aio-x86.cfg";
}

struct PipelineSwitchState {
    bool exists = false;
    bool readable = false;
    int enabled = -1;  // -1 未知；0/1 有效
    DWORD win32Error = ERROR_SUCCESS;
    std::string error;
};

enum class PipelineSwitchError {
    None,
    MissingFile,
    Ambiguous,
    GameRunning,
    Read,
    Backup,
    Write,
    Verify,
};

struct PipelineSwitchResult {
    PipelineSwitchError error = PipelineSwitchError::None;
    DWORD win32Error = ERROR_SUCCESS;
    bool changed = false;
    std::wstring backupPath;
    std::string detail;
    explicit operator bool() const { return error == PipelineSwitchError::None; }
};

inline const wchar_t* PipelineSwitchErrorText(PipelineSwitchError error) {
    switch (error) {
        case PipelineSwitchError::None: return L"";
        case PipelineSwitchError::MissingFile: return L"缺少 dlss5-aio-x86.cfg。";
        case PipelineSwitchError::Ambiguous: return L"AIO 管线开关缺失、重复或数值无效。";
        case PipelineSwitchError::GameRunning: return L"请先退出游戏、启动器和 AIO 载体。";
        case PipelineSwitchError::Read: return L"读取 dlss5-aio-x86.cfg 失败。";
        case PipelineSwitchError::Backup: return L"创建配置备份失败。";
        case PipelineSwitchError::Write: return L"写入 dlss5-aio-x86.cfg 失败。";
        case PipelineSwitchError::Verify: return L"配置回读不一致；备份已保留。";
    }
    return L"";
}

namespace pipeline_detail {

using namespace ds5_detail;

inline bool FileExistsNoReparseLocal(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Finds every "enabled = <value>" line in the whole file (any section, case-insensitive).
inline bool CollectEnabledSpans(const std::string& text, std::vector<ValueSpan>& spans) {
    spans.clear();
    size_t position = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        position = 3;  // 跳过 UTF-8 BOM：配置首行也可能是 enabled。
    }
    while (position < text.size()) {
        IniLine line;
        if (!ReadLine(text, position, text.size(), line)) break;
        size_t valueBegin = 0;
        size_t valueEnd = 0;
        if (MatchKeyLine(text, line, "enabled", valueBegin, valueEnd)) spans.push_back({valueBegin, valueEnd});
        if (line.last) break;
        position = line.nextPosition;
    }
    return !spans.empty();
}

inline bool ValidSwitchValue(const std::string& raw, int& value) {
    const std::string trimmed = TrimBlanks(raw);
    if (trimmed == "0") { value = 0; return true; }
    if (trimmed == "1") { value = 1; return true; }
    return false;
}

}  // namespace pipeline_detail

// 只读：返回 enabled 当前值（0/1）。文件缺失不算错误，enabled=-1 表示未知。
inline PipelineSwitchState ReadPipelineSwitch(const std::wstring& clientRoot) {
    using namespace pipeline_detail;
    PipelineSwitchState state;
    if (clientRoot.empty()) return state;
    const std::wstring path = PipelineCfgPath(clientRoot);
    state.exists = FileExistsNoReparseLocal(path);
    if (!state.exists) return state;
    std::string bytes;
    DWORD readError = ERROR_SUCCESS;
    GuardedPath cfg;
    if (!cfg.Open(clientRoot, L"dlss5-aio-x86.cfg") || !ReadJournalBytes(cfg, bytes, readError)) {
        state.win32Error = cfg.Path().empty() ? cfg.Error() : readError;
        state.error = "read failed";
        return state;
    }
    if (bytes.size() > kPipelineCfgMaxBytes) {
        state.error = "file too large";
        state.win32Error = ERROR_FILE_TOO_LARGE;
        return state;
    }
    std::vector<ValueSpan> spans;
    if (!CollectEnabledSpans(bytes, spans)) {
        state.readable = true;
        state.error = "enabled missing";
        return state;
    }
    if (spans.size() != 1) {
        state.readable = true;
        state.error = "enabled duplicated";
        return state;
    }
    int value = -1;
    if (!ValidSwitchValue(bytes.substr(spans[0].begin, spans[0].end - spans[0].begin), value)) {
        state.readable = true;
        state.error = "enabled invalid";
        return state;
    }
    state.readable = true;
    state.enabled = value;
    return state;
}

// 写入 enabled；值只有 0/1 合法。文件内容不变时不写盘、不建备份（changed=false）。
inline PipelineSwitchResult SetPipelineSwitch(const std::wstring& clientRoot, const std::wstring& operationsRoot,
                                              int enabled) {
    using namespace pipeline_detail;
    PipelineSwitchResult result;
    const auto fail = [&result](PipelineSwitchError error, DWORD win32Error) {
        result.error = error;
        result.win32Error = win32Error;
        return result;
    };
    if (enabled != 0 && enabled != 1) return fail(PipelineSwitchError::Ambiguous, 0);
    if (clientRoot.empty()) return fail(PipelineSwitchError::MissingFile, 0);
    if (GameProcessRunning()) return fail(PipelineSwitchError::GameRunning, 0);
    const std::wstring cfgPath = PipelineCfgPath(clientRoot);
    if (!FileExistsNoReparseLocal(cfgPath)) return fail(PipelineSwitchError::MissingFile, 0);

    std::string original;
    FileIdentity originalIdentity{};
    std::wstring fullPath;
    {
        GuardedPath cfg;
        if (!cfg.Open(clientRoot, L"dlss5-aio-x86.cfg")) return fail(PipelineSwitchError::Read, cfg.Error());
        fullPath = cfg.Path();
        const HashResult hash = HashFile(fullPath);
        if (!hash) return fail(PipelineSwitchError::Read, hash.win32Error);
        originalIdentity = hash.identity;
        DWORD readError = ERROR_SUCCESS;
        if (!ReadJournalBytes(cfg, original, readError)) return fail(PipelineSwitchError::Read, readError);
    }
    if (original.size() > kPipelineCfgMaxBytes) return fail(PipelineSwitchError::Read, ERROR_FILE_TOO_LARGE);

    std::vector<ValueSpan> spans;
    if (!CollectEnabledSpans(original, spans) || spans.size() != 1) {
        return fail(PipelineSwitchError::Ambiguous, 0);
    }
    int current = -1;
    if (!ValidSwitchValue(original.substr(spans[0].begin, spans[0].end - spans[0].begin), current)) {
        return fail(PipelineSwitchError::Ambiguous, 0);
    }
    if (current == enabled) {
        result.changed = false;
        return result;
    }

    std::string updated = original;
    updated.replace(spans[0].begin, spans[0].end - spans[0].begin, enabled == 1 ? "1" : "0");

    const std::string token = UniqueToken();
    DWORD directoryError = ERROR_SUCCESS;
    if (!CreateDirectoryTree(operationsRoot, directoryError)) {
        result.detail = "cannot create operations root";
        return fail(PipelineSwitchError::Backup, directoryError);
    }
    const std::wstring backupMember = L"config-" + Widen(token) + L"\\before.cfg";
    std::wstring backupPath;
    {
        GuardedPath backup;
        if (!backup.Open(operationsRoot, backupMember, true)) return fail(PipelineSwitchError::Backup, backup.Error());
        const WriteNewResult written = WriteNewVerified(backup, original);
        if (!written) return fail(PipelineSwitchError::Backup, written.win32Error);
        backupPath = backup.Path();
    }
    result.backupPath = backupPath;

    const std::wstring temporaryMember = L"dlss5-aio-x86.cfg.dlss5-settings-" + Widen(token);
    std::wstring temporaryPath;
    {
        GuardedPath temporary;
        if (!temporary.Open(clientRoot, temporaryMember)) return fail(PipelineSwitchError::Write, temporary.Error());
        const WriteNewResult written = WriteNewVerified(temporary, updated);
        if (!written) {
            DeleteFileW(temporary.Path().c_str());
            return fail(PipelineSwitchError::Write, written.win32Error);
        }
        temporaryPath = temporary.Path();
    }

    if (GameProcessRunning()) {
        DeleteFileW(temporaryPath.c_str());
        return fail(PipelineSwitchError::GameRunning, 0);
    }
    const HashResult currentHash = HashFile(fullPath);
    if (!currentHash) {
        DeleteFileW(temporaryPath.c_str());
        return fail(PipelineSwitchError::Read, currentHash.win32Error);
    }
    if (!SameFileIdentity(currentHash.identity, originalIdentity)) {
        DeleteFileW(temporaryPath.c_str());
        result.detail = "config changed while saving";
        return fail(PipelineSwitchError::Write, ERROR_INVALID_DATA);
    }
    if (!MoveFileExW(temporaryPath.c_str(), fullPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        CopyFileW(backupPath.c_str(), fullPath.c_str(), FALSE);
        return fail(PipelineSwitchError::Write, moveError);
    }

    std::string readBack;
    bool verified = false;
    {
        GuardedPath verify;
        DWORD verifyError = ERROR_SUCCESS;
        if (verify.Open(clientRoot, L"dlss5-aio-x86.cfg") && ReadJournalBytes(verify, readBack, verifyError)) {
            verified = readBack == updated;
        }
    }
    if (!verified) {
        CopyFileW(backupPath.c_str(), fullPath.c_str(), FALSE);
        return fail(PipelineSwitchError::Verify, 0);
    }
    result.changed = true;
    return result;
}

}  // namespace control_center
