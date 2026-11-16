#pragma once

#include "coordinated_install.hpp"
#include "cleanup_repair.hpp"

namespace control_center {
enum class RepairJobState { Completed, AlreadyCurrent, RolledBack, PreconditionFailed, NeedsAttention };
struct RepairJobResult {
    RepairJobState state = RepairJobState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    size_t repaired = 0;
    bool committedAfterRecovery = false;
    bool cleanupPending = false;
    size_t cleanedOriginals = 0;
    CoordinatedRecoveryResult previousRecovery;
    PendingRepairResult failureRecovery;
};

// Caller holds the client lock across preview approval and this operation.
// A fresh preview is compared by PrepareRepairPlan before any client write.
inline RepairJobResult RunRepairJob(const std::wstring& operationsRoot, const std::wstring& repairsRoot, const std::wstring& uninstallsRoot,
                                    const std::wstring& sourceOperationsRoot, const std::wstring& sourceOperation,
                                    const std::wstring& repairId, const std::wstring& clientRoot,
                                    const std::vector<std::wstring>& protectedRoots, const CatalogResult& catalog,
                                    const Json& manifest, const RepairPreview& approved, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                    , ProcessCheck (*testProcesses)() = nullptr,
                                      bool (*memberInterrupt)(RepairMemberCheckpoint) = nullptr,
                                      bool (*completeInterrupt)(RepairCompleteCheckpoint) = nullptr
#endif
                                    ) {
    RepairJobResult result;
    auto fail = [&result](RepairJobState state, std::string reason, DWORD error = ERROR_SUCCESS) {
        result.state = state; result.reason = std::move(reason); result.win32Error = error; return result;
    };
    if (!ManifestMatchesCatalog(manifest, catalog)) return fail(RepairJobState::PreconditionFailed, "manifest or catalog invalid");
    result.previousRecovery = RecoverBeforeWork(operationsRoot, repairsRoot, uninstallsRoot, sourceOperationsRoot, clientRoot, protectedRoots, catalog, manifest, lock
#ifdef CONTROL_CENTER_TESTING
                                                , testProcesses
#endif
                                                );
    if (!result.previousRecovery.ready) return fail(RepairJobState::NeedsAttention, result.previousRecovery.reason, result.previousRecovery.win32Error);
    const auto plan = PrepareRepairPlan(clientRoot, sourceOperationsRoot, sourceOperation, repairsRoot, repairId, protectedRoots, catalog, approved, lock);
    if (plan.state == RepairPlanState::NoWork) { result.state = RepairJobState::AlreadyCurrent; return result; }
    if (plan.state != RepairPlanState::Ready)
        return fail(plan.state == RepairPlanState::NeedsAttention ? RepairJobState::NeedsAttention : RepairJobState::PreconditionFailed,
                    plan.reason, plan.win32Error);
    auto finish = [&](bool recovered, const std::string& reason) {
        result.state = RepairJobState::Completed; result.committedAfterRecovery = recovered; result.reason = reason;
        const auto cleanup = CleanupCommittedRepair(clientRoot, sourceOperationsRoot, sourceOperation, repairsRoot, repairId, protectedRoots, catalog, lock
#ifdef CONTROL_CENTER_TESTING
                                                    , testProcesses
#endif
                                                    );
        result.cleanedOriginals = result.failureRecovery.cleanedOriginals + cleanup.removed;
        if (!cleanup.completed) { result.cleanupPending = true; result.reason = cleanup.reason; result.win32Error = cleanup.win32Error; }
        return result;
    };
    auto reconcile = [&](const std::string& reason, DWORD error) {
        result.failureRecovery = RecoverPendingRepairs(repairsRoot, clientRoot, sourceOperationsRoot, protectedRoots, catalog, lock
#ifdef CONTROL_CENTER_TESTING
                                                       , testProcesses
#endif
                                                       );
        if (result.failureRecovery.repairId == repairId && result.failureRecovery.state == PendingRepairState::RolledBack)
            return fail(RepairJobState::RolledBack, reason, error);
        if (result.failureRecovery.repairId == repairId && result.failureRecovery.state == PendingRepairState::Completed) {
            return finish(true, reason);
        }
        if (result.failureRecovery.state == PendingRepairState::NoWork) {
            const auto scan = ScanClientRepairs(repairsRoot, clientRoot, sourceOperationsRoot, protectedRoots, catalog, lock);
            if (scan.ok && !scan.blocksNewOperation) for (const auto& item : scan.repairs) {
                if (item.id == repairId && item.state == RepairScanState::Completed) {
                    return finish(true, reason);
                }
            }
        }
        return fail(RepairJobState::NeedsAttention, result.failureRecovery.reason.empty() ? reason : result.failureRecovery.reason,
                    result.failureRecovery.win32Error ? result.failureRecovery.win32Error : error);
    };
    for (size_t index = 0; index < plan.actions; ++index) {
        const auto applied = ApplyRepairMember(clientRoot, sourceOperationsRoot, sourceOperation, repairsRoot, repairId, protectedRoots, catalog, index, lock
#ifdef CONTROL_CENTER_TESTING
                                               , testProcesses, memberInterrupt
#endif
                                               );
        if (applied.state != RepairMemberState::Applied && applied.state != RepairMemberState::AlreadyApplied)
            return reconcile(applied.reason, applied.win32Error);
        ++result.repaired;
    }
    const auto completed = CompleteRepair(clientRoot, sourceOperationsRoot, sourceOperation, repairsRoot, repairId, protectedRoots, catalog, lock
#ifdef CONTROL_CENTER_TESTING
                                           , testProcesses, completeInterrupt
#endif
                                           );
    if (completed.state != RepairCompleteState::Completed) return reconcile(completed.reason, completed.win32Error);
    return finish(false, "");
}
}
