#pragma once

#include "catalog.hpp"
#include "client_identity.hpp"
#include "client_lock.hpp"
#include "copy_verified.hpp"
#include "install_preview.hpp"
#include "journal.hpp"
#include "rename_verified.hpp"
#include "capture_client_record.hpp"

namespace control_center {
enum class MemberInstallState { Applied, NoChange, PreconditionFailed, RecoveryRequired };
enum class MemberCheckpoint { PreparedIntent, BackupReady, PersistentStageReady, StageReady, Prepared, Intent, Quarantined, Published, Applied,
                              RollbackIntent, RollbackCandidateIsolated, RollbackOriginalRestored, RollbackDeleted, RollbackRecorded };
struct MemberInstallContext {
    std::wstring clientRoot, stageRoot, templateRoot, journalRoot, operationId;
    std::vector<std::wstring> protectedRoots;
#ifdef CONTROL_CENTER_TESTING
    ProcessCheck (*testProcesses)() = nullptr;
    bool (*testInterrupt)(MemberCheckpoint) = nullptr;
#endif
};
struct MemberInstallResult {
    MemberInstallState state = MemberInstallState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    JournalItem item;
};
inline bool SameIdentity(const FileIdentity& a, const FileIdentity& b) {
    return a.volume == b.volume && a.indexHigh == b.indexHigh && a.indexLow == b.indexLow &&
        a.size == b.size && a.sha256 == b.sha256;
}
inline bool ValidOperationId(const std::wstring& id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}
inline bool TrustedJournalItem(const JournalItem& item, const CatalogResult& catalog, const std::wstring& operationId) {
    const CatalogMember* match = nullptr;
    for (const auto& member : catalog.members) {
        if (!SameTarget(member.target, item.target)) continue;
        if (match) return false;
        match = &member;
    }
    if (!match || !ValidJournalItem(item) || item.candidateSha256 != match->sha256 ||
        !SameTarget(item.staged, item.target + L".nr-" + operationId + L".stage")) return false;
    if (item.previouslyPresent && (!match->replacesOriginal || !SameTarget(item.target, L"d3d9.dll") ||
        !SameTarget(item.backup, item.target) ||
        !SameTarget(item.quarantine, item.target + L".nr-" + operationId + L".original"))) return false;
    return true;
}
inline bool TrustedJournalCursor(const JournalCursor& cursor, const CatalogResult& catalog, const std::wstring& operationId) {
    for (const auto& item : cursor.Seen()) if (!TrustedJournalItem(item, catalog, operationId)) return false;
    for (const auto& item : cursor.Applied()) if (!TrustedJournalItem(item, catalog, operationId)) return false;
    for (const auto& event : cursor.Unresolved()) if (!TrustedJournalItem(event.item, catalog, operationId)) return false;
    for (const auto& event : cursor.Discarded()) if (!TrustedJournalItem(event.item, catalog, operationId)) return false;
    if (cursor.Prepared() && !TrustedJournalItem(cursor.PreparedItem(), catalog, operationId)) return false;
    if (cursor.Pending() && !TrustedJournalItem(cursor.PendingEvent().item, catalog, operationId)) return false;
    return true;
}
inline bool CatalogDigest(const CatalogResult& catalog, std::string& digest) {
    Json members = Json::array();
    for (const auto& member : catalog.members) {
        std::string target, source;
        if (!WideUtf8(member.target, target) || !WideUtf8(member.source, source) || !ValidSha256(member.sha256)) return false;
        members.push_back({{"target", target}, {"source", source}, {"source_kind", member.sourceKind},
                           {"sha256", member.sha256}, {"required", member.required}, {"replaces_original", member.replacesOriginal}});
    }
    std::array<std::uint8_t, 32> hash{};
    if (HashBytes(members.dump(), hash) < 0) return false;
    digest = HexSha256(hash);
    return true;
}
inline bool BindJournalSessionDigest(const std::wstring& directory, const std::wstring& operationId,
                                     const ClientLock& lock, const std::string& digest,
                                     const CapturedClientRecord& prior, bool hasEvents) {
    BY_HANDLE_FILE_INFORMATION identity{};
    std::string id;
    if (!lock.Identity(identity) || !WideUtf8(operationId, id) || !ValidSha256(digest)) return false;
    if (prior.state != CapturedRecordState::Absent && prior.state != CapturedRecordState::Present) return false;
    const Json expected = {{"schema", 1}, {"operation_id", id}, {"client_volume", identity.dwVolumeSerialNumber},
        {"client_index_high", identity.nFileIndexHigh}, {"client_index_low", identity.nFileIndexLow}, {"catalog_sha256", digest},
        {"prior_record", prior.state == CapturedRecordState::Present ? Json(prior.bytes) : Json(nullptr)},
        {"prior_record_sha256", prior.state == CapturedRecordState::Present ? Json(HexSha256(prior.identity.sha256)) : Json(nullptr)},
        {"prior_record_identity", prior.state == CapturedRecordState::Present ? JournalIdJson(JournalId(prior.identity)) : Json(nullptr)}};
    GuardedPath session;
    if (!session.Open(directory, L"session.json")) return false;
    const DWORD attributes = GetFileAttributesW(session.Path().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (!MissingPathError(GetLastError()) || hasEvents) return false;
        const auto write = WriteNewVerified(session, expected.dump() + "\n");
        if (!write) return false;
    }
    std::string bytes;
    DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(session, bytes, error)) return false;
    const auto parsed = ParseJsonObject(bytes);
    return parsed && parsed.value == expected;
}
inline bool BindJournalSession(const std::wstring& directory, const std::wstring& operationId,
                               const ClientLock& lock, const CatalogResult& catalog,
                               const CapturedClientRecord& prior, bool hasEvents) {
    std::string digest;
    return CatalogDigest(catalog, digest) && BindJournalSessionDigest(directory, operationId, lock, digest, prior, hasEvents);
}

