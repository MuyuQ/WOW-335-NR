#define CONTROL_CENTER_TESTING
#include "../native/control_center/recover_client_record.hpp"
#include <cstdio>

using namespace control_center;
static RecordPublishCheckpoint stopPoint;
static bool Interrupt(RecordPublishCheckpoint point) { return point == stopPoint; }
static ProcessCheck Stopped() { ProcessCheck result; result.state = ProcessState::Stopped; return result; }
static bool InterruptMemberRollback(MemberCheckpoint point) { return point == MemberCheckpoint::RollbackDeleted; }
static HANDLE readyEvent = nullptr, releaseEvent = nullptr;
static bool CrashAt(RecordPublishCheckpoint point) {
    if (point != stopPoint) return false;
    SetEvent(readyEvent);
    WaitForSingleObject(releaseEvent, 30000);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    const bool child = argc == 8 && std::wstring(argv[1]) == L"--child";
    if (!child && argc != 2) return 1;
    const std::wstring root = child ? argv[2] : argv[1];
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 2;
    const auto sourceHash = HashFile(source.Path());
    if (!sourceHash) return 3;
    CatalogMember member;
    member.target = L"d3d9.dll"; member.source = L"real/occupied.bin"; member.sourceKind = "stage";
    member.sha256 = HexSha256(sourceHash.identity.sha256); member.replacesOriginal = true;
    CatalogResult catalog; catalog.members.push_back(member);
    const Json manifest = {{"schema", 1}, {"updated", "fixture"},
        {"entries", Json::array({{{"target", "d3d9.dll"}, {"source_kind", "stage"}, {"folder", "root"},
                                  {"purpose", "fixture"}, {"replaces_original", true}, {"required", true}}})}};
    const Json old = {{"schema", 1}, {"status", "installed"}, {"entries", Json::array({
        {{"target", "host64/extra.dll"}, {"sha256", std::string(64, 'a')}, {"pre_state", "absent"},
         {"replaces_original", false}, {"previous_sha256", nullptr}, {"backup", nullptr}}})}};
    if (child) {
        MemberInstallContext context;
        context.clientRoot = argv[3]; context.journalRoot = argv[4]; context.stageRoot = root; context.templateRoot = root;
        context.operationId = L"00112233445566778899aabbccddeeff"; context.testProcesses = &Stopped;
        stopPoint = static_cast<RecordPublishCheckpoint>(_wtoi(argv[5]));
        readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[6]);
        releaseEvent = OpenEventW(SYNCHRONIZE, FALSE, argv[7]);
        if (!readyEvent || !releaseEvent) return 22;
        ClientLock lock;
        if (!lock.Acquire(context.clientRoot)) return 23;
        (void)PublishClientRecord(context, catalog, manifest, lock, "2026-11-14T00:00:00Z", &CrashAt);
        return 24;
    }
    const RecordPublishCheckpoint points[] = {RecordPublishCheckpoint::IntentToPrepare, RecordPublishCheckpoint::BackupReady,
        RecordPublishCheckpoint::StageReady, RecordPublishCheckpoint::Prepared, RecordPublishCheckpoint::Intent,
        RecordPublishCheckpoint::OriginalIsolated, RecordPublishCheckpoint::Published, RecordPublishCheckpoint::Applied};
    int number = 0;
    auto fixture = [&](bool hasPrior, MemberInstallContext& context, FileIdentity& priorId) {
        ++number;
        context.clientRoot = root + L"\\record-client-" + std::to_wstring(number);
        context.journalRoot = root + L"\\record-operation-" + std::to_wstring(number);
        context.stageRoot = root; context.templateRoot = root; context.operationId = L"00112233445566778899aabbccddeeff";
        context.testProcesses = &Stopped;
        if (!CreateDirectoryW(context.clientRoot.c_str(), nullptr) || !CreateDirectoryW(context.journalRoot.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (context.clientRoot + L"\\wow.exe").c_str(), TRUE)) return false;
        if (hasPrior) {
            GuardedPath path;
            if (!path.Open(context.clientRoot, L"host64/dlss5-install-record.json", true)) return false;
            const auto written = WriteNewVerified(path, old.dump());
            if (!written) return false;
            priorId = written.identity;
        }
        return true;
    };
    for (const bool hasPrior : {false, true}) for (const auto point : points) {
        if (!hasPrior && point == RecordPublishCheckpoint::OriginalIsolated) continue;
        MemberInstallContext context; FileIdentity priorId; ClientLock lock;
        if (!fixture(hasPrior, context, priorId) || !lock.Acquire(context.clientRoot)) return 4;
        const auto preview = PreviewInstall(context.clientRoot, root, root, catalog);
        if (!preview.filesReady || InstallMember(context, catalog, preview.items[0], lock).state != MemberInstallState::Applied) return 5;
        stopPoint = point;
        const auto published = PublishClientRecord(context, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Interrupt);
        if (published.state != RecordPublishState::RecoveryRequired) return 6;
        const auto recovered = RecoverClientRecord(context, catalog, manifest, lock);
        if (recovered.state != RecordRecoveryState::Recovered) {
            std::fprintf(stderr, "recovery prior=%d point=%d: %s %lu\n", hasPrior, static_cast<int>(point), recovered.reason.c_str(), recovered.win32Error); return 7;
        }
        if (hasPrior) {
            GuardedPath path;
            if (!path.Open(context.clientRoot, L"host64/dlss5-install-record.json")) return 8;
            const auto restored = HashFile(path.Path());
            if (!restored || !SameIdentity(restored.identity, priorId)) return 9;
        } else if (CaptureClientRecord(context.clientRoot).state != CapturedRecordState::Absent) return 10;
        if (RecoverClientRecord(context, catalog, manifest, lock).state != RecordRecoveryState::AlreadyTerminal) return 11;
        if (!hasPrior && point == RecordPublishCheckpoint::Applied) {
            GuardedPath marker;
            if (!marker.Open(context.journalRoot, L"commit.pending")) return 34;
            const auto occupied = WriteNewVerified(marker, "commit in progress");
            if (!occupied || RecoverOperation(context, catalog, lock).state != RecoveryState::RecoveryRequired ||
                !DeleteVerified(marker, occupied.identity.sha256, &occupied.identity)) return 35;
        }
        if (hasPrior && point == RecordPublishCheckpoint::Published) {
            context.testInterrupt = &InterruptMemberRollback;
            if (RecoverOperation(context, catalog, lock).state != RecoveryState::RecoveryRequired) return 33;
            context.testInterrupt = nullptr;
        }
        const auto membersRecovered = RecoverOperation(context, catalog, lock);
        if (membersRecovered.state != RecoveryState::Recovered) {
            std::fprintf(stderr, "member recovery after record prior=%d point=%d: %s %lu\n", hasPrior,
                         static_cast<int>(point), membersRecovered.reason.c_str(), membersRecovered.win32Error); return 20;
        }
        GuardedPath memberPath;
        if (!memberPath.Open(context.clientRoot, L"d3d9.dll") ||
            GetFileAttributesW(memberPath.Path().c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()) ||
            RecoverOperation(context, catalog, lock).state != RecoveryState::AlreadyTerminal) return 21;
        lock.Release();
    }
    MemberInstallContext context; FileIdentity unused; ClientLock lock;
    if (!fixture(false, context, unused) || !lock.Acquire(context.clientRoot)) return 12;
    const auto preview = PreviewInstall(context.clientRoot, root, root, catalog);
    if (!preview.filesReady || InstallMember(context, catalog, preview.items[0], lock).state != MemberInstallState::Applied) return 13;
    stopPoint = RecordPublishCheckpoint::Published;
    if (PublishClientRecord(context, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Interrupt).state != RecordPublishState::RecoveryRequired) return 14;
    GuardedPath path;
    if (!path.Open(context.clientRoot, L"host64/dlss5-install-record.json")) return 15;
    HANDLE file = CreateFileW(path.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 16;
    DWORD written = 0;
    if (!WriteFile(file, "user", 4, &written, nullptr) || written != 4) { CloseHandle(file); return 17; }
    CloseHandle(file);
    const auto before = HashFile(path.Path());
    if (!before || RecoverClientRecord(context, catalog, manifest, lock).state != RecordRecoveryState::RecoveryRequired) return 18;
    const auto after = HashFile(path.Path());
    if (!after || !SameIdentity(before.identity, after.identity)) return 19;
    lock.Release();

    const RecordPublishCheckpoint crashPoints[] = {RecordPublishCheckpoint::StageReady,
        RecordPublishCheckpoint::OriginalIsolated, RecordPublishCheckpoint::Published};
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 25;
    for (size_t i = 0; i < sizeof(crashPoints) / sizeof(crashPoints[0]); ++i) {
        FileIdentity priorId;
        if (!fixture(true, context, priorId) || !lock.Acquire(context.clientRoot)) return 26;
        const auto scan = PreviewInstall(context.clientRoot, root, root, catalog);
        if (!scan.filesReady || InstallMember(context, catalog, scan.items[0], lock).state != MemberInstallState::Applied) return 27;
        lock.Release();
        const std::wstring eventName = L"Local\\WOW335-record-crash-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i);
        const std::wstring releaseName = eventName + L"-release";
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        if (!ready || !release) { if (ready) CloseHandle(ready); if (release) CloseHandle(release); return 28; }
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --child \"" + root + L"\" \"" + context.clientRoot +
            L"\" \"" + context.journalRoot + L"\" " + std::to_wstring(static_cast<int>(crashPoints[i])) + L" " + eventName + L" " + releaseName;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            CloseHandle(ready); CloseHandle(release); return 29;
        }
        HANDLE waits[] = {ready, process.hProcess};
        const DWORD observed = WaitForMultipleObjects(2, waits, FALSE, 20000);
        const BOOL terminated = TerminateProcess(process.hProcess, 99);
        if (!terminated) SetEvent(release);
        const DWORD finished = WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(ready); CloseHandle(release);
        if (observed != WAIT_OBJECT_0 || !terminated || finished != WAIT_OBJECT_0 || !lock.Acquire(context.clientRoot)) return 30;
        const auto record = RecoverClientRecord(context, catalog, manifest, lock);
        if (record.state != RecordRecoveryState::Recovered) {
            std::fprintf(stderr, "real record crash %d: %s %lu\n", static_cast<int>(crashPoints[i]), record.reason.c_str(), record.win32Error); return 31;
        }
        GuardedPath priorPath;
        if (!priorPath.Open(context.clientRoot, L"host64/dlss5-install-record.json") ||
            !SameIdentity(HashFile(priorPath.Path()).identity, priorId) ||
            RecoverOperation(context, catalog, lock).state != RecoveryState::Recovered) return 32;
        lock.Release();
    }
    std::puts("client record recovery: 15 interrupted states, conflict and 3 real process terminations passed");
    return 0;
}
