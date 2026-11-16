#define CONTROL_CENTER_TESTING
#include "../native/control_center/recover_pending_repairs.hpp"
#include <cstdio>

using namespace control_center;
static RepairMemberCheckpoint stopPoint;
static bool Interrupt(RepairMemberCheckpoint point) { return point == stopPoint; }
static RepairCompleteCheckpoint completePoint;
static bool InterruptComplete(RepairCompleteCheckpoint point) { return point == completePoint; }
static ProcessCheck Stopped() { ProcessCheck result; result.state = ProcessState::Stopped; return result; }
static ProcessCheck Running() { ProcessCheck result; result.state = ProcessState::Running; return result; }
static HANDLE readyEvent = nullptr, releaseEvent = nullptr;
static bool CrashAt(RepairMemberCheckpoint point) {
    if (point != stopPoint) return false;
    SetEvent(readyEvent);
    WaitForSingleObject(releaseEvent, 30000);
    return true;
}
static bool CrashComplete(RepairCompleteCheckpoint point) {
    if (point != completePoint) return false;
    SetEvent(readyEvent);
    WaitForSingleObject(releaseEvent, 30000);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    const bool child = argc == 10 && (std::wstring(argv[1]) == L"--child" || std::wstring(argv[1]) == L"--commit-child");
    if (!child && argc != 2) return 1;
    const std::wstring root = child ? argv[2] : argv[1];
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 2;
    const auto hash = HashFile(source.Path());
    if (!hash) return 3;
    CatalogMember member; member.target = L"host64/extra.dll"; member.source = L"real/occupied.bin";
    member.sourceKind = "stage"; member.sha256 = HexSha256(hash.identity.sha256);
    CatalogResult catalog; catalog.members = {member};
    const Json entry = {{"target", "host64/extra.dll"}, {"sha256", member.sha256}, {"replaces_original", false},
                        {"pre_state", "absent"}, {"previous_sha256", nullptr}, {"backup", nullptr}};
    const std::wstring id = L"0123456789abcdef0123456789abcdef";
    if (child) {
        const std::wstring client = argv[3], operations = argv[4], operation = argv[5], repairs = argv[6];
        const bool completing = std::wstring(argv[1]) == L"--commit-child";
        stopPoint = static_cast<RepairMemberCheckpoint>(_wtoi(argv[7]));
        completePoint = static_cast<RepairCompleteCheckpoint>(_wtoi(argv[7]));
        readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[8]);
        releaseEvent = OpenEventW(SYNCHRONIZE, FALSE, argv[9]);
        if (!readyEvent || !releaseEvent) return 16;
        ClientLock lock;
        if (!lock.Acquire(client)) return 17;
        if (completing) (void)CompleteRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped, &CrashComplete);
        else (void)ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Stopped, &CrashAt);
        return 18;
    }
    int number = 0;
    auto fixture = [&](bool previous, std::wstring& client, std::wstring& operations, std::wstring& operation,
                       std::wstring& repairs, FileIdentity& original, ClientLock& lock) {
        ++number;
        client = root + L"\\repair-member-client-" + std::to_wstring(number);
        operations = root + L"\\repair-member-source-" + std::to_wstring(number);
        operation = operations + L"\\legacy";
        repairs = root + L"\\repair-member-jobs-" + std::to_wstring(number);
        if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(operations.c_str(), nullptr) ||
            !CreateDirectoryW(operation.c_str(), nullptr) || !CreateDirectoryW(repairs.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return false;
        std::string clientText;
        if (!WideUtf8(client, clientText)) return false;
        const Json record = {{"schema", 1}, {"status", "installed"}, {"entries", Json::array({entry})}};
        const Json transaction = {{"schema", 2}, {"status", "completed"}, {"client", clientText},
                                  {"entries", Json::array({entry})}, {"repair_entries", Json::array({entry})}};
        GuardedPath clientRecord, operationRecord, staged, target;
        if (!clientRecord.Open(client, L"host64/dlss5-install-record.json", true) || !WriteNewVerified(clientRecord, record.dump()) ||
            !operationRecord.Open(operation, L"transaction.json") || !WriteNewVerified(operationRecord, transaction.dump()) ||
            !staged.Open(operation, L"stage/host64/extra.dll", true) || !CopyVerifiedNew(source, staged, hash.identity.sha256) ||
            !target.Open(client, member.target, true)) return false;
        if (previous) {
            const auto written = WriteNewVerified(target, "user modified");
            if (!written) return false;
            original = written.identity;
        }
        if (!lock.Acquire(client)) return false;
        const auto preview = PreviewRepair(client, operations, operation, {}, catalog, lock);
        return preview.valid && preview.canRepair &&
            PrepareRepairPlan(client, operations, operation, repairs, id, {}, catalog, preview, lock).state == RepairPlanState::Ready;
    };
    const RepairMemberCheckpoint points[] = {RepairMemberCheckpoint::Prepare, RepairMemberCheckpoint::Backup,
        RepairMemberCheckpoint::Stage, RepairMemberCheckpoint::Prepared, RepairMemberCheckpoint::Intent,
        RepairMemberCheckpoint::Quarantined, RepairMemberCheckpoint::Published, RepairMemberCheckpoint::Applied};
    for (const bool previous : {false, true}) for (const auto point : points) {
        if (!previous && point == RepairMemberCheckpoint::Quarantined) continue;
        std::wstring client, operations, operation, repairs; FileIdentity original; ClientLock lock;
        if (!fixture(previous, client, operations, operation, repairs, original, lock)) return 4;
        const auto before = ScanClientRepairs(repairs, client, operations, {}, catalog, lock);
        if (!before.ok || !before.blocksNewOperation || before.repairs.size() != 1 ||
            before.repairs[0].state != RepairScanState::Incomplete) return 51;
        stopPoint = point;
        const auto applied = ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Stopped, &Interrupt);
        if (applied.state != RepairMemberState::RecoveryRequired) {
            std::fprintf(stderr, "apply prior=%d point=%d: %s %lu\n", previous, static_cast<int>(point), applied.reason.c_str(), applied.win32Error); return 5;
        }
        const auto recovered = RecoverPendingRepairs(repairs, client, operations, {}, catalog, lock, &Stopped);
        if (recovered.state != PendingRepairState::RolledBack) {
            std::fprintf(stderr, "recover prior=%d point=%d: %s %lu\n", previous, static_cast<int>(point), recovered.reason.c_str(), recovered.win32Error); return 6;
        }
        const auto after = ScanClientRepairs(repairs, client, operations, {}, catalog, lock);
        if (!after.ok || after.blocksNewOperation || after.repairs.size() != 1 ||
            after.repairs[0].state != RepairScanState::RolledBack) return 52;
        GuardedPath target;
        if (!target.Open(client, member.target)) return 7;
        if (previous) {
            const auto restored = HashFile(target.Path());
            if (!restored || !SameIdentity(restored.identity, original)) return 8;
            if (point != RepairMemberCheckpoint::Prepare) {
                GuardedPath backup;
                if (!backup.Open(repairs + L"\\" + id, L"modified/host64/extra.dll") ||
                    HashFile(backup.Path()).identity.sha256 != original.sha256) return 9;
            }
        } else if (GetFileAttributesW(target.Path().c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError())) return 10;
        if (RecoverRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped).state != RepairRecoveryState::AlreadyTerminal) return 11;
    }
    std::wstring client, operations, operation, repairs; FileIdentity original; ClientLock lock;
    if (!fixture(true, client, operations, operation, repairs, original, lock)) return 12;
    GuardedPath unexpected;
    if (!unexpected.Open(repairs + L"\\" + id, L"transaction.json") || !WriteNewVerified(unexpected, "unexpected")) return 58;
    const auto unsafe = ScanClientRepairs(repairs, client, operations, {}, catalog, lock);
    if (!unsafe.ok || !unsafe.blocksNewOperation || unsafe.repairs.size() != 1 ||
        unsafe.repairs[0].state != RepairScanState::Unknown ||
        RecoverPendingRepairs(repairs, client, operations, {}, catalog, lock, &Stopped).state != PendingRepairState::NeedsAttention ||
        !DeleteFileW(unexpected.Path().c_str())) return 59;
    if (RecoverPendingRepairs(repairs, client, operations, {}, catalog, lock, &Stopped).state != PendingRepairState::RolledBack ||
        RecoverPendingRepairs(repairs, client, operations, {}, catalog, lock, &Stopped).state != PendingRepairState::NoWork) return 53;
    lock.Release();
    if (!fixture(true, client, operations, operation, repairs, original, lock)) return 54;
    if (ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Running).state != RepairMemberState::PreconditionFailed ||
        LoadJournal(repairs + L"\\" + id).cursor.Next() != 1) return 13;
    const auto applied = ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Stopped);
    if (applied.state != RepairMemberState::Applied ||
        ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Stopped).state != RepairMemberState::AlreadyApplied) return 14;
    GuardedPath target, backup;
    if (!target.Open(client, member.target) || !backup.Open(repairs + L"\\" + id, L"modified/host64/extra.dll") ||
        HashFile(target.Path()).identity.sha256 != hash.identity.sha256 || HashFile(backup.Path()).identity.sha256 != original.sha256 ||
        RecoverRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped).state != RepairRecoveryState::Recovered ||
        !SameIdentity(HashFile(target.Path()).identity, original)) return 15;
    lock.Release();
    if (!fixture(true, client, operations, operation, repairs, original, lock) ||
        ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Stopped).state != RepairMemberState::Applied) return 25;
    GuardedPath changedTarget, changedBackup;
    if (!changedTarget.Open(client, member.target) || !changedBackup.Open(repairs + L"\\" + id, L"modified/host64/extra.dll")) return 26;
    HANDLE changedFile = CreateFileW(changedTarget.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (changedFile == INVALID_HANDLE_VALUE) return 27;
    DWORD changedBytes = 0;
    if (!WriteFile(changedFile, "later edit", 10, &changedBytes, nullptr) || changedBytes != 10) { CloseHandle(changedFile); return 28; }
    CloseHandle(changedFile);
    const auto changedHash = HashFile(changedTarget.Path());
    if (!changedHash || ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Stopped).state != RepairMemberState::RecoveryRequired ||
        RecoverRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped).state != RepairRecoveryState::Incomplete ||
        !SameIdentity(HashFile(changedTarget.Path()).identity, changedHash.identity) ||
        HashFile(changedBackup.Path()).identity.sha256 != original.sha256) return 29;
    lock.Release();
    if (!fixture(true, client, operations, operation, repairs, original, lock)) return 48;
    GuardedPath changedPlan;
    std::string planBytes; DWORD planError = ERROR_SUCCESS;
    if (!changedPlan.Open(repairs + L"\\" + id, L"plan.json") || !ReadJournalBytes(changedPlan, planBytes, planError)) return 49;
    HANDLE planFile = CreateFileW(changedPlan.Path().c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (planFile == INVALID_HANDLE_VALUE) return 50;
    DWORD appended = 0;
    if (!WriteFile(planFile, " ", 1, &appended, nullptr) || appended != 1) { CloseHandle(planFile); return 51; }
    CloseHandle(planFile);
    if (ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Stopped).state != RepairMemberState::PreconditionFailed ||
        LoadJournal(repairs + L"\\" + id).cursor.Next() != 1) return 52;
    planFile = CreateFileW(changedPlan.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (planFile == INVALID_HANDLE_VALUE) return 53;
    appended = 0;
    if (!WriteFile(planFile, planBytes.data(), static_cast<DWORD>(planBytes.size()), &appended, nullptr) ||
        static_cast<size_t>(appended) != planBytes.size()) { CloseHandle(planFile); return 54; }
    CloseHandle(planFile);
    if (ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Stopped).state != RepairMemberState::Applied ||
        RecoverRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped).state != RepairRecoveryState::Recovered) return 55;
    lock.Release();
    const RepairMemberCheckpoint crashPoints[] = {RepairMemberCheckpoint::Stage, RepairMemberCheckpoint::Quarantined, RepairMemberCheckpoint::Published};
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 19;
    for (size_t i = 0; i < sizeof(crashPoints) / sizeof(crashPoints[0]); ++i) {
        if (!fixture(true, client, operations, operation, repairs, original, lock)) return 20;
        lock.Release();
        const std::wstring eventName = L"Local\\WOW335-repair-crash-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i);
        const std::wstring releaseName = eventName + L"-release";
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        if (!ready || !release) { if (ready) CloseHandle(ready); if (release) CloseHandle(release); return 21; }
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --child \"" + root + L"\" \"" + client +
            L"\" \"" + operations + L"\" \"" + operation + L"\" \"" + repairs + L"\" " +
            std::to_wstring(static_cast<int>(crashPoints[i])) + L" " + eventName + L" " + releaseName;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            CloseHandle(ready); CloseHandle(release); return 22;
        }
        HANDLE waits[] = {ready, process.hProcess};
        const DWORD observed = WaitForMultipleObjects(2, waits, FALSE, 20000);
        const BOOL terminated = TerminateProcess(process.hProcess, 99);
        if (!terminated) SetEvent(release);
        const DWORD finished = WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(ready); CloseHandle(release);
        if (observed != WAIT_OBJECT_0 || !terminated || finished != WAIT_OBJECT_0 || !lock.Acquire(client)) return 23;
        const auto recovered = RecoverRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped);
        GuardedPath restored;
        if (recovered.state != RepairRecoveryState::Recovered || !restored.Open(client, member.target) ||
            !SameIdentity(HashFile(restored.Path()).identity, original)) {
            std::fprintf(stderr, "real repair crash %d: %s %lu\n", static_cast<int>(crashPoints[i]), recovered.reason.c_str(), recovered.win32Error); return 24;
        }
        lock.Release();
    }
    const RepairCompleteCheckpoint completePoints[] = {RepairCompleteCheckpoint::MarkerPrepared, RepairCompleteCheckpoint::MarkerPublished,
        RepairCompleteCheckpoint::ReportPrepared, RepairCompleteCheckpoint::ReportPublished, RepairCompleteCheckpoint::JournalCompleted};
    for (const auto point : completePoints) {
        if (!fixture(true, client, operations, operation, repairs, original, lock) ||
            ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Stopped).state != RepairMemberState::Applied) return 30;
        completePoint = point;
        const auto interrupted = CompleteRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped, &InterruptComplete);
        if (interrupted.state != RepairCompleteState::RecoveryRequired ||
            RecoverRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped).state != RepairRecoveryState::RecoveryRequired) return 31;
        if (point == RepairCompleteCheckpoint::MarkerPublished) {
            GuardedPath edited;
            std::string saved; DWORD error = ERROR_SUCCESS, written = 0;
            if (!edited.Open(client, member.target) || !ReadJournalBytes(edited, saved, error)) return 35;
            HANDLE file = CreateFileW(edited.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return 36;
            if (!WriteFile(file, "later edit", 10, &written, nullptr) || written != 10) { CloseHandle(file); return 37; }
            CloseHandle(file);
            const auto changed = HashFile(edited.Path());
            if (!changed || CompleteRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped).state != RepairCompleteState::RecoveryRequired ||
                !SameIdentity(changed.identity, HashFile(edited.Path()).identity)) return 38;
            file = CreateFileW(edited.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return 39;
            written = 0;
            if (!WriteFile(file, saved.data(), static_cast<DWORD>(saved.size()), &written, nullptr) || static_cast<size_t>(written) != saved.size()) {
                CloseHandle(file); return 40;
            }
            CloseHandle(file);
        }
        const auto dispatch = RecoverPendingRepairs(repairs, client, operations, {}, catalog, lock, &Stopped);
        const auto expected = point == RepairCompleteCheckpoint::JournalCompleted ? PendingRepairState::NoWork : PendingRepairState::Completed;
        if (dispatch.state != expected) {
            std::fprintf(stderr, "repair dispatch point=%d: %s %lu\n", static_cast<int>(point), dispatch.reason.c_str(), dispatch.win32Error); return 57;
        }
        const auto completed = CompleteRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped);
        if (completed.state != RepairCompleteState::Completed ||
            CompleteRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped).state != RepairCompleteState::Completed) {
            std::fprintf(stderr, "complete repair point=%d: %s %lu\n", static_cast<int>(point), completed.reason.c_str(), completed.win32Error); return 32;
        }
        const auto committedScan = ScanClientRepairs(repairs, client, operations, {}, catalog, lock);
        if (!committedScan.ok || committedScan.blocksNewOperation || committedScan.repairs.size() != 1 ||
            committedScan.repairs[0].state != RepairScanState::Completed) return 56;
        GuardedPath report, saved;
        std::string bytes; DWORD error = ERROR_SUCCESS;
        const auto journal = LoadJournal(repairs + L"\\" + id);
        if (!journal || !journal.cursor.Terminal() || journal.cursor.TerminalKind() != JournalKind::Complete ||
            !report.Open(repairs + L"\\" + id, L"result.json") || !ReadJournalBytes(report, bytes, error) ||
            !saved.Open(repairs + L"\\" + id, L"modified/host64/extra.dll") || HashFile(saved.Path()).identity.sha256 != original.sha256) return 33;
        const auto parsed = ParseJsonObject(bytes);
        if (!parsed || parsed.value["status"] != "completed" || parsed.value["saved_modified"].size() != 1) return 34;
        lock.Release();
    }
    const RepairCompleteCheckpoint commitCrashPoints[] = {RepairCompleteCheckpoint::MarkerPrepared,
        RepairCompleteCheckpoint::MarkerPublished, RepairCompleteCheckpoint::ReportPublished};
    for (size_t i = 0; i < sizeof(commitCrashPoints) / sizeof(commitCrashPoints[0]); ++i) {
        if (!fixture(true, client, operations, operation, repairs, original, lock) ||
            ApplyRepairMember(client, operations, operation, repairs, id, {}, catalog, 0, lock, &Stopped).state != RepairMemberState::Applied) return 41;
        lock.Release();
        const std::wstring eventName = L"Local\\WOW335-repair-commit-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i);
        const std::wstring releaseName = eventName + L"-release";
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        if (!ready || !release) { if (ready) CloseHandle(ready); if (release) CloseHandle(release); return 42; }
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --commit-child \"" + root + L"\" \"" + client +
            L"\" \"" + operations + L"\" \"" + operation + L"\" \"" + repairs + L"\" " +
            std::to_wstring(static_cast<int>(commitCrashPoints[i])) + L" " + eventName + L" " + releaseName;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            CloseHandle(ready); CloseHandle(release); return 43;
        }
        HANDLE waits[] = {ready, process.hProcess};
        const DWORD observed = WaitForMultipleObjects(2, waits, FALSE, 20000);
        const BOOL terminated = TerminateProcess(process.hProcess, 99);
        if (!terminated) SetEvent(release);
        const DWORD finished = WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(ready); CloseHandle(release);
        if (observed != WAIT_OBJECT_0 || !terminated || finished != WAIT_OBJECT_0 || !lock.Acquire(client)) return 44;
        if (RecoverRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped).state != RepairRecoveryState::RecoveryRequired) return 45;
        const auto completed = CompleteRepair(client, operations, operation, repairs, id, {}, catalog, lock, &Stopped);
        if (completed.state != RepairCompleteState::Completed) {
            std::fprintf(stderr, "real repair commit crash %d: %s %lu\n", static_cast<int>(commitCrashPoints[i]), completed.reason.c_str(), completed.win32Error); return 46;
        }
        GuardedPath saved;
        if (!saved.Open(repairs + L"\\" + id, L"modified/host64/extra.dll") || HashFile(saved.Path()).identity.sha256 != original.sha256) return 47;
        lock.Release();
    }
    std::puts("repair: 15 simulated interruptions, 6 real terminations, user backup and 5 commit interruptions passed");
    return 0;
}
