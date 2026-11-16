#pragma once

#include "complete_repair.hpp"

namespace control_center {
struct RepairCleanupResult {
    bool completed = false;
    size_t removed = 0;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
};

// The saved copy is retained. A quarantined original may be removed only after
// the commit and saved copy are verified, and only by its exact file identity.
// Missing originals are accepted so a crash after deletion can be retried.
inline RepairCleanupResult CleanupCommittedRepair(const std::wstring& clientRoot, const std::wstring& sourceOperationsRoot,
                                                   const std::wstring& sourceOperation, const std::wstring& repairsRoot,
                                                   const std::wstring& repairId, const std::vector<std::wstring>& protectedRoots,
                                                   const CatalogResult& catalog, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                                   , ProcessCheck (*testProcesses)() = nullptr
#endif
                                                   ) {
    RepairCleanupResult result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) { result.reason = reason; result.win32Error = error; return result; };
    auto committed = [&]() {
        return CompleteRepair(clientRoot, sourceOperationsRoot, sourceOperation, repairsRoot, repairId, protectedRoots, catalog, lock
#ifdef CONTROL_CENTER_TESTING
                              , testProcesses
#endif
                              );
    };
    std::wstring client, repairs;
    if (!ValidOperationId(repairId) || !CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(repairsRoot, repairs)) return fail("invalid roots");
    const auto journal = LoadJournal(repairs + L"\\" + repairId);
    if (!journal || !journal.cursor.Terminal() || journal.cursor.TerminalKind() != JournalKind::Complete)
        return fail("repair is not committed", journal.win32Error);
    const auto ready = committed();
    if (ready.state != RepairCompleteState::Completed) return fail("repair commit cannot be verified", ready.win32Error);
    const auto plan = ReadTrustedRepairPlan(clientRoot, sourceOperationsRoot, sourceOperation, repairsRoot, repairId, protectedRoots, catalog, lock);
    if (!plan.valid) return fail("repair plan cannot be verified", plan.win32Error);
    for (const auto& action : plan.actions) {
        if (!action.previouslyPresent) continue;
        const auto rechecked = committed();
        if (rechecked.state != RepairCompleteState::Completed) return fail("repair changed before cleanup", rechecked.win32Error);
        const auto fresh = ReadTrustedRepairPlan(client, sourceOperationsRoot, sourceOperation, repairs, repairId, protectedRoots, catalog, lock);
        if (!fresh.valid || !SameIdentity(fresh.identity, plan.identity)) return fail("repair plan changed before cleanup", fresh.win32Error);
        GuardedPath backup, original;
        if (!backup.Open(repairs + L"\\" + repairId, L"modified/" + action.member.target) ||
            !original.Open(client, action.member.target + L".nr-repair-" + repairId + L".original"))
            return fail("cleanup path unavailable");
        HANDLE savedHandle = CreateFileW(backup.Path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (savedHandle == INVALID_HANDLE_VALUE) return fail("saved modification unavailable", GetLastError());
        const auto saved = HashOpenFile(savedHandle);
        if (!saved || saved.identity.sha256 != action.previous.sha256) {
            CloseHandle(savedHandle); return fail("saved modification changed", saved.win32Error);
        }
        const auto current = HashFile(original.Path());
        if (!current) {
            CloseHandle(savedHandle);
            if (current.error == HashError::Open && MissingPathError(current.win32Error)) continue;
            return fail("isolated original unavailable", current.win32Error);
        }
        if (!SameIdentity(current.identity, action.previous)) { CloseHandle(savedHandle); return fail("isolated original changed"); }
        const auto deleted = DeleteVerified(original, action.previous.sha256, &action.previous);
        CloseHandle(savedHandle);
        if (!deleted) return fail("isolated original could not be removed", deleted.win32Error);
        ++result.removed;
    }
    result.completed = true;
    return result;
}
}
