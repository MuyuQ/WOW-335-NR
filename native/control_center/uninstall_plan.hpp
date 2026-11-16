#pragma once

#include "uninstall_preview.hpp"
#include "repair_plan.hpp"

namespace control_center {
enum class UninstallPlanState { Ready, PreconditionFailed, NeedsAttention };
struct UninstallPlanResult {
    UninstallPlanState state = UninstallPlanState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    FileIdentity identity{};
    size_t items = 0;
};

inline const char* UninstallStateName(UninstallItemState state) {
    switch (state) {
        case UninstallItemState::AlreadyAbsent: return "absent";
        case UninstallItemState::AlreadyRestored: return "restored";
        case UninstallItemState::DeleteInstalled: return "delete_installed";
        case UninstallItemState::DeleteModified: return "delete_modified";
        case UninstallItemState::RestoreOriginal: return "restore";
        default: return nullptr;
    }
}

// This only publishes metadata; callers must first resolve unfinished install,
// repair, and uninstall transactions. No client member is changed here.
inline UninstallPlanResult PrepareUninstallPlan(const std::wstring& clientRoot, const std::wstring& uninstallRoot,
                                                const std::wstring& uninstallId, const std::vector<std::wstring>& protectedRoots,
                                                const CatalogResult& catalog, const Json& manifest,
                                                const UninstallPreview& approved, const ClientLock& lock) {
    UninstallPlanResult result;
    auto fail = [&result](const char* reason, bool attention = false, DWORD error = ERROR_SUCCESS) {
        result.reason = reason; result.win32Error = error;
        result.state = attention ? UninstallPlanState::NeedsAttention : UninstallPlanState::PreconditionFailed; return result;
    };
    if (!ValidOperationId(uninstallId) || !approved.valid || !approved.canUninstall || !lock.IsFor(clientRoot))
        return fail("invalid approved preview or lock");
    std::wstring client, root;
    GuardedPath rootGuard;
    if (!CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(uninstallRoot, root) ||
        WithinRoot(root, client) || WithinRoot(client, root) || !rootGuard.Open(root, L"__root__"))
        return fail("uninstall root unavailable", false, rootGuard.Error());
    const auto current = PreviewUninstall(client, protectedRoots, catalog, manifest, lock);
    if (!current.valid || !current.canUninstall || !SameIdentity(current.clientRecord, approved.clientRecord) ||
        current.items.size() != approved.items.size() || current.residue.size() != approved.residue.size())
        return fail("uninstall preview changed");
    Json items = Json::array(), residue = Json::array();
    for (size_t i = 0; i < current.items.size(); ++i) {
        const auto& item = current.items[i]; const auto& old = approved.items[i];
        const char* state = UninstallStateName(item.state);
        const bool present = item.state == UninstallItemState::AlreadyRestored || item.state == UninstallItemState::DeleteInstalled ||
            item.state == UninstallItemState::DeleteModified || (item.state == UninstallItemState::RestoreOriginal && item.saveCurrent);
        if (!state || item.state != old.state || item.saveCurrent != old.saveCurrent || !SameTarget(item.entry.target, old.entry.target) ||
            item.entry.sha256 != old.entry.sha256 || item.entry.previousSha256 != old.entry.previousSha256 ||
            item.entry.previouslyPresent != old.entry.previouslyPresent || item.entry.replacesOriginal != old.entry.replacesOriginal ||
            (item.entry.previouslyPresent && !SameTarget(item.entry.backup, old.entry.backup)) ||
            (present && !SameIdentity(item.target, old.target)) ||
            (item.state == UninstallItemState::RestoreOriginal && !SameIdentity(item.backup, old.backup)))
            return fail("uninstall item changed");
        std::string target;
        if (!WideUtf8(item.entry.target, target)) return fail("invalid uninstall target");
        items.push_back({{"target", target}, {"state", state}, {"installed_sha256", item.entry.sha256},
            {"target_identity", present ? JournalIdJson(JournalId(item.target)) : Json(nullptr)},
            {"target_sha256", present ? Json(HexSha256(item.target.sha256)) : Json(nullptr)},
            {"backup_identity", item.state == UninstallItemState::RestoreOriginal ? JournalIdJson(JournalId(item.backup)) : Json(nullptr)},
            {"backup_sha256", item.state == UninstallItemState::RestoreOriginal ? Json(HexSha256(item.backup.sha256)) : Json(nullptr)},
            {"save_current", item.saveCurrent}});
    }
    for (size_t i = 0; i < current.residue.size(); ++i) {
        const auto& item = current.residue[i]; const auto& old = approved.residue[i]; std::string path;
        if (!SameTarget(item.entry.path, old.entry.path) || item.entry.kind != old.entry.kind ||
            item.entry.policy != old.entry.policy || item.matchesManifest != old.matchesManifest || !WideUtf8(item.entry.path, path))
            return fail("uninstall residue changed");
        residue.push_back({{"path", path}, {"kind", item.entry.kind}, {"policy", item.entry.policy}, {"trusted", item.matchesManifest}});
    }
    BY_HANDLE_FILE_INFORMATION clientId{}; std::string id, clientText, catalogHash;
    std::array<std::uint8_t, 32> manifestDigest{};
    if (!lock.Identity(clientId) || !WideUtf8(uninstallId, id) || !WideUtf8(client, clientText) ||
        !CatalogDigest(catalog, catalogHash) || HashBytes(manifest.dump(), manifestDigest) < 0) return fail("cannot bind uninstall session");
    const Json plan = {{"schema", 1}, {"kind", "uninstall"}, {"uninstall_id", id}, {"client", clientText},
        {"client_volume", clientId.dwVolumeSerialNumber}, {"client_index_high", clientId.nFileIndexHigh}, {"client_index_low", clientId.nFileIndexLow},
        {"catalog_sha256", catalogHash}, {"manifest_sha256", HexSha256(manifestDigest)},
        {"client_record_identity", JournalIdJson(JournalId(current.clientRecord))}, {"client_record_sha256", HexSha256(current.clientRecord.sha256)},
        {"items", items}, {"residue", residue}};
    const std::string bytes = plan.dump() + "\n";
    if (bytes.size() > 1024 * 1024) return fail("uninstall plan too large");
    const std::wstring directory = root + L"\\" + uninstallId;
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return fail("cannot create uninstall directory", false, GetLastError());
    GuardedPath temporary, final;
    if (!temporary.Open(directory, L"plan.pending") || !final.Open(directory, L"plan.json")) return fail("uninstall plan path unavailable", true);
    const auto written = WriteNewVerified(temporary, bytes);
    if (!written) return fail("cannot write uninstall plan", true, written.win32Error);
    const auto moved = RenameVerifiedNew(temporary, final, written.identity.sha256, &written.identity);
    if (!moved) return fail("cannot publish uninstall plan", true, moved.win32Error);
    const Json session = {{"schema", 1}, {"kind", "uninstall_session"}, {"uninstall_id", id},
        {"client_volume", clientId.dwVolumeSerialNumber}, {"client_index_high", clientId.nFileIndexHigh},
        {"client_index_low", clientId.nFileIndexLow}, {"catalog_sha256", catalogHash}, {"manifest_sha256", HexSha256(manifestDigest)},
        {"plan_sha256", HexSha256(moved.identity.sha256)}, {"plan_identity", JournalIdJson(JournalId(moved.identity))}};
    GuardedPath sessionPath;
    if (!sessionPath.Open(directory, L"session.json")) return fail("uninstall session path unavailable", true, sessionPath.Error());
    const auto sessionWrite = WriteNewVerified(sessionPath, session.dump() + "\n");
    if (!sessionWrite) return fail("cannot bind uninstall plan", true, sessionWrite.win32Error);
    result.state = UninstallPlanState::Ready; result.identity = moved.identity; result.items = items.size();
    return result;
}
}
