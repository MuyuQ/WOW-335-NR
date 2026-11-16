#pragma once

#include "coordinated_install.hpp"

namespace control_center {
struct CoordinatedUninstallPlanResult {
    CoordinatedRecoveryResult recovery;
    UninstallPlanResult plan;
    bool attemptedPlan = false;
};

// A plan is metadata only. The future executor must keep the lock, revalidate
// identities, and recover its own journal before any subsequent write.
inline CoordinatedUninstallPlanResult StartCoordinatedUninstall(const std::wstring& operationsRoot, const std::wstring& repairsRoot,
                                                                 const std::wstring& uninstallsRoot, const std::wstring& sourceOperationsRoot,
                                                                 const std::wstring& clientRoot, const std::wstring& uninstallId,
                                                                 const std::vector<std::wstring>& protectedRoots, const CatalogResult& catalog,
                                                                 const Json& manifest, const UninstallPreview& approved, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                                                 , ProcessCheck (*testProcesses)() = nullptr
#endif
                                                                 ) {
    CoordinatedUninstallPlanResult result;
    result.recovery = RecoverBeforeWork(operationsRoot, repairsRoot, uninstallsRoot, sourceOperationsRoot, clientRoot, protectedRoots, catalog, manifest, lock
#ifdef CONTROL_CENTER_TESTING
                                        , testProcesses
#endif
                                        );
    if (!result.recovery.ready) return result;
    result.attemptedPlan = true;
    result.plan = PrepareUninstallPlan(clientRoot, uninstallsRoot, uninstallId, protectedRoots, catalog, manifest, approved, lock);
    return result;
}
}
