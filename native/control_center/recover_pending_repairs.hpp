#pragma once

#include "scan_repairs.hpp"
#include "cleanup_repair.hpp"

namespace control_center {
enum class PendingRepairState { NoWork, RolledBack, Completed, NeedsAttention };
struct PendingRepairResult {
    PendingRepairState state = PendingRepairState::NeedsAttention;
    std::wstring repairId;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    bool cleanupPending = false;
    size_t cleanedOriginals = 0;
};

// Scan and resolve at most one repair under the same client lock. The caller
// must also check installation transactions before allowing any new work.
inline PendingRepairResult RecoverPendingRepairs(const std::wstring& repairsRoot, const std::wstring& clientRoot,
                                                  const std::wstring& sourceOperationsRoot, const std::vector<std::wstring>& protectedRoots,
                                                  const CatalogResult& catalog, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                                  , ProcessCheck (*testProcesses)() = nullptr
#endif
                                                  ) {
    PendingRepairResult result;
    auto fail = [&result](std::string reason, DWORD error = ERROR_SUCCESS) {
        result.state = PendingRepairState::NeedsAttention; result.reason = std::move(reason); result.win32Error = error; return result;
    };
    const auto scan = ScanClientRepairs(repairsRoot, clientRoot, sourceOperationsRoot, protectedRoots, catalog, lock);
    if (!scan.ok) return fail("repair scan failed", scan.win32Error);
    const ScannedRepair* pending = nullptr;
    for (const auto& item : scan.repairs) {
        if (item.state == RepairScanState::Unknown) return fail("unknown repair requires inspection");
        if (item.state != RepairScanState::Incomplete) continue;
        if (pending) return fail("multiple unfinished repairs require inspection");
        pending = &item;
    }
    auto cleanupFinished = [&](const RepairScanResult& inventory) {
        for (const auto& item : inventory.repairs) {
            if (item.state != RepairScanState::Completed) continue;
            const auto plan = ReadTrustedRepairPlan(clientRoot, sourceOperationsRoot, item.sourceOperation, repairsRoot,
                                                    item.id, protectedRoots, catalog, lock);
            if (!plan.valid) { result.cleanupPending = true; continue; }
            const auto cleaned = CleanupCommittedRepair(clientRoot, sourceOperationsRoot, item.sourceOperation, repairsRoot,
                                                        item.id, protectedRoots, catalog, lock
#ifdef CONTROL_CENTER_TESTING
                                                        , testProcesses
#endif
                                                        );
            if (!cleaned.completed) result.cleanupPending = true;
            result.cleanedOriginals += cleaned.removed;
        }
    };
    if (!pending) { result.state = PendingRepairState::NoWork; cleanupFinished(scan); return result; }
    result.repairId = pending->id;
    GuardedPath marker, pendingMarker;
    if (!marker.Open(pending->directory, L"commit.json") || !pendingMarker.Open(pending->directory, L"commit.pending"))
        return fail("repair commit paths unavailable");
    auto presence = [](const GuardedPath& path) -> int {
        if (GetFileAttributesW(path.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 1;
        return MissingPathError(GetLastError()) ? 0 : -1;
    };
    const int committed = presence(marker), committing = presence(pendingMarker);
    if (committed < 0 || committing < 0 || (committed && committing)) return fail("repair commit state unavailable");
    if (committed || committing) {
        const auto completed = CompleteRepair(clientRoot, sourceOperationsRoot, pending->sourceOperation, repairsRoot,
                                               pending->id, protectedRoots, catalog, lock
#ifdef CONTROL_CENTER_TESTING
                                               , testProcesses
#endif
                                               );
        if (completed.state != RepairCompleteState::Completed) return fail(completed.reason, completed.win32Error);
        result.state = PendingRepairState::Completed;
    } else {
        const auto recovered = RecoverRepair(clientRoot, sourceOperationsRoot, pending->sourceOperation, repairsRoot,
                                              pending->id, protectedRoots, catalog, lock
#ifdef CONTROL_CENTER_TESTING
                                              , testProcesses
#endif
                                              );
        if (recovered.state != RepairRecoveryState::Recovered && recovered.state != RepairRecoveryState::AlreadyTerminal)
            return fail(recovered.reason.empty() ? "repair recovery requires inspection" : recovered.reason, recovered.win32Error);
        result.state = PendingRepairState::RolledBack;
    }
    const auto after = ScanClientRepairs(repairsRoot, clientRoot, sourceOperationsRoot, protectedRoots, catalog, lock);
    if (!after.ok || after.blocksNewOperation) return fail("recovery left unresolved repair state", after.win32Error);
    cleanupFinished(after);
    return result;
}
}
