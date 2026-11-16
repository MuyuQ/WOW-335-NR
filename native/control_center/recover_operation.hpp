#pragma once

#include "delete_verified.hpp"
#include "install_member.hpp"
#include "reconcile.hpp"

namespace control_center {
enum class RecoveryState { Recovered, Incomplete, NoWork, AlreadyTerminal, PreconditionFailed, RecoveryRequired };
struct RecoveryResult {
    RecoveryState state = RecoveryState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    std::vector<JournalEvent> unresolved, discarded;
};
inline bool ParseSha256Bytes(const std::string& hex, std::array<std::uint8_t, 32>& output) {
    if (!ValidSha256(hex)) return false;
    auto nibble = [](char c) -> unsigned { return c <= '9' ? static_cast<unsigned>(c - '0') : static_cast<unsigned>(c - 'a' + 10); };
    for (size_t i = 0; i < output.size(); ++i) output[i] = static_cast<std::uint8_t>((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
    return true;
}

inline bool NoCommitState(const std::wstring& operation) {
    for (const auto* name : {L"commit.json", L"commit.pending", L"transaction.json", L"transaction.pending"}) {
        GuardedPath marker;
        if (!marker.Open(operation, name) || GetFileAttributesW(marker.Path().c_str()) != INVALID_FILE_ATTRIBUTES ||
            !MissingPathError(GetLastError())) return false;
    }
    return true;
}

// Metadata may be followed by member rollback only after its journal has a
// verified rollback terminal and the prior record is back in place.
inline bool RecordRollbackAllowsMemberRecovery(const std::wstring& operation, const std::wstring& client,
                                                const MemberInstallContext& context, const CatalogResult& catalog,
                                                const ClientLock& lock) {
    if (!NoCommitState(operation)) return false;
    const auto prior = ReadSessionPriorRecord(operation);
    if (!BindJournalSession(operation, context.operationId, lock, catalog, prior, true) ||
        !BindJournalSession(operation, context.operationId, lock, catalog, CaptureClientRecord(client), true)) return false;
    GuardedPath planPath, sessionPath;
    if (!planPath.Open(operation, L"finalization-plan.json") || !sessionPath.Open(operation, L"session.json")) return false;
    std::string bytes; DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(planPath, bytes, error)) return false;
    const auto plan = ParseJsonObject(bytes);
    if (!plan) return false;
    std::string id, digest, planId, planCatalog, planSession, candidate, candidateDigest;
    if (!WideUtf8(context.operationId, id) || !CatalogDigest(catalog, digest) ||
        !StringField(plan.value, "operation_id", planId) || planId != id ||
        !StringField(plan.value, "catalog_sha256", planCatalog) || planCatalog != digest ||
        !StringField(plan.value, "session_sha256", planSession) ||
        !StringField(plan.value, "client_record", candidate) || !ReadClientRecord(candidate) ||
        !StringField(plan.value, "client_record_sha256", candidateDigest)) return false;
    const auto sessionHash = HashFile(sessionPath.Path());
    std::array<std::uint8_t, 32> hash{};
    if (!sessionHash || HexSha256(sessionHash.identity.sha256) != planSession ||
        HashBytes(candidate, hash) < 0 || HexSha256(hash) != candidateDigest) return false;
    JournalItem expected;
    expected.target = L"host64/dlss5-install-record.json";
    expected.staged = expected.target + L".nr-" + context.operationId + L".stage";
    expected.candidateSha256 = candidateDigest;
    expected.previouslyPresent = prior.state == CapturedRecordState::Present;
    if (expected.previouslyPresent) {
        expected.previousSha256 = HexSha256(prior.identity.sha256); expected.previousIdentity = JournalId(prior.identity);
        expected.backup = expected.target; expected.quarantine = expected.target + L".nr-" + context.operationId + L".original";
    }
    if (!ValidJournalItem(expected)) return false;
    const auto recordJournal = LoadJournal(operation + L"\\record-publication");
    if (!recordJournal || !recordJournal.cursor.Terminal() || !recordJournal.cursor.Unresolved().empty() ||
        !recordJournal.cursor.Applied().empty() ||
        (recordJournal.cursor.TerminalKind() != JournalKind::Cancelled && recordJournal.cursor.TerminalKind() != JournalKind::RolledBack)) return false;
    for (const auto& item : recordJournal.cursor.Seen()) if (!SameJournalItem(item, expected)) return false;
    return true;
}

// Roll back a nonterminal sidecar journal. Staged or partially prepared files
// are retained and reported in discarded, rather than guessed to be disposable.
// Legacy transaction.json/client records require their separate recovery path.
inline RecoveryResult RecoverOperation(const MemberInstallContext& context, const CatalogResult& catalog, const ClientLock& lock) {
    RecoveryResult result;
    auto fail = [&result](std::string reason, bool recovery, DWORD error = ERROR_SUCCESS) {
        result.reason = std::move(reason); result.win32Error = error;
        result.state = recovery ? RecoveryState::RecoveryRequired : RecoveryState::PreconditionFailed;
        return result;
    };
    if (!catalog || !ValidOperationId(context.operationId) || !lock.IsFor(context.clientRoot)) return fail("invalid catalog, operation id or lock", false);
    const auto client = ValidateClient(context.clientRoot, context.protectedRoots);
    if (!client) return fail("client validation failed", false, client.win32Error);
    std::wstring journalRoot;
    if (!CanonicalLocalPath(context.journalRoot, journalRoot) || WithinRoot(journalRoot, client.root) ||
        WithinRoot(client.root, journalRoot)) return fail("journal must be outside client", false);
    GuardedPath journalGuard;
    if (!journalGuard.Open(journalRoot, L"__journal_root__")) return fail("journal root unavailable", false, journalGuard.Error());
    if (!NoCommitState(journalRoot)) return fail("commit state requires separate recovery", true);
    GuardedPath metadata;
    if (!metadata.Open(journalRoot, L"record-publication")) return fail("record publication path unavailable", true, metadata.Error());
    const bool metadataExists = GetFileAttributesW(metadata.Path().c_str()) != INVALID_FILE_ATTRIBUTES;
    if (metadataExists) {
        if (!RecordRollbackAllowsMemberRecovery(journalRoot, client.root, context, catalog, lock))
            return fail("record publication requires separate recovery", true);
    } else if (!MissingPathError(GetLastError())) return fail("record publication state unavailable", true);
    auto journal = LoadJournal(journalRoot);
    if (!journal) return fail("journal unreadable", true, journal.win32Error);
    if (journal.cursor.Next() == 1) { result.state = RecoveryState::NoWork; return result; }
    const auto priorRecord = ReadSessionPriorRecord(journalRoot);
    if (!TrustedJournalCursor(journal.cursor, catalog, context.operationId) ||
        !BindJournalSession(journalRoot, context.operationId, lock, catalog, priorRecord, true))
        return fail("journal does not match client, catalog or prior record", true);
    if (journal.cursor.Terminal()) {
        result.unresolved = journal.cursor.Unresolved(); result.discarded = journal.cursor.Discarded();
        result.state = result.unresolved.empty() ? RecoveryState::AlreadyTerminal : RecoveryState::Incomplete;
        return result;
    }
    auto safe = [&]() {
        if (!lock.IsFor(client.root) || !ValidateClient(client.root, context.protectedRoots) ||
            MemberProcesses(context).state != ProcessState::Stopped || !NoCommitState(journalRoot)) return false;
        if (GetFileAttributesW(metadata.Path().c_str()) != INVALID_FILE_ATTRIBUTES)
            return metadataExists && RecordRollbackAllowsMemberRecovery(journalRoot, client.root, context, catalog, lock);
        return !metadataExists && MissingPathError(GetLastError());
    };
    if (!safe()) return fail("client or process state prevents recovery", false);
    const std::wstring backupRoot = client.root + L"\\host64\\dlss5-backup";
    auto append = [&](JournalKind kind, const JournalItem& item, DWORD failure = ERROR_SUCCESS) {
        const JournalEvent event{journal.cursor.Next(), kind, item, failure};
        auto persisted = PersistJournalEvent(journalRoot, journal.cursor, event);
        if (!persisted) { result.win32Error = persisted.win32Error; return false; }
        journal = std::move(persisted); return true;
    };
    auto reconcile = [&]() { return ReconcilePending(client.root, backupRoot, journal.cursor); };
    auto restoreOriginal = [&](const JournalItem& item) {
        GuardedPath original, target;
        std::array<std::uint8_t, 32> expected{};
        if (!safe() || !ParseSha256Bytes(item.previousSha256, expected) ||
            !original.Open(client.root, item.quarantine) || !target.Open(client.root, item.target)) return false;
        const auto originalIdentity = FileIdFromJournal(item.previousIdentity);
        const auto restored = RenameVerifiedNew(original, target, expected, &originalIdentity);
        if (!restored) { result.win32Error = restored.win32Error; return false; }
        return true;
    };
    if (journal.cursor.Pending() && journal.cursor.PendingEvent().kind == JournalKind::Prepare) {
        const auto item = journal.cursor.PendingEvent().item;
        if (!append(JournalKind::CancelPrepare, item)) return fail("could not record cancelled preparation", true, result.win32Error);
    }
    if (!journal.cursor.Pending() && journal.cursor.Prepared()) {
        const auto item = journal.cursor.PreparedItem();
        if (!append(JournalKind::CancelPrepared, item)) return fail("could not record cancelled staging", true, result.win32Error);
    }
    if (journal.cursor.Pending() && journal.cursor.PendingEvent().kind == JournalKind::Intent) {
        const auto item = journal.cursor.PendingEvent().item;
        auto observed = reconcile();
        if (observed.decision == ReconcileDecision::RestoreQuarantinedOriginal) {
            if (!restoreOriginal(item)) return fail("could not restore isolated original", true, result.win32Error);
            observed = reconcile();
        }
        if (observed.decision == ReconcileDecision::BeforeMutation) {
            if (!append(JournalKind::CancelIntent, item)) return fail("could not record cancelled publication", true, result.win32Error);
        } else if (observed.decision == ReconcileDecision::Applied) {
            if (!append(JournalKind::Applied, item)) return fail("could not record published file", true, result.win32Error);
        } else return fail("publication state needs inspection", true, observed.win32Error);
    }
    auto rollbackPending = [&]() -> bool {
        const auto item = journal.cursor.PendingEvent().item;
        auto observed = reconcile();
        if (observed.decision == ReconcileDecision::RestoreQuarantinedOriginal) {
            if (!restoreOriginal(item)) { result.reason = "could not restore isolated original"; return false; }
            if (MemberInterrupted(context, MemberCheckpoint::RollbackOriginalRestored)) { result.reason = "interrupted after original restore"; return false; }
            observed = reconcile();
        }
        if (observed.decision == ReconcileDecision::RolledBack) {
            if (!append(JournalKind::RollbackApplied, item)) { result.reason = "could not record rollback"; return false; }
            return true;
        }
        if (observed.decision != ReconcileDecision::RollbackNotStarted) {
            const DWORD error = observed.win32Error ? observed.win32Error : ERROR_CRC;
            if (!append(JournalKind::RollbackFailed, item, error)) { result.reason = "could not record rollback conflict"; return false; }
            return true;
        }
        if (!safe()) { result.reason = "client or process state changed"; return false; }
        GuardedPath target;
        std::array<std::uint8_t, 32> candidate{};
        const auto candidateId = journal.cursor.PendingCandidateIdentity();
        if (!candidateId.known) { result.reason = "candidate identity missing"; return false; }
        const auto candidateIdentity = FileIdFromJournal(candidateId);
        if (!target.Open(client.root, item.target) || !ParseSha256Bytes(item.candidateSha256, candidate)) {
            result.reason = "target path unavailable"; result.win32Error = target.Error(); return false;
        }
        if (!item.previouslyPresent) {
            const auto deleted = DeleteVerified(target, candidate, &candidateIdentity);
            if (!deleted) {
                if (deleted.dispositionSet) { result.reason = "deletion needs reconciliation"; result.win32Error = deleted.win32Error; return false; }
                if (!append(JournalKind::RollbackFailed, item, deleted.win32Error ? deleted.win32Error : ERROR_CRC)) {
                    result.reason = "could not record deletion failure"; return false;
                }
                return true;
            }
            if (MemberInterrupted(context, MemberCheckpoint::RollbackDeleted)) { result.reason = "interrupted after deletion"; return false; }
        } else {
            GuardedPath staged;
            if (!staged.Open(client.root, item.staged)) { result.reason = "stage path unavailable"; result.win32Error = staged.Error(); return false; }
            const auto moved = RenameVerifiedNew(target, staged, candidate, &candidateIdentity);
            if (!moved) {
                if (moved.renamed) { result.reason = "candidate move needs reconciliation"; result.win32Error = moved.win32Error; return false; }
                if (!append(JournalKind::RollbackFailed, item, moved.win32Error ? moved.win32Error : ERROR_CRC)) {
                    result.reason = "could not record candidate move failure"; return false;
                }
                return true;
            }
            if (MemberInterrupted(context, MemberCheckpoint::RollbackCandidateIsolated)) { result.reason = "interrupted after candidate isolation"; return false; }
            if (!restoreOriginal(item)) { result.reason = "could not restore original"; return false; }
            if (MemberInterrupted(context, MemberCheckpoint::RollbackOriginalRestored)) { result.reason = "interrupted after original restore"; return false; }
        }
        observed = reconcile();
        if (observed.decision != ReconcileDecision::RolledBack) { result.reason = "rollback needs reconciliation"; result.win32Error = observed.win32Error; return false; }
        if (!append(JournalKind::RollbackApplied, item)) { result.reason = "could not record completed rollback"; return false; }
        if (MemberInterrupted(context, MemberCheckpoint::RollbackRecorded)) { result.reason = "interrupted after rollback checkpoint"; return false; }
        return true;
    };
    if (journal.cursor.Pending()) {
        if (journal.cursor.PendingEvent().kind != JournalKind::RollbackIntent || !rollbackPending())
            return fail(result.reason.empty() ? "unsupported pending step" : result.reason, true, result.win32Error);
    }
    while (!journal.cursor.Applied().empty()) {
        if (!safe()) return fail("client or process state changed", true);
        const auto item = journal.cursor.Applied().back();
        if (!append(JournalKind::RollbackIntent, item)) return fail("could not record rollback intent", true, result.win32Error);
        if (MemberInterrupted(context, MemberCheckpoint::RollbackIntent)) return fail("interrupted after rollback intent", true);
        if (!rollbackPending()) return fail(result.reason, true, result.win32Error);
    }
    JournalKind terminal = JournalKind::Cancelled;
    if (journal.cursor.RollingBack()) terminal = journal.cursor.Unresolved().empty() ? JournalKind::RolledBack : JournalKind::RollbackIncomplete;
    if (!append(terminal, {})) return fail("could not record recovery result", true, result.win32Error);
    result.unresolved = journal.cursor.Unresolved(); result.discarded = journal.cursor.Discarded();
    result.state = result.unresolved.empty() ? RecoveryState::Recovered : RecoveryState::Incomplete;
    return result;
}
}
