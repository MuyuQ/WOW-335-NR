#pragma once

#include "final_records.hpp"

namespace control_center {
enum class FinalizationPlanState { Ready, Invalid, RecoveryRequired };
struct FinalizationPlanResult {
    FinalizationPlanState state = FinalizationPlanState::Invalid;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    FileIdentity identity{};
};

// Prepare immutable record bytes before publishing metadata. This only writes
// inside the operation directory. It is not a commit or an installation result.
inline FinalizationPlanResult PrepareFinalization(const MemberInstallContext& context, const CatalogResult& catalog,
                                                  const Json& manifest, const ClientLock& lock,
                                                  const std::string& installedUtc) {
    FinalizationPlanResult result;
    auto fail = [&result](const char* reason, bool recovery = false, DWORD error = ERROR_SUCCESS) {
        result.reason = reason; result.win32Error = error;
        result.state = recovery ? FinalizationPlanState::RecoveryRequired : FinalizationPlanState::Invalid;
        return result;
    };
    if (!catalog || !ValidOperationId(context.operationId) || !lock.IsFor(context.clientRoot)) return fail("invalid context or lock");
    const auto client = ValidateClient(context.clientRoot, context.protectedRoots);
    if (!client || MemberProcesses(context).state != ProcessState::Stopped) return fail("client or process state prevents finalization");
    std::wstring operation;
    if (!CanonicalLocalPath(context.journalRoot, operation) || WithinRoot(operation, client.root) ||
        WithinRoot(client.root, operation)) return fail("invalid operation directory");
    GuardedPath rootGuard;
    if (!rootGuard.Open(operation, L"__journal_root__")) return fail("operation directory unavailable", false, rootGuard.Error());
    const auto journal = LoadJournal(operation);
    if (!journal || journal.cursor.Next() == 1 || journal.cursor.Pending() || journal.cursor.Prepared() ||
        journal.cursor.Terminal() || journal.cursor.RollingBack() || journal.cursor.Applied().empty() ||
        !TrustedJournalCursor(journal.cursor, catalog, context.operationId)) return fail("journal is not ready", true, journal.win32Error);
    const auto prior = ReadSessionPriorRecord(operation);
    const auto currentPrior = CaptureClientRecord(client.root);
    if (!BindJournalSession(operation, context.operationId, lock, catalog, prior, true) ||
        !BindJournalSession(operation, context.operationId, lock, catalog, currentPrior, true))
        return fail("client record or session changed", true);
    GuardedPath session;
    if (!session.Open(operation, L"session.json")) return fail("session unavailable", true, session.Error());
    const auto sessionHash = HashFile(session.Path());
    if (!sessionHash) return fail("session unavailable", true, sessionHash.win32Error);
    RegisteredConfigPolicy registered;
    if (prior.state == CapturedRecordState::Present && ClientRecordOwnedBy(prior.record, client.root)) {
        registered.trusted = true;
        for (const auto& member : catalog.members) {
            if (!IsPreservedConfigTarget(member.target)) continue;
            for (const auto& entry : prior.record.entries)
                if (SameTarget(entry.target, member.target) && entry.sha256 == member.sha256) {
                    registered.entries.push_back({member.target, member.sha256});
                    break;
                }
        }
    }
    const auto snapshot = PreviewInstall(client.root, context.stageRoot, context.templateRoot, catalog,
                                         registered.trusted ? &registered : nullptr);
    if (!snapshot.filesReady) return fail("final file snapshot unavailable", true);
    for (const auto& item : snapshot.items) {
        if (item.state != PreviewState::PreservedConfig) continue;
        if (!registered.trusted || !registered.Matches(item.member.target, item.member.sha256))
            return fail("preserved configuration is no longer registered", true);
    }
    DWORD sourceError = ERROR_SUCCESS;
    if (!VerifyPersistentSources(operation, snapshot, sourceError)) return fail("repair sources incomplete or changed", true, sourceError);
    Json missingOptional = Json::array();
    for (const auto& item : snapshot.items) if (item.state == PreviewState::MissingOptional) {
        std::string target;
        if (!WideUtf8(item.member.target, target)) return fail("invalid optional member path", true);
        missingOptional.push_back(target);
    }
    const auto& items = journal.cursor.Applied();
    const auto& ids = journal.cursor.AppliedIdentities();
    if (items.size() != ids.size()) return fail("journal identities missing", true);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        const PreviewItem* found = nullptr;
        for (const auto& entry : snapshot.items) if (SameTarget(entry.member.target, item.target)) { found = &entry; break; }
        if (!found || found->state != PreviewState::Same || !SameJournalId(JournalId(found->existing), ids[i]))
            return fail("installed member identity changed", true);
        GuardedPath persistent;
        if (!persistent.Open(operation, L"stage/" + item.target)) return fail("repair source unavailable", true, persistent.Error());
        const auto source = HashFile(persistent.Path());
        if (!source || HexSha256(source.identity.sha256) != item.candidateSha256) return fail("repair source changed", true, source.win32Error);
        if (item.previouslyPresent) {
            GuardedPath old, backup, operationBackup;
            if (!old.Open(client.root, item.quarantine) || !backup.Open(client.root, L"host64/dlss5-backup/" + item.backup) ||
                !operationBackup.Open(operation, L"backup/" + item.backup)) return fail("backup paths unavailable", true);
            const auto original = HashFile(old.Path()), copy = HashFile(backup.Path()), opCopy = HashFile(operationBackup.Path());
            if (!original || !copy || !opCopy || !SameJournalId(JournalId(original.identity), item.previousIdentity) ||
                HexSha256(original.identity.sha256) != item.previousSha256 || copy.identity.sha256 != original.identity.sha256 ||
                opCopy.identity.sha256 != original.identity.sha256) return fail("original or backup changed", true);
        }
    }
    const ClientRecord* previous = prior.state == CapturedRecordState::Present ? &prior.record : nullptr;
    const auto records = BuildFinalRecords(context, catalog, manifest, snapshot, journal.cursor, previous, installedUtc);
    if (!records) { result.reason = records.error; result.state = FinalizationPlanState::RecoveryRequired; return result; }
    std::string id, catalogHash;
    if (!WideUtf8(context.operationId, id) || !CatalogDigest(catalog, catalogHash)) return fail("cannot identify operation");
    std::array<std::uint8_t, 32> clientHash{}, operationHash{};
    if (HashBytes(records.clientRecord, clientHash) < 0 || HashBytes(records.operationRecord, operationHash) < 0)
        return fail("cannot hash final records");
    const Json plan = {{"schema", 1}, {"operation_id", id}, {"catalog_sha256", catalogHash},
        {"session_sha256", HexSha256(sessionHash.identity.sha256)},
        {"journal_next", journal.cursor.Next()}, {"installed_utc", installedUtc}, {"missing_optional", missingOptional},
        {"client_record", records.clientRecord}, {"client_record_sha256", HexSha256(clientHash)},
        {"operation_record", records.operationRecord}, {"operation_record_sha256", HexSha256(operationHash)}};
    const std::string expected = plan.dump() + "\n";
    if (expected.size() > 1024 * 1024) return fail("finalization plan exceeds readable limit", true);
    GuardedPath file, temporary;
    if (!file.Open(operation, L"finalization-plan.json") || !temporary.Open(operation, L"finalization-plan.pending"))
        return fail("plan path unavailable", true);
    if (GetFileAttributesW(temporary.Path().c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()))
        return fail("unfinished plan requires inspection", true);
    const auto existing = HashFile(file.Path());
    if (existing) {
        std::string bytes; DWORD error = ERROR_SUCCESS;
        if (!ReadJournalBytes(file, bytes, error) || bytes != expected) return fail("existing plan differs", true, error);
        result.identity = existing.identity; result.state = FinalizationPlanState::Ready; return result;
    }
    if (existing.error != HashError::Open || !MissingPathError(existing.win32Error)) return fail("plan unavailable", true, existing.win32Error);
    const auto written = WriteNewVerified(temporary, expected);
    if (!written) return fail("could not write plan", true, written.win32Error);
    const auto renamed = RenameVerifiedNew(temporary, file, written.identity.sha256, &written.identity);
    if (!renamed) return fail("could not publish plan", true, renamed.win32Error);
    result.identity = renamed.identity; result.state = FinalizationPlanState::Ready;
    return result;
}
}
