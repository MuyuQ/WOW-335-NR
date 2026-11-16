#pragma once

#include "capture_client_record.hpp"
#include "client_identity.hpp"
#include "client_lock.hpp"
#include "final_records.hpp"

namespace control_center {
enum class UninstallItemState { AlreadyAbsent, AlreadyRestored, DeleteInstalled, DeleteModified, RestoreOriginal,
                                MissingBackup, BackupMismatch, UnverifiedBackup, Unavailable };
struct UninstallPreviewItem {
    InstalledEntry entry;
    UninstallItemState state = UninstallItemState::Unavailable;
    FileIdentity target{}, backup{};
    bool saveCurrent = false;
    DWORD win32Error = ERROR_SUCCESS;
};
struct UninstallResiduePreview { ResidueEntry entry; bool matchesManifest = false; };
struct UninstallPreview {
    bool valid = false, canUninstall = false;
    std::string error;
    DWORD win32Error = ERROR_SUCCESS;
    FileIdentity clientRecord{};
    std::vector<UninstallPreviewItem> items;
    std::vector<UninstallResiduePreview> residue;
};

// Read-only inventory. It does not authorize writes: execution must recheck
// all identities, process state, and transaction journals under the lock.
inline UninstallPreview PreviewUninstall(const std::wstring& clientRoot, const std::vector<std::wstring>& protectedRoots,
                                          const CatalogResult& catalog, const Json& manifest, const ClientLock& lock) {
    UninstallPreview result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) { result.error = reason; result.win32Error = error; return result; };
    if (!catalog || catalog.members.empty() || !ManifestMatchesCatalog(manifest, catalog) || !lock.IsFor(clientRoot))
        return fail("invalid catalog, manifest or client lock");
    const auto client = ValidateClient(clientRoot, protectedRoots);
    if (!client) return fail("invalid client", client.win32Error);
    const auto captured = CaptureClientRecord(client.root);
    if (captured.state != CapturedRecordState::Present) return fail("client installation record unavailable", captured.win32Error);
    // The recorded path is historical: a client may have been moved. Only
    // validated members under the locked, selected client can be inspected.
    result.clientRecord = captured.identity;
    for (const auto& entry : captured.record.entries) {
        size_t matches = 0;
        for (const auto& member : catalog.members)
            if (SameTarget(member.target, entry.target) && member.replacesOriginal == entry.replacesOriginal) ++matches;
        if (matches != 1 || (entry.previouslyPresent && !SameTarget(entry.backup, entry.target)))
            return fail("untrusted uninstall target or backup");
        UninstallPreviewItem item; item.entry = entry;
        GuardedPath target;
        bool missing = false, targetKnown = false;
        if (!target.Open(client.root, entry.target)) {
            if (MissingPathError(target.Error())) missing = true;
            else item.win32Error = target.Error();
        } else {
            const auto current = HashFile(target.Path());
            if (current) { item.target = current.identity; targetKnown = true; }
            else if (current.error == HashError::Open && MissingPathError(current.win32Error)) missing = true;
            else item.win32Error = current.win32Error;
        }
        if (!missing && !targetKnown) {
            item.state = UninstallItemState::Unavailable;
        } else if (!entry.previouslyPresent) {
            item.state = missing ? UninstallItemState::AlreadyAbsent :
                HexSha256(item.target.sha256) == entry.sha256 ? UninstallItemState::DeleteInstalled : UninstallItemState::DeleteModified;
            item.saveCurrent = item.state == UninstallItemState::DeleteModified;
        } else if (!missing && !entry.previousSha256.empty() && HexSha256(item.target.sha256) == entry.previousSha256) {
            item.state = UninstallItemState::AlreadyRestored;
        } else {
            item.saveCurrent = !missing;
            GuardedPath backup;
            if (!backup.Open(client.root, L"host64/dlss5-backup/" + entry.backup)) {
                item.win32Error = backup.Error();
                item.state = MissingPathError(item.win32Error) ? UninstallItemState::MissingBackup : UninstallItemState::Unavailable;
            } else {
                const auto saved = HashFile(backup.Path());
                if (!saved) {
                    item.win32Error = saved.win32Error;
                    item.state = saved.error == HashError::Open && MissingPathError(saved.win32Error) ? UninstallItemState::MissingBackup : UninstallItemState::Unavailable;
                } else {
                    item.backup = saved.identity;
                    if (entry.previousSha256.empty()) item.state = UninstallItemState::UnverifiedBackup;
                    else if (HexSha256(saved.identity.sha256) != entry.previousSha256) item.state = UninstallItemState::BackupMismatch;
                    else item.state = UninstallItemState::RestoreOriginal;
                }
            }
        }
        result.items.push_back(std::move(item));
    }
    const auto residues = manifest.find("residue");
    for (const auto& item : captured.record.residue) {
        UninstallResiduePreview observed; observed.entry = item;
        if (residues != manifest.end() && residues->is_array()) for (const auto& raw : *residues) {
            std::string path, kind, policy; std::wstring wide;
            if (raw.is_object() && StringField(raw, "path", path) && Utf8Wide(path, wide) && StringField(raw, "kind", kind) &&
                StringField(raw, "policy", policy) && SameTarget(wide, item.path) && kind == item.kind && policy == item.policy)
                observed.matchesManifest = true;
        }
        result.residue.push_back(std::move(observed));
    }
    result.valid = true; result.canUninstall = true;
    for (const auto& item : result.items)
        if (item.state == UninstallItemState::MissingBackup || item.state == UninstallItemState::BackupMismatch ||
            item.state == UninstallItemState::UnverifiedBackup || item.state == UninstallItemState::Unavailable) result.canUninstall = false;
    return result;
}
}
