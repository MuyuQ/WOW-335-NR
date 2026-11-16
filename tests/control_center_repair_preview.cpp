#include "../native/control_center/repair_plan.hpp"
#include "../native/control_center/delete_verified.hpp"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    using namespace control_center;
    const std::wstring root = argv[1], client = root + L"\\repair-preview-client", operations = root + L"\\repair-preview-operations",
                       operation = operations + L"\\legacy-operation";
    if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(operations.c_str(), nullptr) ||
        !CreateDirectoryW(operation.c_str(), nullptr) ||
        !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return 2;
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 3;
    const auto hash = HashFile(source.Path());
    if (!hash) return 4;
    CatalogMember config; config.target = L"host64/ReShade.ini"; config.source = L"real/occupied.bin"; config.sourceKind = "stage";
    config.sha256 = HexSha256(hash.identity.sha256);
    CatalogMember binary = config; binary.target = L"host64/extra.dll";
    CatalogResult catalog; catalog.members = {config, binary};
    Json entries = Json::array();
    for (const auto& member : catalog.members) {
        std::string target;
        if (!WideUtf8(member.target, target)) return 5;
        entries.push_back({{"target", target}, {"sha256", member.sha256}, {"replaces_original", false},
                           {"pre_state", "absent"}, {"previous_sha256", nullptr}, {"backup", nullptr}});
    }
    std::string clientUtf8;
    if (!WideUtf8(client, clientUtf8)) return 6;
    const Json installed = {{"schema", 1}, {"status", "installed"}, {"entries", entries}};
    const Json transaction = {{"schema", 2}, {"status", "completed"}, {"client", clientUtf8}, {"entries", entries},
                              {"repair_entries", entries}, {"applied", Json::array()}};
    GuardedPath clientRecord, operationRecord, configFile, staged;
    if (!clientRecord.Open(client, L"host64/dlss5-install-record.json", true) || !WriteNewVerified(clientRecord, installed.dump()) ||
        !operationRecord.Open(operation, L"transaction.json") || !WriteNewVerified(operationRecord, transaction.dump()) ||
        !configFile.Open(client, config.target, true) || !WriteNewVerified(configFile, "user configuration") ||
        !staged.Open(operation, L"stage/host64/extra.dll", true) || !CopyVerifiedNew(source, staged, hash.identity.sha256)) return 7;
    ClientLock lock;
    if (!lock.Acquire(client)) return 8;
    auto preview = PreviewRepair(client, operations, operation, {}, catalog, lock);
    if (!preview.valid || !preview.canRepair || preview.items.size() != 2 ||
        preview.items[0].state != RepairItemState::PreservedConfig || preview.items[1].state != RepairItemState::Missing ||
        preview.items[1].source.sha256 != hash.identity.sha256) return 9;
    const auto approvedMissing = preview;
    const std::wstring repairs = root + L"\\repair-preview-runs", firstId = L"0123456789abcdef0123456789abcdef";
    if (!CreateDirectoryW(repairs.c_str(), nullptr)) return 21;
    const auto plan = PrepareRepairPlan(client, operations, operation, repairs, firstId, {}, catalog, preview, lock);
    if (plan.state != RepairPlanState::Ready || plan.actions != 1) return 22;
    GuardedPath planPath;
    std::string planBytes; DWORD error = ERROR_SUCCESS;
    if (!planPath.Open(repairs + L"\\" + firstId, L"plan.json") || !ReadJournalBytes(planPath, planBytes, error)) return 23;
    const auto planJson = ParseJsonObject(planBytes);
    if (!planJson || planJson.value["actions"].size() != 1 || planJson.value["actions"][0]["target"] != "host64/extra.dll" ||
        planJson.value["preserved"].size() != 1) return 24;
    const auto saved = HashFile(configFile.Path());
    if (!saved || !DeleteFileW(staged.Path().c_str())) return 10;
    preview = PreviewRepair(client, operations, operation, {}, catalog, lock);
    const auto after = HashFile(configFile.Path());
    if (!preview.valid || preview.canRepair || preview.items[0].state != RepairItemState::PreservedConfig ||
        preview.items[1].state != RepairItemState::SourceUnavailable || !after ||
        !SameJournalId(JournalId(saved.identity), JournalId(after.identity)) || saved.identity.sha256 != after.identity.sha256) return 11;
    GuardedPath target;
    if (!CopyVerifiedNew(source, staged, hash.identity.sha256) || !target.Open(client, binary.target, true)) return 14;
    const auto changed = WriteNewVerified(target, "user modified binary");
    if (!changed) return 15;
    preview = PreviewRepair(client, operations, operation, {}, catalog, lock);
    if (!preview.valid || !preview.canRepair || preview.items[1].state != RepairItemState::Modified ||
        !SameJournalId(JournalId(preview.items[1].target), JournalId(changed.identity)) ||
        preview.items[1].target.sha256 != changed.identity.sha256) return 16;
    const std::wstring secondId = L"abcdef0123456789abcdef0123456789";
    if (PrepareRepairPlan(client, operations, operation, repairs, secondId, {}, catalog, approvedMissing, lock).state != RepairPlanState::PreconditionFailed ||
        GetFileAttributesW((repairs + L"\\" + secondId).c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError())) return 25;
    const auto changedPlan = PrepareRepairPlan(client, operations, operation, repairs, secondId, {}, catalog, preview, lock);
    if (changedPlan.state != RepairPlanState::Ready || changedPlan.actions != 1) return 26;
    const std::wstring legacyOperation = operations + L"\\schema1-operation";
    if (!CreateDirectoryW(legacyOperation.c_str(), nullptr)) return 17;
    Json legacyMembers = Json::array();
    for (const auto& member : catalog.members) {
        std::string targetUtf8;
        if (!WideUtf8(member.target, targetUtf8)) return 18;
        legacyMembers.push_back({{"target", targetUtf8}, {"sha256", member.sha256}, {"replaces_original", false}});
    }
    const Json legacy = {{"schema", 1}, {"status", "completed"}, {"client", clientUtf8}, {"members", legacyMembers}};
    GuardedPath legacyRecord, legacySource;
    if (!legacyRecord.Open(legacyOperation, L"transaction.json") || !WriteNewVerified(legacyRecord, legacy.dump()) ||
        !legacySource.Open(legacyOperation, L"stage/host64/extra.dll", true) ||
        !CopyVerifiedNew(source, legacySource, hash.identity.sha256)) return 19;
    const auto oldPreview = PreviewRepair(client, operations, legacyOperation, {}, catalog, lock);
    if (!oldPreview.valid || !oldPreview.canRepair || oldPreview.items.size() != 2 ||
        oldPreview.items[0].state != RepairItemState::PreservedConfig || oldPreview.items[1].state != RepairItemState::Modified) return 20;
    if (!DeleteVerified(target, changed.identity.sha256, &changed.identity) || !CopyVerifiedNew(source, target, hash.identity.sha256)) return 27;
    const auto alreadyCurrent = PreviewRepair(client, operations, operation, {}, catalog, lock);
    const std::wstring thirdId = L"11111111111111111111111111111111";
    if (!alreadyCurrent.valid || PrepareRepairPlan(client, operations, operation, repairs, thirdId, {}, catalog, alreadyCurrent, lock).state != RepairPlanState::NoWork ||
        GetFileAttributesW((repairs + L"\\" + thirdId).c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError())) return 28;
    auto badCatalog = catalog; badCatalog.members[1].sha256 = std::string(64, 'a');
    if (PreviewRepair(client, operations, operation, {}, badCatalog, lock).valid ||
        PreviewRepair(client, operations, root, {}, catalog, lock).valid ||
        PreviewRepair(client, operations, operation, {client}, catalog, lock).valid) return 12;
    auto duplicateCatalog = catalog; duplicateCatalog.members.push_back(binary);
    if (PreviewRepair(client, operations, operation, {}, duplicateCatalog, lock).valid) return 13;
    std::puts("repair preview: legacy record, protected config, missing source, catalog and root checks passed");
    return 0;
}
