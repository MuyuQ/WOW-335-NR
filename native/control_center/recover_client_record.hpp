#pragma once

#include "publish_client_record.hpp"
#include "recover_operation.hpp"

namespace control_center {
enum class RecordRecoveryState { Recovered, AlreadyTerminal, RecoveryRequired };
struct RecordRecoveryResult {
    RecordRecoveryState state = RecordRecoveryState::RecoveryRequired;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
};

// Roll back metadata publication before any commit marker exists. A valid
// plan and a journal entry do not by themselves authorize arbitrary paths.
// Temporary and backup files are retained for inspection.
inline RecordRecoveryResult RecoverClientRecord(const MemberInstallContext& context, const CatalogResult& catalog,
                                                 const Json& manifest, const ClientLock& lock) {
    RecordRecoveryResult result;
    auto fail = [&result](const char* message, DWORD error = ERROR_SUCCESS) {
        result.reason = message; result.win32Error = error; return result;
    };
    if (!catalog || !ValidOperationId(context.operationId) || !lock.IsFor(context.clientRoot)) return fail("invalid context or lock");
    const auto client = ValidateClient(context.clientRoot, context.protectedRoots);
    if (!client || MemberProcesses(context).state != ProcessState::Stopped) return fail("client or process state prevents recovery");
    std::wstring operation;
    if (!CanonicalLocalPath(context.journalRoot, operation) || WithinRoot(operation, client.root) || WithinRoot(client.root, operation))
        return fail("invalid operation directory");
    GuardedPath root;
    if (!root.Open(operation, L"__journal_root__")) return fail("operation unavailable", root.Error());
    for (const auto* name : {L"commit.json", L"commit.pending", L"transaction.json", L"transaction.pending"}) {
        GuardedPath marker;
        if (!marker.Open(operation, name)) return fail("commit state unavailable", marker.Error());
        if (GetFileAttributesW(marker.Path().c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()))
            return fail("commit state requires separate recovery");
    }
    const auto members = LoadJournal(operation);
    if (!members || members.cursor.Next() == 1 || members.cursor.Pending() || members.cursor.Prepared() ||
        members.cursor.RollingBack() || members.cursor.Terminal() || !TrustedJournalCursor(members.cursor, catalog, context.operationId))
        return fail("member journal is not ready", members.win32Error);
    const auto prior = ReadSessionPriorRecord(operation);
    if (!BindJournalSession(operation, context.operationId, lock, catalog, prior, true)) return fail("session does not match client");
    GuardedPath planPath, sessionPath;
    if (!planPath.Open(operation, L"finalization-plan.json") || !sessionPath.Open(operation, L"session.json")) return fail("plan or session unavailable");
    std::string planBytes; DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(planPath, planBytes, error)) return fail("cannot read plan", error);
    const auto plan = ParseJsonObject(planBytes);
    if (!plan || !plan.value.contains("installed_utc") || !plan.value["installed_utc"].is_string()) return fail("invalid plan");
    const auto timestamp = plan.value["installed_utc"].get<std::string>();
    std::vector<std::wstring> missingOptional;
    if (!plan.value.contains("missing_optional") || !DecodeMissingOptional(plan.value["missing_optional"], catalog, missingOptional))
        return fail("invalid optional member list");
    const auto snapshot = SnapshotInstalledForRecords(client.root, catalog, missingOptional);
    const ClientRecord* previous = prior.state == CapturedRecordState::Present ? &prior.record : nullptr;
    const auto records = BuildFinalRecords(context, catalog, manifest, snapshot, members.cursor, previous, timestamp);
    if (!records) return fail("cannot reconstruct final records");
    std::array<std::uint8_t, 32> recordHash{}, transactionHash{};
    std::string id, catalogHash;
    const auto sessionHash = HashFile(sessionPath.Path());
    if (!sessionHash || !WideUtf8(context.operationId, id) || !CatalogDigest(catalog, catalogHash) ||
        HashBytes(records.clientRecord, recordHash) < 0 || HashBytes(records.operationRecord, transactionHash) < 0)
        return fail("cannot verify plan");
    const Json expectedPlan = {{"schema", 1}, {"operation_id", id}, {"catalog_sha256", catalogHash},
        {"session_sha256", HexSha256(sessionHash.identity.sha256)}, {"journal_next", members.cursor.Next()},
        {"installed_utc", timestamp}, {"missing_optional", plan.value["missing_optional"]},
        {"client_record", records.clientRecord}, {"client_record_sha256", HexSha256(recordHash)},
        {"operation_record", records.operationRecord}, {"operation_record_sha256", HexSha256(transactionHash)}};
    if (plan.value != expectedPlan) return fail("plan does not match verified inputs");
    JournalItem expected;
    expected.target = L"host64/dlss5-install-record.json";
    expected.staged = expected.target + L".nr-" + context.operationId + L".stage";
    expected.candidateSha256 = HexSha256(recordHash);
    expected.previouslyPresent = prior.state == CapturedRecordState::Present;
    if (expected.previouslyPresent) {
        expected.previousSha256 = HexSha256(prior.identity.sha256); expected.previousIdentity = JournalId(prior.identity);
        expected.backup = expected.target; expected.quarantine = expected.target + L".nr-" + context.operationId + L".original";
    }
    if (!ValidJournalItem(expected)) return fail("invalid expected record paths");
    const std::wstring metadataRoot = operation + L"\\record-publication";
    auto journal = LoadJournal(metadataRoot);
    if (!journal) return fail("record journal unavailable", journal.win32Error);
    auto trusted = [&]() {
        if (journal.cursor.Applied().size() > 1) return false;
        for (const auto& item : journal.cursor.Seen()) if (!SameJournalItem(item, expected)) return false;
        for (const auto& item : journal.cursor.Applied()) if (!SameJournalItem(item, expected)) return false;
        for (const auto& event : journal.cursor.Unresolved()) if (!SameJournalItem(event.item, expected)) return false;
        for (const auto& event : journal.cursor.Discarded()) if (!SameJournalItem(event.item, expected)) return false;
        return (!journal.cursor.Prepared() || SameJournalItem(journal.cursor.PreparedItem(), expected)) &&
               (!journal.cursor.Pending() || SameJournalItem(journal.cursor.PendingEvent().item, expected));
    };
    if (!trusted()) return fail("record journal does not match expected publication");
    if (journal.cursor.Terminal()) {
        if ((journal.cursor.TerminalKind() != JournalKind::Cancelled && journal.cursor.TerminalKind() != JournalKind::RolledBack) ||
            !journal.cursor.Unresolved().empty() ||
            !BindJournalSession(operation, context.operationId, lock, catalog, CaptureClientRecord(client.root), true))
            return fail("terminal record state requires inspection");
        result.state = RecordRecoveryState::AlreadyTerminal; return result;
    }
    auto safe = [&]() { return lock.IsFor(client.root) && ValidateClient(client.root, context.protectedRoots) &&
        MemberProcesses(context).state == ProcessState::Stopped; };
    auto append = [&](JournalKind kind) {
        auto next = PersistJournalEvent(metadataRoot, journal.cursor, {journal.cursor.Next(), kind, expected});
        if (!next) { error = next.win32Error; return false; }
        journal = std::move(next); return true;
    };
    auto restoreOriginal = [&]() {
        GuardedPath original, target;
        if (!safe() || !original.Open(client.root, expected.quarantine) || !target.Open(client.root, expected.target)) return false;
        return static_cast<bool>(RenameVerifiedNew(original, target, prior.identity.sha256, &prior.identity));
    };
    if (journal.cursor.Next() == 1) {
        if (!BindJournalSession(operation, context.operationId, lock, catalog, CaptureClientRecord(client.root), true)) return fail("prior record changed");
        if (!append(JournalKind::Cancelled)) return fail("cannot persist empty publication recovery", error);
        result.state = RecordRecoveryState::Recovered; return result;
    }
    if (journal.cursor.Pending() && journal.cursor.PendingEvent().kind == JournalKind::Prepare) {
        if (!BindJournalSession(operation, context.operationId, lock, catalog, CaptureClientRecord(client.root), true) ||
            !append(JournalKind::CancelPrepare)) return fail("cannot cancel preparation", error);
    }
    if (!journal.cursor.Pending() && journal.cursor.Prepared()) {
        if (!BindJournalSession(operation, context.operationId, lock, catalog, CaptureClientRecord(client.root), true) ||
            !append(JournalKind::CancelPrepared)) return fail("cannot cancel staging", error);
    }
    auto reconcile = [&]() { return ReconcilePending(client.root, operation + L"\\record-backup", journal.cursor); };
    if (journal.cursor.Pending() && journal.cursor.PendingEvent().kind == JournalKind::Intent) {
        auto observed = reconcile();
        if (observed.decision == ReconcileDecision::RestoreQuarantinedOriginal) {
            if (!restoreOriginal()) return fail("cannot restore isolated prior record");
            observed = reconcile();
        }
        if (observed.decision == ReconcileDecision::BeforeMutation) {
            if (!append(JournalKind::CancelIntent)) return fail("cannot cancel record publication", error);
        } else if (observed.decision == ReconcileDecision::Applied) {
            if (!append(JournalKind::Applied)) return fail("cannot record publication", error);
        } else return fail("record publication requires inspection", observed.win32Error);
    }
    if (!journal.cursor.Pending() && !journal.cursor.Applied().empty() && !append(JournalKind::RollbackIntent))
        return fail("cannot persist rollback intent", error);
    if (journal.cursor.Pending()) {
        if (journal.cursor.PendingEvent().kind != JournalKind::RollbackIntent) return fail("unsupported pending record step");
        auto observed = reconcile();
        if (observed.decision == ReconcileDecision::RestoreQuarantinedOriginal) {
            if (!restoreOriginal()) return fail("cannot restore isolated prior record");
            observed = reconcile();
        }
        if (observed.decision == ReconcileDecision::RollbackNotStarted) {
            if (!safe()) return fail("client or process state changed");
            GuardedPath target;
            if (!target.Open(client.root, expected.target)) return fail("record path unavailable", target.Error());
            const auto idToDelete = journal.cursor.PendingCandidateIdentity();
            if (!idToDelete.known) return fail("candidate identity missing");
            const auto identity = FileIdFromJournal(idToDelete);
            if (!expected.previouslyPresent) {
                if (!DeleteVerified(target, recordHash, &identity)) return fail("cannot delete published record");
            } else {
                GuardedPath stage;
                if (!stage.Open(client.root, expected.staged) || !RenameVerifiedNew(target, stage, recordHash, &identity) || !restoreOriginal())
                    return fail("cannot restore prior record");
            }
            observed = reconcile();
        }
        if (observed.decision != ReconcileDecision::RolledBack || !append(JournalKind::RollbackApplied))
            return fail("record rollback requires inspection", observed.win32Error);
    }
    const JournalKind terminal = journal.cursor.RollingBack() ? JournalKind::RolledBack : JournalKind::Cancelled;
    if (!append(terminal)) return fail("cannot persist record recovery", error);
    result.state = RecordRecoveryState::Recovered; return result;
}
}
