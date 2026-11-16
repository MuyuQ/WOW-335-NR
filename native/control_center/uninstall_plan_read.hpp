#pragma once

#include "uninstall_plan.hpp"
#include "recover_operation.hpp"

namespace control_center {
struct TrustedUninstallItem {
    InstalledEntry entry;
    UninstallItemState state = UninstallItemState::Unavailable;
    bool targetPresent = false, saveCurrent = false;
    FileIdentity target{}, backup{};
};
struct TrustedUninstallPlan {
    bool valid = false;
    std::string error;
    DWORD win32Error = ERROR_SUCCESS;
    FileIdentity identity{};
    std::vector<TrustedUninstallItem> items;
    std::vector<UninstallResiduePreview> residue;
};

// A valid plan is not permission to mutate: every target and backup must be
// rechecked against these identities immediately before its operation.
inline TrustedUninstallPlan ReadTrustedUninstallPlan(const std::wstring& clientRoot, const std::wstring& uninstallRoot,
                                                      const std::wstring& uninstallId, const std::vector<std::wstring>& protectedRoots,
                                                      const CatalogResult& catalog, const Json& manifest, const ClientLock& lock) {
    TrustedUninstallPlan result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) { result.error = reason; result.win32Error = error; return result; };
    if (!ValidOperationId(uninstallId) || !lock.IsFor(clientRoot) || !ManifestMatchesCatalog(manifest, catalog)) return fail("invalid context");
    std::wstring client, root;
    GuardedPath rootGuard, planPath, sessionPath;
    if (!CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(uninstallRoot, root) || WithinRoot(client, root) || WithinRoot(root, client) ||
        !rootGuard.Open(root, L"__root__") || !planPath.Open(root + L"\\" + uninstallId, L"plan.json") ||
        !sessionPath.Open(root + L"\\" + uninstallId, L"session.json")) return fail("plan path unavailable");
    std::string bytes, sessionBytes; DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(planPath, bytes, error) || !ReadJournalBytes(sessionPath, sessionBytes, error)) return fail("plan or session unavailable", error);
    const auto parsed = ParseJsonObject(bytes), session = ParseJsonObject(sessionBytes);
    const auto actual = HashFile(planPath.Path());
    std::array<std::uint8_t, 32> digest{}, manifestDigest{};
    if (!parsed || !session || !actual || HashBytes(bytes, digest) < 0 || digest != actual.identity.sha256 ||
        HashBytes(manifest.dump(), manifestDigest) < 0) return fail("plan invalid or changed");
    std::string id, clientText, catalogHash, planId, planClient, planCatalog, planManifest, kind;
    BY_HANDLE_FILE_INFORMATION clientId{};
    if (!lock.Identity(clientId) || !WideUtf8(uninstallId, id) || !WideUtf8(client, clientText) || !CatalogDigest(catalog, catalogHash) ||
        !StringField(parsed.value, "kind", kind) || kind != "uninstall" || !StringField(parsed.value, "uninstall_id", planId) || planId != id ||
        !StringField(parsed.value, "client", planClient) || planClient != clientText ||
        !StringField(parsed.value, "catalog_sha256", planCatalog) || planCatalog != catalogHash ||
        !StringField(parsed.value, "manifest_sha256", planManifest) || planManifest != HexSha256(manifestDigest) ||
        !parsed.value.contains("schema") || parsed.value["schema"] != 1 ||
        !parsed.value.contains("client_volume") || parsed.value["client_volume"] != clientId.dwVolumeSerialNumber ||
        !parsed.value.contains("client_index_high") || parsed.value["client_index_high"] != clientId.nFileIndexHigh ||
        !parsed.value.contains("client_index_low") || parsed.value["client_index_low"] != clientId.nFileIndexLow) return fail("plan binding mismatch");
    const Json expectedSession = {{"schema", 1}, {"kind", "uninstall_session"}, {"uninstall_id", id},
        {"client_volume", clientId.dwVolumeSerialNumber}, {"client_index_high", clientId.nFileIndexHigh},
        {"client_index_low", clientId.nFileIndexLow}, {"catalog_sha256", catalogHash}, {"manifest_sha256", HexSha256(manifestDigest)},
        {"plan_sha256", HexSha256(digest)}, {"plan_identity", JournalIdJson(JournalId(actual.identity))}};
    if (session.value != expectedSession) return fail("session mismatch");
    const auto inventory = PreviewUninstall(client, protectedRoots, catalog, manifest, lock);
    JournalFileId recordId{}; std::string recordHash;
    if (!inventory.valid || !parsed.value.contains("client_record_identity") || !DecodeJournalId(parsed.value["client_record_identity"], recordId) ||
        !StringField(parsed.value, "client_record_sha256", recordHash) || !SameJournalId(recordId, JournalId(inventory.clientRecord)) ||
        recordHash != HexSha256(inventory.clientRecord.sha256)) return fail("client record changed");
    const auto items = parsed.value.find("items"), residues = parsed.value.find("residue");
    if (items == parsed.value.end() || !items->is_array() || items->size() != inventory.items.size() ||
        residues == parsed.value.end() || !residues->is_array() || residues->size() != inventory.residue.size()) return fail("invalid plan contents");
    for (size_t i = 0; i < items->size(); ++i) {
        const auto& raw = (*items)[i]; const auto& entry = inventory.items[i].entry;
        std::string targetText, state, installed, targetHash, backupHash; std::wstring target; bool save = false;
        JournalFileId targetId{}, backupId{};
        if (!raw.is_object() || raw.size() != 8 || !StringField(raw, "target", targetText) || !Utf8Wide(targetText, target) ||
            !SameTarget(target, entry.target) || !StringField(raw, "state", state) || !StringField(raw, "installed_sha256", installed) ||
            installed != entry.sha256 || !BoolField(raw, "save_current", save) || !raw.contains("target_identity") ||
            !raw.contains("target_sha256") || !raw.contains("backup_identity") || !raw.contains("backup_sha256")) return fail("invalid uninstall item");
        const bool present = !raw["target_identity"].is_null();
        if (present != !raw["target_sha256"].is_null() || (present &&
            (!DecodeJournalId(raw["target_identity"], targetId) || !StringField(raw, "target_sha256", targetHash) || !ValidSha256(targetHash))))
            return fail("invalid target identity");
        TrustedUninstallItem item; item.entry = entry; item.targetPresent = present; item.saveCurrent = save;
        if (present) {
            item.target = FileIdFromJournal(targetId);
            if (!ParseSha256Bytes(targetHash, item.target.sha256)) return fail("invalid target hash");
        }
        if (state == "absent" && !entry.previouslyPresent && !present && !save) item.state = UninstallItemState::AlreadyAbsent;
        else if (state == "restored" && entry.previouslyPresent && present && !save && !entry.previousSha256.empty() && targetHash == entry.previousSha256)
            item.state = UninstallItemState::AlreadyRestored;
        else if (state == "delete_installed" && !entry.previouslyPresent && present && !save && targetHash == installed)
            item.state = UninstallItemState::DeleteInstalled;
        else if (state == "delete_modified" && !entry.previouslyPresent && present && save && targetHash != installed)
            item.state = UninstallItemState::DeleteModified;
        else if (state == "restore" && entry.previouslyPresent && save == present && !entry.previousSha256.empty() &&
                 (!present || targetHash != entry.previousSha256)) item.state = UninstallItemState::RestoreOriginal;
        else return fail("uninstall action does not match record");
        if (item.state == UninstallItemState::RestoreOriginal) {
            if (!DecodeJournalId(raw["backup_identity"], backupId) || !StringField(raw, "backup_sha256", backupHash) || backupHash != entry.previousSha256)
                return fail("invalid backup identity");
            item.backup = FileIdFromJournal(backupId);
            if (!ParseSha256Bytes(backupHash, item.backup.sha256)) return fail("invalid backup hash");
        } else if (!raw["backup_identity"].is_null() || !raw["backup_sha256"].is_null()) return fail("unexpected backup identity");
        result.items.push_back(std::move(item));
    }
    Json expectedResidue = Json::array();
    for (const auto& item : inventory.residue) {
        std::string path;
        if (!WideUtf8(item.entry.path, path)) return fail("invalid residue");
        expectedResidue.push_back({{"path", path}, {"kind", item.entry.kind}, {"policy", item.entry.policy}, {"trusted", item.matchesManifest}});
    }
    if (*residues != expectedResidue) return fail("residue mismatch");
    result.residue = inventory.residue; result.identity = actual.identity; result.valid = true;
    return result;
}
}
