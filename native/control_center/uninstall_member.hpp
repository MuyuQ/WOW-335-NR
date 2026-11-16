#pragma once

#include "uninstall_journal.hpp"

namespace control_center {
enum class UninstallMemberState { Applied, NoWork, PreconditionFailed, RecoveryRequired };
enum class UninstallMemberCheckpoint { Prepare, Saved, Staged, Prepared, Intent, Quarantined, Published, Applied };
struct UninstallMemberResult {
    UninstallMemberState state = UninstallMemberState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
};
inline bool NoUninstallFinalState(const std::wstring& work) {
    if (!NoCommitState(work)) return false;
    for (const auto* name : {L"result.json", L"result.pending", L"record-publication"}) {
        GuardedPath path;
        if (!path.Open(work, name) || GetFileAttributesW(path.Path().c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError())) return false;
    }
    return true;
}

// This primitive does not finish an uninstall. Its caller must hold the client
// lock, coordinate all transaction types, and recover any interrupted journal.
inline UninstallMemberResult ApplyUninstallMember(const std::wstring& clientRoot, const std::wstring& uninstallsRoot,
                                                  const std::wstring& uninstallId, const std::vector<std::wstring>& protectedRoots,
                                                  const CatalogResult& catalog, const Json& manifest, size_t index, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                                  , ProcessCheck (*testProcesses)() = nullptr,
                                                    bool (*interrupt)(UninstallMemberCheckpoint) = nullptr
#endif
                                                  ) {
    UninstallMemberResult result;
    auto fail = [&result](const char* reason, bool recovery = false, DWORD error = ERROR_SUCCESS) {
        result.state = recovery ? UninstallMemberState::RecoveryRequired : UninstallMemberState::PreconditionFailed;
        result.reason = reason; result.win32Error = error; return result;
    };
    auto stopped = [&]() {
#ifdef CONTROL_CENTER_TESTING
        const auto process = testProcesses ? testProcesses() : CheckBlockingProcesses();
#else
        const auto process = CheckBlockingProcesses();
#endif
        return process.state == ProcessState::Stopped;
    };
    auto interrupted = [&](UninstallMemberCheckpoint point) {
#ifdef CONTROL_CENTER_TESTING
        return interrupt && interrupt(point);
#else
        (void)point; return false;
#endif
    };
    const auto plan = ReadTrustedUninstallPlan(clientRoot, uninstallsRoot, uninstallId, protectedRoots, catalog, manifest, lock);
    if (!plan.valid || index >= plan.items.size()) return fail("invalid uninstall plan", false, plan.win32Error);
    const auto item = plan.items[index];
    if (item.state == UninstallItemState::AlreadyAbsent || item.state == UninstallItemState::AlreadyRestored) {
        GuardedPath unchanged;
        if (!unchanged.Open(clientRoot, item.entry.target)) {
            if (item.state != UninstallItemState::AlreadyAbsent || !MissingPathError(unchanged.Error())) return fail("unchanged target unavailable", false, unchanged.Error());
        } else {
            const auto actual = HashFile(unchanged.Path());
            if (item.state == UninstallItemState::AlreadyRestored ? (!actual || !SameIdentity(actual.identity, item.target)) :
                (actual || actual.error != HashError::Open || !MissingPathError(actual.win32Error)))
                return fail("unchanged target changed", false, actual.win32Error);
        }
        result.state = UninstallMemberState::NoWork; return result;
    }
    const bool restore = item.state == UninstallItemState::RestoreOriginal;
    if (!restore && item.state != UninstallItemState::DeleteInstalled && item.state != UninstallItemState::DeleteModified)
        return fail("invalid uninstall action");
    std::wstring client, root;
    if (!CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(uninstallsRoot, root)) return fail("invalid roots");
    const std::wstring work = root + L"\\" + uninstallId;
    GuardedPath workGuard;
    if (!workGuard.Open(work, L"__root__") || !NoUninstallFinalState(work)) return fail("uninstall state unavailable", true, workGuard.Error());
    auto journal = LoadUninstallJournal(work, plan);
    if (!journal || journal.cursor.Terminal() || journal.cursor.Preparing() || journal.cursor.Prepared() || journal.cursor.IntentPending() ||
        journal.cursor.RollbackPending() || journal.cursor.RollingBack()) return fail("uninstall journal requires recovery", true, journal.win32Error);
    for (const auto& applied : journal.cursor.Applied()) if (applied.index == index) return fail("uninstall action already applied", true);
    auto safe = [&]() {
        if (!lock.IsFor(client) || !ValidateClient(client, protectedRoots) || !stopped() || !NoUninstallFinalState(work)) return false;
        const auto fresh = ReadTrustedUninstallPlan(client, root, uninstallId, protectedRoots, catalog, manifest, lock);
        return fresh.valid && SameIdentity(fresh.identity, plan.identity);
    };
    if (!safe()) return fail("client, plan or process state changed");
    GuardedPath target, backup;
    const bool targetReady = target.Open(client, item.entry.target);
    if (!targetReady && (item.targetPresent || !MissingPathError(target.Error()))) return fail("target path unavailable", false, target.Error());
    if (targetReady) {
        const auto actual = HashFile(target.Path());
        if (item.targetPresent ? (!actual || !SameIdentity(actual.identity, item.target)) :
            (actual || actual.error != HashError::Open || !MissingPathError(actual.win32Error))) return fail("target changed", false, actual.win32Error);
    }
    if (restore) {
        if (!backup.Open(client, L"host64/dlss5-backup/" + item.entry.backup)) return fail("backup unavailable", false, backup.Error());
        const auto actual = HashFile(backup.Path());
        if (!actual || !SameIdentity(actual.identity, item.backup)) return fail("backup changed", false, actual.win32Error);
    }
    auto append = [&](UninstallJournalKind kind, JournalFileId staged = {}, JournalFileId saved = {}) {
        auto next = PersistUninstallJournal(work, plan, journal.cursor, {journal.cursor.Next(), kind, index, staged, saved});
        if (!next) { result.win32Error = next.win32Error; return false; }
        journal = std::move(next); return true;
    };
    if (!append(UninstallJournalKind::Prepare)) return fail("cannot persist preparation", true, result.win32Error);
    if (interrupted(UninstallMemberCheckpoint::Prepare)) return fail("interrupted after preparation", true);
    if (!targetReady && !target.Open(client, item.entry.target, true)) return fail("target parent unavailable", true, target.Error());
    JournalFileId savedId{}, stagedId{};
    struct HeldFile { HANDLE value = INVALID_HANDLE_VALUE; ~HeldFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } savedHandle;
    GuardedPath saved;
    if (item.saveCurrent) {
        if (!saved.Open(work, L"modified/" + item.entry.target, true)) return fail("modified file backup unavailable", true, saved.Error());
        const auto copied = CopyVerifiedNew(target, saved, item.target.sha256);
        if (!copied || !SameIdentity(copied.source, item.target)) return fail("cannot save modified file", true, copied.win32Error);
        savedHandle.value = CreateFileW(saved.Path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (savedHandle.value == INVALID_HANDLE_VALUE) return fail("saved file unavailable", true, GetLastError());
        const auto held = HashOpenFile(savedHandle.value);
        if (!held || !SameIdentity(held.identity, copied.target)) return fail("saved file changed", true, held.win32Error);
        savedId = JournalId(copied.target);
    }
    if (interrupted(UninstallMemberCheckpoint::Saved)) return fail("interrupted after saving", true);
    GuardedPath stage;
    if (restore) {
        if (!stage.Open(client, item.entry.target + L".nr-uninstall-" + uninstallId + L".stage", true))
            return fail("stage unavailable", true, stage.Error());
        const auto copied = CopyVerifiedNew(backup, stage, item.backup.sha256);
        if (!copied || !SameIdentity(copied.source, item.backup)) return fail("cannot stage original", true, copied.win32Error);
        stagedId = JournalId(copied.target);
    }
    if (interrupted(UninstallMemberCheckpoint::Staged)) return fail("interrupted after staging", true);
    if (!append(UninstallJournalKind::Prepared, stagedId, savedId)) return fail("cannot persist prepared state", true, result.win32Error);
    if (interrupted(UninstallMemberCheckpoint::Prepared)) return fail("interrupted after prepared state", true);
    if (!safe()) return fail("client, plan or process state changed", true);
    if (!append(UninstallJournalKind::Intent)) return fail("cannot persist intent", true, result.win32Error);
    if (interrupted(UninstallMemberCheckpoint::Intent)) return fail("interrupted after intent", true);
    if (!safe()) return fail("client, plan or process state changed", true);
    if (item.targetPresent) {
        GuardedPath isolated;
        if (!isolated.Open(client, item.entry.target + L".nr-uninstall-" + uninstallId + L".original"))
            return fail("isolation path unavailable", true, isolated.Error());
        const auto moved = RenameVerifiedNew(target, isolated, item.target.sha256, &item.target);
        if (!moved) return fail("cannot isolate installed file", true, moved.win32Error);
    } else {
        const auto actual = HashFile(target.Path());
        if (actual || actual.error != HashError::Open || !MissingPathError(actual.win32Error)) return fail("target appeared", true, actual.win32Error);
    }
    if (interrupted(UninstallMemberCheckpoint::Quarantined)) return fail("interrupted after isolation", true);
    if (!safe()) return fail("client, plan or process state changed", true);
    if (restore) {
        const auto stageIdentity = FileIdFromJournal(stagedId);
        const auto moved = RenameVerifiedNew(stage, target, item.backup.sha256, &stageIdentity);
        if (!moved) return fail("cannot publish original", true, moved.win32Error);
    }
    if (interrupted(UninstallMemberCheckpoint::Published)) return fail("interrupted after publication", true);
    if (!append(UninstallJournalKind::Applied)) return fail("cannot persist member result", true, result.win32Error);
    if (interrupted(UninstallMemberCheckpoint::Applied)) return fail("interrupted after member result", true);
    result.state = UninstallMemberState::Applied; return result;
}
}
