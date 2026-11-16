#define CONTROL_CENTER_TESTING
#include "../native/control_center/coordinated_install.hpp"
#include <cstdio>

using namespace control_center;
static ProcessCheck Stopped() { ProcessCheck result; result.state = ProcessState::Stopped; return result; }
static bool StopRecord(RecordPublishCheckpoint point) { return point == RecordPublishCheckpoint::Published; }
static bool StopCommit(CommitCheckpoint point) { return point == CommitCheckpoint::MarkerPrepared; }
static std::wstring conflictOperation;
static bool conflictCreated = false;
static bool InjectRepairConflict(MemberCheckpoint point) {
    if (point != MemberCheckpoint::Applied) return false;
    GuardedPath path;
    if (path.Open(conflictOperation, L"stage/d3d9.dll", true)) conflictCreated = static_cast<bool>(WriteNewVerified(path, "unknown file"));
    return false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1];
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 2;
    const auto sourceHash = HashFile(source.Path());
    if (!sourceHash) return 3;
    CatalogMember member;
    member.target = L"d3d9.dll"; member.source = L"real/occupied.bin"; member.sourceKind = "stage";
    member.sha256 = HexSha256(sourceHash.identity.sha256); member.replacesOriginal = true;
    CatalogResult catalog; catalog.members.push_back(member);
    const Json manifest = {{"schema", 1}, {"updated", "fixture"}, {"entries", Json::array({
        {{"target", "d3d9.dll"}, {"source_kind", "stage"}, {"folder", "root"}, {"purpose", "fixture"},
         {"replaces_original", true}, {"required", true}}})}};
    for (int mode = 0; mode < 4; ++mode) {
        const std::wstring suffix = std::to_wstring(mode), client = root + L"\\job-client-" + suffix,
                           operations = root + L"\\job-operations-" + suffix, repairs = root + L"\\job-repairs-" + suffix,
                           uninstalls = root + L"\\job-uninstalls-" + suffix;
        if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(operations.c_str(), nullptr) ||
            !CreateDirectoryW(repairs.c_str(), nullptr) || !CreateDirectoryW(uninstalls.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return 4;
        GuardedPath original;
        if (!original.Open(client, L"d3d9.dll")) return 5;
        const auto initial = WriteNewVerified(original, "original");
        if (!initial) return 6;
        MemberInstallContext context;
        context.clientRoot = client; context.stageRoot = root; context.templateRoot = root;
        context.operationId = L"0123456789abcdef0123456789abcdef"; context.journalRoot = operations + L"\\" + context.operationId;
        context.testProcesses = &Stopped;
        ClientLock lock;
        if (!lock.Acquire(client)) return 7;
        const auto preview = PreviewInstall(client, root, root, catalog);
        if (!preview.filesReady) return 8;
        auto suppliedManifest = manifest;
        if (mode == 3) suppliedManifest["entries"][0]["required"] = false;
        InstallJobResult result;
        if (mode == 0) {
            GuardedPath unknown;
            if (!unknown.Open(repairs, L"unexpected.txt") || !WriteNewVerified(unknown, "unknown")) return 24;
            const auto blocked = RunCoordinatedInstall(operations, repairs, uninstalls, operations, context, catalog, manifest, preview, "2026-11-14T00:00:00Z", lock);
            if (blocked.attemptedInstallation || blocked.recovery.ready ||
                !SameIdentity(HashFile(original.Path()).identity, initial.identity) || !DeleteFileW(unknown.Path().c_str())) return 25;
            GuardedPath unknownUninstall;
            if (!unknownUninstall.Open(uninstalls, L"unexpected.txt") || !WriteNewVerified(unknownUninstall, "unknown")) return 27;
            const auto uninstallBlocked = RunCoordinatedInstall(operations, repairs, uninstalls, operations, context, catalog, manifest, preview, "2026-11-14T00:00:00Z", lock);
            if (uninstallBlocked.attemptedInstallation || uninstallBlocked.recovery.ready ||
                !SameIdentity(HashFile(original.Path()).identity, initial.identity) || !DeleteFileW(unknownUninstall.Path().c_str())) return 28;
            const auto coordinated = RunCoordinatedInstall(operations, repairs, uninstalls, operations, context, catalog, manifest, preview, "2026-11-14T00:00:00Z", lock);
            if (!coordinated.attemptedInstallation || !coordinated.recovery.ready) return 26;
            result = coordinated.installation;
        } else {
            result = RunInstallJob(operations, context, catalog, suppliedManifest, preview, "2026-11-14T00:00:00Z", lock,
                                   mode == 1 ? &StopRecord : nullptr, mode == 2 ? &StopCommit : nullptr);
        }
        if (mode == 3) {
            if (result.state != InstallJobState::PreconditionFailed || GetFileAttributesW(context.journalRoot.c_str()) != INVALID_FILE_ATTRIBUTES ||
                !MissingPathError(GetLastError()) || !SameIdentity(HashFile(original.Path()).identity, initial.identity)) return 9;
            continue;
        }
        if (mode == 1) {
            if (result.state != InstallJobState::RolledBack || !SameIdentity(HashFile(original.Path()).identity, initial.identity) ||
                result.failureRecovery.state != PendingRecoveryState::RolledBack) return 10;
            continue;
        }
        if (result.state != InstallJobState::Completed || (mode == 2) != result.committedAfterRecovery) {
            std::fprintf(stderr, "job mode %d: %s %lu\n", mode, result.reason.c_str(), result.win32Error); return 11;
        }
        GuardedPath clientRecord, operationRecord;
        std::string bytes; DWORD error = ERROR_SUCCESS;
        if (!clientRecord.Open(client, L"host64/dlss5-install-record.json") || !ReadJournalBytes(clientRecord, bytes, error) || !ReadClientRecord(bytes) ||
            !operationRecord.Open(context.journalRoot, L"transaction.json") || !ReadJournalBytes(operationRecord, bytes, error) ||
            !ReadOperationRecord(bytes) || HashFile(original.Path()).identity.sha256 != sourceHash.identity.sha256) return 12;
        const auto installed = PreviewInstall(client, root, root, catalog);
        auto repeated = context;
        repeated.operationId = L"abcdef0123456789abcdef0123456789"; repeated.journalRoot = operations + L"\\" + repeated.operationId;
        const auto current = RunInstallJob(operations, repeated, catalog, manifest, installed, "2026-11-14T00:00:01Z", lock);
        if (current.state != InstallJobState::AlreadyCurrent) return 13;
    }
    for (int conflict = 0; conflict < 2; ++conflict) {
        const std::wstring suffix = std::to_wstring(conflict), client = root + L"\\job-supplement-client-" + suffix,
                           operations = root + L"\\job-supplement-operations-" + suffix;
        if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(operations.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return 14;
        GuardedPath existing, prior;
        if (!existing.Open(client, L"d3d9.dll") || !CopyVerifiedNew(source, existing, sourceHash.identity.sha256) ||
            !prior.Open(client, L"host64/dlss5-install-record.json", true)) return 15;
        const Json old = {{"schema", 1}, {"status", "installed"}, {"entries", Json::array({
            {{"target", "d3d9.dll"}, {"sha256", member.sha256}, {"pre_state", "absent"}, {"replaces_original", true},
             {"previous_sha256", nullptr}, {"backup", nullptr}}})}};
        const auto priorWritten = WriteNewVerified(prior, old.dump());
        const auto originalBefore = HashFile(existing.Path());
        if (!priorWritten || !originalBefore) return 16;
        CatalogResult supplementCatalog = catalog;
        CatalogMember extra = member; extra.target = L"host64/extra.dll"; extra.replacesOriginal = false;
        supplementCatalog.members.push_back(extra);
        Json supplementManifest = manifest;
        supplementManifest["entries"].push_back({{"target", "host64/extra.dll"}, {"source_kind", "stage"}, {"folder", "host64"},
            {"purpose", "fixture"}, {"replaces_original", false}, {"required", true}});
        MemberInstallContext context;
        context.clientRoot = client; context.stageRoot = root; context.templateRoot = root;
        context.operationId = L"abcdef0123456789abcdef0123456789"; context.journalRoot = operations + L"\\" + context.operationId;
        context.testProcesses = &Stopped;
        if (conflict) { conflictOperation = context.journalRoot; conflictCreated = false; context.testInterrupt = &InjectRepairConflict; }
        ClientLock lock;
        if (!lock.Acquire(client)) return 17;
        const auto preview = PreviewInstall(client, root, root, supplementCatalog);
        if (!preview.filesReady || preview.items.size() != 2 || preview.items[0].state != PreviewState::Same ||
            preview.items[1].state != PreviewState::Absent) return 18;
        const auto result = RunInstallJob(operations, context, supplementCatalog, supplementManifest, preview, "2026-11-14T00:00:00Z", lock);
        if (conflict) {
            GuardedPath extraPath, conflictPath;
            std::array<std::uint8_t, 32> unknownHash{};
            if (HashBytes("unknown file", unknownHash) < 0) return 23;
            if (!conflictCreated || result.state != InstallJobState::RolledBack || !extraPath.Open(client, L"host64/extra.dll") ||
                GetFileAttributesW(extraPath.Path().c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()) ||
                !conflictPath.Open(context.journalRoot, L"stage/d3d9.dll") || !HashFile(conflictPath.Path()) ||
                HashFile(conflictPath.Path()).identity.sha256 != unknownHash ||
                !SameIdentity(originalBefore.identity, HashFile(existing.Path()).identity) ||
                !SameIdentity(priorWritten.identity, HashFile(prior.Path()).identity)) return 22;
            continue;
        }
        if (result.state != InstallJobState::Completed) {
            std::fprintf(stderr, "supplement job: %s %lu\n", result.reason.c_str(), result.win32Error); return 19;
        }
        GuardedPath repairSource, record;
        std::string bytes; DWORD error = ERROR_SUCCESS;
        if (!repairSource.Open(context.journalRoot, L"stage/d3d9.dll") ||
            HashFile(repairSource.Path()).identity.sha256 != sourceHash.identity.sha256 ||
            !record.Open(client, L"host64/dlss5-install-record.json") || !ReadJournalBytes(record, bytes, error)) return 20;
        const auto installed = ReadClientRecord(bytes);
        if (!installed || installed.entries.size() != 2 || installed.entries[0].previouslyPresent) return 21;
    }
    std::puts("install job: commit, rollback, commit recovery, supplement, manifest mismatch and repeated install passed");
    return 0;
}
