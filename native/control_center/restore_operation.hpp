// WOW-335-NR 原生控制中心：恢复安装前的文件（撤销最后一次已完成安装）。
//
// 对应旧实现 Invoke-Dlss5Restore（Core:1720-1860）：读取客户端最后一次已完成的
// 安装事务记录，按记录逆序还原/删除客户端成员，再写回安装前的客户端记录（或
// 删除记录并清理 host64\dlss5-backup），最后写入 restore.json 并返回可读报告。
//
// 安全边界：只接受 schema=2/status=completed 的事务记录；每个成员在操作前都按
// 记录中的哈希验证当前文件与备份；失败成员逐条记录并继续，不猜测、不覆盖身份
// 不符的文件。
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "client_identity.hpp"
#include "client_record.hpp"
#include "copy_verified.hpp"
#include "delete_verified.hpp"
#include "ds5_settings.hpp"  // ds5_detail::{GameProcessRunning, UniqueToken}
#include "file_identity.hpp"
#include "guarded_path.hpp"
#include "install_member.hpp"    // ValidOperationId
#include "journal.hpp"           // ReadJournalBytes
#include "operation_record.hpp"  // ReadOperationRecord
#include "recover_operation.hpp" // ParseSha256Bytes
#include "strict_json.hpp"
#include "write_new.hpp"

namespace control_center {

struct RestoreMemberReport {
    std::wstring target;
    std::wstring action;  // 已还原 / 已删除 / 跳过
    std::wstring note;
};

struct RestoreResult {
    bool ok = false;
    std::wstring error;
    std::wstring status;  // restored（全部完成）/ partial（有成员被跳过）
    std::wstring report;
    std::wstring operation;  // 被恢复的事务目录
    DWORD win32Error = ERROR_SUCCESS;
    std::vector<RestoreMemberReport> members;
    size_t restored = 0, deleted = 0, skipped = 0;
    bool recordRestored = false, recordRemoved = false, backupDirectoryRemoved = false;
    explicit operator bool() const { return ok; }
};

namespace restore_detail {

struct OperationDirInfo {
    std::wstring id, directory;
    FILETIME writeTime{};
    bool completed = false;
    std::string recordText;
    OperationRecord record;
};

inline bool SameClientPath(const std::wstring& a, const std::wstring& b) {
    std::wstring left, right;
    if (!CanonicalLocalPath(a, left) || !CanonicalLocalPath(b, right)) return false;
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// 转成 UTF-8；restore.json 是 UTF-8 文本，不能直接把 std::wstring 交给 nlohmann，
// 否则宽字符会被当成整数序列写出（曾出现 members 变成数字数组）。
inline std::string Utf8Of(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
    return out;
}

inline FILETIME DirectoryWriteTime(const std::wstring& directory) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    FILETIME result{};
    if (GetFileAttributesExW(directory.c_str(), GetFileExInfoStandard, &data)) result = data.ftLastWriteTime;
    return result;
}

inline bool EnumerateOperationDirs(const std::wstring& operationsRoot, std::vector<OperationDirInfo>& output,
                                   DWORD& error) {
    WIN32_FIND_DATAW found{};
    const std::wstring pattern = operationsRoot + L"\\*";
    HANDLE finder = FindFirstFileW(pattern.c_str(), &found);
    if (finder == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND;
    }
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !ValidOperationId(name)) {
            continue;
        }
        OperationDirInfo info;
        info.id = name;
        info.directory = operationsRoot + L"\\" + name;
        info.writeTime = DirectoryWriteTime(info.directory);
        GuardedPath recordPath;
        DWORD readError = ERROR_SUCCESS;
        if (recordPath.Open(info.directory, L"transaction.json") &&
            ReadJournalBytes(recordPath, info.recordText, readError)) {
            info.record = ReadOperationRecord(info.recordText);
            info.completed = info.record && info.record.schema == 2 && info.record.status == "completed";
        }
        output.push_back(std::move(info));
    } while (FindNextFileW(finder, &found));
    const DWORD enumerationError = GetLastError();
    FindClose(finder);
    if (enumerationError != ERROR_NO_MORE_FILES) {
        error = enumerationError;
        return false;
    }
    return true;
}

