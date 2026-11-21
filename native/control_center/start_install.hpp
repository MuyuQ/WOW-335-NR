#pragma once

#include "install_batch.hpp"
#include "scan_operations.hpp"

namespace control_center {
// Removes an operation directory that this attempt created but provably holds no
// work: nothing but session.json (or nothing at all). Never recursive and never
// follows links; on any doubt the directory is kept as evidence.
// Needed because a no-write reinstall otherwise leaves a permanent empty
// transaction behind, and because a directory created before a failed
// BindJournalSession has no session.json at all - later scans classify that as
// "unknown" and block every subsequent installation.
inline bool DiscardFreshOperation(const std::wstring& journalRoot) {
    if (journalRoot.empty()) return false;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = journalRoot + L"\\*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool onlySession = true;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        if (wcscmp(data.cFileName, L"session.json") != 0) { onlySession = false; break; }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) { onlySession = false; break; }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (!onlySession) return false;
    const std::wstring session = journalRoot + L"\\session.json";
    if (!DeleteFileW(session.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    return RemoveDirectoryW(journalRoot.c_str()) != FALSE;
}

// Serialize on the client's lock, inspect the durable operations directory,
// and bind a new operation before any member is written. The caller must use
// this entry point rather than invoking individual member helpers as a job.
inline BatchInstallResult StartInstallBatch(const std::wstring& operationsRoot, const MemberInstallContext& context,
                                            const CatalogResult& catalog, const InstallPreview& approved,
                                            const ClientLock& lock) {
    BatchInstallResult result;
    auto fail = [&result](const char* reason, bool recovery = false, DWORD error = ERROR_SUCCESS) {
        result.reason = reason; result.win32Error = error;
        result.state = recovery ? BatchInstallState::RecoveryRequired : BatchInstallState::PreconditionFailed;
        return result;
    };
    if (!catalog || !ValidOperationId(context.operationId) || !lock.IsFor(context.clientRoot)) return fail("invalid context or lock");
    const auto client = ValidateClient(context.clientRoot, context.protectedRoots);
    if (!client || MemberProcesses(context).state != ProcessState::Stopped) return fail("client or process state prevents installation");
    std::wstring root, operation;
    GuardedPath rootGuard;
    if (!CanonicalLocalPath(operationsRoot, root) || !CanonicalLocalPath(context.journalRoot, operation) ||
        WithinRoot(root, client.root) || WithinRoot(client.root, root) ||
        CompareStringOrdinal((root + L"\\" + context.operationId).c_str(), -1, operation.c_str(), -1, TRUE) != CSTR_EQUAL ||
        !rootGuard.Open(root, L"__root__")) return fail("invalid operations root", false, rootGuard.Error());
    const auto scan = ScanClientOperations(root, lock, catalog);
    if (!scan.ok || scan.blocksNewOperation) return fail("unfinished or unknown operation requires inspection", true, scan.win32Error);
    const auto prior = CaptureClientRecord(client.root);
    if (prior.state != CapturedRecordState::Absent && prior.state != CapturedRecordState::Present)
        return fail("prior client record invalid or unavailable", false, prior.win32Error);
    if (!CreateDirectoryW(operation.c_str(), nullptr)) return fail("cannot create new operation", false, GetLastError());
    if (!BindJournalSession(operation, context.operationId, lock, catalog, prior, false)) {
        // A session-less directory would be classified "unknown" and block later
        // installs, so remove only this attempt's provably empty directory.
        DiscardFreshOperation(operation);
        return fail("cannot bind operation session", true);
    }
    const auto after = ScanClientOperations(root, lock, catalog);
    if (!after.ok || after.blocksNewOperation) {
        DiscardFreshOperation(operation);
        return fail("operation state changed during creation", true, after.win32Error);
    }
    return InstallBatch(context, catalog, approved, lock);
}
}
