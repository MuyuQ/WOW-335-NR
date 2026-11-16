#pragma once

#include "recover_uninstall.hpp"

namespace control_center {
// The last step of an uninstall: after every member was applied the record
// snapshot and the finalization plan exist, but nothing has decided that the
// uninstall is final. This header publishes that decision and then removes the
// leftovers (quarantined originals, backup copies, trusted residue) before it
// deletes the client record and publishes a completion report.
//
// The decision marker is commit.json. Before it exists a caller may still roll
// the uninstall back; afterwards only forward recovery is allowed. Every step
// after the decision is idempotent and re-observes the exact bytes it deletes.
enum class UninstallFinalizeState { Completed, AlreadyCompleted, PreconditionFailed, RecoveryRequired };
enum class UninstallFinalizeCheckpoint { MembersCompleted, DecisionPrepared, DecisionPublished, IsolatedRemoved,
                                         BackupsRemoved, ResidueRemoved, RecordRemoved, ResultPublished };
struct UninstallFinalizeResult {
    UninstallFinalizeState state = UninstallFinalizeState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    size_t deleted = 0, restored = 0, absent = 0, keptModified = 0;
    size_t backupsRemoved = 0, backupsKept = 0;
    size_t residueDeleted = 0, residueAbsent = 0, residueSkipped = 0;
    bool backupDirectoryRemoved = false;
};

// Presence of a transaction artifact inside a canonical work directory: 1 when
// it exists, 0 when it is confirmed absent, -1 when the path cannot be read.
inline bool UninstallArtifactPresence(const std::wstring& work, const wchar_t* name, int& presence) {
    GuardedPath path;
    if (!path.Open(work, name)) return false;
    if (GetFileAttributesW(path.Path().c_str()) != INVALID_FILE_ATTRIBUTES) { presence = 1; return true; }
    presence = MissingPathError(GetLastError()) ? 0 : -1;
    return presence >= 0;
}

// The journal needs the action list that produced it. After the decision the
// client record may already be gone, so the immutable plan is re-read here and
// only the cursor-relevant fields are reconstructed.
inline bool ParseUninstallStateText(const std::string& text, UninstallItemState& state) {
    if (text == "absent") state = UninstallItemState::AlreadyAbsent;
    else if (text == "restored") state = UninstallItemState::AlreadyRestored;
    else if (text == "delete_installed") state = UninstallItemState::DeleteInstalled;
    else if (text == "delete_modified") state = UninstallItemState::DeleteModified;
    else if (text == "restore") state = UninstallItemState::RestoreOriginal;
    else return false;
    return true;
}
inline TrustedUninstallPlan RebuildUninstallJournalPlan(const Json& plan, const FileIdentity& identity) {
    TrustedUninstallPlan rebuilt;
    const auto items = plan.find("items");
    if (items == plan.end() || !items->is_array()) return rebuilt;
    for (const auto& raw : *items) {
        std::string targetText, stateText;
        bool save = false;
        if (!raw.is_object() || !StringField(raw, "target", targetText) || !StringField(raw, "state", stateText) ||
            !BoolField(raw, "save_current", save)) return TrustedUninstallPlan{};
        TrustedUninstallItem item;
        item.saveCurrent = save;
        if (!ParseUninstallStateText(stateText, item.state)) return TrustedUninstallPlan{};
        JournalFileId id{};
        if (raw.contains("target_identity") && !raw["target_identity"].is_null()) {
            std::string hash;
            if (!DecodeJournalId(raw["target_identity"], id) || !StringField(raw, "target_sha256", hash) ||
                !ParseSha256Bytes(hash, item.target.sha256)) return TrustedUninstallPlan{};
            item.target.volume = id.volume; item.target.indexHigh = id.high;
            item.target.indexLow = id.low; item.target.size = id.size;
            item.targetPresent = true;
        }
        if (raw.contains("backup_identity") && !raw["backup_identity"].is_null()) {
            std::string hash;
            if (!DecodeJournalId(raw["backup_identity"], id)) return TrustedUninstallPlan{};
            item.backup.volume = id.volume; item.backup.indexHigh = id.high;
            item.backup.indexLow = id.low; item.backup.size = id.size;
            if (!StringField(raw, "backup_sha256", hash) || !ParseSha256Bytes(hash, item.backup.sha256))
                return TrustedUninstallPlan{};
        }
        std::wstring target;
        if (!Utf8Wide(targetText, target)) return TrustedUninstallPlan{};
        if (target.empty() || item.saveCurrent && !item.targetPresent) return TrustedUninstallPlan{};
        rebuilt.items.push_back(std::move(item));
    }
    rebuilt.valid = true;
    rebuilt.identity = identity;
    return rebuilt;
}

// The fixed decision marker bytes. Callers must compare the exact bytes so a
// changed decision can never become a silent second decision.
inline bool UninstallFinalizationDecisionBytes(const std::string& uninstallId, const std::array<std::uint8_t, 32>& planDigest,
                                               const std::array<std::uint8_t, 32>& recordDigest,
                                               const std::array<std::uint8_t, 32>& finalizationPlanDigest,
                                               std::uint64_t journalNext, std::string& bytes) {
    const Json marker = {{"schema", 1}, {"kind", "uninstall_finalization_commit"}, {"uninstall_id", uninstallId},
        {"plan_sha256", HexSha256(planDigest)}, {"client_record_sha256", HexSha256(recordDigest)},
        {"finalization_plan_sha256", HexSha256(finalizationPlanDigest)}, {"journal_next", journalNext}};
    bytes = marker.dump() + "\n";
    return true;
}

inline bool UninstallFinalizationResultBytes(const std::string& uninstallId, const std::array<std::uint8_t, 32>& planDigest,
                                             const std::array<std::uint8_t, 32>& recordDigest, std::uint64_t journalNext,
                                             const std::string& completedUtc, const UninstallFinalizeResult& counts, std::string& bytes) {
    const Json report = {{"schema", 1}, {"kind", "uninstall_result"}, {"status", "completed"}, {"uninstall_id", uninstallId},
        {"plan_sha256", HexSha256(planDigest)}, {"client_record_sha256", HexSha256(recordDigest)}, {"journal_next", journalNext},
        {"completed_utc", completedUtc}, {"deleted", counts.deleted}, {"restored", counts.restored}, {"absent", counts.absent},
        {"kept_modified", counts.keptModified}, {"backups_removed", counts.backupsRemoved}, {"backups_kept", counts.backupsKept},
        {"residue_deleted", counts.residueDeleted}, {"residue_absent", counts.residueAbsent}, {"residue_skipped", counts.residueSkipped},
        {"backup_directory_removed", counts.backupDirectoryRemoved}};
    bytes = report.dump(2) + "\n";
    return true;
}

// Reads the two artifacts prepared before the decision: the record snapshot and
// the finalization plan. Only their bytes and internal bindings are checked.
struct UninstallFinalizationArtifacts {
    bool ok = false;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    std::string finalizationPlanBytes;
    FileIdentity finalizationPlanIdentity{};
    FileIdentity snapshotIdentity{};
    std::string snapshotBytes;
    std::uint64_t journalNext = 0;
    ClientRecord record;
};
inline UninstallFinalizationArtifacts ReadUninstallFinalizationArtifacts(const std::wstring& work, const std::string& uninstallId,
                                                                         const std::array<std::uint8_t, 32>& planDigest,
                                                                         const FileIdentity& planIdentity) {
    UninstallFinalizationArtifacts result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) {
        result.reason = reason; result.win32Error = error; return result;
    };
    std::wstring canonical;
    GuardedPath snapshotPath, planPath;
    if (!CanonicalLocalPath(work, canonical) || !snapshotPath.Open(canonical, L"record.snapshot") ||
        !planPath.Open(canonical, L"finalization-plan.json")) return fail("finalization artifacts unavailable");
    const auto snapshotHash = HashFile(snapshotPath.Path());
    if (!snapshotHash) return fail("client record snapshot unavailable", snapshotHash.win32Error);
    const auto planHash = HashFile(planPath.Path());
    if (!planHash) return fail("finalization plan unavailable", planHash.win32Error);
    DWORD error = ERROR_SUCCESS;
    std::string planBytes, snapshotBytes;
    if (!ReadJournalBytes(planPath, planBytes, error)) return fail("finalization plan unreadable", error);
    if (!ReadJournalBytes(snapshotPath, snapshotBytes, error)) return fail("client record snapshot unreadable", error);
    std::array<std::uint8_t, 32> snapshotDigest{};
    if (HashBytes(planBytes, snapshotDigest) != 0 || snapshotDigest != planHash.identity.sha256)
        return fail("finalization plan changed while reading");
    const auto planJson = ParseJsonObject(planBytes);
    if (!planJson || planJson.value.size() != 8 || !planJson.value.contains("schema") || planJson.value["schema"] != 1)
        return fail("finalization plan invalid");
    std::string kind, id, planSha, recordSha;
    JournalFileId planFileId{}, snapshotFileId{};
    if (!StringField(planJson.value, "kind", kind) || kind != "uninstall_finalization" ||
        !StringField(planJson.value, "uninstall_id", id) || id != uninstallId ||
        !StringField(planJson.value, "uninstall_plan_sha256", planSha) || planSha != HexSha256(planDigest) ||
        !StringField(planJson.value, "client_record_sha256", recordSha) || recordSha != HexSha256(snapshotHash.identity.sha256) ||
        !planJson.value.contains("journal_next") || !planJson.value["journal_next"].is_number_unsigned() ||
        !planJson.value.contains("uninstall_plan_identity") ||
        !DecodeJournalId(planJson.value["uninstall_plan_identity"], planFileId) ||
        !SameJournalId(planFileId, JournalId(planIdentity)) || !planJson.value.contains("snapshot_identity") ||
        !DecodeJournalId(planJson.value["snapshot_identity"], snapshotFileId) ||
        !SameJournalId(snapshotFileId, JournalId(snapshotHash.identity)))
        return fail("finalization plan binding mismatch");
    std::array<std::uint8_t, 32> snapshotBytesDigest{};
    if (HashBytes(snapshotBytes, snapshotBytesDigest) != 0 || snapshotBytesDigest != snapshotHash.identity.sha256)
        return fail("client record snapshot changed while reading");
    result.record = ReadClientRecord(snapshotBytes);
    if (!result.record) return fail("client record snapshot invalid");
    result.finalizationPlanBytes = planBytes;
    result.finalizationPlanIdentity = planHash.identity;
    result.snapshotIdentity = snapshotHash.identity;
    result.snapshotBytes = snapshotBytes;
    result.journalNext = planJson.value["journal_next"].get<std::uint64_t>();
    result.ok = true;
    return result;
}

