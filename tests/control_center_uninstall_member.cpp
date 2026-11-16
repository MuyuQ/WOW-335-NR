#define CONTROL_CENTER_TESTING
#include "../native/control_center/recover_uninstall.hpp"
#include "../native/control_center/coordinated_install.hpp"
#include "../native/control_center/uninstall_finalize_prepare.hpp"
#include <cstdio>

using namespace control_center;
static UninstallMemberCheckpoint stopAt;
static bool Interrupt(UninstallMemberCheckpoint point) { return point == stopAt; }
static UninstallRecoveryCheckpoint recoveryStop;
static bool InterruptRecovery(UninstallRecoveryCheckpoint point) { return point == recoveryStop; }
static UninstallFinalizePrepareCheckpoint finalizeStop;
static bool InterruptFinalize(UninstallFinalizePrepareCheckpoint point) { return point == finalizeStop; }
static HANDLE readyEvent = nullptr, releaseEvent = nullptr;
static bool CrashRecovery(UninstallRecoveryCheckpoint point) {
    if (point != recoveryStop) return false;
    SetEvent(readyEvent); WaitForSingleObject(releaseEvent, 30000); return true;
}
static bool CrashMember(UninstallMemberCheckpoint point) {
    if (point != stopAt) return false;
    SetEvent(readyEvent); WaitForSingleObject(releaseEvent, 30000); return true;
}
static bool CrashFinalize(UninstallFinalizePrepareCheckpoint point) {
    if (point != finalizeStop) return false;
    SetEvent(readyEvent); WaitForSingleObject(releaseEvent, 30000); return true;
}
static ProcessCheck Stopped() { ProcessCheck result; result.state = ProcessState::Stopped; return result; }
static ProcessCheck Running() { ProcessCheck result; result.state = ProcessState::Running; return result; }
int wmain(int argc, wchar_t** argv) {
    const bool child = argc == 8 && (std::wstring(argv[1]) == L"--recover-child" || std::wstring(argv[1]) == L"--member-child" ||
                                     std::wstring(argv[1]) == L"--finalize-child");
    if (!child && argc != 2) return 1;
    const std::wstring root = child ? argv[2] : argv[1], id = L"0123456789abcdef0123456789abcdef";
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 2;
    const auto hash = HashFile(source.Path()); std::array<std::uint8_t, 32> previous{};
    if (!hash || HashBytes("original binary", previous) < 0) return 3;
    const char* names[] = {"d3d9.dll", "host64/new.dll", "host64/missing.dll"};
    CatalogResult catalog; Json entries = Json::array(), manifestEntries = Json::array();
    for (size_t i = 0; i < 3; ++i) {
        CatalogMember member; if (!Utf8Wide(names[i], member.target)) return 4;
        member.source = L"real/occupied.bin"; member.sourceKind = "stage"; member.sha256 = HexSha256(hash.identity.sha256);
        member.replacesOriginal = i == 0; const bool existed = i != 1;
        catalog.members.push_back(member);
        entries.push_back({{"target", names[i]}, {"sha256", member.sha256}, {"replaces_original", member.replacesOriginal},
            {"pre_state", existed ? "present" : "absent"}, {"previous_sha256", existed ? Json(HexSha256(previous)) : Json(nullptr)},
            {"backup", existed ? Json(names[i]) : Json(nullptr)}});
        manifestEntries.push_back({{"target", names[i]}, {"source_kind", "stage"}, {"folder", "fixture"}, {"purpose", "fixture"},
                                   {"replaces_original", member.replacesOriginal}, {"required", true}});
    }
    const Json manifest = {{"schema", 1}, {"updated", "fixture"}, {"entries", manifestEntries}};
    if (child) {
        recoveryStop = static_cast<UninstallRecoveryCheckpoint>(_wtoi(argv[5]));
        stopAt = static_cast<UninstallMemberCheckpoint>(_wtoi(argv[5]));
        finalizeStop = static_cast<UninstallFinalizePrepareCheckpoint>(_wtoi(argv[5]));
        readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[6]); releaseEvent = OpenEventW(SYNCHRONIZE, FALSE, argv[7]);
        if (!readyEvent || !releaseEvent) return 26;
        ClientLock childLock;
        if (!childLock.Acquire(argv[3])) return 27;
        if (std::wstring(argv[1]) == L"--recover-child")
            (void)RecoverUninstall(argv[3], argv[4], id, {}, catalog, manifest, childLock, &Stopped, &CrashRecovery);
        else if (std::wstring(argv[1]) == L"--member-child")
            (void)ApplyUninstallMember(argv[3], argv[4], id, {}, catalog, manifest, 0, childLock, &Stopped, &CrashMember);
        else (void)PrepareUninstallFinalization(argv[3], argv[4], id, {}, catalog, manifest, childLock, &Stopped, &CrashFinalize);
        return 28;
    }
    int counter = 0;
    auto fixture = [&](std::wstring& client, std::wstring& jobs, FileIdentity& first, FileIdentity& second, ClientLock& lock, bool matching = false) {
        ++counter; const auto suffix = std::to_wstring(counter);
        client = root + L"\\uninstall-member-client-" + suffix; jobs = root + L"\\uninstall-member-jobs-" + suffix;
        if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(jobs.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return false;
        const Json record = {{"schema", 1}, {"status", "installed"}, {"entries", entries}};
        GuardedPath recordPath, target, other, backup, missingBackup;
        if (!recordPath.Open(client, L"host64/dlss5-install-record.json", true) || !WriteNewVerified(recordPath, record.dump()) ||
            !target.Open(client, L"d3d9.dll") || !other.Open(client, L"host64/new.dll") ||
            !backup.Open(client, L"host64/dlss5-backup/d3d9.dll", true) || !WriteNewVerified(backup, "original binary") ||
            !missingBackup.Open(client, L"host64/dlss5-backup/host64/missing.dll", true) || !WriteNewVerified(missingBackup, "original binary")) return false;
        const auto one = WriteNewVerified(target, "user modified first");
        if (!one) return false;
        if (matching) {
            const auto copied = CopyVerifiedNew(source, other, hash.identity.sha256);
            if (!copied) return false;
            second = copied.target;
        } else {
            const auto written = WriteNewVerified(other, "user modified second");
            if (!written) return false;
            second = written.identity;
        }
        if (!lock.Acquire(client)) return false;
        first = one.identity;
        const auto preview = PreviewUninstall(client, {}, catalog, manifest, lock);
        return preview.valid && preview.canUninstall &&
            PrepareUninstallPlan(client, jobs, id, {}, catalog, manifest, preview, lock).state == UninstallPlanState::Ready;
    };
    const UninstallMemberCheckpoint points[] = {UninstallMemberCheckpoint::Prepare, UninstallMemberCheckpoint::Saved,
        UninstallMemberCheckpoint::Staged, UninstallMemberCheckpoint::Prepared, UninstallMemberCheckpoint::Intent,
        UninstallMemberCheckpoint::Quarantined, UninstallMemberCheckpoint::Published, UninstallMemberCheckpoint::Applied};
    for (const auto point : points) {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock)) return 5;
        stopAt = point;
        const auto applied = ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Stopped, &Interrupt);
        const auto plan = ReadTrustedUninstallPlan(client, jobs, id, {}, catalog, manifest, lock);
        const auto journal = LoadUninstallJournal(jobs + L"\\" + id, plan);
        if (applied.state != UninstallMemberState::RecoveryRequired || !plan.valid || !journal ||
            (point == UninstallMemberCheckpoint::Applied ? journal.cursor.Applied().size() != 1 : journal.cursor.Terminal()) ||
            ((point == UninstallMemberCheckpoint::Prepare || point == UninstallMemberCheckpoint::Saved || point == UninstallMemberCheckpoint::Staged) && !journal.cursor.Preparing()) ||
            (point == UninstallMemberCheckpoint::Prepared && !journal.cursor.Prepared()) ||
            ((point == UninstallMemberCheckpoint::Intent || point == UninstallMemberCheckpoint::Quarantined || point == UninstallMemberCheckpoint::Published) &&
             !journal.cursor.IntentPending())) return 6;
        const auto original = HashFile((client + L"\\d3d9.dll"));
        const auto isolated = HashFile(client + L"\\d3d9.dll.nr-uninstall-" + id + L".original");
        const auto saved = HashFile(jobs + L"\\" + id + L"\\modified\\d3d9.dll");
        if ((!original || !SameIdentity(original.identity, first)) && (!isolated || !SameIdentity(isolated.identity, first)) &&
            (!saved || saved.identity.sha256 != first.sha256)) return 7;
        const auto recovery = RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped);
        if (recovery.state != UninstallRecoveryState::Recovered || !SameIdentity(HashFile(client + L"\\d3d9.dll").identity, first) ||
            RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped).state != UninstallRecoveryState::AlreadyTerminal) {
            std::fprintf(stderr, "uninstall recovery point %d: %s %lu\n", static_cast<int>(point), recovery.reason.c_str(), recovery.win32Error); return 18;
        }
    }
    const UninstallRecoveryCheckpoint recoveryPoints[] = {UninstallRecoveryCheckpoint::RollbackIntent, UninstallRecoveryCheckpoint::CandidateIsolated,
        UninstallRecoveryCheckpoint::OriginalRestored, UninstallRecoveryCheckpoint::RollbackApplied};
    for (const auto point : recoveryPoints) {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock) ||
            ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Stopped).state != UninstallMemberState::Applied) return 24;
        recoveryStop = point;
        const auto interrupted = RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped, &InterruptRecovery);
        const auto retried = RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped);
        if (interrupted.state != UninstallRecoveryState::RecoveryRequired || retried.state != UninstallRecoveryState::Recovered ||
            !SameIdentity(HashFile(client + L"\\d3d9.dll").identity, first)) {
            std::fprintf(stderr, "uninstall rollback point %d: %s %lu\n", static_cast<int>(point), retried.reason.c_str(), retried.win32Error); return 25;
        }
    }
    const UninstallFinalizePrepareCheckpoint finalizePoints[] = {UninstallFinalizePrepareCheckpoint::SnapshotPrepared,
        UninstallFinalizePrepareCheckpoint::SnapshotPublished, UninstallFinalizePrepareCheckpoint::PlanPrepared,
        UninstallFinalizePrepareCheckpoint::PlanPublished};
    for (const auto point : finalizePoints) {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock)) return 53;
        if (PrepareUninstallFinalization(client, jobs, id, {}, catalog, manifest, lock, &Stopped).ready) return 54;
        for (size_t i = 0; i < 3; ++i)
            if (ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, i, lock, &Stopped).state != UninstallMemberState::Applied) return 55;
        const auto originalRecord = CaptureClientRecord(client);
        finalizeStop = point;
        const auto interrupted = PrepareUninstallFinalization(client, jobs, id, {}, catalog, manifest, lock, &Stopped, &InterruptFinalize);
        const auto ready = PrepareUninstallFinalization(client, jobs, id, {}, catalog, manifest, lock, &Stopped);
        if (interrupted.ready || !ready.ready || !SameIdentity(CaptureClientRecord(client).identity, originalRecord.identity) ||
            ready.snapshot.sha256 != originalRecord.identity.sha256 ||
            RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped).state != UninstallRecoveryState::Recovered ||
            !SameIdentity(HashFile(client + L"\\d3d9.dll").identity, first) || !SameIdentity(HashFile(client + L"\\host64\\new.dll").identity, second)) {
            std::fprintf(stderr, "uninstall finalization point %d: %s %lu\n", static_cast<int>(point), ready.reason.c_str(), ready.win32Error); return 56;
        }
    }
    {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock)) return 59;
        for (size_t i = 0; i < 3; ++i)
            if (ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, i, lock, &Stopped).state != UninstallMemberState::Applied) return 60;
        GuardedPath later;
        if (!later.Open(client, L"host64/new.dll") || !WriteNewVerified(later, "new user data")) return 61;
        if (PrepareUninstallFinalization(client, jobs, id, {}, catalog, manifest, lock, &Stopped).ready ||
            GetFileAttributesW((jobs + L"\\" + id + L"\\record.snapshot").c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError())) return 62;
    }
    {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock)) return 63;
        for (size_t i = 0; i < 3; ++i)
            if (ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, i, lock, &Stopped).state != UninstallMemberState::Applied) return 64;
        finalizeStop = UninstallFinalizePrepareCheckpoint::SnapshotPublished;
        if (PrepareUninstallFinalization(client, jobs, id, {}, catalog, manifest, lock, &Stopped, &InterruptFinalize).ready) return 65;
        const std::wstring snapshotPath = jobs + L"\\" + id + L"\\record.snapshot";
        HANDLE changedSnapshot = CreateFileW(snapshotPath.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (changedSnapshot == INVALID_HANDLE_VALUE) return 66;
        DWORD count = 0; const bool written = WriteFile(changedSnapshot, "changed", 7, &count, nullptr) && count == 7; CloseHandle(changedSnapshot);
        const auto altered = HashFile(snapshotPath);
        if (!written || !altered || PrepareUninstallFinalization(client, jobs, id, {}, catalog, manifest, lock, &Stopped).ready ||
            !SameIdentity(HashFile(snapshotPath).identity, altered.identity) ||
            RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped).state != UninstallRecoveryState::Recovered) return 67;
    }
    const int rollbackCrashPoints[] = {static_cast<int>(UninstallRecoveryCheckpoint::RollbackIntent),
        static_cast<int>(UninstallRecoveryCheckpoint::CandidateIsolated), static_cast<int>(UninstallRecoveryCheckpoint::OriginalRestored)};
    const int forwardCrashPoints[] = {static_cast<int>(UninstallMemberCheckpoint::Staged),
        static_cast<int>(UninstallMemberCheckpoint::Quarantined), static_cast<int>(UninstallMemberCheckpoint::Published)};
    const int finalizeCrashPoints[] = {static_cast<int>(UninstallFinalizePrepareCheckpoint::SnapshotPrepared),
        static_cast<int>(UninstallFinalizePrepareCheckpoint::PlanPrepared), static_cast<int>(UninstallFinalizePrepareCheckpoint::PlanPublished)};
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 29;
    for (int mode = 0; mode < 3; ++mode) for (size_t i = 0; i < 3; ++i) {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock) || (mode != 1 &&
            ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Stopped).state != UninstallMemberState::Applied)) return 30;
        if (mode == 2) for (size_t index = 1; index < 3; ++index)
            if (ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, index, lock, &Stopped).state != UninstallMemberState::Applied) return 57;
        lock.Release();
        const int point = mode == 0 ? rollbackCrashPoints[i] : mode == 1 ? forwardCrashPoints[i] : finalizeCrashPoints[i];
        const std::wstring eventName = L"Local\\WOW335-uninstall-crash-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(mode) + L"-" + std::to_wstring(i);
        const std::wstring releaseName = eventName + L"-release";
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        if (!ready || !release) { if (ready) CloseHandle(ready); if (release) CloseHandle(release); return 31; }
        const wchar_t* option = mode == 0 ? L"\" --recover-child \"" : mode == 1 ? L"\" --member-child \"" : L"\" --finalize-child \"";
        std::wstring command = L"\"" + std::wstring(executable) + option + root + L"\" \"" + client +
            L"\" \"" + jobs + L"\" " + std::to_wstring(point) + L" " + eventName + L" " + releaseName;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            CloseHandle(ready); CloseHandle(release); return 32;
        }
        HANDLE waits[] = {ready, process.hProcess};
        const DWORD observed = WaitForMultipleObjects(2, waits, FALSE, 20000);
        const BOOL terminated = TerminateProcess(process.hProcess, 99);
        if (!terminated) SetEvent(release);
        const DWORD finished = WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(ready); CloseHandle(release);
        if (observed != WAIT_OBJECT_0 || !terminated || finished != WAIT_OBJECT_0 || !lock.Acquire(client)) return 33;
        if (mode == 2 && !PrepareUninstallFinalization(client, jobs, id, {}, catalog, manifest, lock, &Stopped).ready) return 58;
        const auto recovered = RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped);
        if (recovered.state != UninstallRecoveryState::Recovered || !SameIdentity(HashFile(client + L"\\d3d9.dll").identity, first)) {
            std::fprintf(stderr, "uninstall real crash mode=%d point=%d: %s %lu\n", mode, point, recovered.reason.c_str(), recovered.win32Error); return 34;
        }
    }
    std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
    if (!fixture(client, jobs, first, second, lock)) return 8;
    if (ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Running).state != UninstallMemberState::PreconditionFailed) return 9;
    HANDLE changed = CreateFileW((client + L"\\d3d9.dll").c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (changed == INVALID_HANDLE_VALUE) return 13;
    DWORD count = 0; const bool edited = WriteFile(changed, "x", 1, &count, nullptr) && count == 1; CloseHandle(changed);
    if (!edited || ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Stopped).state != UninstallMemberState::PreconditionFailed ||
        HashFile(client + L"\\d3d9.dll").identity.sha256 == first.sha256) return 14;
    lock.Release();
    if (!fixture(client, jobs, first, second, lock)) return 15;
    for (size_t i = 0; i < 3; ++i) {
        const auto result = ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, i, lock, &Stopped);
        if (result.state != UninstallMemberState::Applied) {
            std::fprintf(stderr, "uninstall member %zu: %s %lu\n", i, result.reason.c_str(), result.win32Error); return 10;
        }
    }
    if (HashFile(client + L"\\d3d9.dll").identity.sha256 != previous ||
        HashFile(client + L"\\host64\\missing.dll").identity.sha256 != previous ||
        GetFileAttributesW((client + L"\\host64\\new.dll").c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()) ||
        HashFile(jobs + L"\\" + id + L"\\modified\\d3d9.dll").identity.sha256 != first.sha256 ||
        HashFile(jobs + L"\\" + id + L"\\modified\\host64\\new.dll").identity.sha256 != second.sha256) return 11;
    const auto plan = ReadTrustedUninstallPlan(client, jobs, id, {}, catalog, manifest, lock);
    const auto journal = LoadUninstallJournal(jobs + L"\\" + id, plan);
    if (!journal || journal.cursor.Applied().size() != 3 || journal.cursor.Terminal()) return 12;
    const auto recovered = RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped);
    const auto scanRecovered = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
    if (recovered.state != UninstallRecoveryState::Recovered || !SameIdentity(HashFile(client + L"\\d3d9.dll").identity, first) ||
        !SameIdentity(HashFile(client + L"\\host64\\new.dll").identity, second) ||
        GetFileAttributesW((client + L"\\host64\\missing.dll").c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()) ||
        !scanRecovered.ok || scanRecovered.blocksNewOperation || scanRecovered.uninstalls.size() != 1 ||
        scanRecovered.uninstalls[0].state != UninstallScanState::RolledBack) return 19;
    lock.Release();
    if (!fixture(client, jobs, first, second, lock, true) ||
        ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Stopped).state != UninstallMemberState::Applied ||
        ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 1, lock, &Stopped).state != UninstallMemberState::Applied) return 16;
    const auto isolated = HashFile(client + L"\\host64\\new.dll.nr-uninstall-" + id + L".original");
    if (!isolated || !SameIdentity(isolated.identity, second) ||
        GetFileAttributesW((jobs + L"\\" + id + L"\\modified\\host64\\new.dll").c_str()) != INVALID_FILE_ATTRIBUTES ||
        !MissingPathError(GetLastError())) return 17;
    if (RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped).state != UninstallRecoveryState::Recovered ||
        !SameIdentity(HashFile(client + L"\\host64\\new.dll").identity, second)) return 20;
    lock.Release();
    if (!fixture(client, jobs, first, second, lock) ||
        ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Stopped).state != UninstallMemberState::Applied) return 21;
    HANDLE editedFile = CreateFileW((client + L"\\d3d9.dll").c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (editedFile == INVALID_HANDLE_VALUE) return 22;
    DWORD editedBytes = 0; const bool editedOk = WriteFile(editedFile, "later edit", 10, &editedBytes, nullptr) && editedBytes == 10; CloseHandle(editedFile);
    const auto editedHash = HashFile(client + L"\\d3d9.dll");
    const auto conflict = RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped);
    if (!editedOk || !editedHash || conflict.state != UninstallRecoveryState::Incomplete || conflict.unresolved.size() != 1 ||
        !SameIdentity(HashFile(client + L"\\d3d9.dll").identity, editedHash.identity) ||
        !SameIdentity(HashFile(client + L"\\d3d9.dll.nr-uninstall-" + id + L".original").identity, first)) return 23;
    lock.Release();
    if (!fixture(client, jobs, first, second, lock)) return 35;
    for (size_t i = 0; i < 3; ++i)
        if (ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, i, lock, &Stopped).state != UninstallMemberState::Applied) return 36;
    GuardedPath newUserFile;
    if (!newUserFile.Open(client, L"host64/new.dll")) return 37;
    const auto newer = WriteNewVerified(newUserFile, "later user file");
    if (!newer) return 38;
    const auto partial = RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped);
    const auto partialScan = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
    if (partial.state != UninstallRecoveryState::Incomplete || partial.unresolved.size() != 1 || partial.unresolved[0].index != 1 ||
        !SameIdentity(HashFile(client + L"\\d3d9.dll").identity, first) || !SameIdentity(HashFile(newUserFile.Path()).identity, newer.identity) ||
        !SameIdentity(HashFile(client + L"\\host64\\new.dll.nr-uninstall-" + id + L".original").identity, second) ||
        GetFileAttributesW((client + L"\\host64\\missing.dll").c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()) ||
        !partialScan.ok || !partialScan.blocksNewOperation || partialScan.uninstalls[0].state != UninstallScanState::Unknown) return 39;
    lock.Release();
    if (!fixture(client, jobs, first, second, lock) ||
        ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Stopped).state != UninstallMemberState::Applied) return 48;
    stopAt = UninstallMemberCheckpoint::Published;
    if (ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 1, lock, &Stopped, &Interrupt).state != UninstallMemberState::RecoveryRequired) return 49;
    GuardedPath intentConflict;
    if (!intentConflict.Open(client, L"host64/new.dll")) return 50;
    const auto userFile = WriteNewVerified(intentConflict, "post-intent edit");
    if (!userFile) return 51;
    recoveryStop = UninstallRecoveryCheckpoint::IntentConflict;
    const auto interruptedConflict = RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped, &InterruptRecovery);
    const auto pendingConflict = RecoverUninstall(client, jobs, id, {}, catalog, manifest, lock, &Stopped);
    if (interruptedConflict.state != UninstallRecoveryState::RecoveryRequired || pendingConflict.state != UninstallRecoveryState::Incomplete || pendingConflict.unresolved.size() != 1 ||
        pendingConflict.unresolved[0].kind != UninstallJournalKind::IntentConflict || pendingConflict.unresolved[0].index != 1 ||
        !SameIdentity(HashFile(client + L"\\d3d9.dll").identity, first) || !SameIdentity(HashFile(intentConflict.Path()).identity, userFile.identity) ||
        !SameIdentity(HashFile(client + L"\\host64\\new.dll.nr-uninstall-" + id + L".original").identity, second)) return 52;
    lock.Release();
    if (!fixture(client, jobs, first, second, lock) ||
        ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Stopped).state != UninstallMemberState::Applied) return 40;
    const std::wstring operations = root + L"\\uninstall-auto-operations", repairs = root + L"\\uninstall-auto-repairs";
    if (!CreateDirectoryW(operations.c_str(), nullptr) || !CreateDirectoryW(repairs.c_str(), nullptr)) return 41;
    const auto automatic = RecoverBeforeWork(operations, repairs, jobs, operations, client, {}, catalog, manifest, lock, &Stopped);
    if (!automatic.ready || !automatic.recoveredUninstall || automatic.uninstall.state != UninstallRecoveryState::Recovered ||
        !SameIdentity(HashFile(client + L"\\d3d9.dll").identity, first)) return 42;
    lock.Release();
    if (!fixture(client, jobs, first, second, lock) ||
        ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Stopped).state != UninstallMemberState::Applied) return 43;
    const std::wstring mixedOperations = root + L"\\uninstall-mixed-operations", mixedRepairs = root + L"\\uninstall-mixed-repairs",
                       installId = L"abcdef0123456789abcdef0123456789", installDir = mixedOperations + L"\\" + installId;
    if (!CreateDirectoryW(mixedOperations.c_str(), nullptr) || !CreateDirectoryW(mixedRepairs.c_str(), nullptr) ||
        !CreateDirectoryW(installDir.c_str(), nullptr)) return 44;
    const auto prior = CaptureClientRecord(client);
    if (prior.state != CapturedRecordState::Present || !BindJournalSession(installDir, installId, lock, catalog, prior, false)) return 45;
    JournalItem installItem; installItem.target = catalog.members[1].target; installItem.candidateSha256 = catalog.members[1].sha256;
    installItem.staged = installItem.target + L".nr-" + installId + L".stage";
    JournalCursor installCursor;
    if (!PersistJournalEvent(installDir, installCursor, {1, JournalKind::Prepare, installItem})) return 46;
    const auto beforeMixed = HashFile(client + L"\\d3d9.dll");
    const auto mixed = RecoverBeforeWork(mixedOperations, mixedRepairs, jobs, mixedOperations, client, {}, catalog, manifest, lock, &Stopped);
    const auto installationScan = ScanClientOperations(mixedOperations, lock, catalog);
    if (mixed.ready || mixed.recoveredUninstall || !beforeMixed ||
        !SameIdentity(HashFile(client + L"\\d3d9.dll").identity, beforeMixed.identity) ||
        !SameIdentity(HashFile(client + L"\\d3d9.dll.nr-uninstall-" + id + L".original").identity, first) ||
        !installationScan.ok || !installationScan.blocksNewOperation) return 47;
    std::puts("uninstall member: write, rollback and finalization interruptions, 9 real terminations and conflicts passed");
    return 0;
}
