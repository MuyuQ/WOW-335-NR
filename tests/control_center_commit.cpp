#define CONTROL_CENTER_TESTING
#include "../native/control_center/commit_install.hpp"
#include "../native/control_center/publish_client_record.hpp"
#include "../native/control_center/recover_client_record.hpp"
#include "../native/control_center/scan_operations.hpp"
#include "../native/control_center/start_install.hpp"
#include "../native/control_center/recover_pending.hpp"
#include "../native/control_center/start_with_recovery.hpp"
#include <cstdio>

using namespace control_center;
static CommitCheckpoint stopPoint;
static bool Interrupt(CommitCheckpoint point) { return point == stopPoint; }
static bool InterruptRecord(RecordPublishCheckpoint point) { return point == RecordPublishCheckpoint::Published; }
static ProcessCheck Stopped() { ProcessCheck result; result.state = ProcessState::Stopped; return result; }
static HANDLE readyEvent = nullptr, releaseEvent = nullptr;
static bool CrashAt(CommitCheckpoint point) {
    if (point != stopPoint) return false;
    SetEvent(readyEvent);
    WaitForSingleObject(releaseEvent, 30000);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    const bool child = argc == 9 && std::wstring(argv[1]) == L"--child";
    if (!child && argc != 2) return 1;
    const std::wstring root = child ? argv[2] : argv[1];
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 2;
    const auto hash = HashFile(source.Path());
    if (!hash) return 3;
    CatalogMember member;
    member.target = L"d3d9.dll"; member.source = L"real/occupied.bin"; member.sourceKind = "stage";
    member.sha256 = HexSha256(hash.identity.sha256); member.replacesOriginal = true;
    CatalogResult catalog; catalog.members.push_back(member);
    const Json manifest = {{"schema", 1}, {"updated", "fixture"}, {"entries", Json::array({
        {{"target", "d3d9.dll"}, {"source_kind", "stage"}, {"folder", "root"}, {"purpose", "fixture"},
         {"replaces_original", true}, {"required", true}}})}};
    const Json old = {{"schema", 1}, {"status", "installed"}, {"entries", Json::array({
        {{"target", "host64/extra.dll"}, {"sha256", std::string(64, 'a')}, {"pre_state", "absent"},
         {"replaces_original", false}, {"previous_sha256", nullptr}, {"backup", nullptr}}})}};
    if (child) {
        MemberInstallContext context;
        context.clientRoot = argv[3]; context.journalRoot = argv[4]; context.stageRoot = argv[5];
        context.templateRoot = context.stageRoot; context.operationId = L"00112233445566778899aabbccddeeff";
        context.testProcesses = &Stopped;
        stopPoint = static_cast<CommitCheckpoint>(_wtoi(argv[6]));
        readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[7]);
        releaseEvent = OpenEventW(SYNCHRONIZE, FALSE, argv[8]);
        if (!readyEvent || !releaseEvent) return 21;
        ClientLock lock;
        if (!lock.Acquire(context.clientRoot)) return 22;
        (void)CommitInstall(context, catalog, manifest, lock, &CrashAt);
        return 23;
    }
    const CommitCheckpoint points[] = {CommitCheckpoint::MarkerPrepared, CommitCheckpoint::MarkerPublished,
        CommitCheckpoint::TransactionPrepared, CommitCheckpoint::TransactionPublished,
        CommitCheckpoint::RecordCompleted, CommitCheckpoint::MembersCompleted};
    int serial = 0;
    for (const bool hasPrior : {false, true}) for (const auto point : points) {
        ++serial;
        MemberInstallContext context;
        context.clientRoot = root + L"\\commit-client-" + std::to_wstring(serial);
        context.journalRoot = root + L"\\commit-journal-" + std::to_wstring(serial);
        context.stageRoot = root + L"\\commit-stage-" + std::to_wstring(serial);
        context.templateRoot = context.stageRoot; context.operationId = L"00112233445566778899aabbccddeeff";
        context.testProcesses = &Stopped;
        if (!CreateDirectoryW(context.clientRoot.c_str(), nullptr) || !CreateDirectoryW(context.journalRoot.c_str(), nullptr) ||
            !CreateDirectoryW(context.stageRoot.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (context.clientRoot + L"\\wow.exe").c_str(), TRUE)) return 4;
        {
            GuardedPath stage;
            if (!stage.Open(context.stageRoot, L"real/occupied.bin", true) || !CopyVerifiedNew(source, stage, hash.identity.sha256)) return 5;
        }
        if (hasPrior) {
            GuardedPath prior;
            if (!prior.Open(context.clientRoot, L"host64/dlss5-install-record.json", true) || !WriteNewVerified(prior, old.dump())) return 6;
        }
        ClientLock lock;
        if (!lock.Acquire(context.clientRoot)) return 7;
        const auto preview = PreviewInstall(context.clientRoot, context.stageRoot, context.templateRoot, catalog);
        if (!preview.filesReady || InstallMember(context, catalog, preview.items[0], lock).state != MemberInstallState::Applied ||
            PublishClientRecord(context, catalog, manifest, lock, "2026-11-14T00:00:00Z").state != RecordPublishState::Published) return 8;
        const std::wstring movedStage = context.stageRoot + L"-removed";
        if (!MoveFileW(context.stageRoot.c_str(), movedStage.c_str())) return 9;
        stopPoint = point;
        const auto first = CommitInstall(context, catalog, manifest, lock, &Interrupt);
        if (first.state != CommitState::RecoveryRequired) return 10;
        if (RecoverOperation(context, catalog, lock).state != RecoveryState::RecoveryRequired) return 11;
        if (!hasPrior && point == CommitCheckpoint::MarkerPrepared) {
            GuardedPath pending;
            if (!pending.Open(context.journalRoot, L"commit.pending")) return 14;
            std::string saved; DWORD error = ERROR_SUCCESS;
            if (!ReadJournalBytes(pending, saved, error)) return 15;
            HANDLE file = CreateFileW(pending.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return 16;
            DWORD count = 0;
            if (!WriteFile(file, "user", 4, &count, nullptr) || count != 4) { CloseHandle(file); return 17; }
            CloseHandle(file);
            const auto changed = HashFile(pending.Path());
            if (!changed || CommitInstall(context, catalog, manifest, lock).state != CommitState::RecoveryRequired ||
                !SameIdentity(changed.identity, HashFile(pending.Path()).identity)) return 18;
            file = CreateFileW(pending.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return 19;
            count = 0;
            if (!WriteFile(file, saved.data(), static_cast<DWORD>(saved.size()), &count, nullptr) || static_cast<size_t>(count) != saved.size()) {
                CloseHandle(file); return 20;
            }
            CloseHandle(file);
        }
        const auto resumed = CommitInstall(context, catalog, manifest, lock);
        if (resumed.state != CommitState::Completed) {
            std::fprintf(stderr, "commit prior=%d point=%d: %s %lu\n", hasPrior, static_cast<int>(point), resumed.reason.c_str(), resumed.win32Error); return 12;
        }
        const auto again = CommitInstall(context, catalog, manifest, lock);
        const auto members = LoadJournal(context.journalRoot), record = LoadJournal(context.journalRoot + L"\\record-publication");
        if (again.state != CommitState::Completed || !members || !record ||
            members.cursor.TerminalKind() != JournalKind::Complete || record.cursor.TerminalKind() != JournalKind::Complete) return 13;
    }
    {
        ++serial;
        MemberInstallContext context;
        context.clientRoot = root + L"\\commit-client-" + std::to_wstring(serial);
        context.journalRoot = root + L"\\commit-journal-" + std::to_wstring(serial);
        context.stageRoot = root + L"\\commit-stage-" + std::to_wstring(serial);
        context.templateRoot = context.stageRoot; context.operationId = L"00112233445566778899aabbccddeeff";
        context.testProcesses = &Stopped;
        if (!CreateDirectoryW(context.clientRoot.c_str(), nullptr) || !CreateDirectoryW(context.journalRoot.c_str(), nullptr) ||
            !CreateDirectoryW(context.stageRoot.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (context.clientRoot + L"\\wow.exe").c_str(), TRUE)) return 35;
        {
            GuardedPath stage;
            if (!stage.Open(context.stageRoot, L"real/occupied.bin", true) || !CopyVerifiedNew(source, stage, hash.identity.sha256)) return 36;
        }
        ClientLock lock;
        if (!lock.Acquire(context.clientRoot)) return 37;
        const auto preview = PreviewInstall(context.clientRoot, context.stageRoot, context.templateRoot, catalog);
        if (!preview.filesReady || InstallMember(context, catalog, preview.items[0], lock).state != MemberInstallState::Applied ||
            PublishClientRecord(context, catalog, manifest, lock, "2026-11-14T00:00:00Z", &InterruptRecord).state != RecordPublishState::RecoveryRequired) return 38;
        if (!MoveFileW(context.stageRoot.c_str(), (context.stageRoot + L"-removed").c_str())) return 39;
        const auto record = RecoverClientRecord(context, catalog, manifest, lock);
        if (record.state != RecordRecoveryState::Recovered || RecoverOperation(context, catalog, lock).state != RecoveryState::Recovered) {
            std::fprintf(stderr, "record recovery without source: %s %lu\n", record.reason.c_str(), record.win32Error); return 40;
        }
    }
    const CommitCheckpoint crashPoints[] = {CommitCheckpoint::MarkerPrepared, CommitCheckpoint::MarkerPublished,
        CommitCheckpoint::TransactionPrepared, CommitCheckpoint::RecordCompleted};
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 24;
    for (size_t i = 0; i < sizeof(crashPoints) / sizeof(crashPoints[0]); ++i) {
        ++serial;
        MemberInstallContext context;
        context.clientRoot = root + L"\\commit-client-" + std::to_wstring(serial);
        context.journalRoot = root + L"\\commit-journal-" + std::to_wstring(serial);
        context.stageRoot = root + L"\\commit-stage-" + std::to_wstring(serial);
        context.templateRoot = context.stageRoot; context.operationId = L"00112233445566778899aabbccddeeff";
        context.testProcesses = &Stopped;
        if (!CreateDirectoryW(context.clientRoot.c_str(), nullptr) || !CreateDirectoryW(context.journalRoot.c_str(), nullptr) ||
            !CreateDirectoryW(context.stageRoot.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (context.clientRoot + L"\\wow.exe").c_str(), TRUE)) return 25;
        {
            GuardedPath stage, prior;
            if (!stage.Open(context.stageRoot, L"real/occupied.bin", true) || !CopyVerifiedNew(source, stage, hash.identity.sha256) ||
                !prior.Open(context.clientRoot, L"host64/dlss5-install-record.json", true) || !WriteNewVerified(prior, old.dump())) return 26;
        }
        ClientLock lock;
        if (!lock.Acquire(context.clientRoot)) return 27;
        const auto preview = PreviewInstall(context.clientRoot, context.stageRoot, context.templateRoot, catalog);
        if (!preview.filesReady || InstallMember(context, catalog, preview.items[0], lock).state != MemberInstallState::Applied ||
            PublishClientRecord(context, catalog, manifest, lock, "2026-11-14T00:00:00Z").state != RecordPublishState::Published) return 28;
        if (!MoveFileW(context.stageRoot.c_str(), (context.stageRoot + L"-removed").c_str())) return 29;
        lock.Release();
        const std::wstring eventName = L"Local\\WOW335-commit-crash-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i);
        const std::wstring releaseName = eventName + L"-release";
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        if (!ready || !release) { if (ready) CloseHandle(ready); if (release) CloseHandle(release); return 30; }
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --child \"" + root + L"\" \"" + context.clientRoot +
            L"\" \"" + context.journalRoot + L"\" \"" + context.stageRoot + L"\" " +
            std::to_wstring(static_cast<int>(crashPoints[i])) + L" " + eventName + L" " + releaseName;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            CloseHandle(ready); CloseHandle(release); return 31;
        }
        HANDLE waits[] = {ready, process.hProcess};
        const DWORD observed = WaitForMultipleObjects(2, waits, FALSE, 20000);
        const BOOL terminated = TerminateProcess(process.hProcess, 99);
        if (!terminated) SetEvent(release);
        const DWORD finished = WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(ready); CloseHandle(release);
        if (observed != WAIT_OBJECT_0 || !terminated || finished != WAIT_OBJECT_0 || !lock.Acquire(context.clientRoot)) return 32;
        if (RecoverOperation(context, catalog, lock).state != RecoveryState::RecoveryRequired) return 33;
        const auto resumed = CommitInstall(context, catalog, manifest, lock);
        if (resumed.state != CommitState::Completed) {
            std::fprintf(stderr, "real commit crash %d: %s %lu\n", static_cast<int>(crashPoints[i]), resumed.reason.c_str(), resumed.win32Error); return 34;
        }
    }
    {
        const std::wstring client = root + L"\\optional-client", operation = root + L"\\optional-operation";
        if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(operation.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return 55;
        CatalogResult optionalCatalog = catalog;
        CatalogMember optional; optional.target = L"host64/optional.dll"; optional.source = L"missing/optional.dll";
        optional.sourceKind = "stage"; optional.sha256 = std::string(64, 'a'); optional.required = false;
        optionalCatalog.members.push_back(optional);
        Json optionalManifest = manifest;
        optionalManifest["entries"].push_back({{"target", "host64/optional.dll"}, {"source_kind", "stage"},
            {"folder", "host64"}, {"purpose", "fixture"}, {"replaces_original", false}, {"required", false}});
        GuardedPath userFile;
        if (!userFile.Open(client, L"host64/optional.dll", true)) return 56;
        const auto saved = WriteNewVerified(userFile, "existing user file");
        if (!saved) return 57;
        MemberInstallContext context;
        context.clientRoot = client; context.journalRoot = operation; context.stageRoot = root; context.templateRoot = root;
        context.operationId = L"00112233445566778899aabbccddeeff"; context.testProcesses = &Stopped;
        ClientLock lock;
        if (!lock.Acquire(client)) return 58;
        const auto preview = PreviewInstall(client, root, root, optionalCatalog);
        if (!preview.filesReady || preview.items.size() != 2 || preview.items[1].state != PreviewState::MissingOptional ||
            InstallMember(context, optionalCatalog, preview.items[0], lock).state != MemberInstallState::Applied ||
            PublishClientRecord(context, optionalCatalog, optionalManifest, lock, "2026-11-14T00:00:00Z").state != RecordPublishState::Published ||
            CommitInstall(context, optionalCatalog, optionalManifest, lock).state != CommitState::Completed ||
            !SameIdentity(saved.identity, HashFile(userFile.Path()).identity)) return 59;
    }
    {
        const std::wstring scanRoot = root + L"\\scan-operations";
        const std::wstring scanClient = root + L"\\scan-client";
        const std::wstring id = L"abcdef0123456789abcdef0123456789";
        if (!CreateDirectoryW(scanRoot.c_str(), nullptr) || !CreateDirectoryW(scanClient.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (scanClient + L"\\wow.exe").c_str(), TRUE)) return 41;
        MemberInstallContext context;
        context.clientRoot = scanClient; context.journalRoot = scanRoot + L"\\" + id;
        context.operationId = id; context.stageRoot = root; context.templateRoot = root; context.testProcesses = &Stopped;
        ClientLock lock;
        if (!lock.Acquire(scanClient)) return 42;
        auto scan = ScanClientOperations(scanRoot, lock, catalog);
        if (!scan.ok || scan.blocksNewOperation || !scan.operations.empty()) return 43;
        if (!CreateDirectoryW(context.journalRoot.c_str(), nullptr)) return 44;
        scan = ScanClientOperations(scanRoot, lock, catalog);
        if (!scan.ok || !scan.blocksNewOperation || scan.operations.size() != 1 || scan.operations[0].state != OperationScanState::Unknown) return 45;
        const auto preview = PreviewInstall(scanClient, root, root, catalog);
        if (!preview.filesReady || InstallMember(context, catalog, preview.items[0], lock).state != MemberInstallState::Applied) return 46;
        scan = ScanClientOperations(scanRoot, lock, catalog);
        if (!scan.ok || !scan.blocksNewOperation || scan.operations[0].state != OperationScanState::Incomplete) return 47;
        auto olderCatalog = catalog;
        olderCatalog.members[0].sha256 = std::string(64, 'b');
        const auto unfinishedMismatch = ScanClientOperations(scanRoot, lock, olderCatalog);
        if (!unfinishedMismatch.ok || !unfinishedMismatch.blocksNewOperation || unfinishedMismatch.operations.size() != 1 ||
            unfinishedMismatch.operations[0].state != OperationScanState::Unknown) return 79;
        if (PublishClientRecord(context, catalog, manifest, lock, "2026-11-14T00:00:00Z").state != RecordPublishState::Published) return 48;
        stopPoint = CommitCheckpoint::MarkerPublished;
        if (CommitInstall(context, catalog, manifest, lock, &Interrupt).state != CommitState::RecoveryRequired) return 49;
        scan = ScanClientOperations(scanRoot, lock, catalog);
        if (!scan.ok || !scan.blocksNewOperation || scan.operations[0].state != OperationScanState::Incomplete) return 50;
        const auto resumed = RecoverPendingOperations(scanRoot, scanClient, {}, catalog, manifest, lock, &Stopped);
        if (resumed.state != PendingRecoveryState::Committed || resumed.operationId != id ||
            RecoverPendingOperations(scanRoot, scanClient, {}, catalog, manifest, lock, &Stopped).state != PendingRecoveryState::NoWork) return 51;
        scan = ScanClientOperations(scanRoot, lock, catalog);
        if (!scan.ok || scan.blocksNewOperation || scan.operations.size() != 1 || scan.operations[0].state != OperationScanState::Completed) return 52;
        auto differentCatalog = catalog;
        differentCatalog.members[0].sha256 = std::string(64, 'b');
        const auto mismatch = ScanClientOperations(scanRoot, lock, differentCatalog);
        if (!mismatch.ok || mismatch.blocksNewOperation || mismatch.operations.size() != 1 || mismatch.operations[0].state != OperationScanState::Completed) return 67;
        const std::wstring otherClient = root + L"\\scan-other-client";
        if (!CreateDirectoryW(otherClient.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (otherClient + L"\\wow.exe").c_str(), TRUE)) return 68;
        ClientLock otherLock;
        if (!otherLock.Acquire(otherClient)) return 69;
        const auto other = ScanClientOperations(scanRoot, otherLock, catalog);
        if (!other.ok || other.blocksNewOperation || !other.operations.empty()) return 70;
        otherLock.Release();
        if (!CreateDirectoryW((scanRoot + L"\\unknown-entry").c_str(), nullptr)) return 53;
        scan = ScanClientOperations(scanRoot, lock, catalog);
        if (!scan.ok || !scan.blocksNewOperation || scan.operations.size() != 2) return 54;
        if (RecoverPendingOperations(scanRoot, scanClient, {}, catalog, manifest, lock, &Stopped).state != PendingRecoveryState::NeedsAttention) return 71;
        auto next = context;
        next.operationId = L"0123456789abcdef0123456789abcdef"; next.journalRoot = scanRoot + L"\\" + next.operationId;
        const auto blocked = StartInstallBatch(scanRoot, next, catalog, preview, lock);
        if (blocked.state != BatchInstallState::RecoveryRequired || GetFileAttributesW(next.journalRoot.c_str()) != INVALID_FILE_ATTRIBUTES ||
            !MissingPathError(GetLastError())) return 60;
        const auto blockedAutomatic = StartInstallWithRecovery(scanRoot, next, catalog, manifest, preview, lock);
        if (blockedAutomatic.recovery.state != PendingRecoveryState::NeedsAttention || blockedAutomatic.attemptedInstallation ||
            GetFileAttributesW(next.journalRoot.c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError())) return 78;
    }
    {
        const std::wstring scanRoot = root + L"\\start-operations", client = root + L"\\start-client";
        if (!CreateDirectoryW(scanRoot.c_str(), nullptr) || !CreateDirectoryW(client.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return 61;
        MemberInstallContext context;
        context.clientRoot = client; context.stageRoot = root; context.templateRoot = root;
        context.operationId = L"0123456789abcdef0123456789abcdef"; context.journalRoot = scanRoot + L"\\" + context.operationId;
        context.testProcesses = &Stopped;
        ClientLock lock;
        if (!lock.Acquire(client)) return 62;
        const auto preview = PreviewInstall(client, root, root, catalog);
        const auto started = StartInstallBatch(scanRoot, context, catalog, preview, lock);
        if (started.state != BatchInstallState::ReadyToFinalize || started.applied != 1) return 63;
        auto next = context;
        next.operationId = L"abcdef0123456789abcdef0123456789"; next.journalRoot = scanRoot + L"\\" + next.operationId;
        const auto blocked = StartInstallBatch(scanRoot, next, catalog, preview, lock);
        if (blocked.state != BatchInstallState::RecoveryRequired || GetFileAttributesW(next.journalRoot.c_str()) != INVALID_FILE_ATTRIBUTES ||
            !MissingPathError(GetLastError())) return 64;
        if (PublishClientRecord(context, catalog, manifest, lock, "2026-11-14T00:00:00Z", &InterruptRecord).state != RecordPublishState::RecoveryRequired ||
            RecoverPendingOperations(scanRoot, client, {}, catalog, manifest, lock, &Stopped).state != PendingRecoveryState::RolledBack) return 65;
        const auto recoveredScan = ScanClientOperations(scanRoot, lock, catalog);
        if (!recoveredScan.ok || recoveredScan.blocksNewOperation || recoveredScan.operations.size() != 1 ||
            recoveredScan.operations[0].state != OperationScanState::RolledBack ||
            StartInstallBatch(scanRoot, next, catalog, preview, lock).state != BatchInstallState::ReadyToFinalize) return 66;
        auto third = context;
        third.operationId = L"11111111111111111111111111111111"; third.journalRoot = scanRoot + L"\\" + third.operationId;
        const auto automaticallyStarted = StartInstallWithRecovery(scanRoot, third, catalog, manifest, preview, lock);
        if (automaticallyStarted.recovery.state != PendingRecoveryState::RolledBack || !automaticallyStarted.attemptedInstallation ||
            automaticallyStarted.installation.state != BatchInstallState::ReadyToFinalize ||
            RecoverPendingOperations(scanRoot, client, {}, catalog, manifest, lock, &Stopped).state != PendingRecoveryState::RolledBack ||
            RecoverPendingOperations(scanRoot, client, {}, catalog, manifest, lock, &Stopped).state != PendingRecoveryState::NoWork) return 72;
    }
    {
        const std::wstring scanRoot = root + L"\\multiple-operations", client = root + L"\\multiple-client";
        if (!CreateDirectoryW(scanRoot.c_str(), nullptr) || !CreateDirectoryW(client.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return 73;
        CatalogResult multipleCatalog = catalog;
        CatalogMember extra = member; extra.target = L"host64/extra.dll"; extra.replacesOriginal = false;
        multipleCatalog.members.push_back(extra);
        MemberInstallContext first;
        first.clientRoot = client; first.stageRoot = root; first.templateRoot = root; first.testProcesses = &Stopped;
        first.operationId = L"0123456789abcdef0123456789abcdef"; first.journalRoot = scanRoot + L"\\" + first.operationId;
        auto second = first;
        second.operationId = L"abcdef0123456789abcdef0123456789"; second.journalRoot = scanRoot + L"\\" + second.operationId;
        if (!CreateDirectoryW(first.journalRoot.c_str(), nullptr) || !CreateDirectoryW(second.journalRoot.c_str(), nullptr)) return 74;
        ClientLock lock;
        if (!lock.Acquire(client)) return 75;
        const auto preview = PreviewInstall(client, root, root, multipleCatalog);
        if (!preview.filesReady || preview.items.size() != 2 ||
            InstallMember(first, multipleCatalog, preview.items[0], lock).state != MemberInstallState::Applied ||
            InstallMember(second, multipleCatalog, preview.items[1], lock).state != MemberInstallState::Applied) return 76;
        const auto recovered = RecoverPendingOperations(scanRoot, client, {}, multipleCatalog, manifest, lock, &Stopped);
        GuardedPath firstFile, secondFile;
        if (recovered.state != PendingRecoveryState::NeedsAttention || !firstFile.Open(client, L"d3d9.dll") ||
            !secondFile.Open(client, L"host64/extra.dll") || !HashFile(firstFile.Path()) || !HashFile(secondFile.Path())) return 77;
    }
    std::puts("commit: 12 simulated interruptions, 4 real terminations, removed source and operation scan passed");
    return 0;
}
