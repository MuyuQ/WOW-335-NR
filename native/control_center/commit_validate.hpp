#pragma once

#include "final_records.hpp"
#include "recover_operation.hpp"
#include "registered_install_config.hpp"

namespace control_center {
struct CommitValidation {
    std::string error, planBytes, transactionBytes, operationId, catalogDigest;
    DWORD win32Error = ERROR_SUCCESS;
    FileIdentity planIdentity{}, recordIdentity{};
    JournalResult members, recordJournal;
    JournalCursor membersBeforeComplete;
    explicit operator bool() const { return error.empty(); }
};

// Reconstruct the pre-completion cursor after the completion checkpoint so a
// restart can verify the same record bytes without the original staging tree.
inline bool BeforeComplete(const std::wstring& root, const JournalResult& journal, JournalCursor& before) {
    if (!journal) return false;
    if (!journal.cursor.Terminal()) { before = journal.cursor; return true; }
    if (journal.cursor.TerminalKind() != JournalKind::Complete || journal.cursor.Next() < 2) return false;
    for (std::uint64_t i = 1; i + 1 < journal.cursor.Next(); ++i) {
        GuardedPath file; std::string bytes; DWORD error = ERROR_SUCCESS; JournalEvent event;
        if (!file.Open(root, JournalFileName(i)) || !ReadJournalBytes(file, bytes, error) ||
            !DecodeJournalEvent(bytes, event) || !before.Accept(event)) return false;
    }
    return !before.Terminal() && before.Next() + 1 == journal.cursor.Next();
}

// No external source tree is needed: installed members and operation-owned
// repair copies are rehashed and compared to trusted catalog and journal data.
inline CommitValidation ValidateCommit(const MemberInstallContext& context, const CatalogResult& catalog,
                                        const Json& manifest, const ClientLock& lock) {
    CommitValidation result;
    auto fail = [&result](const char* text, DWORD error = ERROR_SUCCESS) { result.error = text; result.win32Error = error; return result; };
    if (!catalog || !ValidOperationId(context.operationId) || !lock.IsFor(context.clientRoot)) return fail("invalid context or lock");
    const auto client = ValidateClient(context.clientRoot, context.protectedRoots);
    if (!client || MemberProcesses(context).state != ProcessState::Stopped) return fail("client or process state prevents commit");
    std::wstring operation;
    if (!CanonicalLocalPath(context.journalRoot, operation) || WithinRoot(operation, client.root) || WithinRoot(client.root, operation))
        return fail("invalid operation path");
    GuardedPath root, plan, session;
    if (!root.Open(operation, L"__journal_root__") || !plan.Open(operation, L"finalization-plan.json") ||
        !session.Open(operation, L"session.json")) return fail("operation paths unavailable");
    result.members = LoadJournal(operation);
    if (!BeforeComplete(operation, result.members, result.membersBeforeComplete) ||
        result.membersBeforeComplete.Pending() || result.membersBeforeComplete.Prepared() || result.membersBeforeComplete.RollingBack() ||
        result.membersBeforeComplete.Applied().empty() || !TrustedJournalCursor(result.membersBeforeComplete, catalog, context.operationId))
        return fail("invalid member journal", result.members.win32Error);
    const auto prior = ReadSessionPriorRecord(operation);
    if (!BindJournalSession(operation, context.operationId, lock, catalog, prior, true)) return fail("session mismatch");
    DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(plan, result.planBytes, error)) return fail("plan unavailable", error);
    const auto parsed = ParseJsonObject(result.planBytes);
    if (!parsed || !parsed.value.contains("installed_utc") || !parsed.value["installed_utc"].is_string()) return fail("invalid plan");
    std::array<std::uint8_t, 32> planHash{};
    const auto actualPlan = HashFile(plan.Path()), actualSession = HashFile(session.Path());
    if (!actualPlan || !actualSession || HashBytes(result.planBytes, planHash) < 0 || actualPlan.identity.sha256 != planHash)
        return fail("plan or session changed");
    result.planIdentity = actualPlan.identity;
    std::vector<std::wstring> missingOptional;
    if (!parsed.value.contains("missing_optional") || !DecodeMissingOptional(parsed.value["missing_optional"], catalog, missingOptional))
        return fail("invalid optional member list");
    // Preserved personalised configurations are part of the record but not of
    // the release identity; the plan's client record proves they were
    // registered before this transaction.
    std::vector<std::wstring> preservedTargets;
    if (!parsed.value.contains("client_record") || !parsed.value["client_record"].is_string())
        return fail("invalid plan record");
    const auto planRecord = ReadClientRecord(parsed.value["client_record"].get<std::string>());
    if (!planRecord || !planRecord.error.empty() || !ClientRecordOwnedBy(planRecord, client.root))
        return fail("plan record is not usable");
    preservedTargets = PreservedTargetsFromRecord(planRecord, catalog);
    const auto snapshot = SnapshotInstalledForRecords(client.root, catalog, missingOptional, preservedTargets);
    if (!snapshot.filesReady) { result.error = snapshot.error; return result; }
    if (!VerifyPersistentSources(operation, snapshot, error)) return fail("repair sources incomplete or changed", error);
    const auto& applied = result.membersBeforeComplete.Applied();
    const auto& ids = result.membersBeforeComplete.AppliedIdentities();
    if (applied.size() != ids.size()) return fail("member identity missing");
    for (size_t i = 0; i < applied.size(); ++i) {
        const auto& item = applied[i]; const PreviewItem* current = nullptr;
        for (const auto& candidate : snapshot.items) if (SameTarget(candidate.member.target, item.target)) { current = &candidate; break; }
        if (!current || current->state != PreviewState::Same || !SameJournalId(JournalId(current->existing), ids[i])) return fail("member identity changed");
        GuardedPath repair;
        if (!repair.Open(operation, L"stage/" + item.target)) return fail("repair source unavailable", repair.Error());
        const auto repairHash = HashFile(repair.Path());
        if (!repairHash || HexSha256(repairHash.identity.sha256) != item.candidateSha256) return fail("repair source changed");
        if (item.previouslyPresent) {
            GuardedPath original, backup, backupOperation;
            if (!original.Open(client.root, item.quarantine) || !backup.Open(client.root, L"host64/dlss5-backup/" + item.backup) ||
                !backupOperation.Open(operation, L"backup/" + item.backup)) return fail("backup paths unavailable");
            const auto old = HashFile(original.Path()), copy = HashFile(backup.Path()), opCopy = HashFile(backupOperation.Path());
            if (!old || !copy || !opCopy || !SameJournalId(JournalId(old.identity), item.previousIdentity) ||
                HexSha256(old.identity.sha256) != item.previousSha256 || copy.identity.sha256 != old.identity.sha256 ||
                opCopy.identity.sha256 != old.identity.sha256) return fail("backup changed");
        }
    }
    const auto timestamp = parsed.value["installed_utc"].get<std::string>();
    const auto records = BuildFinalRecords(context, catalog, manifest, snapshot, result.membersBeforeComplete,
        prior.state == CapturedRecordState::Present ? &prior.record : nullptr, timestamp);
    if (!records) return fail("cannot reconstruct records");
    std::array<std::uint8_t, 32> recordHash{}, transactionHash{};
    if (!WideUtf8(context.operationId, result.operationId) || !CatalogDigest(catalog, result.catalogDigest) ||
        HashBytes(records.clientRecord, recordHash) < 0 || HashBytes(records.operationRecord, transactionHash) < 0) return fail("cannot hash records");
    const Json expected = {{"schema", 1}, {"operation_id", result.operationId}, {"catalog_sha256", result.catalogDigest},
        {"session_sha256", HexSha256(actualSession.identity.sha256)}, {"journal_next", result.membersBeforeComplete.Next()},
        {"installed_utc", timestamp}, {"missing_optional", parsed.value["missing_optional"]},
        {"client_record", records.clientRecord}, {"client_record_sha256", HexSha256(recordHash)},
        {"operation_record", records.operationRecord}, {"operation_record_sha256", HexSha256(transactionHash)}};
    if (parsed.value != expected) return fail("plan does not match verified inputs");
    result.transactionBytes = records.operationRecord;
    JournalItem expectedRecord;
    expectedRecord.target = L"host64/dlss5-install-record.json";
    expectedRecord.staged = expectedRecord.target + L".nr-" + context.operationId + L".stage";
    expectedRecord.candidateSha256 = HexSha256(recordHash);
    expectedRecord.previouslyPresent = prior.state == CapturedRecordState::Present;
    if (expectedRecord.previouslyPresent) {
        expectedRecord.previousSha256 = HexSha256(prior.identity.sha256); expectedRecord.previousIdentity = JournalId(prior.identity);
        expectedRecord.backup = expectedRecord.target; expectedRecord.quarantine = expectedRecord.target + L".nr-" + context.operationId + L".original";
    }
    result.recordJournal = LoadJournal(operation + L"\\record-publication");
    if (!result.recordJournal || (result.recordJournal.cursor.Terminal() && result.recordJournal.cursor.TerminalKind() != JournalKind::Complete) ||
        result.recordJournal.cursor.Pending() || result.recordJournal.cursor.Prepared() || result.recordJournal.cursor.RollingBack() ||
        result.recordJournal.cursor.Applied().size() != 1 || result.recordJournal.cursor.AppliedIdentities().size() != 1 ||
        !SameJournalItem(result.recordJournal.cursor.Applied()[0], expectedRecord)) return fail("record journal not applied");
    for (const auto& seen : result.recordJournal.cursor.Seen()) if (!SameJournalItem(seen, expectedRecord)) return fail("record journal mismatch");
    GuardedPath record;
    if (!record.Open(client.root, expectedRecord.target)) return fail("client record unavailable", record.Error());
    const auto currentRecord = HashFile(record.Path());
    if (!currentRecord || currentRecord.identity.sha256 != recordHash ||
        !SameJournalId(JournalId(currentRecord.identity), result.recordJournal.cursor.AppliedIdentities()[0])) return fail("client record changed");
    result.recordIdentity = currentRecord.identity;
    if (expectedRecord.previouslyPresent) {
        GuardedPath old, backup;
        if (!old.Open(client.root, expectedRecord.quarantine) || !backup.Open(operation, L"record-backup/" + expectedRecord.backup)) return fail("prior record unavailable");
        const auto oldHash = HashFile(old.Path()), backupHash = HashFile(backup.Path());
        if (!oldHash || !backupHash || !SameIdentity(oldHash.identity, prior.identity) || backupHash.identity.sha256 != prior.identity.sha256)
            return fail("prior record backup changed");
    }
    return result;
}
}
