#pragma once

#include "repair_member.hpp"

namespace control_center {
enum class RepairRecoveryState { Recovered, NoWork, AlreadyTerminal, Completed, Incomplete, RecoveryRequired };
struct RepairRecoveryResult {
    RepairRecoveryState state = RepairRecoveryState::RecoveryRequired;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    std::vector<JournalEvent> unresolved, discarded;
};

inline RepairRecoveryResult RecoverRepair(const std::wstring& clientRoot, const std::wstring& sourceOperationsRoot,
                                           const std::wstring& sourceOperation, const std::wstring& repairsRoot,
                                           const std::wstring& repairId, const std::vector<std::wstring>& protectedRoots,
                                           const CatalogResult& catalog, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                           , ProcessCheck (*testProcesses)() = nullptr
#endif
                                           ) {
    RepairRecoveryResult result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) {
        result.reason = reason; result.win32Error = error; return result;
    };
    const auto plan = ReadTrustedRepairPlan(clientRoot, sourceOperationsRoot, sourceOperation, repairsRoot, repairId, protectedRoots, catalog, lock);
    if (!plan.valid) return fail("repair plan invalid", plan.win32Error);
    std::wstring client, source, repairs;
    if (!CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(sourceOperation, source) || !CanonicalLocalPath(repairsRoot, repairs))
        return fail("invalid roots");
    const std::wstring work = repairs + L"\\" + repairId;
    GuardedPath root;
    if (!root.Open(work, L"__root__")) return fail("repair root unavailable", root.Error());
    if (!NoCommitState(work)) return fail("repair commit state requires separate recovery");
    auto journal = LoadJournal(work);
    if (!journal || !TrustedRepairCursor(journal.cursor, plan, repairId)) return fail("repair journal invalid", journal.win32Error);
    if (journal.cursor.Terminal()) {
        result.unresolved = journal.cursor.Unresolved(); result.discarded = journal.cursor.Discarded();
        if (journal.cursor.TerminalKind() == JournalKind::Complete) result.state = RepairRecoveryState::Completed;
        else if (!result.unresolved.empty()) result.state = RepairRecoveryState::Incomplete;
        else result.state = RepairRecoveryState::AlreadyTerminal;
        return result;
    }
    auto safe = [&]() {
        if (!lock.IsFor(client) || !ValidateClient(client, protectedRoots) || !NoCommitState(work)) return false;
#ifdef CONTROL_CENTER_TESTING
        const auto process = testProcesses ? testProcesses() : CheckBlockingProcesses();
#else
        const auto process = CheckBlockingProcesses();
#endif
        if (process.state != ProcessState::Stopped) return false;
        const auto fresh = ReadTrustedRepairPlan(client, sourceOperationsRoot, source, repairs, repairId, protectedRoots, catalog, lock);
        return fresh.valid && SameIdentity(fresh.identity, plan.identity);
    };
    if (!safe()) return fail("client, plan or process state prevents recovery");
    if (journal.cursor.Next() == 1) {
        const auto cancelled = PersistJournalEvent(work, journal.cursor, {journal.cursor.Next(), JournalKind::Cancelled, {}});
        if (!cancelled) return fail("cannot record empty repair cancellation", cancelled.win32Error);
        result.state = RepairRecoveryState::Recovered; return result;
    }
    auto append = [&](JournalKind kind, const JournalItem& item, DWORD failure = ERROR_SUCCESS) {
        auto next = PersistJournalEvent(work, journal.cursor, {journal.cursor.Next(), kind, item, failure});
        if (!next) { result.win32Error = next.win32Error; return false; }
        journal = std::move(next); return true;
    };
    auto reconcile = [&]() { return ReconcilePending(client, work + L"\\modified", journal.cursor); };
    auto restore = [&](const JournalItem& item) {
        GuardedPath old, target; std::array<std::uint8_t, 32> digest{};
        if (!safe() || !ParseSha256Bytes(item.previousSha256, digest) || !old.Open(client, item.quarantine) || !target.Open(client, item.target)) return false;
        const auto identity = FileIdFromJournal(item.previousIdentity);
        const auto moved = RenameVerifiedNew(old, target, digest, &identity);
        if (!moved) { result.win32Error = moved.win32Error; return false; }
        return true;
    };
    if (journal.cursor.Pending() && journal.cursor.PendingEvent().kind == JournalKind::Prepare) {
        if (!append(JournalKind::CancelPrepare, journal.cursor.PendingEvent().item)) return fail("cannot cancel preparation", result.win32Error);
    }
    if (!journal.cursor.Pending() && journal.cursor.Prepared()) {
        if (!append(JournalKind::CancelPrepared, journal.cursor.PreparedItem())) return fail("cannot cancel staging", result.win32Error);
    }
    if (journal.cursor.Pending() && journal.cursor.PendingEvent().kind == JournalKind::Intent) {
        const auto item = journal.cursor.PendingEvent().item;
        auto observed = reconcile();
        if (observed.decision == ReconcileDecision::RestoreQuarantinedOriginal) {
            if (!restore(item)) return fail("cannot restore isolated file", result.win32Error);
            observed = reconcile();
        }
        if (observed.decision == ReconcileDecision::BeforeMutation) {
            if (!append(JournalKind::CancelIntent, item)) return fail("cannot cancel publication", result.win32Error);
        } else if (observed.decision == ReconcileDecision::Applied) {
            if (!append(JournalKind::Applied, item)) return fail("cannot record publication", result.win32Error);
        } else return fail("publication requires inspection", observed.win32Error);
    }
    auto rollbackPending = [&]() {
        const auto item = journal.cursor.PendingEvent().item;
        auto observed = reconcile();
        if (observed.decision == ReconcileDecision::RestoreQuarantinedOriginal) {
            if (!restore(item)) return false;
            observed = reconcile();
        }
        if (observed.decision == ReconcileDecision::RolledBack) return append(JournalKind::RollbackApplied, item);
        if (observed.decision != ReconcileDecision::RollbackNotStarted)
            return append(JournalKind::RollbackFailed, item, observed.win32Error ? observed.win32Error : ERROR_CRC);
        if (!safe()) return false;
        GuardedPath target; std::array<std::uint8_t, 32> candidate{};
        const auto id = journal.cursor.PendingCandidateIdentity();
        if (!id.known || !target.Open(client, item.target) || !ParseSha256Bytes(item.candidateSha256, candidate)) return false;
        const auto identity = FileIdFromJournal(id);
        if (!item.previouslyPresent) {
            const auto deleted = DeleteVerified(target, candidate, &identity);
            if (!deleted) {
                if (deleted.dispositionSet) return false;
                return append(JournalKind::RollbackFailed, item, deleted.win32Error ? deleted.win32Error : ERROR_CRC);
            }
        } else {
            GuardedPath staged;
            if (!staged.Open(client, item.staged)) return false;
            const auto moved = RenameVerifiedNew(target, staged, candidate, &identity);
            if (!moved) {
                if (moved.renamed) return false;
                return append(JournalKind::RollbackFailed, item, moved.win32Error ? moved.win32Error : ERROR_CRC);
            }
            if (!restore(item)) return false;
        }
        observed = reconcile();
        return observed.decision == ReconcileDecision::RolledBack && append(JournalKind::RollbackApplied, item);
    };
    if (journal.cursor.Pending() && (journal.cursor.PendingEvent().kind != JournalKind::RollbackIntent || !rollbackPending()))
        return fail("pending repair rollback requires inspection", result.win32Error);
    while (!journal.cursor.Applied().empty()) {
        if (!safe()) return fail("client, plan or process state changed");
        const auto item = journal.cursor.Applied().back();
        if (!append(JournalKind::RollbackIntent, item) || !rollbackPending()) return fail("repair rollback requires inspection", result.win32Error);
    }
    const auto terminal = journal.cursor.RollingBack() ?
        (journal.cursor.Unresolved().empty() ? JournalKind::RolledBack : JournalKind::RollbackIncomplete) : JournalKind::Cancelled;
    if (!append(terminal, {})) return fail("cannot persist repair recovery", result.win32Error);
    result.unresolved = journal.cursor.Unresolved(); result.discarded = journal.cursor.Discarded();
    result.state = result.unresolved.empty() ? RepairRecoveryState::Recovered : RepairRecoveryState::Incomplete;
    return result;
}
}