// Read the immutable snapshot captured before the operation. Its contents do
// not authorize writes to the client record; they only bind ordinary member
// rollback to the same client, catalog and operation as the original session.
inline CapturedClientRecord ReadSessionPriorRecord(const std::wstring& directory) {
    CapturedClientRecord result;
    GuardedPath session;
    if (!session.Open(directory, L"session.json")) { result.win32Error = session.Error(); return result; }
    std::string bytes;
    if (!ReadJournalBytes(session, bytes, result.win32Error)) return result;
    const auto parsed = ParseJsonObject(bytes);
    if (!parsed) { result.state = CapturedRecordState::Invalid; return result; }
    const auto record = parsed.value.find("prior_record"), hash = parsed.value.find("prior_record_sha256"),
               identity = parsed.value.find("prior_record_identity");
    if (record == parsed.value.end() || hash == parsed.value.end() || identity == parsed.value.end()) {
        result.state = CapturedRecordState::Invalid; return result;
    }
    if (record->is_null() && hash->is_null() && identity->is_null()) {
        result.state = CapturedRecordState::Absent; return result;
    }
    JournalFileId id;
    if (!record->is_string() || !hash->is_string() || !DecodeJournalId(*identity, id)) {
        result.state = CapturedRecordState::Invalid; return result;
    }
    result.bytes = record->get<std::string>();
    std::array<std::uint8_t, 32> digest{};
    result.record = ReadClientRecord(result.bytes);
    if (!result.record || id.size != result.bytes.size() || HashBytes(result.bytes, digest) < 0 ||
        HexSha256(digest) != hash->get<std::string>()) {
        result.state = CapturedRecordState::Invalid; return result;
    }
    result.identity = FileIdFromJournal(id);
    result.identity.sha256 = digest;
    result.state = CapturedRecordState::Present;
    return result;
}
inline ProcessCheck MemberProcesses(const MemberInstallContext& context) {
#ifdef CONTROL_CENTER_TESTING
    if (context.testProcesses) return context.testProcesses();
#else
    (void)context;
#endif
    return CheckBlockingProcesses();
}
inline bool MemberInterrupted(const MemberInstallContext& context, MemberCheckpoint point) {
#ifdef CONTROL_CENTER_TESTING
    return context.testInterrupt && context.testInterrupt(point);
#else
    (void)context; (void)point; return false;
#endif
}

