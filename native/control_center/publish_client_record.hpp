#pragma once

#include "finalization_plan.hpp"

namespace control_center {
enum class RecordPublishState { Published, PreconditionFailed, RecoveryRequired };
enum class RecordPublishCheckpoint { IntentToPrepare, BackupReady, StageReady, Prepared, Intent, OriginalIsolated, Published, Applied };
struct RecordPublishResult {
    RecordPublishState state = RecordPublishState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
};

// Publish only the client record under a separate durable journal. The caller
// must not report installation complete until the commit protocol also writes
// the operation record and a recoverable commit marker.
inline RecordPublishResult PublishClientRecord(const MemberInstallContext& context, const CatalogResult& catalog,
                                                const Json& manifest, const ClientLock& lock,
                                                const std::string& installedUtc
#ifdef CONTROL_CENTER_TESTING
                                                , bool (*interrupt)(RecordPublishCheckpoint) = nullptr
#endif
                                                ) {
    RecordPublishResult result;
    auto fail = [&result](const char* reason, bool recovery = false, DWORD error = ERROR_SUCCESS) {
        result.reason = reason; result.win32Error = error;
        result.state = recovery ? RecordPublishState::RecoveryRequired : RecordPublishState::PreconditionFailed;
        return result;
    };
    auto stopped = [&context, &lock]() {
        return lock.IsFor(context.clientRoot) && ValidateClient(context.clientRoot, context.protectedRoots) &&
            MemberProcesses(context).state == ProcessState::Stopped;
    };
    auto interrupted = [&](RecordPublishCheckpoint point) {
#ifdef CONTROL_CENTER_TESTING
        return interrupt && interrupt(point);
#else
        (void)point; return false;
#endif
    };
    const auto prepared = PrepareFinalization(context, catalog, manifest, lock, installedUtc);
    if (prepared.state != FinalizationPlanState::Ready) return fail("finalization plan is not ready", true, prepared.win32Error);
    std::wstring operation, client;
    if (!CanonicalLocalPath(context.journalRoot, operation) || !CanonicalLocalPath(context.clientRoot, client)) return fail("invalid paths");
    GuardedPath plan;
    if (!plan.Open(operation, L"finalization-plan.json")) return fail("plan unavailable", true, plan.Error());
    std::string bytes; DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(plan, bytes, error)) return fail("cannot read plan", true, error);
    std::array<std::uint8_t, 32> planHash{};
    if (HashBytes(bytes, planHash) < 0 || planHash != prepared.identity.sha256) return fail("plan changed", true);
    const auto parsed = ParseJsonObject(bytes);
    if (!parsed || !parsed.value.contains("client_record") || !parsed.value["client_record"].is_string()) return fail("invalid plan", true);
    const auto recordBytes = parsed.value["client_record"].get<std::string>();
    std::array<std::uint8_t, 32> recordHash{};
    if (!ReadClientRecord(recordBytes) || HashBytes(recordBytes, recordHash) < 0) return fail("invalid client record", true);
    const auto prior = ReadSessionPriorRecord(operation);
    if (!BindJournalSession(operation, context.operationId, lock, catalog, prior, true) ||
        !BindJournalSession(operation, context.operationId, lock, catalog, CaptureClientRecord(client), true))
        return fail("client record or session changed", true);
    GuardedPath publication;
    if (!publication.Open(operation, L"record-publication")) return fail("publication path unavailable", true, publication.Error());
    if (GetFileAttributesW(publication.Path().c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()))
        return fail("record publication already exists", true);
    if (!stopped()) return fail("client or process state changed");
    GuardedPath journalGuard;
    if (!journalGuard.Open(operation, L"record-publication/__root__", true)) return fail("cannot create publication journal", true, journalGuard.Error());
    const std::wstring metadataRoot = operation + L"\\record-publication";
    auto journal = LoadJournal(metadataRoot);
    if (!journal || journal.cursor.Next() != 1) return fail("publication journal invalid", true, journal.win32Error);
    JournalItem item;
    item.target = L"host64/dlss5-install-record.json";
    item.staged = item.target + L".nr-" + context.operationId + L".stage";
    item.candidateSha256 = HexSha256(recordHash);
    item.previouslyPresent = prior.state == CapturedRecordState::Present;
    if (item.previouslyPresent) {
        item.previousSha256 = HexSha256(prior.identity.sha256); item.previousIdentity = JournalId(prior.identity);
        item.backup = item.target; item.quarantine = item.target + L".nr-" + context.operationId + L".original";
    }
    if (!ValidJournalItem(item)) return fail("invalid record publication paths", true);
    auto append = [&](JournalKind kind, const FileIdentity* staged = nullptr) {
        JournalEvent event{journal.cursor.Next(), kind, item};
        if (staged) event.stagedIdentity = JournalId(*staged);
        auto next = PersistJournalEvent(metadataRoot, journal.cursor, event);
        if (!next) { error = next.win32Error; return false; }
        journal = std::move(next); return true;
    };
    if (!append(JournalKind::Prepare)) return fail("cannot persist preparation", true, error);
    if (interrupted(RecordPublishCheckpoint::IntentToPrepare)) return fail("interrupted during preparation", true);
    GuardedPath target, stage;
    if (!target.Open(client, item.target, true) || !stage.Open(client, item.staged, true)) return fail("record paths unavailable", true);
    if (item.previouslyPresent) {
        GuardedPath backup;
        if (!backup.Open(operation, L"record-backup/" + item.backup, true)) return fail("record backup path unavailable", true, backup.Error());
        const auto write = WriteNewVerified(backup, prior.bytes);
        if (!write || write.identity.sha256 != prior.identity.sha256) return fail("record backup failed", true, write.win32Error);
    }
    if (interrupted(RecordPublishCheckpoint::BackupReady)) return fail("interrupted after backup", true);
    const auto stageWrite = WriteNewVerified(stage, recordBytes);
    if (!stageWrite || stageWrite.identity.sha256 != recordHash) return fail("record staging failed", true, stageWrite.win32Error);
    if (interrupted(RecordPublishCheckpoint::StageReady)) return fail("interrupted after staging", true);
    if (!append(JournalKind::Prepared, &stageWrite.identity)) return fail("cannot persist staging", true, error);
    if (interrupted(RecordPublishCheckpoint::Prepared)) return fail("interrupted after staging checkpoint", true);
    if (!stopped() || !BindJournalSession(operation, context.operationId, lock, catalog, CaptureClientRecord(client), true))
        return fail("client or record changed before publication", true);
    if (!append(JournalKind::Intent)) return fail("cannot persist publication intent", true, error);
    if (interrupted(RecordPublishCheckpoint::Intent)) return fail("interrupted after publication intent", true);
    if (!stopped()) return fail("client or process state changed", true);
    if (item.previouslyPresent) {
        GuardedPath isolated;
        if (!isolated.Open(client, item.quarantine)) return fail("record quarantine unavailable", true, isolated.Error());
        const auto moved = RenameVerifiedNew(target, isolated, prior.identity.sha256, &prior.identity);
        if (!moved) return fail("cannot isolate prior record", true, moved.win32Error);
        if (interrupted(RecordPublishCheckpoint::OriginalIsolated)) return fail("interrupted after isolating record", true);
    }
    if (!stopped()) return fail("client or process state changed", true);
    const auto published = RenameVerifiedNew(stage, target, recordHash, &stageWrite.identity);
    if (!published) return fail("cannot publish client record", true, published.win32Error);
    if (interrupted(RecordPublishCheckpoint::Published)) return fail("interrupted after record publication", true);
    if (!append(JournalKind::Applied)) return fail("cannot persist publication result", true, error);
    if (interrupted(RecordPublishCheckpoint::Applied)) return fail("interrupted after publication checkpoint", true);
    result.state = RecordPublishState::Published; return result;
}
}
