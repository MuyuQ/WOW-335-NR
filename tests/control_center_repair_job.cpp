#define CONTROL_CENTER_TESTING
#include "../native/control_center/repair_job.hpp"
#include <cstdio>

using namespace control_center;
static ProcessCheck Stopped() { ProcessCheck result; result.state = ProcessState::Stopped; return result; }
static bool StopMember(RepairMemberCheckpoint point) { return point == RepairMemberCheckpoint::Published; }
static bool StopCommit(RepairCompleteCheckpoint point) { return point == RepairCompleteCheckpoint::MarkerPublished; }

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1], id = L"0123456789abcdef0123456789abcdef";
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 2;
    const auto hash = HashFile(source.Path());
    if (!hash) return 3;
    CatalogMember member; member.target = L"host64/extra.dll"; member.source = L"real/occupied.bin";
    member.sourceKind = "stage"; member.sha256 = HexSha256(hash.identity.sha256);
    CatalogResult catalog; catalog.members = {member};
    const Json manifest = {{"schema", 1}, {"updated", "fixture"}, {"entries", Json::array({
        {{"target", "host64/extra.dll"}, {"source_kind", "stage"}, {"folder", "host64"}, {"purpose", "fixture"},
         {"replaces_original", false}, {"required", true}}})}};
    const Json entry = {{"target", "host64/extra.dll"}, {"sha256", member.sha256}, {"replaces_original", false},
                        {"pre_state", "absent"}, {"previous_sha256", nullptr}, {"backup", nullptr}};
    for (int mode = 0; mode < 7; ++mode) {
        const auto suffix = std::to_wstring(mode);
        const std::wstring client = root + L"\\repair-job-client-" + suffix, operations = root + L"\\repair-job-native-" + suffix,
            sources = root + L"\\repair-job-legacy-" + suffix, operation = sources + L"\\old-install", repairs = root + L"\\repair-job-repairs-" + suffix,
            uninstalls = root + L"\\repair-job-uninstalls-" + suffix;
        if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(operations.c_str(), nullptr) ||
            !CreateDirectoryW(sources.c_str(), nullptr) || !CreateDirectoryW(operation.c_str(), nullptr) ||
            !CreateDirectoryW(repairs.c_str(), nullptr) || !CreateDirectoryW(uninstalls.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return 4;
        std::string clientText, operationText;
        if (!WideUtf8(client, clientText) || !WideUtf8(operation, operationText)) return 5;
        if (mode == 5 && !WideUtf8(sources + L"\\different-install", operationText)) return 5;
        const Json record = {{"schema", 1}, {"status", "installed"}, {"operation", operationText}, {"entries", Json::array({entry})}};
        const Json transaction = {{"schema", 2}, {"status", "completed"}, {"client", clientText},
                                  {"entries", Json::array({entry})}, {"repair_entries", Json::array({entry})}};
        GuardedPath clientRecord, operationRecord, staged, target;
        if (!clientRecord.Open(client, L"host64/dlss5-install-record.json", true) || !WriteNewVerified(clientRecord, record.dump()) ||
            !operationRecord.Open(operation, L"transaction.json") || !WriteNewVerified(operationRecord, transaction.dump()) ||
            !staged.Open(operation, L"stage/host64/extra.dll", true) || !CopyVerifiedNew(source, staged, hash.identity.sha256) ||
            !target.Open(client, member.target)) return 6;
        FileIdentity original;
        if (mode == 1) {
            const auto copied = CopyVerifiedNew(source, target, hash.identity.sha256);
            if (!copied) return 7;
            const auto actual = HashFile(target.Path());
            if (!actual) return 7;
            original = actual.identity;
        } else {
            const auto written = WriteNewVerified(target, "user modification");
            if (!written) return 7;
            original = written.identity;
        }
        ClientLock lock;
        if (!lock.Acquire(client)) return 8;
        const auto preview = PreviewRepair(client, sources, operation, {}, catalog, lock);
        if (mode == 5) { if (preview.valid) return 18; continue; }
        if (!preview.valid || !preview.canRepair) return 9;
        if (mode == 6) {
            GuardedPath unknown;
            if (!unknown.Open(uninstalls, L"unknown.txt") || !WriteNewVerified(unknown, "unknown")) return 19;
        }
        if (mode == 4) {
            HANDLE file = CreateFileW(target.Path().c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return 10;
            DWORD written = 0; const bool ok = WriteFile(file, "x", 1, &written, nullptr) && written == 1; CloseHandle(file);
            if (!ok) return 11;
        }
        const auto result = RunRepairJob(operations, repairs, uninstalls, sources, operation, id, client, {}, catalog, manifest, preview, lock, &Stopped,
                                         mode == 2 ? &StopMember : nullptr, mode == 3 ? &StopCommit : nullptr);
        const auto current = HashFile(target.Path());
        if (!current) return 12;
        if (mode == 1 && (result.state != RepairJobState::AlreadyCurrent || !SameIdentity(current.identity, original))) return 13;
        if (mode == 2 && (result.state != RepairJobState::RolledBack || !SameIdentity(current.identity, original))) return 14;
        if (mode == 4 && (result.state != RepairJobState::PreconditionFailed || current.identity.sha256 == original.sha256)) return 15;
        if (mode == 6 && (result.state != RepairJobState::NeedsAttention || !SameIdentity(current.identity, original))) return 20;
        if ((mode == 0 || mode == 3) && (result.state != RepairJobState::Completed || current.identity.sha256 != hash.identity.sha256 ||
            result.committedAfterRecovery != (mode == 3))) {
            std::fprintf(stderr, "repair job mode %d: %s %lu\n", mode, result.reason.c_str(), result.win32Error); return 16;
        }
        if (mode == 0 || mode == 3) {
            GuardedPath saved;
            if (!saved.Open(repairs + L"\\" + id, L"modified/host64/extra.dll") ||
                HashFile(saved.Path()).identity.sha256 != original.sha256) return 17;
        }
    }
    std::puts("repair job: legacy source, active record, current, stale preview, rollback and commit recovery passed");
    return 0;
}