// Apply exactly one already-previewed member. A client lock must remain held
// by the calling thread. An operation journal is required outside the client;
// this does not finalize the batch or write the legacy client install record.
// On any failure after the first checkpoint, stop and reconcile the journal.
inline MemberInstallResult InstallMember(const MemberInstallContext& context, const CatalogResult& catalog,
                                         const PreviewItem& selected, const ClientLock& lock) {
    MemberInstallResult result;
    auto fail = [&result](const char* reason, bool recovery = false, DWORD error = ERROR_SUCCESS) {
        result.reason = reason; result.win32Error = error;
        result.state = recovery ? MemberInstallState::RecoveryRequired : MemberInstallState::PreconditionFailed;
        return result;
    };
    if (!catalog || !ValidOperationId(context.operationId) || !lock.IsFor(context.clientRoot)) return fail("invalid catalog, operation id or client lock");
    const CatalogMember* member = nullptr;
    for (const auto& candidate : catalog.members) {
        if (!SameTarget(candidate.target, selected.member.target)) continue;
        if (member) return fail("duplicate catalog member");
        member = &candidate;
    }
    if (!member || !SameTarget(member->source, selected.member.source) || member->sourceKind != selected.member.sourceKind ||
        member->sha256 != selected.member.sha256 || member->required != selected.member.required ||
        member->replacesOriginal != selected.member.replacesOriginal || !ValidSha256(member->sha256) ||
        (member->replacesOriginal && !SameTarget(member->target, L"d3d9.dll"))) return fail("preview does not match catalog");
    const auto client = ValidateClient(context.clientRoot, context.protectedRoots);
    if (!client) return fail("client validation failed", false, client.win32Error);
    std::wstring journalRoot;
    if (!CanonicalLocalPath(context.journalRoot, journalRoot) || WithinRoot(journalRoot, client.root) ||
        WithinRoot(client.root, journalRoot)) return fail("journal must be outside client");
    GuardedPath journalGuard;
    if (!journalGuard.Open(journalRoot, L"__journal_root__")) return fail("journal root unavailable", false, journalGuard.Error());
    auto journal = LoadJournal(journalRoot);
    if (!journal) return fail("journal unreadable", true, journal.win32Error);
    if (!TrustedJournalCursor(journal.cursor, catalog, context.operationId)) return fail("journal members do not match catalog", true);
    if (journal.cursor.Pending() || journal.cursor.Prepared() || journal.cursor.RollingBack() || journal.cursor.Terminal())
        return fail("journal requires recovery", true);
    if (MemberProcesses(context).state != ProcessState::Stopped) return fail("game or launcher running or process state unknown");
    CatalogResult one; one.members.push_back(*member);
    const auto now = PreviewInstall(client.root, context.stageRoot, context.templateRoot, one);
    if (!now.filesReady || now.items.size() != 1) return fail("file preview failed");
    const auto& current = now.items[0];
    if (current.state != selected.state || (current.state == PreviewState::Present && !SameIdentity(current.existing, selected.existing)))
        return fail("preview changed");
    if (current.state == PreviewState::Same || current.state == PreviewState::MissingOptional) {
        result.state = MemberInstallState::NoChange; return result;
    }
    if (current.state != PreviewState::Absent && current.state != PreviewState::Present) return fail("preview not installable");
    JournalItem item;
    item.target = member->target; item.candidateSha256 = member->sha256;
    item.staged = member->target + L".nr-" + context.operationId + L".stage";
    item.previouslyPresent = current.state == PreviewState::Present;
    if (item.previouslyPresent) {
        item.previousSha256 = HexSha256(current.existing.sha256); item.backup = member->target;
        item.previousIdentity = JournalId(current.existing);
        item.quarantine = member->target + L".nr-" + context.operationId + L".original";
    }
    result.item = item;
    if (!ValidJournalItem(item)) return fail("invalid transaction paths");
    const auto priorRecord = CaptureClientRecord(client.root);
    if (priorRecord.state == CapturedRecordState::Invalid || priorRecord.state == CapturedRecordState::Unavailable)
        return fail("prior client record invalid or unavailable", false, priorRecord.win32Error);
    if (!BindJournalSession(journalRoot, context.operationId, lock, catalog, priorRecord, journal.cursor.Next() != 1))
        return fail("journal session does not match client or catalog", true);
    auto append = [&](JournalKind kind, const FileIdentity* stagedIdentity = nullptr) {
        JournalEvent event{journal.cursor.Next(), kind, item};
        if (stagedIdentity) event.stagedIdentity = JournalId(*stagedIdentity);
        auto persisted = PersistJournalEvent(journalRoot, journal.cursor, event);
        if (!persisted) { result.win32Error = persisted.win32Error; return false; }
        journal = std::move(persisted);
        return true;
    };
    if (!append(JournalKind::Prepare)) return fail("could not persist preparation", true, result.win32Error);
    if (MemberInterrupted(context, MemberCheckpoint::PreparedIntent)) return fail("interrupted after preparation intent", true);
    GuardedPath target, staged, source, quarantine, backup, operationBackup, persistentStage;
    if (!target.Open(client.root, item.target, true) || !staged.Open(client.root, item.staged, true) ||
        !source.Open(member->sourceKind == "template" ? context.templateRoot : context.stageRoot, member->source) ||
        !persistentStage.Open(journalRoot, L"stage/" + item.target, true))
        return fail("could not open guarded paths", true);
    if (item.previouslyPresent) {
        if (!quarantine.Open(client.root, item.quarantine, true) ||
            !backup.Open(client.root, L"host64/dlss5-backup/" + item.backup, true) ||
            !operationBackup.Open(journalRoot, L"backup/" + item.backup, true)) return fail("backup path unavailable", true);
        const auto currentTarget = HashFile(target.Path());
        if (!currentTarget || !SameIdentity(currentTarget.identity, current.existing)) return fail("target changed before backup", true);
        auto ensureBackup = [&](const GuardedPath& path) {
            const auto existing = HashFile(path.Path());
            if (existing) return HexSha256(existing.identity.sha256) == item.previousSha256;
            if (existing.error != HashError::Open || !MissingPathError(existing.win32Error)) return false;
            const auto copied = CopyVerifiedNew(target, path, current.existing.sha256);
            return copied && SameIdentity(copied.source, current.existing);
        };
        if (!ensureBackup(backup) || !ensureBackup(operationBackup)) return fail("backup failed, unavailable, or target changed", true);
        if (GetFileAttributesW(quarantine.Path().c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()))
            return fail("quarantine path occupied or unreadable", true);
    }
    if (MemberInterrupted(context, MemberCheckpoint::BackupReady)) return fail("interrupted after backup", true);
    const auto persistentCopy = CopyVerifiedNew(source, persistentStage, current.source.sha256);
    if (!persistentCopy) return fail("persistent staging failed", true, persistentCopy.win32Error);
    if (MemberInterrupted(context, MemberCheckpoint::PersistentStageReady)) return fail("interrupted after persistent staging", true);
    const auto stageCopy = CopyVerifiedNew(persistentStage, staged, current.source.sha256);
    if (!stageCopy) return fail("staging failed", true, stageCopy.win32Error);
    if (MemberInterrupted(context, MemberCheckpoint::StageReady)) return fail("interrupted after staging", true);
    if (!append(JournalKind::Prepared, &stageCopy.target)) return fail("could not persist prepared state", true, result.win32Error);
    if (MemberInterrupted(context, MemberCheckpoint::Prepared)) return fail("interrupted after prepared checkpoint", true);
    if (!lock.IsFor(client.root) || !ValidateClient(client.root, context.protectedRoots) ||
        MemberProcesses(context).state != ProcessState::Stopped) return fail("client or process state changed", true);
    const auto targetBefore = HashFile(target.Path());
    if (item.previouslyPresent) {
        if (!targetBefore || !SameIdentity(targetBefore.identity, current.existing)) return fail("target changed before publication", true);
    } else if (targetBefore || targetBefore.error != HashError::Open || !MissingPathError(targetBefore.win32Error)) {
        return fail("target appeared before publication", true);
    }
    if (!append(JournalKind::Intent)) return fail("could not persist publication intent", true, result.win32Error);
    if (MemberInterrupted(context, MemberCheckpoint::Intent)) return fail("interrupted after publication intent", true);
    if (MemberProcesses(context).state != ProcessState::Stopped) return fail("game or launcher started", true);
    if (item.previouslyPresent) {
        const auto old = RenameVerifiedNew(target, quarantine, current.existing.sha256, &current.existing);
        if (!old) return fail("could not isolate original", true, old.win32Error);
        if (MemberInterrupted(context, MemberCheckpoint::Quarantined)) return fail("interrupted after original isolation", true);
        if (MemberProcesses(context).state != ProcessState::Stopped) return fail("game or launcher started after original isolation", true);
    }
    const auto published = RenameVerifiedNew(staged, target, current.source.sha256, &stageCopy.target);
    if (!published) return fail("publication failed", true, published.win32Error);
    if (MemberInterrupted(context, MemberCheckpoint::Published)) return fail("interrupted after publication", true);
    if (!append(JournalKind::Applied)) return fail("could not persist applied state", true, result.win32Error);
    if (MemberInterrupted(context, MemberCheckpoint::Applied)) return fail("interrupted after applied checkpoint", true);
    result.state = MemberInstallState::Applied;
    return result;
}
}
