#pragma once

#include "recover_uninstall.hpp"

namespace control_center {
enum class UninstallFinalizePrepareCheckpoint { SnapshotPrepared, SnapshotPublished, PlanPrepared, PlanPublished };
struct UninstallFinalizePrepareResult {
    bool ready = false;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    FileIdentity snapshot{}, plan{};
};

inline bool UninstallPreparationDecisionState(const std::wstring& work, const std::string* expected) {
    if (!expected) return NoUninstallFinalState(work);
    GuardedPath pending, published;
    if (!pending.Open(work, L"commit.pending") || !published.Open(work, L"commit.json")) return false;
    auto presence = [](const GuardedPath& path) -> int {
        if (GetFileAttributesW(path.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 1;
        return MissingPathError(GetLastError()) ? 0 : -1;
    };
    const int hasPending = presence(pending), hasPublished = presence(published);
    if (hasPending < 0 || hasPublished < 0 || hasPending + hasPublished != 1) return false;
    for (const auto* name : {L"transaction.json", L"transaction.pending", L"result.json", L"result.pending", L"record-publication"}) {
        GuardedPath path;
        if (!path.Open(work, name) || presence(path) != 0) return false;
    }
    const auto& selected = hasPending ? pending : published;
    const auto hash = HashFile(selected.Path()); std::string bytes; DWORD error = ERROR_SUCCESS; std::array<std::uint8_t, 32> digest{};
    return hash && ReadJournalBytes(selected, bytes, error) && bytes == *expected && HashBytes(bytes, digest) >= 0 && digest == hash.identity.sha256;
}

// Preparation only: no commit decision is recorded and the client record is
// not changed. The decision path is used only to revalidate an exact marker.
inline UninstallFinalizePrepareResult PrepareUninstallFinalizationImpl(const std::wstring& clientRoot, const std::wstring& uninstallsRoot,
                                                                        const std::wstring& uninstallId, const std::vector<std::wstring>& protectedRoots,
                                                                        const CatalogResult& catalog, const Json& manifest, const ClientLock& lock,
                                                                        const std::string* expectedDecision
#ifdef CONTROL_CENTER_TESTING
                                                                    , ProcessCheck (*testProcesses)() = nullptr,
                                                                      bool (*interrupt)(UninstallFinalizePrepareCheckpoint) = nullptr
#endif
                                                                    ) {
    UninstallFinalizePrepareResult result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) { result.reason = reason; result.win32Error = error; return result; };
    auto interrupted = [&](UninstallFinalizePrepareCheckpoint point) {
#ifdef CONTROL_CENTER_TESTING
        return interrupt && interrupt(point);
#else
        (void)point; return false;
#endif
    };
    const auto uninstall = ReadTrustedUninstallPlan(clientRoot, uninstallsRoot, uninstallId, protectedRoots, catalog, manifest, lock);
    if (!uninstall.valid) return fail("uninstall plan invalid", uninstall.win32Error);
    std::wstring client, root;
    if (!CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(uninstallsRoot, root)) return fail("invalid roots");
    const std::wstring work = root + L"\\" + uninstallId;
    GuardedPath workGuard, recordPath, snapshot, snapshotPending, finalPlan, finalPlanPending;
    if (!workGuard.Open(work, L"__root__") || !recordPath.Open(client, L"host64/dlss5-install-record.json") ||
        !snapshot.Open(work, L"record.snapshot") || !snapshotPending.Open(work, L"record.snapshot.pending") ||
        !finalPlan.Open(work, L"finalization-plan.json") || !finalPlanPending.Open(work, L"finalization-plan.pending"))
        return fail("finalization path unavailable");
    const auto journal = LoadUninstallJournal(work, uninstall);
    if (!journal || journal.cursor.Terminal() || journal.cursor.Preparing() || journal.cursor.Prepared() || journal.cursor.IntentPending() ||
        journal.cursor.RollbackPending() || journal.cursor.RollingBack()) return fail("uninstall journal is not ready", journal.win32Error);
    const auto originalRecord = CaptureClientRecord(client);
    if (originalRecord.state != CapturedRecordState::Present) return fail("client record unavailable", originalRecord.win32Error);
    auto safe = [&]() {
#ifdef CONTROL_CENTER_TESTING
        const auto process = testProcesses ? testProcesses() : CheckBlockingProcesses();
#else
        const auto process = CheckBlockingProcesses();
#endif
        if (!lock.IsFor(client) || !ValidateClient(client, protectedRoots) || process.state != ProcessState::Stopped ||
            !UninstallPreparationDecisionState(work, expectedDecision)) return false;
        const auto fresh = ReadTrustedUninstallPlan(client, root, uninstallId, protectedRoots, catalog, manifest, lock);
        if (!fresh.valid || !SameIdentity(fresh.identity, uninstall.identity)) return false;
        const auto record = CaptureClientRecord(client);
        if (record.state != CapturedRecordState::Present || !SameIdentity(record.identity, originalRecord.identity)) return false;
        const auto latest = LoadUninstallJournal(work, fresh);
        if (!latest || latest.cursor.Next() != journal.cursor.Next() || latest.cursor.Terminal() || latest.cursor.Preparing() ||
            latest.cursor.Prepared() || latest.cursor.IntentPending() || latest.cursor.RollbackPending() || latest.cursor.RollingBack()) return false;
        size_t position = 0;
        for (size_t i = 0; i < fresh.items.size(); ++i) {
            const auto& item = fresh.items[i];
            const auto target = ObserveUninstallFile(client, item.entry.target);
            if (item.state == UninstallItemState::AlreadyAbsent) { if (!target.missing) return false; continue; }
            if (item.state == UninstallItemState::AlreadyRestored) { if (!UninstallObservedMatches(target, item.target)) return false; continue; }
            if (position >= latest.cursor.Applied().size() || latest.cursor.Applied()[position].index != i) return false;
            const auto& applied = latest.cursor.Applied()[position++];
            const auto isolated = ObserveUninstallFile(client, item.entry.target + L".nr-uninstall-" + uninstallId + L".original");
            if (item.targetPresent ? !UninstallObservedMatches(isolated, item.target) : !isolated.missing) return false;
            if (item.state == UninstallItemState::RestoreOriginal) {
                if (!applied.staged.known) return false;
                auto candidate = FileIdFromJournal(applied.staged); candidate.sha256 = item.backup.sha256;
                const auto staged = ObserveUninstallFile(client, item.entry.target + L".nr-uninstall-" + uninstallId + L".stage");
                if (!UninstallObservedMatches(target, candidate) || !staged.missing) return false;
            } else if (!target.missing || applied.staged.known) return false;
            if (item.saveCurrent) {
                const auto saved = ObserveUninstallFile(work, L"modified/" + item.entry.target);
                if (!applied.saved.known || !saved.valid || !SameJournalId(JournalId(saved.identity), applied.saved) || saved.identity.sha256 != item.target.sha256)
                    return false;
            } else if (applied.saved.known) return false;
        }
        return position == latest.cursor.Applied().size();
    };
    if (!safe()) return fail("members or client record changed");
    auto presence = [](const GuardedPath& path) -> int {
        if (GetFileAttributesW(path.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 1;
        return MissingPathError(GetLastError()) ? 0 : -1;
    };
    auto readExact = [](const GuardedPath& path, const std::string& expected, FileIdentity& identity, DWORD& error) {
        const auto hash = HashFile(path.Path()); std::string bytes; std::array<std::uint8_t, 32> digest{};
        if (!hash || !ReadJournalBytes(path, bytes, error) || bytes != expected || HashBytes(bytes, digest) < 0 || digest != hash.identity.sha256) return false;
        identity = hash.identity; return true;
    };
    DWORD error = ERROR_SUCCESS;
    int hasSnapshot = presence(snapshot), hasPending = presence(snapshotPending);
    if (hasSnapshot < 0 || hasPending < 0 || (hasSnapshot && hasPending)) return fail("snapshot state conflict");
    FileIdentity snapshotIdentity;
    if (hasSnapshot) {
        if (!readExact(snapshot, originalRecord.bytes, snapshotIdentity, error)) return fail("snapshot changed", error);
    } else {
        if (hasPending) {
            if (!readExact(snapshotPending, originalRecord.bytes, snapshotIdentity, error)) return fail("pending snapshot changed", error);
        } else {
            const auto copied = CopyVerifiedNew(recordPath, snapshotPending, originalRecord.identity.sha256);
            if (!copied || !SameIdentity(copied.source, originalRecord.identity)) return fail("cannot stage client record snapshot", copied.win32Error);
            snapshotIdentity = copied.target;
        }
        if (interrupted(UninstallFinalizePrepareCheckpoint::SnapshotPrepared) || !safe()) return fail("snapshot preparation requires retry");
        const auto published = RenameVerifiedNew(snapshotPending, snapshot, snapshotIdentity.sha256, &snapshotIdentity);
        if (!published) return fail("cannot publish client record snapshot", published.win32Error);
        snapshotIdentity = published.identity;
        if (interrupted(UninstallFinalizePrepareCheckpoint::SnapshotPublished)) return fail("snapshot publication requires retry");
    }
    std::string id;
    if (!WideUtf8(uninstallId, id)) return fail("invalid uninstall id");
    const Json metadata = {{"schema", 1}, {"kind", "uninstall_finalization"}, {"uninstall_id", id},
        {"uninstall_plan_sha256", HexSha256(uninstall.identity.sha256)}, {"uninstall_plan_identity", JournalIdJson(JournalId(uninstall.identity))},
        {"client_record_sha256", HexSha256(originalRecord.identity.sha256)}, {"snapshot_identity", JournalIdJson(JournalId(snapshotIdentity))},
        {"journal_next", journal.cursor.Next()}};
    const std::string bytes = metadata.dump() + "\n";
    const int hasPlan = presence(finalPlan), hasPlanPending = presence(finalPlanPending);
    if (hasPlan < 0 || hasPlanPending < 0 || (hasPlan && hasPlanPending)) return fail("finalization plan state conflict");
    FileIdentity planIdentity;
    if (hasPlan) {
        if (!readExact(finalPlan, bytes, planIdentity, error)) return fail("finalization plan changed", error);
    } else {
        if (hasPlanPending) {
            if (!readExact(finalPlanPending, bytes, planIdentity, error)) return fail("pending finalization plan changed", error);
        } else {
            const auto written = WriteNewVerified(finalPlanPending, bytes);
            if (!written) return fail("cannot stage finalization plan", written.win32Error);
            planIdentity = written.identity;
        }
        if (interrupted(UninstallFinalizePrepareCheckpoint::PlanPrepared) || !safe()) return fail("finalization plan requires retry");
        const auto published = RenameVerifiedNew(finalPlanPending, finalPlan, planIdentity.sha256, &planIdentity);
        if (!published) return fail("cannot publish finalization plan", published.win32Error);
        planIdentity = published.identity;
        if (interrupted(UninstallFinalizePrepareCheckpoint::PlanPublished)) return fail("finalization plan publication requires retry");
    }
    FileIdentity checkSnapshot, checkPlan;
    if (!readExact(snapshot, originalRecord.bytes, checkSnapshot, error) || !SameIdentity(checkSnapshot, snapshotIdentity) ||
        !readExact(finalPlan, bytes, checkPlan, error) || !SameIdentity(checkPlan, planIdentity) || !safe()) return fail("finalization preparation changed", error);
    result.ready = true; result.snapshot = snapshotIdentity; result.plan = planIdentity; return result;
}
inline UninstallFinalizePrepareResult PrepareUninstallFinalization(const std::wstring& clientRoot, const std::wstring& uninstallsRoot,
                                                                    const std::wstring& uninstallId, const std::vector<std::wstring>& protectedRoots,
                                                                    const CatalogResult& catalog, const Json& manifest, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                                                    , ProcessCheck (*testProcesses)() = nullptr,
                                                                      bool (*interrupt)(UninstallFinalizePrepareCheckpoint) = nullptr
#endif
                                                                    ) {
    return PrepareUninstallFinalizationImpl(clientRoot, uninstallsRoot, uninstallId, protectedRoots, catalog, manifest, lock, nullptr
#ifdef CONTROL_CENTER_TESTING
                                            , testProcesses, interrupt
#endif
                                            );
}
}
