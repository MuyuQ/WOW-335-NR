#pragma once

#include <cstdio>

#include "install_job.hpp"
#include "recover_pending_repairs.hpp"
#include "scan_uninstalls.hpp"

namespace control_center {
struct CoordinatedRecoveryResult {
    bool ready = false;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    PendingRecoveryResult installation;
    PendingRepairResult repair;
    UninstallRecoveryResult uninstall;
    bool recoveredUninstall = false;
};

// No transaction may be recovered until all inventories have been checked.
inline CoordinatedRecoveryResult RecoverBeforeWork(const std::wstring& operationsRoot, const std::wstring& repairsRoot,
                                                    const std::wstring& uninstallsRoot, const std::wstring& sourceOperationsRoot,
                                                    const std::wstring& clientRoot, const std::vector<std::wstring>& protectedRoots,
                                                    const CatalogResult& catalog, const Json& manifest, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                                    , ProcessCheck (*testProcesses)() = nullptr
#endif
                                                    ) {
    CoordinatedRecoveryResult result;
    auto fail = [&result](std::string reason, DWORD error = ERROR_SUCCESS) {
        result.reason = std::move(reason); result.win32Error = error; return result;
    };
    std::wstring operations, repairsDirectory, uninstalls;
    if (!CanonicalLocalPath(operationsRoot, operations) || !CanonicalLocalPath(repairsRoot, repairsDirectory) ||
        !CanonicalLocalPath(uninstallsRoot, uninstalls) || WithinRoot(operations, repairsDirectory) || WithinRoot(repairsDirectory, operations) ||
        WithinRoot(operations, uninstalls) || WithinRoot(uninstalls, operations) || WithinRoot(repairsDirectory, uninstalls) ||
        WithinRoot(uninstalls, repairsDirectory)) return fail("transaction roots overlap or are invalid");
    const auto installations = ScanClientOperations(operationsRoot, lock, catalog);
    if (!installations.ok) return fail("installation scan failed", installations.win32Error);
    const auto repairs = ScanClientRepairs(repairsRoot, clientRoot, sourceOperationsRoot, protectedRoots, catalog, lock);
    if (!repairs.ok) return fail("repair scan failed", repairs.win32Error);
    auto uninstallsScan = ScanClientUninstalls(uninstallsRoot, clientRoot, protectedRoots, catalog, manifest, lock);
    if (!uninstallsScan.ok) return fail("uninstall scan failed", uninstallsScan.win32Error);
    // An uninstall whose members and record snapshot are complete only needs the
    // forward finalization. Finish it now, then rescan: the record and residue
    // cleanup is what makes the directory complete.
    auto utcNow = []() {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        char buffer[32]{};
        const int written = std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                                          static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
                                          static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
                                          static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond));
        return written > 0 ? std::string(buffer) : std::string();
    };
    bool sawFinalization = false;
    for (size_t pass = 0; pass < 4; ++pass) {
        bool needsFinalization = false;
        for (const auto& item : uninstallsScan.uninstalls)
            if (item.state == UninstallScanState::AwaitingFinalization || item.state == UninstallScanState::Finalizing)
                needsFinalization = true;
        if (!needsFinalization) break;
        sawFinalization = true;
        for (const auto& item : uninstallsScan.uninstalls) {
            if (item.state != UninstallScanState::AwaitingFinalization && item.state != UninstallScanState::Finalizing) continue;
            const auto finalized = FinalizeUninstall(clientRoot, uninstallsRoot, item.id, protectedRoots, catalog, manifest, lock, utcNow()
#ifdef CONTROL_CENTER_TESTING
                                                     , testProcesses
#endif
                                                     );
            if (finalized.state != UninstallFinalizeState::Completed && finalized.state != UninstallFinalizeState::AlreadyCompleted)
                return fail(finalized.reason.empty() ? "uninstall finalization requires inspection" : finalized.reason, finalized.win32Error);
            result.uninstall.state = UninstallRecoveryState::Recovered;
            result.uninstall.reason.clear();
            result.recoveredUninstall = true;
        }
        uninstallsScan = ScanClientUninstalls(uninstallsRoot, clientRoot, protectedRoots, catalog, manifest, lock);
        if (!uninstallsScan.ok) return fail("uninstall scan failed", uninstallsScan.win32Error);
    }
    if (sawFinalization) {
        for (const auto& item : uninstallsScan.uninstalls)
            if (item.state == UninstallScanState::AwaitingFinalization || item.state == UninstallScanState::Finalizing)
                return fail("uninstall finalization did not converge");
    }
    size_t incompleteInstallations = 0, incompleteRepairs = 0, incompleteUninstalls = 0;
    const ScannedUninstall* pendingUninstall = nullptr;
    for (const auto& item : uninstallsScan.uninstalls) {
        if (item.state == UninstallScanState::Unknown) return fail("unknown uninstall requires inspection");
        if (item.state == UninstallScanState::Incomplete) { ++incompleteUninstalls; pendingUninstall = &item; }
    }
    for (const auto& item : installations.operations) {
        if (item.state == OperationScanState::Unknown) return fail("unknown installation requires inspection");
        if (item.state == OperationScanState::Incomplete) ++incompleteInstallations;
    }
    for (const auto& item : repairs.repairs) {
        if (item.state == RepairScanState::Unknown) return fail("unknown repair requires inspection");
        if (item.state == RepairScanState::Incomplete) ++incompleteRepairs;
    }
    if (incompleteInstallations + incompleteRepairs + incompleteUninstalls > 1)
        return fail("multiple unfinished transactions require inspection");
    if (pendingUninstall) {
        result.uninstall = RecoverUninstall(clientRoot, uninstallsRoot, pendingUninstall->id, protectedRoots, catalog, manifest, lock
#ifdef CONTROL_CENTER_TESTING
                                            , testProcesses
#endif
                                            );
        if (result.uninstall.state != UninstallRecoveryState::Recovered && result.uninstall.state != UninstallRecoveryState::AlreadyTerminal)
            return fail(result.uninstall.reason.empty() ? "uninstall recovery requires inspection" : result.uninstall.reason, result.uninstall.win32Error);
        const auto after = ScanClientUninstalls(uninstallsRoot, clientRoot, protectedRoots, catalog, manifest, lock);
        if (!after.ok || after.blocksNewOperation) return fail("uninstall recovery left unresolved state", after.win32Error);
        result.recoveredUninstall = true;
    }
    result.installation = RecoverPendingOperations(operationsRoot, clientRoot, protectedRoots, catalog, manifest, lock
#ifdef CONTROL_CENTER_TESTING
                                                   , testProcesses
#endif
                                                   );
    if (result.installation.state == PendingRecoveryState::NeedsAttention)
        return fail(result.installation.reason, result.installation.win32Error);
    result.repair = RecoverPendingRepairs(repairsRoot, clientRoot, sourceOperationsRoot, protectedRoots, catalog, lock
#ifdef CONTROL_CENTER_TESTING
                                         , testProcesses
#endif
                                         );
    if (result.repair.state == PendingRepairState::NeedsAttention) return fail(result.repair.reason, result.repair.win32Error);
    result.ready = true;
    return result;
}

struct CoordinatedInstallResult {
    CoordinatedRecoveryResult recovery;
    InstallJobResult installation;
    bool attemptedInstallation = false;
};
inline CoordinatedInstallResult RunCoordinatedInstall(const std::wstring& operationsRoot, const std::wstring& repairsRoot,
                                                       const std::wstring& uninstallsRoot, const std::wstring& sourceOperationsRoot,
                                                       const MemberInstallContext& context, const CatalogResult& catalog,
                                                       const Json& manifest, const InstallPreview& approved,
                                                       const std::string& installedUtc, const ClientLock& lock) {
    CoordinatedInstallResult result;
    result.recovery = RecoverBeforeWork(operationsRoot, repairsRoot, uninstallsRoot, sourceOperationsRoot, context.clientRoot, context.protectedRoots, catalog, manifest, lock
#ifdef CONTROL_CENTER_TESTING
                                        , context.testProcesses
#endif
                                        );
    if (!result.recovery.ready) return result;
    result.attemptedInstallation = true;
    result.installation = RunInstallJob(operationsRoot, context, catalog, manifest, approved, installedUtc, lock);
    return result;
}
}