// 只删除普通文件与目录，不跟随 reparse point；失败时返回 false，但继续尝试其它项。
inline bool RemoveTreeBestEffort(const std::wstring& directory) {
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    bool clean = true;
    WIN32_FIND_DATAW found{};
    HANDLE finder = FindFirstFileW((directory + L"\\*").c_str(), &found);
    if (finder == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring path = directory + L"\\" + name;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                clean = false;
                continue;
            }
            if (!RemoveTreeBestEffort(path)) clean = false;
        } else if (!(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(path.c_str())) clean = false;
        } else {
            clean = false;
        }
    } while (FindNextFileW(finder, &found));
    FindClose(finder);
    if (!RemoveDirectoryW(directory.c_str())) return false;
    return clean;
}

inline bool ReadClientRecordFile(const std::wstring& clientRoot, std::string& text, FileIdentity& identity) {
    GuardedPath record;
    DWORD error = ERROR_SUCCESS;
    if (!record.Open(clientRoot, L"host64/dlss5-install-record.json") ||
        !ReadJournalBytes(record, text, error)) {
        return false;
    }
    const HashResult hashed = HashFile(record.Path());
    if (!hashed) return false;
    identity = hashed.identity;
    return true;
}

inline bool ReplaceClientRecord(const std::wstring& clientRoot, const std::string& bytes, std::wstring& error) {
    const std::wstring token = ds5_detail::Widen(ds5_detail::UniqueToken());
    GuardedPath temporary;
    if (!temporary.Open(clientRoot, L"host64/dlss5-install-record.json.nr-restore-" + token, true)) {
        error = L"无法创建安装记录临时文件。";
        return false;
    }
    const WriteNewResult written = WriteNewVerified(temporary, bytes);
    if (!written) {
        error = L"无法写入安装记录临时文件。";
        return false;
    }
    if (!MoveFileExW(temporary.Path().c_str(), (clientRoot + L"\\host64\\dlss5-install-record.json").c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.Path().c_str());
        error = L"无法替换安装记录文件。";
        return false;
    }
    const HashResult hashed = HashFile(clientRoot + L"\\host64\\dlss5-install-record.json");
    std::array<std::uint8_t, 32> expected{};
    if (!hashed || HashBytes(bytes, expected) < 0 || hashed.identity.sha256 != expected) {
        error = L"安装记录写入后校验失败。";
        return false;
    }
    return true;
}

}  // namespace restore_detail

