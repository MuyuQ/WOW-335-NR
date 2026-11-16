#pragma once

#include "install_batch.hpp"
#include "scan_operations.hpp"

namespace control_center {
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
    if (!BindJournalSession(operation, context.operationId, lock, catalog, prior, false))
        return fail("cannot bind operation session", true);
    const auto after = ScanClientOperations(root, lock, catalog);
    if (!after.ok || after.blocksNewOperation) return fail("operation state changed during creation", true, after.win32Error);
    return InstallBatch(context, catalog, approved, lock);
}
}
