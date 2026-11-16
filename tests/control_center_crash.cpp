#define CONTROL_CENTER_TESTING
#include "../native/control_center/install_member.hpp"
#include "../native/control_center/recover_operation.hpp"
#include <cstdio>
#include <vector>

static HANDLE signalEvent = nullptr, releaseEvent = nullptr;
static control_center::MemberCheckpoint stopPoint;
static control_center::ProcessCheck Stopped() { control_center::ProcessCheck c; c.state = control_center::ProcessState::Stopped; return c; }
static bool CrashCheckpoint(control_center::MemberCheckpoint point) {
    if (point != stopPoint) return false;
    SetEvent(signalEvent);
    WaitForSingleObject(releaseEvent, 30000);
    return true;
}
static bool Inputs(const std::wstring& root, const std::wstring& client, const std::wstring& journal,
                   control_center::MemberInstallContext& context, control_center::CatalogResult& catalog,
                   control_center::PreviewItem& preview) {
    control_center::GuardedPath candidate;
    if (!candidate.Open(root, L"real/occupied.bin")) return false;
    const auto hash = control_center::HashFile(candidate.Path());
    if (!hash) return false;
    control_center::CatalogMember member;
    member.target = L"d3d9.dll"; member.source = L"real/occupied.bin"; member.sourceKind = "stage";
    member.sha256 = control_center::HexSha256(hash.identity.sha256); member.replacesOriginal = true;
    catalog.members = {member};
    context.clientRoot = client; context.journalRoot = journal; context.stageRoot = root; context.templateRoot = root;
    context.operationId = L"00112233445566778899aabbccddeeff"; context.testProcesses = &Stopped;
    const auto scan = control_center::PreviewInstall(client, root, root, catalog);
    if (!scan.filesReady || scan.items.size() != 1) return false;
    preview = scan.items[0]; return true;
}
int wmain(int argc, wchar_t** argv) {
    using namespace control_center;
    if (argc == 8 && std::wstring(argv[1]) == L"--child") {
        const std::wstring root = argv[2], client = argv[3], journal = argv[4];
        stopPoint = static_cast<MemberCheckpoint>(_wtoi(argv[5]));
        signalEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[6]);
        releaseEvent = OpenEventW(SYNCHRONIZE, FALSE, argv[7]);
        if (!signalEvent || !releaseEvent) return 2;
        MemberInstallContext context; CatalogResult catalog; PreviewItem preview;
        if (!Inputs(root, client, journal, context, catalog, preview)) return 3;
        context.testInterrupt = &CrashCheckpoint;
        ClientLock lock;
        if (!lock.Acquire(client)) return 4;
        const auto install = InstallMember(context, catalog, preview, lock);
        if (install.state == MemberInstallState::Applied) (void)RecoverOperation(context, catalog, lock);
        return 5;
    }
    if (argc != 2) return 1;
    const std::wstring root = argv[1];
    const MemberCheckpoint points[] = {MemberCheckpoint::StageReady, MemberCheckpoint::Intent, MemberCheckpoint::Quarantined,
        MemberCheckpoint::Published, MemberCheckpoint::RollbackCandidateIsolated, MemberCheckpoint::RollbackOriginalRestored};
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 6;
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
        const std::wstring suffix = std::to_wstring(i);
        const std::wstring client = root + L"\\crash-client-" + suffix, journal = root + L"\\crash-journal-" + suffix;
        if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(journal.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return 7;
        GuardedPath target;
        if (!target.Open(client, L"d3d9.dll") || !WriteNewVerified(target, "abc")) return 8;
        const auto original = HashFile(target.Path());
        if (!original) return 9;
        const std::wstring eventName = L"Local\\WOW335-crash-event-" + std::to_wstring(GetCurrentProcessId()) + L"-" + suffix;
        const std::wstring releaseName = eventName + L"-release";
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        if (!ready || !release) { if (ready) CloseHandle(ready); if (release) CloseHandle(release); return 10; }
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --child \"" + root + L"\" \"" + client +
            L"\" \"" + journal + L"\" " + std::to_wstring(static_cast<int>(points[i])) + L" " + eventName + L" " + releaseName;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            CloseHandle(ready); CloseHandle(release); return 11;
        }
        HANDLE waits[] = {ready, process.hProcess};
        const DWORD observed = WaitForMultipleObjects(2, waits, FALSE, 20000);
        const BOOL terminated = TerminateProcess(process.hProcess, 99);
        if (!terminated) SetEvent(release);
        const DWORD finished = WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(ready); CloseHandle(release);
        if (observed != WAIT_OBJECT_0 || !terminated || finished != WAIT_OBJECT_0) return 12;
        MemberInstallContext context; CatalogResult catalog;
        // The crash may have removed the target; build the trusted catalog without requiring a fresh install preview.
        GuardedPath source;
        if (!source.Open(root, L"real/occupied.bin")) return 13;
        const auto sourceHash = HashFile(source.Path());
        if (!sourceHash) return 14;
        CatalogMember member;
        member.target = L"d3d9.dll"; member.source = L"real/occupied.bin"; member.sourceKind = "stage";
        member.sha256 = HexSha256(sourceHash.identity.sha256); member.replacesOriginal = true;
        catalog.members = {member};
        context.clientRoot = client; context.journalRoot = journal; context.stageRoot = root; context.templateRoot = root;
        context.operationId = L"00112233445566778899aabbccddeeff"; context.testProcesses = &Stopped;
        ClientLock lock;
        if (!lock.Acquire(client)) return 15;
        const auto recovery = RecoverOperation(context, catalog, lock);
        if (recovery.state != RecoveryState::Recovered) {
            std::fprintf(stderr, "crash recovery at step %d failed: %s (%lu)\n", static_cast<int>(points[i]), recovery.reason.c_str(), recovery.win32Error);
            return 16;
        }
        const auto restored = HashFile(target.Path());
        if (!restored || restored.identity.sha256 != original.identity.sha256 ||
            restored.identity.indexHigh != original.identity.indexHigh || restored.identity.indexLow != original.identity.indexLow) return 17;
    }
    std::puts("real process termination: six installation and rollback checkpoints recovered");
    return 0;
}
