#define CONTROL_CENTER_TESTING
#include "../native/control_center/coordinated_install.hpp"
#include "../native/control_center/uninstall_finalize.hpp"
#include "../native/control_center/uninstall_finalize_prepare.hpp"
#include <cstdio>

using namespace control_center;
static UninstallFinalizeCheckpoint stopAt;
static bool InterruptFinalize(UninstallFinalizeCheckpoint point) { return point == stopAt; }
static HANDLE readyEvent = nullptr, releaseEvent = nullptr;
static bool CrashFinalize(UninstallFinalizeCheckpoint point) {
    if (point != stopAt) return false;
    SetEvent(readyEvent); WaitForSingleObject(releaseEvent, 30000); return true;
}
static ProcessCheck Stopped() { ProcessCheck result; result.state = ProcessState::Stopped; return result; }
static ProcessCheck Running() { ProcessCheck result; result.state = ProcessState::Running; return result; }
static bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool Missing(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && MissingPathError(GetLastError());
}

int wmain(int argc, wchar_t** argv) {
    const bool child = argc == 8 && std::wstring(argv[1]) == L"--finalize-uninstall-child";
    if (!child && argc != 2) return 1;
    const std::wstring root = child ? argv[2] : argv[1], id = L"0123456789abcdef0123456789abcdef";
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 2;
    const auto hash = HashFile(source.Path());
    std::array<std::uint8_t, 32> previous{};
    if (!hash || HashBytes("original binary", previous) < 0) return 3;
    const char* names[] = {"d3d9.dll", "host64/new.dll", "host64/missing.dll"};
    CatalogResult catalog;
    Json entries = Json::array(), manifestEntries = Json::array();
    for (size_t i = 0; i < 3; ++i) {
        CatalogMember member;
        if (!Utf8Wide(names[i], member.target)) return 4;
        member.source = L"real/occupied.bin"; member.sourceKind = "stage"; member.sha256 = HexSha256(hash.identity.sha256);
        member.replacesOriginal = i == 0;
        const bool existed = i != 1;
        catalog.members.push_back(member);
        entries.push_back({{"target", names[i]}, {"sha256", member.sha256}, {"replaces_original", member.replacesOriginal},
            {"pre_state", existed ? "present" : "absent"}, {"previous_sha256", existed ? Json(HexSha256(previous)) : Json(nullptr)},
            {"backup", existed ? Json(names[i]) : Json(nullptr)}});
        manifestEntries.push_back({{"target", names[i]}, {"source_kind", "stage"}, {"folder", "fixture"}, {"purpose", "fixture"},
                                   {"replaces_original", member.replacesOriginal}, {"required", true}});
    }
    const Json residue = Json::array({{{"path", "host64/live-params.json"}, {"kind", "file"}, {"policy", "delete"}},
                                      {{"path", "host64/panel-logs"}, {"kind", "tree"}, {"policy", "delete"}}});
    const Json manifest = {{"schema", 1}, {"updated", "fixture"}, {"entries", manifestEntries}, {"residue", residue}};
    if (child) {
        stopAt = static_cast<UninstallFinalizeCheckpoint>(_wtoi(argv[5]));
        readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[6]);
        releaseEvent = OpenEventW(SYNCHRONIZE, FALSE, argv[7]);
        if (!readyEvent || !releaseEvent) return 26;
        ClientLock childLock;
        if (!childLock.Acquire(argv[3])) return 27;
        const auto finalized = FinalizeUninstall(argv[3], argv[4], id, {}, catalog, manifest, childLock,
                                                 "2026-11-14T00:00:00Z", &Stopped, &CrashFinalize);
        return finalized.state == UninstallFinalizeState::RecoveryRequired ? 0 : 28;
    }
    int counter = 0;
    auto fixture = [&](std::wstring& client, std::wstring& jobs, FileIdentity& first, FileIdentity& second, ClientLock& lock) {
        ++counter; const auto suffix = std::to_wstring(counter);
        client = root + L"\\uninstall-finalize-client-" + suffix;
        jobs = root + L"\\uninstall-finalize-jobs-" + suffix;
        if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(jobs.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return false;
        const Json record = {{"schema", 1}, {"status", "installed"}, {"entries", entries}, {"residue", residue}};
        GuardedPath recordPath, target, other, backup, missingBackup, liveParams, panelLog, panelSubLog;
        if (!recordPath.Open(client, L"host64/dlss5-install-record.json", true) || !WriteNewVerified(recordPath, record.dump()) ||
            !target.Open(client, L"d3d9.dll") || !other.Open(client, L"host64/new.dll") ||
            !backup.Open(client, L"host64/dlss5-backup/d3d9.dll", true) || !WriteNewVerified(backup, "original binary") ||
            !missingBackup.Open(client, L"host64/dlss5-backup/host64/missing.dll", true) || !WriteNewVerified(missingBackup, "original binary") ||
            !liveParams.Open(client, L"host64/live-params.json", true) || !WriteNewVerified(liveParams, "live params") ||
            !panelLog.Open(client, L"host64/panel-logs/a.log", true) || !WriteNewVerified(panelLog, "panel log") ||
            !panelSubLog.Open(client, L"host64/panel-logs/sub/b.log", true) || !WriteNewVerified(panelSubLog, "nested log")) return false;
        const auto one = WriteNewVerified(target, "user modified first");
        if (!one) return false;
        const auto written = WriteNewVerified(other, "user modified second");
        if (!written) return false;
        second = written.identity;
        if (!lock.Acquire(client)) return false;
        first = one.identity;
        const auto preview = PreviewUninstall(client, {}, catalog, manifest, lock);
        return preview.valid && preview.canUninstall &&
            PrepareUninstallPlan(client, jobs, id, {}, catalog, manifest, preview, lock).state == UninstallPlanState::Ready;
    };
    auto advance = [&](const std::wstring& client, const std::wstring& jobs, ClientLock& lock) {
        for (size_t i = 0; i < 3; ++i)
            if (ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, i, lock, &Stopped).state != UninstallMemberState::Applied)
                return false;
        return PrepareUninstallFinalization(client, jobs, id, {}, catalog, manifest, lock, &Stopped).ready;
    };
    auto completed = [&](const std::wstring& client, const std::wstring& jobs, const FileIdentity& first, const FileIdentity& second) {
        (void)first; (void)second;
        if (!Missing(client + L"\\host64\\dlss5-install-record.json") || !Missing(client + L"\\host64\\dlss5-backup") ||
            !Missing(client + L"\\host64\\live-params.json") || !Missing(client + L"\\host64\\panel-logs") ||
            !Missing(client + L"\\d3d9.dll.nr-uninstall-" + id + L".original") ||
            !Missing(client + L"\\host64\\new.dll.nr-uninstall-" + id + L".original") ||
            Exists(client + L"\\host64\\new.dll") || !Exists(jobs + L"\\" + id + L"\\result.json")) return false;
        const auto restored = HashFile(client + L"\\d3d9.dll");
        const auto missingRestored = HashFile(client + L"\\host64\\missing.dll");
        return restored && missingRestored && restored.identity.sha256 == previous && missingRestored.identity.sha256 == previous;
    };
    // 1. The normal path completes every cleanup step and unblocks the client.
    {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock) || !advance(client, jobs, lock)) return 5;
        const auto prepared = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
        if (!prepared.ok || prepared.uninstalls.size() != 1 ||
            prepared.uninstalls[0].state != UninstallScanState::AwaitingFinalization || !prepared.blocksNewOperation) {
            std::fprintf(stderr, "prepared scan: ok=%d block=%d error=%s n=%zu", prepared.ok ? 1 : 0, prepared.blocksNewOperation ? 1 : 0,
                         prepared.error.c_str(), prepared.uninstalls.size());
            if (prepared.uninstalls.size() == 1) std::fprintf(stderr, " state=%d", static_cast<int>(prepared.uninstalls[0].state));
            std::fprintf(stderr, "\n");
            return 6;
        }
        const auto finalized = FinalizeUninstall(client, jobs, id, {}, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Stopped);
        if (finalized.state != UninstallFinalizeState::Completed) {
            std::fprintf(stderr, "uninstall finalize: %s %lu\n", finalized.reason.c_str(), finalized.win32Error); return 7;
        }
        if (!completed(client, jobs, first, second)) return 8;
        const auto scanned = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
        if (!scanned.ok || scanned.blocksNewOperation || scanned.uninstalls.size() != 1 ||
            scanned.uninstalls[0].state != UninstallScanState::Finalized) return 9;
        const auto again = FinalizeUninstall(client, jobs, id, {}, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Stopped);
        if (again.state != UninstallFinalizeState::AlreadyCompleted) return 10;
        const std::wstring operations = root + L"\\uninstall-finalize-operations-1", repairs = root + L"\\uninstall-finalize-repairs-1";
        if (!CreateDirectoryW(operations.c_str(), nullptr) || !CreateDirectoryW(repairs.c_str(), nullptr)) return 11;
        const auto recovered = RecoverBeforeWork(operations, repairs, jobs, operations, client, {}, catalog, manifest, lock, &Stopped);
        if (!recovered.ready || recovered.recoveredUninstall) {
            std::fprintf(stderr, "recover before work: ready=%d recoveredUninstall=%d reason=%s win32=%lu\n",
                         recovered.ready ? 1 : 0, recovered.recoveredUninstall ? 1 : 0, recovered.reason.c_str(),
                         recovered.win32Error);
            return 12;
        }
        lock.Release();
    }
    // 1b. Recovery finishes a completed member run before any new work starts.
    {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock) || !advance(client, jobs, lock)) return 12;
        const auto waiting = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
        if (!waiting.ok || !waiting.blocksNewOperation || waiting.uninstalls.size() != 1 ||
            waiting.uninstalls[0].state != UninstallScanState::AwaitingFinalization) return 12;
        const std::wstring operations = root + L"\\uninstall-finalize-operations-1b", repairs = root + L"\\uninstall-finalize-repairs-1b";
        if (!CreateDirectoryW(operations.c_str(), nullptr) || !CreateDirectoryW(repairs.c_str(), nullptr)) return 12;
        const auto recovered = RecoverBeforeWork(operations, repairs, jobs, operations, client, {}, catalog, manifest, lock, &Stopped);
        if (!recovered.ready || !recovered.recoveredUninstall) {
            std::fprintf(stderr, "recover before work (pending): ready=%d recoveredUninstall=%d reason=%s win32=%lu\n",
                         recovered.ready ? 1 : 0, recovered.recoveredUninstall ? 1 : 0, recovered.reason.c_str(),
                         recovered.win32Error);
            return 12;
        }
        if (!completed(client, jobs, first, second)) return 12;
        const auto finalizedScan = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
        if (!finalizedScan.ok || finalizedScan.blocksNewOperation || finalizedScan.uninstalls.size() != 1 ||
            finalizedScan.uninstalls[0].state != UninstallScanState::Finalized) return 12;
        lock.Release();
    }
    // 2. Every checkpoint resumes without a second decision and without data loss.
    const UninstallFinalizeCheckpoint points[] = {UninstallFinalizeCheckpoint::DecisionPrepared,
        UninstallFinalizeCheckpoint::DecisionPublished, UninstallFinalizeCheckpoint::IsolatedRemoved,
        UninstallFinalizeCheckpoint::BackupsRemoved, UninstallFinalizeCheckpoint::ResidueRemoved,
        UninstallFinalizeCheckpoint::RecordRemoved, UninstallFinalizeCheckpoint::ResultPublished};
    for (const auto point : points) {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock) || !advance(client, jobs, lock)) return 13;
        stopAt = point;
        const auto interrupted = FinalizeUninstall(client, jobs, id, {}, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Stopped, &InterruptFinalize);
        if (interrupted.state != UninstallFinalizeState::RecoveryRequired) return 14;
        if (point == UninstallFinalizeCheckpoint::DecisionPublished) {
            const auto scanning = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
            if (!scanning.ok || scanning.uninstalls.size() != 1 || scanning.uninstalls[0].state != UninstallScanState::Finalizing ||
                !scanning.blocksNewOperation) return 15;
        }
        const auto resumed = FinalizeUninstall(client, jobs, id, {}, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Stopped);
        if (resumed.state != UninstallFinalizeState::Completed && resumed.state != UninstallFinalizeState::AlreadyCompleted) {
            std::fprintf(stderr, "uninstall finalize resume point %d: %s %lu\n", static_cast<int>(point), resumed.reason.c_str(), resumed.win32Error);
            return 16;
        }
        if (!completed(client, jobs, first, second)) return 17;
        lock.Release();
    }
    // 3. A killed process leaves recoverable state, not a second decision.
    {
        const UninstallFinalizeCheckpoint crashPoints[] = {UninstallFinalizeCheckpoint::DecisionPrepared,
            UninstallFinalizeCheckpoint::IsolatedRemoved, UninstallFinalizeCheckpoint::ResultPublished};
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        wchar_t executable[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 18;
        for (size_t i = 0; i < 3; ++i) {
            if (!fixture(client, jobs, first, second, lock) || !advance(client, jobs, lock)) return 19;
            lock.Release();
            const int point = static_cast<int>(crashPoints[i]);
            const std::wstring eventName = L"Local\\WOW335-uninstall-finalize-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i);
            const std::wstring releaseName = eventName + L"-release";
            HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
            HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
            if (!ready || !release) { if (ready) CloseHandle(ready); if (release) CloseHandle(release); return 20; }
            std::wstring command = L"\"" + std::wstring(executable) + L"\" --finalize-uninstall-child \"" + root + L"\" \"" + client +
                L"\" \"" + jobs + L"\" " + std::to_wstring(point) + L" " + eventName + L" " + releaseName;
            STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
                CloseHandle(ready); CloseHandle(release); return 21;
            }
            HANDLE waits[] = {ready, process.hProcess};
            const DWORD observed = WaitForMultipleObjects(2, waits, FALSE, 20000);
            const BOOL terminated = TerminateProcess(process.hProcess, 99);
            if (!terminated) SetEvent(release);
            const DWORD finished = WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(ready); CloseHandle(release);
            if (observed != WAIT_OBJECT_0 || !terminated || finished != WAIT_OBJECT_0 || !lock.Acquire(client)) return 22;
            const auto resumed = FinalizeUninstall(client, jobs, id, {}, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Stopped);
            if (resumed.state != UninstallFinalizeState::Completed && resumed.state != UninstallFinalizeState::AlreadyCompleted) {
                std::fprintf(stderr, "uninstall finalize crash point %d: %s %lu\n", point, resumed.reason.c_str(), resumed.win32Error);
                return 23;
            }
            if (!completed(client, jobs, first, second)) return 24;
            lock.Release();
        }
    }
    // 4. Preconditions: blocking process, unfinished members, changed artifacts.
    {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock)) return 30;
        for (size_t i = 0; i < 3; ++i)
            if (ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, i, lock, &Stopped).state != UninstallMemberState::Applied) return 31;
        if (FinalizeUninstall(client, jobs, id, {}, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Running).state !=
            UninstallFinalizeState::PreconditionFailed) return 32;
        if (!PrepareUninstallFinalization(client, jobs, id, {}, catalog, manifest, lock, &Stopped).ready) return 33;
        const std::wstring snapshotPath = jobs + L"\\" + id + L"\\record.snapshot";
        HANDLE changedSnapshot = CreateFileW(snapshotPath.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (changedSnapshot == INVALID_HANDLE_VALUE) return 34;
        DWORD written = 0; const bool changed = WriteFile(changedSnapshot, "changed", 7, &written, nullptr) && written == 7;
        CloseHandle(changedSnapshot);
        if (!changed || FinalizeUninstall(client, jobs, id, {}, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Stopped).state !=
            UninstallFinalizeState::PreconditionFailed) return 35;
        lock.Release();
    }
    {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock) || !advance(client, jobs, lock)) return 36;
        const std::wstring planPath = jobs + L"\\" + id + L"\\plan.json";
        HANDLE planFile = CreateFileW(planPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (planFile == INVALID_HANDLE_VALUE) return 37;
        DWORD written = 0; const bool changed = WriteFile(planFile, " ", 1, &written, nullptr) && written == 1;
        CloseHandle(planFile);
        if (!changed || FinalizeUninstall(client, jobs, id, {}, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Stopped).state !=
            UninstallFinalizeState::PreconditionFailed) return 38;
        lock.Release();
    }
    {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock)) return 39;
        if (ApplyUninstallMember(client, jobs, id, {}, catalog, manifest, 0, lock, &Stopped).state != UninstallMemberState::Applied) return 40;
        if (FinalizeUninstall(client, jobs, id, {}, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Stopped).state !=
            UninstallFinalizeState::PreconditionFailed) return 41;
        lock.Release();
    }
    {
        std::wstring client, jobs; FileIdentity first, second; ClientLock lock;
        if (!fixture(client, jobs, first, second, lock) || !advance(client, jobs, lock)) return 42;
        const std::wstring finalizationPlanPath = jobs + L"\\" + id + L"\\finalization-plan.json";
        HANDLE finalizationPlan = CreateFileW(finalizationPlanPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (finalizationPlan == INVALID_HANDLE_VALUE) return 43;
        DWORD written = 0; const bool changed = WriteFile(finalizationPlan, " ", 1, &written, nullptr) && written == 1;
        CloseHandle(finalizationPlan);
        if (!changed || FinalizeUninstall(client, jobs, id, {}, catalog, manifest, lock, "2026-11-14T00:00:00Z", &Stopped).state !=
            UninstallFinalizeState::PreconditionFailed) return 44;
        lock.Release();
    }
    std::puts("uninstall finalize: completion, residue, backups, checkpoints, terminations and scan states passed");
    return 0;
}
