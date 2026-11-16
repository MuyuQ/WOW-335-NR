#pragma once

#include "repair_plan_read.hpp"

namespace control_center {
enum class RepairMemberState { Applied, AlreadyApplied, PreconditionFailed, RecoveryRequired };
enum class RepairMemberCheckpoint { Prepare, Backup, Stage, Prepared, Intent, Quarantined, Published, Applied };
struct RepairMemberResult {
    RepairMemberState state = RepairMemberState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
};

inline JournalItem RepairJournalItem(const RepairAction& action, const std::wstring& repairId) {
    JournalItem item;
    item.target = action.member.target; item.candidateSha256 = action.member.sha256;
    item.staged = item.target + L".nr-repair-" + repairId + L".stage";
    item.previouslyPresent = action.previouslyPresent;
    if (item.previouslyPresent) {
        item.previousSha256 = HexSha256(action.previous.sha256); item.previousIdentity = JournalId(action.previous);
        item.backup = item.target; item.quarantine = item.target + L".nr-repair-" + repairId + L".original";
    }
    return item;
}
inline bool TrustedRepairCursor(const JournalCursor& cursor, const TrustedRepairPlan& plan, const std::wstring& repairId) {
    for (const auto& seen : cursor.Seen()) {
        bool found = false;
        for (const auto& action : plan.actions) if (SameJournalItem(seen, RepairJournalItem(action, repairId))) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

// This primitive does not complete the repair job. The caller must resolve the
// journal after every interruption and must scan repair jobs before other writes.
inline RepairMemberResult ApplyRepairMember(const std::wstring& clientRoot, const std::wstring& sourceOperationsRoot,
                                             const std::wstring& sourceOperation, const std::wstring& repairsRoot,
                                             const std::wstring& repairId, const std::vector<std::wstring>& protectedRoots,
                                             const CatalogResult& catalog, size_t actionIndex, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                             , ProcessCheck (*testProcesses)() = nullptr,
                                               bool (*interrupt)(RepairMemberCheckpoint) = nullptr
#endif
                                             ) {
    RepairMemberResult result;
    auto fail = [&result](const char* reason, bool recovery = false, DWORD error = ERROR_SUCCESS) {
        result.reason = reason; result.win32Error = error;
        result.state = recovery ? RepairMemberState::RecoveryRequired : RepairMemberState::PreconditionFailed;
        return result;
    };
    auto processes = [&]() {
#ifdef CONTROL_CENTER_TESTING
        return testProcesses ? testProcesses() : CheckBlockingProcesses();
#else
        return CheckBlockingProcesses();
#endif
    };
    auto stopped = [&](RepairMemberCheckpoint point) {
#ifdef CONTROL_CENTER_TESTING
        return interrupt && interrupt(point);
#else
        (void)point; return false;
#endif
    };
    const auto plan = ReadTrustedRepairPlan(clientRoot, sourceOperationsRoot, sourceOperation, repairsRoot, repairId, protectedRoots, catalog, lock);
    if (!plan.valid || actionIndex >= plan.actions.size()) return fail("repair plan invalid", false, plan.win32Error);
    const auto action = plan.actions[actionIndex];
    const auto item = RepairJournalItem(action, repairId);
    if (!ValidJournalItem(item)) return fail("invalid repair paths");
    std::wstring client, source, repairs;
    if (!CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(sourceOperation, source) || !CanonicalLocalPath(repairsRoot, repairs))
        return fail("invalid repair roots");
    const std::wstring work = repairs + L"\\" + repairId;
    GuardedPath workGuard;
    if (!workGuard.Open(work, L"__root__")) return fail("repair directory unavailable", false, workGuard.Error());
    if (!NoCommitState(work)) return fail("repair commit state requires recovery", true);
    auto journal = LoadJournal(work);
    if (!journal || !TrustedRepairCursor(journal.cursor, plan, repairId)) return fail("repair journal invalid", true, journal.win32Error);
    if (journal.cursor.Pending() || journal.cursor.Prepared() || journal.cursor.RollingBack() || journal.cursor.Terminal())
        return fail("repair journal requires recovery", true);
    for (size_t i = 0; i < journal.cursor.Applied().size(); ++i) if (SameJournalItem(journal.cursor.Applied()[i], item)) {
        if (i >= journal.cursor.AppliedIdentities().size()) return fail("applied repair identity missing", true);
        GuardedPath target;
        if (!target.Open(client, item.target)) return fail("applied repair target unavailable", true, target.Error());
        const auto current = HashFile(target.Path());
        if (!current || HexSha256(current.identity.sha256) != item.candidateSha256 ||
            !SameJournalId(JournalId(current.identity), journal.cursor.AppliedIdentities()[i]))
            return fail("applied repair target changed", true, current.win32Error);
        result.state = RepairMemberState::AlreadyApplied; return result;
    }
    auto safe = [&]() {
        if (!lock.IsFor(client) || !ValidateClient(client, protectedRoots) || processes().state != ProcessState::Stopped || !NoCommitState(work)) return false;
        const auto fresh = ReadTrustedRepairPlan(client, sourceOperationsRoot, source, repairs, repairId, protectedRoots, catalog, lock);
        return fresh.valid && SameIdentity(fresh.identity, plan.identity);
    };
    if (!safe()) return fail("client, plan or process state changed");
    GuardedPath target, staged, repairSource;
    const bool targetAvailable = target.Open(client, item.target);
    if ((!targetAvailable && (action.previouslyPresent || !MissingPathError(target.Error()))) ||
        !repairSource.Open(source, L"stage/" + item.target)) return fail("repair paths unavailable");
    const auto actualSource = HashFile(repairSource.Path());
    if (!actualSource || !SameIdentity(actualSource.identity, action.source)) return fail("repair source changed", false, actualSource.win32Error);
    if (targetAvailable) {
        const auto actualTarget = HashFile(target.Path());
        if (action.previouslyPresent) {
            if (!actualTarget || !SameIdentity(actualTarget.identity, action.previous)) return fail("repair target changed", false, actualTarget.win32Error);
        } else if (actualTarget || actualTarget.error != HashError::Open || !MissingPathError(actualTarget.win32Error))
            return fail("repair target appeared", false, actualTarget.win32Error);
    }
    auto append = [&](JournalKind kind, const FileIdentity* candidate = nullptr) {
        JournalEvent event{journal.cursor.Next(), kind, item};
        if (candidate) event.stagedIdentity = JournalId(*candidate);
        auto persisted = PersistJournalEvent(work, journal.cursor, event);
        if (!persisted) { result.win32Error = persisted.win32Error; return false; }
        journal = std::move(persisted); return true;
    };
    if (!append(JournalKind::Prepare)) return fail("cannot persist preparation", true, result.win32Error);
    if (stopped(RepairMemberCheckpoint::Prepare)) return fail("interrupted after preparation", true);
    if ((!targetAvailable && !target.Open(client, item.target, true)) || !staged.Open(client, item.staged, true))
        return fail("repair paths unavailable", true);
    if (action.previouslyPresent) {
        GuardedPath backup;
        if (!backup.Open(work, L"modified/" + item.backup, true)) return fail("modified-file backup unavailable", true, backup.Error());
        const auto copied = CopyVerifiedNew(target, backup, action.previous.sha256);
        if (!copied || !SameIdentity(copied.source, action.previous)) return fail("cannot preserve modified file", true, copied.win32Error);
    }
    if (stopped(RepairMemberCheckpoint::Backup)) return fail("interrupted after backup", true);
    const auto copied = CopyVerifiedNew(repairSource, staged, action.source.sha256);
    if (!copied || !SameIdentity(copied.source, action.source)) return fail("cannot stage repair source", true, copied.win32Error);
    if (stopped(RepairMemberCheckpoint::Stage)) return fail("interrupted after staging", true);
    if (!append(JournalKind::Prepared, &copied.target)) return fail("cannot persist staged repair", true, result.win32Error);
    if (stopped(RepairMemberCheckpoint::Prepared)) return fail("interrupted after staging checkpoint", true);
    if (!safe()) return fail("client, plan or process state changed", true);
    const auto before = HashFile(target.Path());
    if (action.previouslyPresent) {
        if (!before || !SameIdentity(before.identity, action.previous)) return fail("repair target changed", true, before.win32Error);
    } else if (before || before.error != HashError::Open || !MissingPathError(before.win32Error)) return fail("repair target appeared", true, before.win32Error);
    if (!append(JournalKind::Intent)) return fail("cannot persist repair intent", true, result.win32Error);
    if (stopped(RepairMemberCheckpoint::Intent)) return fail("interrupted after repair intent", true);
    if (!safe()) return fail("client, plan or process state changed", true);
    if (action.previouslyPresent) {
        GuardedPath original;
        if (!original.Open(client, item.quarantine)) return fail("quarantine path unavailable", true, original.Error());
        const auto moved = RenameVerifiedNew(target, original, action.previous.sha256, &action.previous);
        if (!moved) return fail("cannot isolate modified file", true, moved.win32Error);
        if (stopped(RepairMemberCheckpoint::Quarantined)) return fail("interrupted after isolation", true);
        if (!safe()) return fail("client, plan or process state changed", true);
    }
    const auto moved = RenameVerifiedNew(staged, target, action.source.sha256, &copied.target);
    if (!moved) return fail("cannot publish repair", true, moved.win32Error);
    if (stopped(RepairMemberCheckpoint::Published)) return fail("interrupted after publication", true);
    if (!append(JournalKind::Applied)) return fail("cannot persist repair result", true, result.win32Error);
    if (stopped(RepairMemberCheckpoint::Applied)) return fail("interrupted after repair checkpoint", true);
    result.state = RepairMemberState::Applied;
    return result;
}
}
