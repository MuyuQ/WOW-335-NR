#pragma once

#include "recover_pending.hpp"
#include "start_install.hpp"

namespace control_center {
struct StartWithRecoveryResult {
    PendingRecoveryResult recovery;
    BatchInstallResult installation;
    bool attemptedInstallation = false;
};

// Recovery is resolved under the same client lock before a new operation is
// created. A stale approved preview is still rejected by the batch installer.
inline StartWithRecoveryResult StartInstallWithRecovery(const std::wstring& operationsRoot, const MemberInstallContext& context,
                                                         const CatalogResult& catalog, const Json& manifest,
                                                         const InstallPreview& approved, const ClientLock& lock) {
    StartWithRecoveryResult result;
    result.recovery = RecoverPendingOperations(operationsRoot, context.clientRoot, context.protectedRoots, catalog, manifest, lock
#ifdef CONTROL_CENTER_TESTING
                                                , context.testProcesses
#endif
                                                );
    if (result.recovery.state == PendingRecoveryState::NeedsAttention) return result;
    result.attemptedInstallation = true;
    result.installation = StartInstallBatch(operationsRoot, context, catalog, approved, lock);
    return result;
}
}