// 恢复最后一次已完成安装：逆序还原/删除客户端成员，恢复安装前的客户端记录。
inline RestoreResult RestoreLastInstallOperation(const std::wstring& clientRoot, const std::wstring& operationsRoot,
                                                 const std::vector<std::wstring>& protectedRoots,
                                                 const CatalogResult& catalog) {
    using namespace restore_detail;
    RestoreResult result;
    if (!catalog) {
        result.error = L"安装清单无效。";
        return result;
    }
    const ClientResult client = ValidateClient(clientRoot, protectedRoots);
    if (!client) {
        result.error = L"客户端目录无效。";
        return result;
    }
    if (ds5_detail::GameProcessRunning()) {
        result.error = L"请先退出游戏、启动器和 AIO 载体。";
        return result;
    }
    std::wstring root;
    if (!CanonicalLocalPath(operationsRoot, root) || WithinRoot(root, client.root) || WithinRoot(client.root, root)) {
        result.error = L"安装事务目录不安全。";
        return result;
    }
    GuardedPath rootGuard;
    if (!rootGuard.Open(root, L"__operations_root__")) {
        result.error = L"安装事务目录不可用。";
        result.win32Error = rootGuard.Error();
        return result;
    }
    std::vector<OperationDirInfo> operations;
    DWORD enumerationError = ERROR_SUCCESS;
    if (!EnumerateOperationDirs(root, operations, enumerationError)) {
        result.error = L"无法枚举安装事务目录。";
        result.win32Error = enumerationError;
        return result;
    }
    const OperationDirInfo* newest = nullptr;
    const OperationDirInfo* chosen = nullptr;
    for (const OperationDirInfo& operation : operations) {
        if (newest == nullptr || CompareFileTime(&operation.writeTime, &newest->writeTime) > 0) newest = &operation;
        if (!operation.completed || !SameClientPath(operation.record.client, client.root)) continue;
        if (chosen == nullptr || CompareFileTime(&operation.writeTime, &chosen->writeTime) > 0) chosen = &operation;
    }
    if (chosen == nullptr) {
        result.error = L"没有可恢复的安装事务记录。";
        return result;
    }
    if (newest != chosen && CompareFileTime(&newest->writeTime, &chosen->writeTime) > 0) {
        result.error = L"客户端有后续安装事务，请先恢复最近一次安装。";
        return result;
    }
    if (chosen->record.applied.empty() || chosen->record.entries.empty()) {
        result.error = L"安装事务记录缺少已应用成员。";
        return result;
    }
    result.operation = chosen->directory;
    const std::wstring operationId = chosen->id;
    const std::wstring quarantineSuffix = L".nr-" + operationId + L".original";
    auto skip = [&result](const std::wstring& target, const std::wstring& note) {
        RestoreMemberReport report;
        report.target = target;
        report.action = L"跳过";
        report.note = note;
        result.members.push_back(std::move(report));
        ++result.skipped;
    };
    auto failMember = [&result](const std::wstring& target, const std::wstring& note, DWORD error) {
        RestoreMemberReport report;
        report.target = target;
        report.action = L"失败";
        report.note = note;
        result.members.push_back(std::move(report));
        ++result.skipped;
        if (result.win32Error == ERROR_SUCCESS) result.win32Error = error;
    };
    for (auto applied = chosen->record.applied.rbegin(); applied != chosen->record.applied.rend(); ++applied) {
        const InstalledEntry* entry = nullptr;
        for (const InstalledEntry& candidate : chosen->record.entries) {
            if (SameTarget(candidate.target, *applied)) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            skip(*applied, L"事务记录缺少该成员。");
            continue;
        }
        std::array<std::uint8_t, 32> candidateBytes{};
        if (!ParseSha256Bytes(entry->sha256, candidateBytes)) {
            skip(entry->target, L"成员哈希无效。");
            continue;
        }
        GuardedPath targetPath;
        if (!targetPath.Open(client.root, entry->target)) {
            skip(entry->target, L"目标路径不可用。");
            continue;
        }
        if (!entry->previouslyPresent) {
            const DeleteResult deleted = DeleteVerified(targetPath, candidateBytes);
            if (deleted) {
                RestoreMemberReport report;
                report.target = entry->target;
                report.action = L"已删除";
                report.note = L"安装时新增的文件已移除。";
                result.members.push_back(std::move(report));
                ++result.deleted;
            } else if (deleted.error == DeleteError::Open &&
                       (deleted.win32Error == ERROR_FILE_NOT_FOUND || deleted.win32Error == ERROR_PATH_NOT_FOUND)) {
                skip(entry->target, L"文件已不存在。");
            } else {
                failMember(entry->target, L"文件身份与记录不符，未删除。", deleted.win32Error);
            }
            continue;
        }
        std::array<std::uint8_t, 32> previousBytes{};
        if (entry->previousSha256.empty() || !ParseSha256Bytes(entry->previousSha256, previousBytes)) {
            skip(entry->target, L"缺少安装前哈希。");
            continue;
        }
        const std::wstring backupMember = entry->backup.empty() ? entry->target : entry->backup;
        GuardedPath backupPath;
        if (!backupPath.Open(chosen->directory, L"backup/" + backupMember)) {
            skip(entry->target, L"备份文件不可用。");
            continue;
        }
        const HashResult backupHash = HashFile(backupPath.Path());
        if (!backupHash || backupHash.identity.sha256 != previousBytes) {
            skip(entry->target, L"备份文件哈希不符。");
            continue;
        }
        GuardedPath temporary;
        if (!temporary.Open(client.root,
                            entry->target + L".nr-restore-" + ds5_detail::Widen(ds5_detail::UniqueToken()), true)) {
            skip(entry->target, L"无法创建临时文件。");
            continue;
        }
        const CopyResult copied = CopyVerifiedNew(backupPath, temporary, previousBytes);
        if (!copied) {
            DeleteFileW(temporary.Path().c_str());
            failMember(entry->target, L"备份复制失败。", copied.win32Error);
            continue;
        }
        if (!MoveFileExW(temporary.Path().c_str(), targetPath.Path().c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD moveError = GetLastError();
            DeleteFileW(temporary.Path().c_str());
            failMember(entry->target, L"原文件还原失败。", moveError);
            continue;
        }
        const HashResult restoredHash = HashFile(targetPath.Path());
        if (!restoredHash || restoredHash.identity.sha256 != previousBytes) {
            failMember(entry->target, L"还原后校验失败。", ERROR_CRC);
            continue;
        }
        RestoreMemberReport report;
        report.target = entry->target;
        report.action = L"已还原";
        report.note = L"安装前文件已写回。";
        result.members.push_back(std::move(report));
        ++result.restored;
        const std::wstring quarantine = client.root + L"\\" + entry->target + quarantineSuffix;
        DeleteFileW(quarantine.c_str());
    }
    // 记录与备份目录：有 previous_client_record 就写回，否则删除记录。
    const Json& original = chosen->record.original;
    Json previous = nullptr;
    if (original.is_object() && original.contains("previous_client_record")) {
        previous = original["previous_client_record"];
    }
    if (previous.is_object()) {
        const std::string recordText = previous.dump(2) + "\n";
        if (!ReadClientRecord(recordText)) {
            result.error = L"事务记录中的安装前记录无效。";
            return result;
        }
        std::wstring recordError;
        if (!ReplaceClientRecord(client.root, recordText, recordError)) {
            result.error = recordError;
            return result;
        }
        result.recordRestored = true;
    } else {
        std::string existingText;
        FileIdentity existingIdentity{};
        if (ReadClientRecordFile(client.root, existingText, existingIdentity)) {
            GuardedPath recordPath;
            if (recordPath.Open(client.root, L"host64/dlss5-install-record.json")) {
                const DeleteResult removed = DeleteVerified(recordPath, existingIdentity.sha256);
                result.recordRemoved = static_cast<bool>(removed);
            }
        } else {
            result.recordRemoved = true;
        }
    }
    if (result.recordRemoved) {
        // 只有“恢复到首次安装之前”才清理备份目录；写回上一次安装记录时，那份记录仍然
        // 引用 host64\dlss5-backup，删掉会让后续卸载/修复失去原件。
        result.backupDirectoryRemoved = RemoveTreeBestEffort(client.root + L"\\host64\\dlss5-backup");
    }
    // 结果文件：只记录，不改写 transaction.json。
    Json restore = {{"schema", 1},
                    {"status", result.skipped == 0 ? "restored" : "partial"},
                    {"client", restore_detail::Utf8Of(client.root)},
                    {"operation_id", restore_detail::Utf8Of(chosen->id)},
                    {"restored", result.restored},
                    {"deleted", result.deleted},
                    {"skipped", result.skipped}};
    std::wstring reportText;
    for (const RestoreMemberReport& member : result.members) {
        if (!reportText.empty()) reportText += L"；";
        reportText += member.target + L"：" + member.action;
        if (!member.note.empty()) reportText += L"（" + member.note + L"）";
    }
    if (reportText.empty()) reportText = L"没有需要处理的成员。";
    Json reportMembers = Json::array();
    for (const RestoreMemberReport& member : result.members) {
        Json item = {{"target", restore_detail::Utf8Of(member.target)},
                     {"action", restore_detail::Utf8Of(member.action)},
                     {"note", restore_detail::Utf8Of(member.note)}};
        reportMembers.push_back(std::move(item));
    }
    restore["members"] = reportMembers;
    GuardedPath restoreTemp;
    if (restoreTemp.Open(chosen->directory, L"restore.json.nr-" + ds5_detail::Widen(ds5_detail::UniqueToken()), true)) {
        const std::string bytes = restore.dump(2) + "\n";
        const WriteNewResult written = WriteNewVerified(restoreTemp, bytes);
        if (written) {
            MoveFileExW(restoreTemp.Path().c_str(), (chosen->directory + L"\\restore.json").c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        } else {
            DeleteFileW(restoreTemp.Path().c_str());
        }
    }
    result.ok = true;
    result.status = result.skipped == 0 ? L"restored" : L"partial";
    result.report = reportText;
    return result;
}

}  // namespace control_center
