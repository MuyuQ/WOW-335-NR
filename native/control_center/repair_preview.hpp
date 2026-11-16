#pragma once

#include "capture_client_record.hpp"
#include "client_identity.hpp"
#include "client_lock.hpp"
#include "operation_record.hpp"

namespace control_center {
enum class RepairItemState { Current, Missing, Modified, PreservedConfig, SourceUnavailable, TargetUnavailable };
struct RepairPreviewItem {
    CatalogMember member;
    RepairItemState state = RepairItemState::TargetUnavailable;
    FileIdentity source{}, target{};
    DWORD win32Error = ERROR_SUCCESS;
};
struct RepairPreview {
    bool valid = false, canRepair = false;
    std::string error;
    DWORD win32Error = ERROR_SUCCESS;
    FileIdentity operationRecord{}, clientRecord{};
    std::vector<RepairPreviewItem> items;
};

inline bool IsProtectedRepairConfig(const std::wstring& target) {
    for (const auto* path : {L"host64/ReShade.ini", L"dlss5-aio-x86.cfg", L"ReShade.ini", L"host64/ReShadePreset.ini"})
        if (SameTarget(target, path)) return true;
    return false;
}

// A repair preview never grants write permission. Callers must pin and recheck
// every identity immediately before a durable, recoverable repair operation.
inline RepairPreview PreviewRepair(const std::wstring& clientRoot, const std::wstring& allowedOperationsRoot,
                                   const std::wstring& operationRoot, const std::vector<std::wstring>& protectedRoots,
                                   const CatalogResult& catalog, const ClientLock& lock) {
    RepairPreview result;
    auto fail = [&result](const char* message, DWORD error = ERROR_SUCCESS) {
        result.error = message; result.win32Error = error; return result;
    };
    if (!catalog || catalog.members.empty() || !lock.IsFor(clientRoot)) return fail("invalid catalog or client lock");
    const auto client = ValidateClient(clientRoot, protectedRoots);
    if (!client) return fail("invalid client", client.win32Error);
    std::wstring allowed, operation;
    if (!CanonicalLocalPath(allowedOperationsRoot, allowed) || !CanonicalLocalPath(operationRoot, operation) ||
        !WithinRoot(operation, allowed) || CompareStringOrdinal(operation.c_str(), -1, allowed.c_str(), -1, TRUE) == CSTR_EQUAL ||
        WithinRoot(operation, client.root) || WithinRoot(client.root, operation)) return fail("invalid operation path");
    GuardedPath rootGuard, recordPath;
    if (!rootGuard.Open(allowed, L"__root__") || !recordPath.Open(operation, L"transaction.json"))
        return fail("operation record path unavailable", recordPath.Error() ? recordPath.Error() : rootGuard.Error());
    std::string bytes; DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(recordPath, bytes, error)) return fail("operation record unavailable", error);
    const auto operationHash = HashFile(recordPath.Path());
    std::array<std::uint8_t, 32> digest{};
    if (!operationHash || HashBytes(bytes, digest) < 0 || operationHash.identity.sha256 != digest) return fail("operation record changed");
    const auto operationRecord = ReadOperationRecord(bytes);
    if (!operationRecord || operationRecord.status != "completed" ||
        CompareStringOrdinal(operationRecord.client.c_str(), -1, client.root.c_str(), -1, TRUE) != CSTR_EQUAL)
        return fail("operation record does not match client");
    const auto installed = CaptureClientRecord(client.root);
    if (installed.state != CapturedRecordState::Present) return fail("client installation record unavailable", installed.win32Error);
    const auto active = installed.record.original.find("operation");
    if (active != installed.record.original.end()) {
        std::wstring recorded, canonical;
        if (!active->is_string() || !Utf8Wide(active->get<std::string>(), recorded) || !CanonicalLocalPath(recorded, canonical) ||
            CompareStringOrdinal(canonical.c_str(), -1, operation.c_str(), -1, TRUE) != CSTR_EQUAL)
            return fail("repair source is not the active installation");
    }
    result.operationRecord = operationHash.identity; result.clientRecord = installed.identity;
    std::vector<InstalledEntry> entries = operationRecord.entries;
    const auto repairEntries = operationRecord.original.find("repair_entries");
    if (repairEntries != operationRecord.original.end() && !repairEntries->is_null()) {
        if (!repairEntries->is_array() || repairEntries->empty()) return fail("invalid repair entries");
        const Json wrapper = {{"schema", 1}, {"status", "installed"}, {"entries", *repairEntries}};
        const auto parsed = ReadClientRecord(wrapper.dump());
        if (!parsed) return fail("invalid repair entries");
        entries = parsed.entries;
    }
    for (const auto& entry : entries) {
        const CatalogMember* match = nullptr;
        for (const auto& member : catalog.members) if (SameTarget(member.target, entry.target)) {
            if (match) return fail("duplicate catalog target");
            match = &member;
        }
        if (!match || match->sha256 != entry.sha256 || match->replacesOriginal != entry.replacesOriginal)
            return fail("repair entries do not match trusted catalog");
        bool owned = false;
        for (const auto& current : installed.record.entries)
            if (SameTarget(current.target, entry.target) && current.sha256 == entry.sha256 &&
                current.replacesOriginal == entry.replacesOriginal) { owned = true; break; }
        if (!owned) return fail("repair entry is not present in client record");
        RepairPreviewItem item; item.member = *match;
        GuardedPath target;
        if (!target.Open(client.root, entry.target)) {
            if (MissingPathError(target.Error())) item.state = RepairItemState::Missing;
            else { item.state = RepairItemState::TargetUnavailable; item.win32Error = target.Error(); }
        } else {
            const auto current = HashFile(target.Path());
            if (!current) {
                if (current.error == HashError::Open && MissingPathError(current.win32Error)) item.state = RepairItemState::Missing;
                else { item.state = RepairItemState::TargetUnavailable; item.win32Error = current.win32Error; }
            } else {
                item.target = current.identity;
                if (HexSha256(current.identity.sha256) == match->sha256) item.state = RepairItemState::Current;
                else item.state = IsProtectedRepairConfig(entry.target) ? RepairItemState::PreservedConfig : RepairItemState::Modified;
            }
        }
        if (item.state == RepairItemState::Missing || item.state == RepairItemState::Modified) {
            GuardedPath source;
            if (!source.Open(operation, L"stage/" + entry.target)) {
                item.state = RepairItemState::SourceUnavailable; item.win32Error = source.Error();
            } else {
                const auto sourceHash = HashFile(source.Path());
                if (!sourceHash || HexSha256(sourceHash.identity.sha256) != match->sha256) {
                    item.state = RepairItemState::SourceUnavailable; item.win32Error = sourceHash.win32Error;
                } else item.source = sourceHash.identity;
            }
        }
        result.items.push_back(std::move(item));
    }
    result.valid = true; result.canRepair = true;
    for (const auto& item : result.items)
        if (item.state == RepairItemState::SourceUnavailable || item.state == RepairItemState::TargetUnavailable) result.canRepair = false;
    return result;
}
}
