#pragma once

#include "repair_plan.hpp"
#include "recover_operation.hpp"

namespace control_center {
struct RepairAction {
    CatalogMember member;
    FileIdentity source{}, previous{};
    bool previouslyPresent = false;
};
struct TrustedRepairPlan {
    bool valid = false;
    std::string error;
    DWORD win32Error = ERROR_SUCCESS;
    FileIdentity identity{};
    std::vector<RepairAction> actions;
};

// The persisted plan is untrusted input. A target is authorized only when it
// also belongs to the selected client record, source record and trusted catalog.
inline TrustedRepairPlan ReadTrustedRepairPlan(const std::wstring& clientRoot, const std::wstring& sourceOperationsRoot,
                                                const std::wstring& sourceOperation, const std::wstring& repairsRoot,
                                                const std::wstring& repairId, const std::vector<std::wstring>& protectedRoots,
                                                const CatalogResult& catalog, const ClientLock& lock) {
    TrustedRepairPlan result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) { result.error = reason; result.win32Error = error; return result; };
    if (!ValidOperationId(repairId) || !lock.IsFor(clientRoot) || !catalog) return fail("invalid context or lock");
    std::wstring client, source, repairs;
    if (!CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(sourceOperation, source) || !CanonicalLocalPath(repairsRoot, repairs) ||
        WithinRoot(repairs, client) || WithinRoot(client, repairs) || WithinRoot(repairs, source) || WithinRoot(source, repairs))
        return fail("invalid roots");
    GuardedPath rootGuard, planPath;
    if (!rootGuard.Open(repairs, L"__root__") || !planPath.Open(repairs + L"\\" + repairId, L"plan.json")) return fail("plan path unavailable");
    std::string bytes; DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(planPath, bytes, error)) return fail("plan unavailable", error);
    const auto parsed = ParseJsonObject(bytes);
    const auto actual = HashFile(planPath.Path());
    std::array<std::uint8_t, 32> bytesHash{};
    if (!parsed || !actual || HashBytes(bytes, bytesHash) < 0 || bytesHash != actual.identity.sha256) return fail("plan invalid or changed");
    std::string id, clientText, sourceText, digest, planId, planClient, planSource, planDigest, kind;
    BY_HANDLE_FILE_INFORMATION clientId{};
    if (!lock.Identity(clientId) || !WideUtf8(repairId, id) || !WideUtf8(client, clientText) || !WideUtf8(source, sourceText) ||
        !CatalogDigest(catalog, digest) || !StringField(parsed.value, "repair_id", planId) || planId != id ||
        !StringField(parsed.value, "kind", kind) || kind != "repair" || !StringField(parsed.value, "client", planClient) || planClient != clientText ||
        !StringField(parsed.value, "source_operation", planSource) || planSource != sourceText ||
        !StringField(parsed.value, "catalog_sha256", planDigest) || planDigest != digest ||
        !parsed.value.contains("schema") || parsed.value["schema"] != 1 ||
        !parsed.value.contains("client_volume") || parsed.value["client_volume"] != clientId.dwVolumeSerialNumber ||
        !parsed.value.contains("client_index_high") || parsed.value["client_index_high"] != clientId.nFileIndexHigh ||
        !parsed.value.contains("client_index_low") || parsed.value["client_index_low"] != clientId.nFileIndexLow) return fail("plan binding mismatch");
    GuardedPath sessionPath;
    std::string sessionBytes;
    if (!sessionPath.Open(repairs + L"\\" + repairId, L"session.json") || !ReadJournalBytes(sessionPath, sessionBytes, error))
        return fail("repair session unavailable", error);
    const auto session = ParseJsonObject(sessionBytes);
    const Json expectedSession = {{"schema", 1}, {"kind", "repair_session"}, {"repair_id", id},
        {"client_volume", clientId.dwVolumeSerialNumber}, {"client_index_high", clientId.nFileIndexHigh},
        {"client_index_low", clientId.nFileIndexLow}, {"catalog_sha256", digest},
        {"plan_sha256", HexSha256(actual.identity.sha256)}, {"plan_identity", JournalIdJson(JournalId(actual.identity))}};
    if (!session || session.value != expectedSession) return fail("repair session mismatch");
    const auto preview = PreviewRepair(client, sourceOperationsRoot, source, protectedRoots, catalog, lock);
    if (!preview.valid) return fail("source or client record unavailable", preview.win32Error);
    JournalFileId operationId{}, recordId{};
    std::string operationHash, recordHash;
    if (!parsed.value.contains("operation_record_identity") || !DecodeJournalId(parsed.value["operation_record_identity"], operationId) ||
        !parsed.value.contains("client_record_identity") || !DecodeJournalId(parsed.value["client_record_identity"], recordId) ||
        !StringField(parsed.value, "operation_record_sha256", operationHash) || !StringField(parsed.value, "client_record_sha256", recordHash) ||
        !SameJournalId(operationId, JournalId(preview.operationRecord)) || operationHash != HexSha256(preview.operationRecord.sha256) ||
        !SameJournalId(recordId, JournalId(preview.clientRecord)) || recordHash != HexSha256(preview.clientRecord.sha256))
        return fail("source or client record changed");
    const auto actions = parsed.value.find("actions"), preserved = parsed.value.find("preserved");
    if (actions == parsed.value.end() || !actions->is_array() || actions->empty() ||
        preserved == parsed.value.end() || !preserved->is_array()) return fail("invalid plan actions");
    for (const auto& raw : *actions) {
        std::string targetText, hashText;
        std::wstring target;
        JournalFileId sourceId{}, previousId{};
        if (!raw.is_object() || raw.size() != 5 || !StringField(raw, "target", targetText) || !Utf8Wide(targetText, target) ||
            !StringField(raw, "sha256", hashText) || !raw.contains("source_identity") || !DecodeJournalId(raw["source_identity"], sourceId) ||
            !raw.contains("previous_identity") || !raw.contains("previous_sha256")) return fail("invalid plan action");
        const RepairPreviewItem* known = nullptr;
        for (const auto& item : preview.items) if (SameTarget(item.member.target, target)) { if (known) return fail("duplicate target"); known = &item; }
        if (!known || known->member.sha256 != hashText) return fail("untrusted repair target");
        for (const auto& prior : result.actions) if (SameTarget(prior.member.target, target)) return fail("duplicate action");
        RepairAction action; action.member = known->member; action.source = FileIdFromJournal(sourceId);
        if (!ParseSha256Bytes(hashText, action.source.sha256)) return fail("invalid candidate hash");
        if (raw["previous_identity"].is_null() && raw["previous_sha256"].is_null()) action.previouslyPresent = false;
        else {
            std::string previousHash;
            if (IsProtectedRepairConfig(target) || !DecodeJournalId(raw["previous_identity"], previousId) ||
                !StringField(raw, "previous_sha256", previousHash) || previousHash == hashText ||
                !ParseSha256Bytes(previousHash, action.previous.sha256)) return fail("invalid previous identity");
            action.previouslyPresent = true;
            const auto previousBytes = action.previous.sha256;
            action.previous = FileIdFromJournal(previousId); action.previous.sha256 = previousBytes;
        }
        result.actions.push_back(std::move(action));
    }
    for (const auto& raw : *preserved) {
        std::wstring target;
        if (!raw.is_string() || !Utf8Wide(raw.get<std::string>(), target) || !IsProtectedRepairConfig(target)) return fail("invalid preserved target");
        bool found = false;
        for (const auto& item : preview.items) if (SameTarget(item.member.target, target)) { found = true; break; }
        if (!found) return fail("unknown preserved target");
    }
    result.identity = actual.identity; result.valid = true;
    return result;
}
}
