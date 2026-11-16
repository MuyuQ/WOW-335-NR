#define CONTROL_CENTER_TESTING
#include "../native/control_center/repair_job.hpp"
#include <cstdio>

using namespace control_center;
static ProcessCheck Stopped() { ProcessCheck result; result.state = ProcessState::Stopped; return result; }
static int testMode = 0, appliedCount = 0, publishedCount = 0;
static std::wstring sourceToMove, movedSource, isolatedOriginal;
static bool moved = false, isolatedChanged = false;
static bool Interrupt(RepairMemberCheckpoint point) {
    if (point == RepairMemberCheckpoint::Applied && ++appliedCount == 1 && testMode == 2)
        moved = MoveFileW(sourceToMove.c_str(), movedSource.c_str()) != FALSE;
    if (point == RepairMemberCheckpoint::Published && ++publishedCount == 2 && testMode == 1) return true;
    return false;
}
static bool ChangeIsolated(RepairCompleteCheckpoint point) {
    if (testMode != 3 || point != RepairCompleteCheckpoint::JournalCompleted) return false;
    HANDLE file = CreateFileW(isolatedOriginal.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD bytes = 0; isolatedChanged = WriteFile(file, "x", 1, &bytes, nullptr) && bytes == 1;
    CloseHandle(file);
    return false;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1], id = L"fedcba9876543210fedcba9876543210";
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 2;
    const auto hash = HashFile(source.Path());
    if (!hash) return 3;
    CatalogResult catalog; Json manifestEntries = Json::array(), entries = Json::array();
    for (const auto* name : {"one.dll", "two.dll"}) {
        const std::string path = std::string("host64/") + name;
        CatalogMember member; if (!Utf8Wide(path, member.target)) return 4;
        member.source = L"real/occupied.bin"; member.sourceKind = "stage"; member.sha256 = HexSha256(hash.identity.sha256);
        catalog.members.push_back(member);
        manifestEntries.push_back({{"target", path}, {"source_kind", "stage"}, {"folder", "host64"}, {"purpose", "fixture"},
                                   {"replaces_original", false}, {"required", true}});
        entries.push_back({{"target", path}, {"sha256", member.sha256}, {"replaces_original", false},
                           {"pre_state", "absent"}, {"previous_sha256", nullptr}, {"backup", nullptr}});
    }
    const Json manifest = {{"schema", 1}, {"updated", "fixture"}, {"entries", manifestEntries}};
    for (testMode = 0; testMode < 5; ++testMode) {
        appliedCount = publishedCount = 0; moved = false; isolatedChanged = false;
        const auto suffix = std::to_wstring(testMode);
        const std::wstring client = root + L"\\repair-multi-client-" + suffix, operations = root + L"\\repair-multi-native-" + suffix,
            sources = root + L"\\repair-multi-legacy-" + suffix, operation = sources + L"\\old-install", repairs = root + L"\\repair-multi-repairs-" + suffix,
            uninstalls = root + L"\\repair-multi-uninstalls-" + suffix;
        if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(operations.c_str(), nullptr) ||
            !CreateDirectoryW(sources.c_str(), nullptr) || !CreateDirectoryW(operation.c_str(), nullptr) ||
            !CreateDirectoryW(repairs.c_str(), nullptr) || !CreateDirectoryW(uninstalls.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return 5;
        std::string clientText, operationText;
        if (!WideUtf8(client, clientText) || !WideUtf8(operation, operationText)) return 6;
        const Json record = {{"schema", 1}, {"status", "installed"}, {"operation", operationText}, {"entries", entries}};
        const Json transaction = {{"schema", 2}, {"status", "completed"}, {"client", clientText}, {"entries", entries}, {"repair_entries", entries}};
        GuardedPath clientRecord, operationRecord;
        if (!clientRecord.Open(client, L"host64/dlss5-install-record.json", true) || !WriteNewVerified(clientRecord, record.dump()) ||
            !operationRecord.Open(operation, L"transaction.json") || !WriteNewVerified(operationRecord, transaction.dump())) return 7;
        std::vector<FileIdentity> originals;
        for (size_t index = 0; index < catalog.members.size(); ++index) {
            GuardedPath staged, target;
            if (!staged.Open(operation, L"stage/" + catalog.members[index].target, true) ||
                !CopyVerifiedNew(source, staged, hash.identity.sha256) || !target.Open(client, catalog.members[index].target)) return 8;
            const auto written = WriteNewVerified(target, index ? "second user modification" : "first user modification");
            if (!written) return 9;
            originals.push_back(written.identity);
        }
        sourceToMove = operation + L"\\stage\\host64\\two.dll"; movedSource = sourceToMove + L".removed";
        isolatedOriginal = client + L"\\host64\\one.dll.nr-repair-" + id + L".original";
        ClientLock lock;
        if (!lock.Acquire(client)) return 10;
        const auto preview = PreviewRepair(client, sources, operation, {}, catalog, lock);
        if (!preview.valid || !preview.canRepair || preview.items.size() != 2) return 11;
        if (testMode == 4) {
            if (PrepareRepairPlan(client, sources, operation, repairs, id, {}, catalog, preview, lock).state != RepairPlanState::Ready ||
                ApplyRepairMember(client, sources, operation, repairs, id, {}, catalog, 0, lock, &Stopped).state != RepairMemberState::Applied ||
                ApplyRepairMember(client, sources, operation, repairs, id, {}, catalog, 1, lock, &Stopped).state != RepairMemberState::Applied ||
                CleanupCommittedRepair(client, sources, operation, repairs, id, {}, catalog, lock, &Stopped).completed ||
                GetFileAttributesW(isolatedOriginal.c_str()) == INVALID_FILE_ATTRIBUTES ||
                CompleteRepair(client, sources, operation, repairs, id, {}, catalog, lock, &Stopped).state != RepairCompleteState::Completed) return 18;
            const auto recovery = RecoverPendingRepairs(repairs, client, sources, {}, catalog, lock, &Stopped);
            if (recovery.state != PendingRepairState::NoWork || recovery.cleanupPending || recovery.cleanedOriginals != 2 ||
                GetFileAttributesW(isolatedOriginal.c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError())) return 19;
            continue;
        }
        const auto result = RunRepairJob(operations, repairs, uninstalls, sources, operation, id, client, {}, catalog, manifest, preview, lock, &Stopped, &Interrupt, &ChangeIsolated);
        if (((testMode == 0 || testMode == 3) && (result.state != RepairJobState::Completed || result.repaired != 2)) ||
            ((testMode == 1 || testMode == 2) && result.state != RepairJobState::RolledBack) || (testMode == 2 && !moved) ||
            (testMode == 0 && (result.cleanupPending || result.cleanedOriginals != 2)) ||
            (testMode == 3 && (!isolatedChanged || !result.cleanupPending))) {
            std::fprintf(stderr, "multi repair mode %d: state=%d reason=%s error=%lu\n", testMode,
                         static_cast<int>(result.state), result.reason.c_str(), result.win32Error); return 12;
        }
        for (size_t index = 0; index < catalog.members.size(); ++index) {
            GuardedPath target, backup;
            if (!target.Open(client, catalog.members[index].target)) return 13;
            const auto actual = HashFile(target.Path());
            if (!actual || ((testMode == 0 || testMode == 3) ? actual.identity.sha256 != hash.identity.sha256 : !SameIdentity(actual.identity, originals[index]))) return 14;
            if ((testMode != 2 || index == 0) && (!backup.Open(repairs + L"\\" + id, L"modified/" + catalog.members[index].target) ||
                HashFile(backup.Path()).identity.sha256 != originals[index].sha256)) return 15;
        }
        if (testMode == 0 && (GetFileAttributesW(isolatedOriginal.c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()) ||
            !CleanupCommittedRepair(client, sources, operation, repairs, id, {}, catalog, lock, &Stopped).completed)) return 16;
        if (testMode == 3 && GetFileAttributesW(isolatedOriginal.c_str()) == INVALID_FILE_ATTRIBUTES) return 17;
    }
    std::puts("repair multi: rollback, missing source, cleanup retry and changed-original preservation passed");
    return 0;
}
