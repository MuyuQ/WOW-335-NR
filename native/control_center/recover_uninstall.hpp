#pragma once

#include "uninstall_member.hpp"

namespace control_center {
enum class UninstallRecoveryState { Recovered, AlreadyTerminal, Incomplete, RecoveryRequired };
enum class UninstallRecoveryCheckpoint { RollbackIntent, CandidateIsolated, OriginalRestored, RollbackApplied, IntentConflict };
struct UninstallRecoveryResult {
    UninstallRecoveryState state = UninstallRecoveryState::RecoveryRequired;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    std::vector<UninstallJournalEvent> unresolved;
};
struct UninstallObservedFile { bool missing = false, valid = false; FileIdentity identity{}; DWORD error = ERROR_SUCCESS; };
inline UninstallObservedFile ObserveUninstallFile(const std::wstring& root, const std::wstring& relative) {
    UninstallObservedFile result; GuardedPath path;
    if (!path.Open(root, relative)) {
        result.error = path.Error(); result.missing = MissingPathError(result.error); return result;
    }
    const auto hash = HashFile(path.Path());
    if (!hash) { result.error = hash.win32Error; result.missing = hash.error == HashError::Open && MissingPathError(hash.win32Error); return result; }
    result.valid = true; result.identity = hash.identity; return result;
}
inline bool UninstallObservedMatches(const UninstallObservedFile& observed, const FileIdentity& expected) {
    return observed.valid && SameIdentity(observed.identity, expected);
}

// No commit/finalization is attempted here. Unrecognized file states are
// preserved; rollback conflicts are recorded and remaining members continue.
inline UninstallRecoveryResult RecoverUninstall(const std::wstring& clientRoot, const std::wstring& uninstallsRoot,
                                                 const std::wstring& uninstallId, const std::vector<std::wstring>& protectedRoots,
                                                 const CatalogResult& catalog, const Json& manifest, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                                 , ProcessCheck (*testProcesses)() = nullptr,
                                                   bool (*interrupt)(UninstallRecoveryCheckpoint) = nullptr
#endif
                                                 ) {
    UninstallRecoveryResult result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) { result.reason = reason; result.win32Error = error; return result; };
    auto interrupted = [&](UninstallRecoveryCheckpoint point) {
#ifdef CONTROL_CENTER_TESTING
        return interrupt && interrupt(point);
#else
        (void)point; return false;
#endif
    };
    const auto plan = ReadTrustedUninstallPlan(clientRoot, uninstallsRoot, uninstallId, protectedRoots, catalog, manifest, lock);
    if (!plan.valid) return fail("invalid uninstall plan", plan.win32Error);
    std::wstring client, root;
    if (!CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(uninstallsRoot, root)) return fail("invalid roots");
    const std::wstring work = root + L"\\" + uninstallId;
    GuardedPath rootGuard;
    if (!rootGuard.Open(work, L"__root__") || !NoUninstallFinalState(work)) return fail("uninstall commit state requires inspection", rootGuard.Error());
    auto journal = LoadUninstallJournal(work, plan);
    if (!journal) return fail("invalid uninstall journal", journal.win32Error);
    if (journal.cursor.Terminal()) {
        result.unresolved = journal.cursor.Unresolved();
        result.state = result.unresolved.empty() ? UninstallRecoveryState::AlreadyTerminal : UninstallRecoveryState::Incomplete;
        return result;
    }
    auto safe = [&]() {
#ifdef CONTROL_CENTER_TESTING
        const auto process = testProcesses ? testProcesses() : CheckBlockingProcesses();
#else
        const auto process = CheckBlockingProcesses();
#endif
        if (!lock.IsFor(client) || !ValidateClient(client, protectedRoots) || process.state != ProcessState::Stopped || !NoUninstallFinalState(work)) return false;
        const auto fresh = ReadTrustedUninstallPlan(client, root, uninstallId, protectedRoots, catalog, manifest, lock);
        return fresh.valid && SameIdentity(fresh.identity, plan.identity);
    };
    if (!safe()) return fail("client, plan or process state prevents recovery");
    auto append = [&](UninstallJournalKind kind, size_t index = 0, DWORD failure = ERROR_SUCCESS) {
        const auto next = PersistUninstallJournal(work, plan, journal.cursor, {journal.cursor.Next(), kind, index, {}, {}, failure});
        if (!next) { result.win32Error = next.win32Error; return false; }
        journal = next; return true;
    };
    bool moveWithoutMutation = false;
    auto move = [&](const std::wstring& from, const std::wstring& to, const FileIdentity& expected) {
        moveWithoutMutation = false;
        if (!safe()) return false;
        GuardedPath source, target;
        if (!source.Open(client, from) || !target.Open(client, to)) {
            moveWithoutMutation = true; result.win32Error = source.Error() ? source.Error() : target.Error(); return false;
        }
        const auto moved = RenameVerifiedNew(source, target, expected.sha256, &expected);
        if (!moved) { moveWithoutMutation = !moved.renamed; result.win32Error = moved.win32Error; return false; }
        return true;
    };
    auto savedMatches = [&](const TrustedUninstallItem& item, const JournalFileId& saved) {
        if (!item.saveCurrent) return !saved.known;
        if (!saved.known) return false;
        const auto actual = ObserveUninstallFile(work, L"modified/" + item.entry.target);
        return actual.valid && SameJournalId(JournalId(actual.identity), saved) && actual.identity.sha256 == item.target.sha256;
    };
    auto stageIdentity = [](const TrustedUninstallItem& item, const JournalFileId& staged) {
        auto identity = FileIdFromJournal(staged); identity.sha256 = item.backup.sha256; return identity;
    };
    if (journal.cursor.Preparing() && !append(UninstallJournalKind::CancelPrepare, journal.cursor.Current()))
        return fail("cannot cancel preparation", result.win32Error);
    if (journal.cursor.Prepared() && !append(UninstallJournalKind::CancelPrepared, journal.cursor.Current()))
        return fail("cannot cancel staging", result.win32Error);
    if (journal.cursor.IntentPending()) {
        const size_t index = journal.cursor.Current(); const auto& item = plan.items[index];
        const bool restore = item.state == UninstallItemState::RestoreOriginal;
        const bool savedValid = savedMatches(item, journal.cursor.Saved());
        const auto target = ObserveUninstallFile(client, item.entry.target);
        const auto isolated = ObserveUninstallFile(client, item.entry.target + L".nr-uninstall-" + uninstallId + L".original");
        const auto staged = restore ? ObserveUninstallFile(client, item.entry.target + L".nr-uninstall-" + uninstallId + L".stage") : UninstallObservedFile{};
        const auto candidate = stageIdentity(item, journal.cursor.Staged());
        const bool stageReady = !restore || (journal.cursor.Staged().known && UninstallObservedMatches(staged, candidate));
        const bool originalAtTarget = item.targetPresent ? UninstallObservedMatches(target, item.target) : target.missing;
        if (!savedValid) {
            if (!append(UninstallJournalKind::IntentConflict, index, ERROR_CRC)) return fail("cannot record intent conflict", result.win32Error);
            if (interrupted(UninstallRecoveryCheckpoint::IntentConflict)) return fail("interrupted after intent conflict");
        } else if (originalAtTarget && isolated.missing && stageReady) {
            if (!append(UninstallJournalKind::CancelIntent, index)) return fail("cannot cancel intent", result.win32Error);
        } else if (item.targetPresent && target.missing && UninstallObservedMatches(isolated, item.target) && stageReady) {
            if (!move(item.entry.target + L".nr-uninstall-" + uninstallId + L".original", item.entry.target, item.target) ||
                !append(UninstallJournalKind::CancelIntent, index)) return fail("cannot restore isolated file", result.win32Error);
        } else {
            const bool after = (item.targetPresent ? UninstallObservedMatches(isolated, item.target) : isolated.missing) &&
                (restore ? (journal.cursor.Staged().known && UninstallObservedMatches(target, candidate) && staged.missing) : target.missing);
            if (after) {
                if (!append(UninstallJournalKind::Applied, index)) return fail("cannot record applied intent", result.win32Error);
            } else {
                if (!append(UninstallJournalKind::IntentConflict, index, ERROR_CRC)) return fail("cannot record intent conflict", result.win32Error);
                if (interrupted(UninstallRecoveryCheckpoint::IntentConflict)) return fail("interrupted after intent conflict");
            }
        }
    }
    auto rollbackPending = [&]() {
        const size_t index = journal.cursor.Current(); const auto& item = plan.items[index];
        auto failedMove = [&]() {
            if (!moveWithoutMutation || !safe()) return false;
            return append(UninstallJournalKind::RollbackFailed, index, result.win32Error ? result.win32Error : ERROR_CRC);
        };
        const auto stagedId = journal.cursor.PendingRollbackStaged(), savedId = journal.cursor.PendingRollbackSaved();
        if (!savedMatches(item, savedId)) return append(UninstallJournalKind::RollbackFailed, index, ERROR_CRC);
        const bool restore = item.state == UninstallItemState::RestoreOriginal;
        const auto candidate = stageIdentity(item, stagedId);
        auto target = ObserveUninstallFile(client, item.entry.target);
        auto isolated = ObserveUninstallFile(client, item.entry.target + L".nr-uninstall-" + uninstallId + L".original");
        auto staged = restore ? ObserveUninstallFile(client, item.entry.target + L".nr-uninstall-" + uninstallId + L".stage") : UninstallObservedFile{};
        if (restore && stagedId.known && UninstallObservedMatches(target, candidate) && staged.missing) {
            if (!move(item.entry.target, item.entry.target + L".nr-uninstall-" + uninstallId + L".stage", candidate)) return failedMove();
            if (interrupted(UninstallRecoveryCheckpoint::CandidateIsolated)) return false;
            target = ObserveUninstallFile(client, item.entry.target);
            staged = ObserveUninstallFile(client, item.entry.target + L".nr-uninstall-" + uninstallId + L".stage");
        }
        if (item.targetPresent && target.missing && UninstallObservedMatches(isolated, item.target) &&
            (!restore || (stagedId.known && UninstallObservedMatches(staged, candidate)))) {
            if (!move(item.entry.target + L".nr-uninstall-" + uninstallId + L".original", item.entry.target, item.target)) return failedMove();
            if (interrupted(UninstallRecoveryCheckpoint::OriginalRestored)) return false;
            target = ObserveUninstallFile(client, item.entry.target);
            isolated = ObserveUninstallFile(client, item.entry.target + L".nr-uninstall-" + uninstallId + L".original");
        }
        const bool restored = (item.targetPresent ? UninstallObservedMatches(target, item.target) : target.missing) && isolated.missing &&
            (!restore || (stagedId.known && UninstallObservedMatches(staged, candidate)));
        if (!restored) return append(UninstallJournalKind::RollbackFailed, index, ERROR_CRC);
        return append(UninstallJournalKind::RollbackApplied, index) && !interrupted(UninstallRecoveryCheckpoint::RollbackApplied);
    };
    if (journal.cursor.RollbackPending() && !rollbackPending()) return fail("pending rollback requires inspection", result.win32Error);
    while (!journal.cursor.Applied().empty()) {
        const size_t index = journal.cursor.Applied().back().index;
        if (!append(UninstallJournalKind::RollbackIntent, index) || interrupted(UninstallRecoveryCheckpoint::RollbackIntent) || !rollbackPending())
            return fail("rollback requires inspection", result.win32Error);
    }
    if (!safe()) return fail("client, plan or process state changed during recovery");
    const auto terminal = journal.cursor.RollingBack() ?
        (journal.cursor.Unresolved().empty() ? UninstallJournalKind::RolledBack : UninstallJournalKind::RollbackIncomplete) : UninstallJournalKind::Cancelled;
    if (!append(terminal)) return fail("cannot persist recovery result", result.win32Error);
    result.unresolved = journal.cursor.Unresolved();
    result.state = result.unresolved.empty() ? UninstallRecoveryState::Recovered : UninstallRecoveryState::Incomplete;
    return result;
}
}
