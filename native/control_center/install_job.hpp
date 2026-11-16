#pragma once

#include "start_with_recovery.hpp"

namespace control_center {
enum class InstallJobState { Completed, AlreadyCurrent, RolledBack, PreconditionFailed, NeedsAttention };
struct InstallJobResult {
    InstallJobState state = InstallJobState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    bool committedAfterRecovery = false;
    size_t applied = 0;
    PendingRecoveryResult previousRecovery, failureRecovery;
};

// Runs one complete installation under the caller's client lock. A failed
// publication is reconciled according to the durable decision state; unknown
// state remains available for inspection and is never reported as success.
inline InstallJobResult RunInstallJob(const std::wstring& operationsRoot, const MemberInstallContext& context,
                                      const CatalogResult& catalog, const Json& manifest, const InstallPreview& approved,
                                      const std::string& installedUtc, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                      , bool (*recordInterrupt)(RecordPublishCheckpoint) = nullptr,
                                        bool (*commitInterrupt)(CommitCheckpoint) = nullptr
#endif
                                      ) {
    InstallJobResult result;
    auto fail = [&result](InstallJobState state, std::string reason, DWORD error = ERROR_SUCCESS) {
        result.state = state; result.reason = std::move(reason); result.win32Error = error; return result;
    };
    if (!ManifestMatchesCatalog(manifest, catalog) || !ValidUtcTimestamp(installedUtc))
        return fail(InstallJobState::PreconditionFailed, "manifest, catalog or timestamp invalid");
    const auto start = StartInstallWithRecovery(operationsRoot, context, catalog, manifest, approved, lock);
    result.previousRecovery = start.recovery;
    if (!start.attemptedInstallation) return fail(InstallJobState::NeedsAttention, start.recovery.reason, start.recovery.win32Error);
    result.applied = start.installation.applied;
    if (start.installation.state == BatchInstallState::AlreadyCurrent) { result.state = InstallJobState::AlreadyCurrent; return result; }
    if (start.installation.state == BatchInstallState::PreconditionFailed)
        return fail(InstallJobState::PreconditionFailed, start.installation.reason, start.installation.win32Error);
    if (start.installation.state == BatchInstallState::RolledBack)
        return fail(InstallJobState::RolledBack, start.installation.reason, start.installation.win32Error);
    auto reconcileFailure = [&](const std::string& reason, DWORD error) {
        result.failureRecovery = RecoverPendingOperations(operationsRoot, context.clientRoot, context.protectedRoots, catalog, manifest, lock
#ifdef CONTROL_CENTER_TESTING
                                                           , context.testProcesses
#endif
                                                           );
        if (result.failureRecovery.operationId != context.operationId)
            return fail(InstallJobState::NeedsAttention, reason, error);
        if (result.failureRecovery.state == PendingRecoveryState::Committed) {
            result.state = InstallJobState::Completed; result.committedAfterRecovery = true; result.reason = reason; return result;
        }
        if (result.failureRecovery.state == PendingRecoveryState::RolledBack) return fail(InstallJobState::RolledBack, reason, error);
        return fail(InstallJobState::NeedsAttention, result.failureRecovery.reason.empty() ? reason : result.failureRecovery.reason,
                    result.failureRecovery.win32Error ? result.failureRecovery.win32Error : error);
    };
    if (start.installation.state != BatchInstallState::ReadyToFinalize)
        return reconcileFailure(start.installation.reason, start.installation.win32Error);
    const auto record = PublishClientRecord(context, catalog, manifest, lock, installedUtc
#ifdef CONTROL_CENTER_TESTING
                                            , recordInterrupt
#endif
                                            );
    if (record.state != RecordPublishState::Published) return reconcileFailure(record.reason, record.win32Error);
    const auto commit = CommitInstall(context, catalog, manifest, lock
#ifdef CONTROL_CENTER_TESTING
                                      , commitInterrupt
#endif
                                      );
    if (commit.state != CommitState::Completed) return reconcileFailure(commit.reason, commit.win32Error);
    result.state = InstallJobState::Completed;
    return result;
}
}