// A bounded residue tree deletion. Only plain files and empty directories below
// the recorded tree are removed; a reparse point or an invalid member path aborts
// instead of touching something outside the client.
inline bool DeleteUninstallResidueTree(const std::wstring& client, const std::wstring& relative, size_t depth,
                                       size_t& deleted, std::string& reason) {
    if (depth > 12) { reason = "residue tree is too deep"; return false; }
    if (deleted > 8192) { reason = "residue tree is too large"; return false; }
    GuardedPath tree;
    if (!tree.Open(client, relative)) return MissingPathError(tree.Error());
    WIN32_FIND_DATAW found{};
    HANDLE finder = FindFirstFileW((tree.Path() + L"\\*").c_str(), &found);
    if (finder == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (!MissingPathError(error)) { reason = "cannot enumerate residue tree"; return false; }
        return RemoveDirectoryW(tree.Path().c_str()) != 0 || MissingPathError(GetLastError());
    }
    bool ok = true;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring child = relative + L"/" + name;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !ValidMemberPath(child)) {
            reason = "residue tree contains an unsafe entry"; ok = false; break;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!DeleteUninstallResidueTree(client, child, depth + 1, deleted, reason)) { ok = false; break; }
            continue;
        }
        GuardedPath file;
        if (!file.Open(client, child)) { reason = "residue file unavailable"; ok = false; break; }
        const auto hash = HashFile(file.Path());
        if (!hash) {
            if (hash.error == HashError::Open && MissingPathError(hash.win32Error)) continue;
            reason = "residue file unreadable"; ok = false; break;
        }
        const auto removed = DeleteVerified(file, hash.identity.sha256, &hash.identity);
        if (!removed) { reason = "cannot remove residue file"; ok = false; break; }
        ++deleted;
    } while (FindNextFileW(finder, &found));
    const DWORD enumeration = GetLastError();
    FindClose(finder);
    if (!ok) return false;
    if (enumeration != ERROR_NO_MORE_FILES) { reason = "residue enumeration failed"; return false; }
    return RemoveDirectoryW(tree.Path().c_str()) != 0 || MissingPathError(GetLastError());
}

