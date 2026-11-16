#pragma once

#include "install_member.hpp"
#include "repair_preview.hpp"

namespace control_center {
enum class RepairPlanState { Ready, NoWork, PreconditionFailed, NeedsAttention };
struct RepairPlanResult {
    RepairPlanState state = RepairPlanState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    FileIdentity identity{};
    size_t actions = 0;
};

// The plan only records validated identities; it does not write the client.
// The caller must discover unfinished repair plans before starting a new job.
inline RepairPlanResult PrepareRepairPlan(const std::wstring& clientRoot, const std::wstring& sourceOperationsRoot,
                                          const std::wstring& sourceOperation, const std::wstring& repairOperationsRoot,
                                          const std::wstring& repairId, const std::vector<std::wstring>& protectedRoots,
                                          const CatalogResult& catalog, const RepairPreview& approved, const ClientLock& lock) {
    RepairPlanResult result;
    auto fail = [&result](const char* reason, bool attention = false, DWORD error = ERROR_SUCCESS) {
        result.reason = reason; result.win32Error = error;
        result.state = attention ? RepairPlanState::NeedsAttention : RepairPlanState::PreconditionFailed;
        return result;
    };
    if (!ValidOperationId(repairId) || !approved.valid || !approved.canRepair || !lock.IsFor(clientRoot)) return fail("invalid approved preview or lock");
    std::wstring client, source, repairs;
    if (!CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(sourceOperation, source) ||
        !CanonicalLocalPath(repairOperationsRoot, repairs) || WithinRoot(repairs, client) || WithinRoot(client, repairs) ||
        WithinRoot(repairs, source) || WithinRoot(source, repairs)) return fail("invalid repair roots");
    GuardedPath root;
    if (!root.Open(repairs, L"__root__")) return fail("repair root unavailable", false, root.Error());
    const auto current = PreviewRepair(client, sourceOperationsRoot, source, protectedRoots, catalog, lock);
    if (!current.valid || !current.canRepair || current.items.size() != approved.items.size() ||
        !SameIdentity(current.operationRecord, approved.operationRecord) || !SameIdentity(current.clientRecord, approved.clientRecord))
        return fail("repair preview changed");
    BY_HANDLE_FILE_INFORMATION clientId{};
    std::string id, clientUtf8, sourceUtf8, catalogHash;
    if (!lock.Identity(clientId) || !WideUtf8(repairId, id) || !WideUtf8(client, clientUtf8) || !WideUtf8(source, sourceUtf8) ||
        !CatalogDigest(catalog, catalogHash)) return fail("cannot identify repair session");
    Json actions = Json::array(), preserved = Json::array();
    for (size_t i = 0; i < current.items.size(); ++i) {
        const auto& item = current.items[i]; const auto& expected = approved.items[i];
        if (!SameTarget(item.member.target, expected.member.target) || item.member.sha256 != expected.member.sha256 ||
            item.member.sourceKind != expected.member.sourceKind || !SameTarget(item.member.source, expected.member.source) ||
            item.member.required != expected.member.required || item.member.replacesOriginal != expected.member.replacesOriginal ||
            item.state != expected.state) return fail("repair preview changed");
        if ((item.state == RepairItemState::Current || item.state == RepairItemState::Modified || item.state == RepairItemState::PreservedConfig) &&
            !SameIdentity(item.target, expected.target)) return fail("repair target changed");
        if ((item.state == RepairItemState::Missing || item.state == RepairItemState::Modified) &&
            !SameIdentity(item.source, expected.source)) return fail("repair source changed");
        std::string target;
        if (!WideUtf8(item.member.target, target)) return fail("invalid repair target");
        if (item.state == RepairItemState::PreservedConfig) { preserved.push_back(target); continue; }
        if (item.state != RepairItemState::Missing && item.state != RepairItemState::Modified) continue;
        const bool present = item.state == RepairItemState::Modified;
        actions.push_back({{"target", target}, {"sha256", item.member.sha256}, {"source_identity", JournalIdJson(JournalId(item.source))},
            {"previous_identity", present ? JournalIdJson(JournalId(item.target)) : Json(nullptr)},
            {"previous_sha256", present ? Json(HexSha256(item.target.sha256)) : Json(nullptr)}});
    }
    result.actions = actions.size();
    if (actions.empty()) { result.state = RepairPlanState::NoWork; return result; }
    const Json plan = {{"schema", 1}, {"kind", "repair"}, {"repair_id", id}, {"client", clientUtf8},
        {"client_volume", clientId.dwVolumeSerialNumber}, {"client_index_high", clientId.nFileIndexHigh}, {"client_index_low", clientId.nFileIndexLow},
        {"source_operation", sourceUtf8}, {"catalog_sha256", catalogHash},
        {"operation_record_identity", JournalIdJson(JournalId(current.operationRecord))}, {"operation_record_sha256", HexSha256(current.operationRecord.sha256)},
        {"client_record_identity", JournalIdJson(JournalId(current.clientRecord))}, {"client_record_sha256", HexSha256(current.clientRecord.sha256)},
        {"actions", actions}, {"preserved", preserved}};
    const std::string bytes = plan.dump() + "\n";
    if (bytes.size() > 1024 * 1024) return fail("repair plan too large");
    const std::wstring directory = repairs + L"\\" + repairId;
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return fail("cannot create repair directory", false, GetLastError());
    GuardedPath temporary, final;
    if (!temporary.Open(directory, L"plan.pending") || !final.Open(directory, L"plan.json")) return fail("repair plan path unavailable", true);
    const auto written = WriteNewVerified(temporary, bytes);
    if (!written) return fail("cannot write repair plan", true, written.win32Error);
    const auto moved = RenameVerifiedNew(temporary, final, written.identity.sha256, &written.identity);
    if (!moved) return fail("cannot publish repair plan", true, moved.win32Error);
    const Json session = {{"schema", 1}, {"kind", "repair_session"}, {"repair_id", id},
        {"client_volume", clientId.dwVolumeSerialNumber}, {"client_index_high", clientId.nFileIndexHigh},
        {"client_index_low", clientId.nFileIndexLow}, {"catalog_sha256", catalogHash},
        {"plan_sha256", HexSha256(moved.identity.sha256)}, {"plan_identity", JournalIdJson(JournalId(moved.identity))}};
    GuardedPath sessionPath;
    if (!sessionPath.Open(directory, L"session.json")) return fail("repair session path unavailable", true, sessionPath.Error());
    const auto sessionWrite = WriteNewVerified(sessionPath, session.dump() + "\n");
    if (!sessionWrite) return fail("cannot bind repair plan", true, sessionWrite.win32Error);
    result.identity = moved.identity; result.state = RepairPlanState::Ready;
    return result;
}
}