// Removes the empty backup directory tree left after verified backups are
// deleted. Files and reparse points are never touched here; a directory that
// still holds something is left alone and reported through `removed`.
inline bool RemoveEmptyBackupDirectories(const std::wstring& client, const std::wstring& relative, size_t depth, bool& removed) {
    removed = false;
    if (depth > 12 || !ValidMemberPath(relative)) return false;
    GuardedPath directory;
    if (!directory.Open(client, relative)) return MissingPathError(directory.Error());
    WIN32_FIND_DATAW found{};
    HANDLE finder = FindFirstFileW((directory.Path() + L"\\*").c_str(), &found);
    if (finder == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (!MissingPathError(error)) return false;
        removed = RemoveDirectoryW(directory.Path().c_str()) != 0;
        return true;
    }
    bool ok = true, allChildrenRemoved = true;
    std::vector<std::wstring> children;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { allChildrenRemoved = false; continue; }
        const std::wstring child = relative + L"/" + name;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !ValidMemberPath(child)) { ok = false; break; }
        children.push_back(child);
    } while (FindNextFileW(finder, &found));
    const DWORD enumeration = GetLastError();
    FindClose(finder);
    if (!ok || enumeration != ERROR_NO_MORE_FILES) return false;
    for (const auto& child : children) {
        bool childRemoved = false;
        if (!RemoveEmptyBackupDirectories(client, child, depth + 1, childRemoved)) return false;
        if (!childRemoved) allChildrenRemoved = false;
    }
    if (!allChildrenRemoved) return true;
    removed = RemoveDirectoryW(directory.Path().c_str()) != 0;
    return true;
}

// Finishes a fully applied uninstall. The function requires the client lock to be
// held on the calling thread, refuses to run while a blocking process exists and
// is safe to repeat after every checkpoint.
inline UninstallFinalizeResult FinalizeUninstall(const std::wstring& clientRoot, const std::wstring& uninstallsRoot,
                                                 const std::wstring& uninstallId, const std::vector<std::wstring>& protectedRoots,
                                                 const CatalogResult& catalog, const Json& manifest, const ClientLock& lock,
                                                 const std::string& completedUtc
#ifdef CONTROL_CENTER_TESTING
                                                 , ProcessCheck (*testProcesses)() = nullptr,
                                                   bool (*interrupt)(UninstallFinalizeCheckpoint) = nullptr
#endif
                                                 ) {
    UninstallFinalizeResult result;
    auto fail = [&result](UninstallFinalizeState state, const char* reason, DWORD error = ERROR_SUCCESS) {
        result.state = state; result.reason = reason; result.win32Error = error; return result;
    };
    auto stopped = [&]() {
#ifdef CONTROL_CENTER_TESTING
        const auto process = testProcesses ? testProcesses() : CheckBlockingProcesses();
#else
        const auto process = CheckBlockingProcesses();
#endif
        return lock.IsFor(clientRoot) && ValidateClient(clientRoot, protectedRoots) && process.state == ProcessState::Stopped;
    };
    auto interrupted = [&](UninstallFinalizeCheckpoint point) {
#ifdef CONTROL_CENTER_TESTING
        return interrupt && interrupt(point);
#else
        (void)point; return false;
#endif
    };
    std::wstring client, root;
    std::string idText;
    if (!ValidOperationId(uninstallId) || !WideUtf8(uninstallId, idText) ||
        !ManifestMatchesCatalog(manifest, catalog) ||
        !CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(uninstallsRoot, root) || WithinRoot(client, root) ||
        WithinRoot(root, client)) return fail(UninstallFinalizeState::PreconditionFailed, "invalid context");
    BY_HANDLE_FILE_INFORMATION clientId{};
    if (!lock.IsFor(client) || !lock.Identity(clientId))
        return fail(UninstallFinalizeState::PreconditionFailed, "client lock is not held on this thread");
    if (!completedUtc.empty() && completedUtc.size() > 64)
        return fail(UninstallFinalizeState::PreconditionFailed, "completion time is invalid");
    const std::wstring work = root + L"\\" + uninstallId;
    GuardedPath workGuard;
    if (!workGuard.Open(work, L"__root__")) return fail(UninstallFinalizeState::PreconditionFailed, "uninstall work unavailable", workGuard.Error());
    // 1. Verify the immutable plan against this lock, catalog and manifest.
    GuardedPath planPath, sessionPath;
    if (!planPath.Open(work, L"plan.json") || !sessionPath.Open(work, L"session.json"))
        return fail(UninstallFinalizeState::PreconditionFailed, "uninstall plan unavailable");
    std::string planBytes, sessionBytes, planId, kind, planClient, planCatalog, planManifest;
    DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(planPath, planBytes, error) || !ReadJournalBytes(sessionPath, sessionBytes, error))
        return fail(UninstallFinalizeState::PreconditionFailed, "uninstall plan unreadable", error);
    const auto planJson = ParseJsonObject(planBytes), sessionJson = ParseJsonObject(sessionBytes);
    const auto planHash = HashFile(planPath.Path());
    std::array<std::uint8_t, 32> planDigest{}, manifestDigest{};
    std::string catalogHash;
    if (!planJson || !sessionJson || !planHash || HashBytes(planBytes, planDigest) != 0 ||
        planDigest != planHash.identity.sha256 || HashBytes(manifest.dump(), manifestDigest) != 0 ||
        !CatalogDigest(catalog, catalogHash) || !StringField(planJson.value, "kind", kind) || kind != "uninstall" ||
        !StringField(planJson.value, "uninstall_id", planId) || planId != idText ||
        !StringField(planJson.value, "client", planClient) ||
        !StringField(planJson.value, "catalog_sha256", planCatalog) || planCatalog != catalogHash ||
        !StringField(planJson.value, "manifest_sha256", planManifest) || planManifest != HexSha256(manifestDigest) ||
        !planJson.value.contains("schema") || planJson.value["schema"] != 1 || !planJson.value.contains("client_volume") ||
        planJson.value["client_volume"] != clientId.dwVolumeSerialNumber || !planJson.value.contains("client_index_high") ||
        planJson.value["client_index_high"] != clientId.nFileIndexHigh || !planJson.value.contains("client_index_low") ||
        planJson.value["client_index_low"] != clientId.nFileIndexLow)
        return fail(UninstallFinalizeState::PreconditionFailed, "uninstall plan binding mismatch");
    {
        std::wstring recordedClient, canonicalClient;
        if (!Utf8Wide(planClient, recordedClient) || !CanonicalLocalPath(recordedClient, canonicalClient) ||
            CompareStringOrdinal(canonicalClient.c_str(), -1, client.c_str(), -1, TRUE) != CSTR_EQUAL)
            return fail(UninstallFinalizeState::PreconditionFailed, "uninstall plan belongs to another client");
    }
    const Json expectedSession = {{"schema", 1}, {"kind", "uninstall_session"}, {"uninstall_id", idText},
        {"client_volume", planJson.value["client_volume"]}, {"client_index_high", planJson.value["client_index_high"]},
        {"client_index_low", planJson.value["client_index_low"]}, {"catalog_sha256", planCatalog},
        {"manifest_sha256", planManifest}, {"plan_sha256", HexSha256(planDigest)},
        {"plan_identity", JournalIdJson(JournalId(planHash.identity))}};
    if (sessionJson.value != expectedSession)
        return fail(UninstallFinalizeState::PreconditionFailed, "uninstall session binding mismatch");
    // 2. Read the record snapshot and the finalization plan.
    const auto artifacts = ReadUninstallFinalizationArtifacts(work, idText, planDigest, planHash.identity);
    if (!artifacts.ok) return fail(UninstallFinalizeState::PreconditionFailed, artifacts.reason.c_str(), artifacts.win32Error);
    const auto& snapshot = artifacts.snapshotIdentity;
    const auto& record = artifacts.record;
    std::array<std::uint8_t, 32> snapshotDigest{};
    if (HashBytes(artifacts.snapshotBytes, snapshotDigest) != 0 || snapshotDigest != snapshot.sha256)
        return fail(UninstallFinalizeState::PreconditionFailed, "client record snapshot changed");
    // 2b. The fixed decision bytes are needed by both the finished and the fresh path.
    std::string decisionBytes;
    if (!UninstallFinalizationDecisionBytes(idText, planDigest, snapshot.sha256,
                                            artifacts.finalizationPlanIdentity.sha256, artifacts.journalNext, decisionBytes))
        return fail(UninstallFinalizeState::PreconditionFailed, "cannot encode uninstall decision");
    // 3. Parse the plan actions once. Journal replay and cleanup both use them.
    const auto itemsJson = planJson.value.find("items");
    const auto residueJson = planJson.value.find("residue");
    if (itemsJson == planJson.value.end() || !itemsJson->is_array() || residueJson == planJson.value.end() || !residueJson->is_array())
        return fail(UninstallFinalizeState::PreconditionFailed, "uninstall plan contents invalid");
    struct Action {
        std::wstring target;
        UninstallItemState state = UninstallItemState::Unavailable;
        bool targetPresent = false, saveCurrent = false;
        std::array<std::uint8_t, 32> targetSha{}, backupSha{};
        JournalFileId targetId{}, backupId{};
    };
    std::vector<Action> actions;
    actions.reserve(itemsJson->size());
    for (const auto& raw : *itemsJson) {
        std::string targetText, state, targetHash, backupHash;
        bool save = false, targetPresent = false, backupPresent = false;
        JournalFileId targetId{}, backupId{};
        std::wstring target;
        if (!raw.is_object() || !StringField(raw, "target", targetText) || !Utf8Wide(targetText, target) ||
            !ValidMemberPath(target) || !StringField(raw, "state", state) || !BoolField(raw, "save_current", save) ||
            !raw.contains("target_identity") || !raw.contains("target_sha256") || !raw.contains("backup_identity") ||
            !raw.contains("backup_sha256")) return fail(UninstallFinalizeState::PreconditionFailed, "uninstall item invalid");
        Action action;
        action.target = target;
        action.saveCurrent = save;
        if (!raw["target_identity"].is_null()) {
            if (!DecodeJournalId(raw["target_identity"], targetId) || !StringField(raw, "target_sha256", targetHash) ||
                !ParseSha256Bytes(targetHash, action.targetSha))
                return fail(UninstallFinalizeState::PreconditionFailed, "uninstall target invalid");
            targetPresent = true; action.targetId = targetId;
        }
        if (!raw["backup_identity"].is_null()) {
            if (!DecodeJournalId(raw["backup_identity"], backupId) || !StringField(raw, "backup_sha256", backupHash) ||
                !ParseSha256Bytes(backupHash, action.backupSha))
                return fail(UninstallFinalizeState::PreconditionFailed, "uninstall backup invalid");
            backupPresent = true; action.backupId = backupId;
        }
        action.targetPresent = targetPresent;
        // The plan state is authoritative; only the shapes that the member
        // applier could have produced are accepted here.
        if (state == "absent" && !targetPresent && !save) action.state = UninstallItemState::AlreadyAbsent;
        else if (state == "restored" && targetPresent && !save) action.state = UninstallItemState::AlreadyRestored;
        else if (state == "delete_installed" && targetPresent && !save && !backupPresent)
            action.state = UninstallItemState::DeleteInstalled;
        else if (state == "delete_modified" && targetPresent && save) action.state = UninstallItemState::DeleteModified;
        else if (state == "restore" && backupPresent) action.state = UninstallItemState::RestoreOriginal;
        else return fail(UninstallFinalizeState::PreconditionFailed, "uninstall action invalid");
        if ((action.state == UninstallItemState::DeleteInstalled || action.state == UninstallItemState::DeleteModified) &&
            !targetPresent) return fail(UninstallFinalizeState::PreconditionFailed, "uninstall action is missing its target");
        actions.push_back(std::move(action));
    }
    size_t actionCount = 0;
    for (const auto& action : actions)
        if (action.state == UninstallItemState::DeleteInstalled || action.state == UninstallItemState::DeleteModified ||
            action.state == UninstallItemState::RestoreOriginal) ++actionCount;
    TrustedUninstallPlan journalPlan = RebuildUninstallJournalPlan(planJson.value, planHash.identity);
    if (!journalPlan.valid || journalPlan.items.size() != actions.size())
        return fail(UninstallFinalizeState::PreconditionFailed, "uninstall journal plan invalid");
    // 4. Look at the fixed markers. A present record-publication is not part of
    // this finalization and always requires inspection.
    GuardedPath resultFinal, resultPending, commitFinal, commitPending, recordPublication, recordPath;
    if (!recordPath.Open(client, L"host64/dlss5-install-record.json") || !resultFinal.Open(work, L"result.json") ||
        !resultPending.Open(work, L"result.pending") || !commitFinal.Open(work, L"commit.json") ||
        !commitPending.Open(work, L"commit.pending") || !recordPublication.Open(work, L"record-publication"))
        return fail(UninstallFinalizeState::PreconditionFailed, "finalization markers unavailable");
    int hasResult = 0, hasResultPending = 0, hasCommit = 0, hasCommitPending = 0, hasPublication = 0;
    if (!UninstallArtifactPresence(work, L"result.json", hasResult) ||
        !UninstallArtifactPresence(work, L"result.pending", hasResultPending) ||
        !UninstallArtifactPresence(work, L"commit.json", hasCommit) ||
        !UninstallArtifactPresence(work, L"commit.pending", hasCommitPending) ||
        !UninstallArtifactPresence(work, L"record-publication", hasPublication))
        return fail(UninstallFinalizeState::PreconditionFailed, "finalization markers unavailable");
    if (hasResult < 0 || hasResultPending < 0 || hasCommit < 0 || hasCommitPending < 0 || hasPublication < 0)
        return fail(UninstallFinalizeState::PreconditionFailed, "finalization markers unreadable");
    if (hasPublication) return fail(UninstallFinalizeState::RecoveryRequired, "unexpected record publication state");
    auto readExact = [](const GuardedPath& path, const std::string& expected, FileIdentity& identity, DWORD& readError) {
        const auto hash = HashFile(path.Path());
        std::string bytes;
        std::array<std::uint8_t, 32> digest{};
        if (!hash || !ReadJournalBytes(path, bytes, readError) || bytes != expected || HashBytes(bytes, digest) != 0 ||
            digest != hash.identity.sha256) return false;
        identity = hash.identity; return true;
    };
    auto decisionExact = [&](const GuardedPath& path) {
        const auto hash = HashFile(path.Path());
        std::string bytes;
        std::array<std::uint8_t, 32> digest{};
        return hash && ReadJournalBytes(path, bytes, error) && bytes == decisionBytes && HashBytes(bytes, digest) == 0 &&
            digest == hash.identity.sha256;
    };
    auto resultBinding = [&](const std::string& bytes) {
        const auto report = ParseJsonObject(bytes);
        if (!report) return false;
        std::string reportKind, status, reportId, planSha, recordSha;
        return report.value.contains("schema") && report.value["schema"] == 1 &&
            StringField(report.value, "kind", reportKind) && reportKind == "uninstall_result" &&
            StringField(report.value, "status", status) && status == "completed" &&
            StringField(report.value, "uninstall_id", reportId) && reportId == idText &&
            StringField(report.value, "plan_sha256", planSha) && planSha == HexSha256(planDigest) &&
            StringField(report.value, "client_record_sha256", recordSha) && recordSha == HexSha256(snapshot.sha256) &&
            report.value.contains("journal_next") && report.value["journal_next"].is_number_unsigned() &&
            report.value["journal_next"].get<std::uint64_t>() == artifacts.journalNext;
    };
    // 5. The record is only meaningful before the decision; afterwards its
    // absence is part of the finished state.
    const auto liveRecord = ObserveUninstallFile(client, L"host64/dlss5-install-record.json");
    // 6. Already finished: the report is published last, so its presence implies
    // that every cleanup step succeeded (including removal of the client record).
    // A record written afterwards by a newer install belongs to that install and is
    // not evidence that this finalization is incomplete. The decision marker must
    // still match byte for byte.
    if (hasResult || hasResultPending) {
        std::string bytes;
        DWORD readError = ERROR_SUCCESS;
        if (!ReadJournalBytes(hasResult ? resultFinal : resultPending, bytes, readError) || !resultBinding(bytes))
            return fail(UninstallFinalizeState::RecoveryRequired, "uninstall result changed", readError);
        if (hasCommit != 1 || !decisionExact(commitFinal))
            return fail(UninstallFinalizeState::RecoveryRequired, "uninstall decision marker is missing");
        if (hasResultPending) {
            const auto hash = HashFile(resultPending.Path());
            if (!hash) return fail(UninstallFinalizeState::RecoveryRequired, "uninstall result unreadable", hash.win32Error);
            FileIdentity pendingIdentity = hash.identity;
            const auto renamed = RenameVerifiedNew(resultPending, resultFinal, pendingIdentity.sha256, &pendingIdentity);
            if (!renamed) return fail(UninstallFinalizeState::RecoveryRequired, "cannot publish uninstall result", renamed.win32Error);
        }
        result.state = UninstallFinalizeState::AlreadyCompleted;
        return result;
    }
    // 7. Complete the member journal if needed. The decision may only be published
    // after MembersDone and after the record snapshot still matches the live record.
    UninstallJournalResult journal(TrustedUninstallPlan{});
    if (hasCommit || hasCommitPending) {
        journal = LoadUninstallJournal(work, journalPlan);
        if (!journal || !journal.cursor.Terminal() || journal.cursor.TerminalKind() != UninstallJournalKind::MembersDone ||
            journal.cursor.Next() != artifacts.journalNext + 1)
            return fail(UninstallFinalizeState::PreconditionFailed, "uninstall journal is not ready", journal.win32Error);
    } else {
        const auto trusted = ReadTrustedUninstallPlan(client, root, uninstallId, protectedRoots, catalog, manifest, lock);
        if (!trusted.valid || !stopped())
            return fail(UninstallFinalizeState::PreconditionFailed, "client record or uninstall plan changed", trusted.win32Error);
        journal = LoadUninstallJournal(work, trusted);
        if (!journal) return fail(UninstallFinalizeState::PreconditionFailed, "uninstall journal unreadable", journal.win32Error);
        if (journal.cursor.Preparing() || journal.cursor.Prepared() || journal.cursor.IntentPending() ||
            journal.cursor.RollbackPending() || journal.cursor.RollingBack())
            return fail(UninstallFinalizeState::PreconditionFailed, "uninstall journal has an unfinished action");
        if (journal.cursor.Terminal()) {
            if (journal.cursor.TerminalKind() != UninstallJournalKind::MembersDone ||
                journal.cursor.Next() != artifacts.journalNext + 1)
                return fail(UninstallFinalizeState::PreconditionFailed, "uninstall journal is not completed");
        } else {
            if (journal.cursor.Applied().size() != actionCount || journal.cursor.Next() != artifacts.journalNext ||
                !liveRecord.valid || liveRecord.identity.sha256 != snapshot.sha256)
                return fail(UninstallFinalizeState::PreconditionFailed, "uninstall members are not complete");
            const auto persisted = PersistUninstallJournal(work, trusted, journal.cursor,
                                                           {journal.cursor.Next(), UninstallJournalKind::MembersDone, 0});
            if (!persisted) return fail(UninstallFinalizeState::PreconditionFailed, "cannot complete uninstall journal", persisted.win32Error);
            journal = persisted;
        }
    }
    if (interrupted(UninstallFinalizeCheckpoint::MembersCompleted))
        return fail(UninstallFinalizeState::RecoveryRequired, "interrupted after uninstall members");
    // 8. The decision marker. commit.json is the point of no return.
    if (hasCommit && hasCommitPending)
        return fail(UninstallFinalizeState::PreconditionFailed, "decision marker conflict");
    if (hasCommit) {
        if (!decisionExact(commitFinal)) return fail(UninstallFinalizeState::RecoveryRequired, "uninstall decision changed");
    } else {
        if (!stopped() || !liveRecord.valid || liveRecord.identity.sha256 != snapshot.sha256)
            return fail(UninstallFinalizeState::PreconditionFailed, "client record changed before the decision");
        if (hasCommitPending) {
            if (!decisionExact(commitPending))
                return fail(UninstallFinalizeState::RecoveryRequired, "pending uninstall decision changed");
        } else {
            if (!NoUninstallFinalState(work)) return fail(UninstallFinalizeState::PreconditionFailed, "uninstall state conflict");
            const auto written = WriteNewVerified(commitPending, decisionBytes);
            if (!written) return fail(UninstallFinalizeState::PreconditionFailed, "cannot stage uninstall decision", written.win32Error);
        }
        if (interrupted(UninstallFinalizeCheckpoint::DecisionPrepared) || !stopped() || !decisionExact(commitPending))
            return fail(UninstallFinalizeState::RecoveryRequired, "uninstall decision requires recovery");
        FileIdentity decisionIdentity;
        if (!readExact(commitPending, decisionBytes, decisionIdentity, error))
            return fail(UninstallFinalizeState::RecoveryRequired, "uninstall decision changed");
        const auto renamed = RenameVerifiedNew(commitPending, commitFinal, decisionIdentity.sha256, &decisionIdentity);
        if (!renamed) return fail(UninstallFinalizeState::RecoveryRequired, "cannot publish uninstall decision", renamed.win32Error);
        if (!decisionExact(commitFinal))
            return fail(UninstallFinalizeState::RecoveryRequired, "uninstall decision changed after publish");
    }
    if (interrupted(UninstallFinalizeCheckpoint::DecisionPublished))
        return fail(UninstallFinalizeState::RecoveryRequired, "interrupted after uninstall decision");
    // 9. Remove the quarantined originals created by the successful applications.
    for (const auto& action : actions) {
        if (action.state == UninstallItemState::AlreadyAbsent || action.state == UninstallItemState::AlreadyRestored) ++result.absent;
        else if (action.state == UninstallItemState::RestoreOriginal) ++result.restored;
        else ++result.deleted;
        if (action.saveCurrent) ++result.keptModified;
        if ((action.state != UninstallItemState::DeleteInstalled && action.state != UninstallItemState::DeleteModified &&
             action.state != UninstallItemState::RestoreOriginal) || !action.targetPresent) continue;
        const std::wstring isolatedRelative = action.target + L".nr-uninstall-" + uninstallId + L".original";
        const auto observed = ObserveUninstallFile(client, isolatedRelative);
        if (observed.missing) continue;
        if (!observed.valid || observed.identity.sha256 != action.targetSha ||
            !SameJournalId(JournalId(observed.identity), action.targetId))
            return fail(UninstallFinalizeState::RecoveryRequired, "quarantined file changed");
        GuardedPath isolated;
        if (!isolated.Open(client, isolatedRelative))
            return fail(UninstallFinalizeState::RecoveryRequired, "quarantined file unavailable", isolated.Error());
        const auto removed = DeleteVerified(isolated, action.targetSha, &observed.identity);
        if (!removed) return fail(UninstallFinalizeState::RecoveryRequired, "cannot remove quarantined file", removed.win32Error);
    }
    if (!stopped()) return fail(UninstallFinalizeState::RecoveryRequired, "blocking process started during uninstall");
    if (interrupted(UninstallFinalizeCheckpoint::IsolatedRemoved))
        return fail(UninstallFinalizeState::RecoveryRequired, "interrupted while removing quarantined files");
    // 10. Remove the backup copies listed by the record snapshot. A backup whose
    // bytes no longer match the recorded hash is kept, never deleted by name.
    for (const auto& entry : record.entries) {
        if (!entry.previouslyPresent || entry.backup.empty()) continue;
        const std::wstring relative = L"host64/dlss5-backup/" + entry.backup;
        if (!ValidMemberPath(relative)) return fail(UninstallFinalizeState::RecoveryRequired, "backup path invalid");
        GuardedPath backup;
        if (!backup.Open(client, relative)) {
            if (MissingPathError(backup.Error())) continue;
            return fail(UninstallFinalizeState::RecoveryRequired, "backup path unavailable", backup.Error());
        }
        const auto hash = HashFile(backup.Path());
        if (!hash) {
            if (hash.error == HashError::Open && MissingPathError(hash.win32Error)) continue;
            return fail(UninstallFinalizeState::RecoveryRequired, "backup file unreadable", hash.win32Error);
        }
        if (HexSha256(hash.identity.sha256) != entry.previousSha256) { ++result.backupsKept; continue; }
        const auto removed = DeleteVerified(backup, hash.identity.sha256, &hash.identity);
        if (!removed) return fail(UninstallFinalizeState::RecoveryRequired, "cannot remove backup file", removed.win32Error);
        ++result.backupsRemoved;
    }
    {
        bool removed = false;
        if (!RemoveEmptyBackupDirectories(client, L"host64/dlss5-backup", 0, removed))
            return fail(UninstallFinalizeState::RecoveryRequired, "backup directory cleanup failed");
        result.backupDirectoryRemoved = removed;
    }
    if (!stopped()) return fail(UninstallFinalizeState::RecoveryRequired, "blocking process started during uninstall");
    if (interrupted(UninstallFinalizeCheckpoint::BackupsRemoved))
        return fail(UninstallFinalizeState::RecoveryRequired, "interrupted while removing backups");
    // 11. Trusted residue. Anything not explicitly trusted and deletable is kept.
    // Report-only entries are never touched and may legitimately carry paths that are
    // not package member paths (for example %LOCALAPPDATA%\RHI), so they are skipped
    // before any member-path validation happens.
    for (const auto& raw : *residueJson) {
        std::string pathText, residueKind, policy;
        bool trusted = false;
        std::wstring path;
        if (!raw.is_object() || !StringField(raw, "path", pathText) || !StringField(raw, "kind", residueKind) ||
            !StringField(raw, "policy", policy) || !BoolField(raw, "trusted", trusted))
            return fail(UninstallFinalizeState::RecoveryRequired, "residue entry invalid");
        if (!trusted || policy != "delete") { ++result.residueSkipped; continue; }
        if (!Utf8Wide(pathText, path) || !ValidMemberPath(path))
            return fail(UninstallFinalizeState::RecoveryRequired, "residue entry invalid");
        if (residueKind == "file") {
            const auto observed = ObserveUninstallFile(client, path);
            if (observed.missing) { ++result.residueAbsent; continue; }
            if (!observed.valid) return fail(UninstallFinalizeState::RecoveryRequired, "residue file unreadable");
            GuardedPath file;
            if (!file.Open(client, path)) return fail(UninstallFinalizeState::RecoveryRequired, "residue file unavailable", file.Error());
            const auto removed = DeleteVerified(file, observed.identity.sha256, &observed.identity);
            if (!removed) return fail(UninstallFinalizeState::RecoveryRequired, "cannot remove residue file", removed.win32Error);
            ++result.residueDeleted;
        } else if (residueKind == "tree") {
            size_t deleted = 0;
            std::string reason;
            if (!DeleteUninstallResidueTree(client, path, 0, deleted, reason))
                return fail(UninstallFinalizeState::RecoveryRequired, reason.empty() ? "cannot remove residue tree" : reason.c_str());
            result.residueDeleted += deleted;
        } else return fail(UninstallFinalizeState::RecoveryRequired, "residue kind invalid");
    }
    if (!stopped()) return fail(UninstallFinalizeState::RecoveryRequired, "blocking process started during uninstall");
    if (interrupted(UninstallFinalizeCheckpoint::ResidueRemoved))
        return fail(UninstallFinalizeState::RecoveryRequired, "interrupted while removing residue");
    // 12. Remove the client record last so a crash never hides an unfinished uninstall.
    const auto currentRecord = ObserveUninstallFile(client, L"host64/dlss5-install-record.json");
    if (!currentRecord.missing) {
        if (!currentRecord.valid || currentRecord.identity.sha256 != snapshot.sha256)
            return fail(UninstallFinalizeState::RecoveryRequired, "client record changed during uninstall");
        const auto removed = DeleteVerified(recordPath, snapshot.sha256, &currentRecord.identity);
        if (!removed) return fail(UninstallFinalizeState::RecoveryRequired, "cannot remove client record", removed.win32Error);
        if (!ObserveUninstallFile(client, L"host64/dlss5-install-record.json").missing)
            return fail(UninstallFinalizeState::RecoveryRequired, "client record still present");
    }
    if (interrupted(UninstallFinalizeCheckpoint::RecordRemoved))
        return fail(UninstallFinalizeState::RecoveryRequired, "interrupted while removing client record");
    // 13. Publish the completion report and verify the published bytes.
    {
        std::string bytes;
        if (!UninstallFinalizationResultBytes(idText, planDigest, snapshot.sha256, artifacts.journalNext, completedUtc, result, bytes))
            return fail(UninstallFinalizeState::PreconditionFailed, "cannot encode uninstall result");
        const auto written = WriteNewVerified(resultPending, bytes);
        if (!written) return fail(UninstallFinalizeState::RecoveryRequired, "cannot stage uninstall result", written.win32Error);
        FileIdentity resultIdentity = written.identity;
        if (!stopped()) return fail(UninstallFinalizeState::RecoveryRequired, "blocking process started during uninstall");
        const auto renamed = RenameVerifiedNew(resultPending, resultFinal, resultIdentity.sha256, &resultIdentity);
        if (!renamed) return fail(UninstallFinalizeState::RecoveryRequired, "cannot publish uninstall result", renamed.win32Error);
        if (interrupted(UninstallFinalizeCheckpoint::ResultPublished))
            return fail(UninstallFinalizeState::RecoveryRequired, "interrupted after uninstall result");
        std::string published;
        if (!ReadJournalBytes(resultFinal, published, error) || !resultBinding(published))
            return fail(UninstallFinalizeState::RecoveryRequired, "uninstall result changed");
        if (!decisionExact(commitFinal)) return fail(UninstallFinalizeState::RecoveryRequired, "uninstall decision changed");
    }
    result.state = UninstallFinalizeState::Completed;
    return result;
}
}  // namespace control_center
