#define CONTROL_CENTER_TESTING
#include "../native/control_center/install_member.hpp"
#include "../native/control_center/reconcile.hpp"
#include "../native/control_center/recover_operation.hpp"
#include "../native/control_center/install_batch.hpp"
#include <cstdio>

static control_center::MemberCheckpoint stopPoint;
static bool interruptEnabled = false;
static bool Interrupt(control_center::MemberCheckpoint point) { return interruptEnabled && point == stopPoint; }
static int publishIntents = 0;
static bool InterruptSecond(control_center::MemberCheckpoint point) {
    return point == control_center::MemberCheckpoint::Intent && ++publishIntents == 2;
}
static control_center::ProcessCheck Stopped() { control_center::ProcessCheck c; c.state = control_center::ProcessState::Stopped; return c; }
static control_center::ProcessCheck Running() { control_center::ProcessCheck c; c.state = control_center::ProcessState::Running; return c; }
static int processCalls = 0;
static control_center::ProcessCheck StartsDuringPublish() { return ++processCalls >= 4 ? Running() : Stopped(); }

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    using namespace control_center;
    const std::wstring root = argv[1];
    GuardedPath candidate;
    if (!candidate.Open(root, L"real/occupied.bin")) return 2;
    const auto candidateHash = HashFile(candidate.Path());
    if (!candidateHash) return 3;
    CatalogMember member;
    member.target = L"d3d9.dll"; member.source = L"real/occupied.bin"; member.sourceKind = "stage";
    member.sha256 = HexSha256(candidateHash.identity.sha256); member.replacesOriginal = true;
    CatalogResult catalog; catalog.members.push_back(member);
    const std::wstring id = L"00112233445566778899aabbccddeeff";
    int serial = 0;
    auto fixture = [&](bool existing, MemberInstallContext& ctx, PreviewItem& preview) {
        ++serial;
        ctx.clientRoot = root + L"\\member-client-" + std::to_wstring(serial);
        ctx.journalRoot = root + L"\\member-journal-" + std::to_wstring(serial);
        ctx.stageRoot = root; ctx.templateRoot = root; ctx.operationId = id;
        ctx.testProcesses = &Stopped; ctx.testInterrupt = &Interrupt;
        if (!CreateDirectoryW(ctx.clientRoot.c_str(), nullptr) || !CreateDirectoryW(ctx.journalRoot.c_str(), nullptr) ||
            !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (ctx.clientRoot + L"\\wow.exe").c_str(), TRUE)) return false;
        if (existing) {
            GuardedPath original;
            if (!original.Open(ctx.clientRoot, L"d3d9.dll") || !WriteNewVerified(original, "abc")) return false;
        }
        const auto scan = PreviewInstall(ctx.clientRoot, ctx.stageRoot, ctx.templateRoot, catalog);
        if (!scan.filesReady || scan.items.size() != 1) return false;
        preview = scan.items[0]; return true;
    };
    MemberInstallContext ctx; PreviewItem preview;
    if (!fixture(false, ctx, preview)) return 4;
    ClientLock lock;
    if (!lock.Acquire(ctx.clientRoot)) return 5;
    auto result = InstallMember(ctx, catalog, preview, lock);
    if (result.state != MemberInstallState::Applied) { std::fprintf(stderr, "absent install: %s %lu\n", result.reason.c_str(), result.win32Error); return 6; }
    auto journal = LoadJournal(ctx.journalRoot);
    if (!journal || journal.cursor.Applied().size() != 1 || journal.cursor.Pending() || journal.cursor.Prepared()) return 7;
    GuardedPath persistent;
    if (!persistent.Open(ctx.journalRoot, L"stage/d3d9.dll") ||
        HashFile(persistent.Path()).identity.sha256 != candidateHash.identity.sha256) return 47;
    GuardedPath installed;
    if (!installed.Open(ctx.clientRoot, L"d3d9.dll") || HashFile(installed.Path()).identity.sha256 != candidateHash.identity.sha256) return 8;
    const std::wstring usedJournal = ctx.journalRoot;
    lock.Release();

    if (!fixture(false, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 27;
    ctx.journalRoot = usedJournal;
    result = InstallMember(ctx, catalog, preview, lock);
    if (result.state != MemberInstallState::RecoveryRequired || result.reason != "journal session does not match client or catalog") return 28;
    GuardedPath untouched;
    if (!untouched.Open(ctx.clientRoot, L"d3d9.dll") || GetFileAttributesW(untouched.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 29;
    lock.Release();

    if (!fixture(true, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 9;
    result = InstallMember(ctx, catalog, preview, lock);
    if (result.state != MemberInstallState::Applied) { std::fprintf(stderr, "replacement: %s %lu\n", result.reason.c_str(), result.win32Error); return 10; }
    GuardedPath backup, old;
    if (!backup.Open(ctx.clientRoot, L"host64/dlss5-backup/d3d9.dll") || !old.Open(ctx.clientRoot, result.item.quarantine) ||
        HexSha256(HashFile(backup.Path()).identity.sha256) != result.item.previousSha256 ||
        HexSha256(HashFile(old.Path()).identity.sha256) != result.item.previousSha256) return 11;
    GuardedPath operationBackup;
    if (!operationBackup.Open(ctx.journalRoot, L"backup/d3d9.dll") ||
        HexSha256(HashFile(operationBackup.Path()).identity.sha256) != result.item.previousSha256) return 48;
    lock.Release();

    const MemberCheckpoint steps[] = {MemberCheckpoint::PreparedIntent, MemberCheckpoint::BackupReady, MemberCheckpoint::PersistentStageReady, MemberCheckpoint::StageReady,
        MemberCheckpoint::Prepared, MemberCheckpoint::Intent, MemberCheckpoint::Quarantined, MemberCheckpoint::Published, MemberCheckpoint::Applied};
    for (const auto step : steps) {
        if (!fixture(true, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 12;
        stopPoint = step; interruptEnabled = true;
        result = InstallMember(ctx, catalog, preview, lock);
        interruptEnabled = false;
        if (result.state != MemberInstallState::RecoveryRequired) return 13;
        journal = LoadJournal(ctx.journalRoot);
        if (!journal) return 14;
        const auto decision = ReconcilePending(ctx.clientRoot, ctx.clientRoot + L"\\host64\\dlss5-backup", journal.cursor).decision;
        if ((step == MemberCheckpoint::PreparedIntent || step == MemberCheckpoint::BackupReady ||
             step == MemberCheckpoint::PersistentStageReady || step == MemberCheckpoint::StageReady) &&
            decision != ReconcileDecision::PrepareIncomplete) return 15;
        if (step == MemberCheckpoint::Prepared && !journal.cursor.Prepared()) return 16;
        if (step == MemberCheckpoint::Intent && decision != ReconcileDecision::BeforeMutation) return 17;
        if (step == MemberCheckpoint::Quarantined && decision != ReconcileDecision::RestoreQuarantinedOriginal) return 18;
        if (step == MemberCheckpoint::Published && decision != ReconcileDecision::Applied) return 19;
        if (step == MemberCheckpoint::Applied && journal.cursor.Applied().size() != 1) return 20;
        const auto recovered = RecoverOperation(ctx, catalog, lock);
        if (recovered.state != RecoveryState::Recovered) {
            std::fprintf(stderr, "recovery after step %d: %s %lu\n", static_cast<int>(step), recovered.reason.c_str(), recovered.win32Error);
            return 32;
        }
        GuardedPath restored;
        if (!restored.Open(ctx.clientRoot, L"d3d9.dll")) return 33;
        const auto original = HashFile(restored.Path());
        if (!original || original.identity.sha256 != preview.existing.sha256) return 34;
        journal = LoadJournal(ctx.journalRoot);
        if (!journal || !journal.cursor.Terminal()) return 35;
        lock.Release();
    }
    if (!fixture(true, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 30;
    processCalls = 0; ctx.testProcesses = &StartsDuringPublish;
    result = InstallMember(ctx, catalog, preview, lock);
    journal = LoadJournal(ctx.journalRoot);
    if (result.state != MemberInstallState::RecoveryRequired || !journal ||
        ReconcilePending(ctx.clientRoot, ctx.clientRoot + L"\\host64\\dlss5-backup", journal.cursor).decision !=
            ReconcileDecision::RestoreQuarantinedOriginal) return 31;
    ctx.testProcesses = &Stopped;
    if (RecoverOperation(ctx, catalog, lock).state != RecoveryState::Recovered) return 36;
    lock.Release();
    const MemberCheckpoint rollbackSteps[] = {MemberCheckpoint::RollbackIntent, MemberCheckpoint::RollbackCandidateIsolated,
        MemberCheckpoint::RollbackOriginalRestored, MemberCheckpoint::RollbackRecorded};
    for (const auto step : rollbackSteps) {
        if (!fixture(true, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 37;
        result = InstallMember(ctx, catalog, preview, lock);
        if (result.state != MemberInstallState::Applied) return 38;
        stopPoint = step; interruptEnabled = true;
        auto recovered = RecoverOperation(ctx, catalog, lock);
        interruptEnabled = false;
        if (recovered.state != RecoveryState::RecoveryRequired) return 39;
        recovered = RecoverOperation(ctx, catalog, lock);
        if (recovered.state != RecoveryState::Recovered) return 40;
        GuardedPath restored;
        if (!restored.Open(ctx.clientRoot, L"d3d9.dll")) return 41;
        const auto original = HashFile(restored.Path());
        if (!original || original.identity.sha256 != preview.existing.sha256) return 42;
        lock.Release();
    }
    if (!fixture(false, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 43;
    result = InstallMember(ctx, catalog, preview, lock);
    if (result.state != MemberInstallState::Applied) return 44;
    stopPoint = MemberCheckpoint::RollbackDeleted; interruptEnabled = true;
    auto recovered = RecoverOperation(ctx, catalog, lock);
    interruptEnabled = false;
    if (recovered.state != RecoveryState::RecoveryRequired || RecoverOperation(ctx, catalog, lock).state != RecoveryState::Recovered) return 45;
    GuardedPath deleted;
    if (!deleted.Open(ctx.clientRoot, L"d3d9.dll") || GetFileAttributesW(deleted.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 46;
    lock.Release();
    if (!fixture(false, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 21;
    GuardedPath unexpected;
    if (!unexpected.Open(ctx.clientRoot, L"d3d9.dll") || !WriteNewVerified(unexpected, "user file")) return 22;
    result = InstallMember(ctx, catalog, preview, lock);
    if (result.state != MemberInstallState::PreconditionFailed) return 23;
    const auto preserved = HashFile(unexpected.Path());
    std::array<std::uint8_t, 32> userHash{};
    if (HashBytes("user file", userHash) < 0 || !preserved || preserved.identity.sha256 != userHash) return 24;
    lock.Release();
    if (!fixture(false, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 25;
    ctx.testProcesses = &Running;
    result = InstallMember(ctx, catalog, preview, lock);
    if (result.state != MemberInstallState::PreconditionFailed || LoadJournal(ctx.journalRoot).cursor.Next() != 1) return 26;
    lock.Release();

    if (!fixture(true, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 49;
    CatalogMember second = member;
    second.target = L"host64/extra.dll"; second.replacesOriginal = false;
    catalog.members.push_back(second);
    const auto batch = PreviewInstall(ctx.clientRoot, ctx.stageRoot, ctx.templateRoot, catalog);
    if (!batch.filesReady || batch.items.size() != 2) return 50;
    if (InstallMember(ctx, catalog, batch.items[0], lock).state != MemberInstallState::Applied ||
        InstallMember(ctx, catalog, batch.items[1], lock).state != MemberInstallState::Applied) return 51;
    GuardedPath changed;
    if (!changed.Open(ctx.clientRoot, second.target)) return 52;
    HANDLE changedFile = CreateFileW(changed.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (changedFile == INVALID_HANDLE_VALUE) return 53;
    DWORD changedBytes = 0;
    if (!WriteFile(changedFile, "user", 4, &changedBytes, nullptr) || changedBytes != 4) { CloseHandle(changedFile); return 54; }
    CloseHandle(changedFile);
    const auto changedHash = HashFile(changed.Path());
    if (!changedHash) return 55;
    recovered = RecoverOperation(ctx, catalog, lock);
    if (recovered.state != RecoveryState::Incomplete || recovered.unresolved.size() != 1 ||
        !SameTarget(recovered.unresolved[0].item.target, second.target)) return 56;
    const auto afterUserFile = HashFile(changed.Path());
    GuardedPath restored;
    if (!afterUserFile || afterUserFile.identity.sha256 != changedHash.identity.sha256 ||
        !restored.Open(ctx.clientRoot, L"d3d9.dll")) return 57;
    const auto original = HashFile(restored.Path());
    if (!original || original.identity.sha256 != batch.items[0].existing.sha256) return 58;
    journal = LoadJournal(ctx.journalRoot);
    if (!journal || !journal.cursor.Terminal() || journal.cursor.Unresolved().size() != 1) return 59;
    lock.Release();
    catalog.members.pop_back();

    // A damaged prior record must stop the operation before the first event.
    if (!fixture(false, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 60;
    GuardedPath recordPath;
    if (!recordPath.Open(ctx.clientRoot, L"host64/dlss5-install-record.json", true) ||
        !WriteNewVerified(recordPath, "invalid record")) return 61;
    result = InstallMember(ctx, catalog, preview, lock);
    if (result.state != MemberInstallState::PreconditionFailed || LoadJournal(ctx.journalRoot).cursor.Next() != 1) return 62;
    lock.Release();

    // Recovery uses the session snapshot, and leaves the changed record alone.
    if (!fixture(true, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 63;
    const Json prior = {{"schema", 1}, {"status", "installed"}, {"entries", Json::array({
        {{"target", "host64/extra.dll"}, {"sha256", std::string(64, 'a')}, {"pre_state", "absent"},
         {"replaces_original", false}, {"previous_sha256", nullptr}, {"backup", nullptr}}})}};
    GuardedPath priorPath;
    if (!priorPath.Open(ctx.clientRoot, L"host64/dlss5-install-record.json", true) ||
        !WriteNewVerified(priorPath, prior.dump())) return 64;
    result = InstallMember(ctx, catalog, preview, lock);
    if (result.state != MemberInstallState::Applied) return 65;
    const auto captured = ReadSessionPriorRecord(ctx.journalRoot);
    if (captured.state != CapturedRecordState::Present || captured.bytes != prior.dump()) return 66;
    HANDLE corrupt = CreateFileW(priorPath.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (corrupt == INVALID_HANDLE_VALUE) return 67;
    DWORD written = 0;
    if (!WriteFile(corrupt, "changed", 7, &written, nullptr) || written != 7) { CloseHandle(corrupt); return 68; }
    CloseHandle(corrupt);
    const auto changedRecord = HashFile(priorPath.Path());
    recovered = RecoverOperation(ctx, catalog, lock);
    if (recovered.state != RecoveryState::Recovered) return 69;
    const auto afterRecord = HashFile(priorPath.Path());
    GuardedPath restoredPrior;
    if (!changedRecord || !afterRecord || !SameIdentity(changedRecord.identity, afterRecord.identity) ||
        !restoredPrior.Open(ctx.clientRoot, L"d3d9.dll") ||
        HashFile(restoredPrior.Path()).identity.sha256 != preview.existing.sha256) return 70;
    lock.Release();

    // Corrupt session metadata must not authorize rollback.
    if (!fixture(false, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 71;
    if (InstallMember(ctx, catalog, preview, lock).state != MemberInstallState::Applied) return 72;
    GuardedPath sessionPath;
    if (!sessionPath.Open(ctx.journalRoot, L"session.json")) return 73;
    HANDLE sessionFile = CreateFileW(sessionPath.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sessionFile == INVALID_HANDLE_VALUE) return 74;
    written = 0;
    if (!WriteFile(sessionFile, "{}", 2, &written, nullptr) || written != 2) { CloseHandle(sessionFile); return 75; }
    CloseHandle(sessionFile);
    recovered = RecoverOperation(ctx, catalog, lock);
    GuardedPath stillInstalled;
    if (recovered.state != RecoveryState::RecoveryRequired || !stillInstalled.Open(ctx.clientRoot, L"d3d9.dll") ||
        !HashFile(stillInstalled.Path())) return 76;
    lock.Release();

    if (!fixture(true, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 77;
    catalog.members.push_back(second);
    auto approved = PreviewInstall(ctx.clientRoot, ctx.stageRoot, ctx.templateRoot, catalog);
    if (!approved.filesReady || approved.items.size() != 2) return 78;
    auto batchResult = InstallBatch(ctx, catalog, approved, lock);
    if (batchResult.state != BatchInstallState::ReadyToFinalize || batchResult.applied != 2) return 79;
    if (RecoverOperation(ctx, catalog, lock).state != RecoveryState::Recovered) return 80;
    lock.Release();
    catalog.members.pop_back();

    if (!fixture(true, ctx, preview) || !lock.Acquire(ctx.clientRoot)) return 81;
    catalog.members.push_back(second);
    approved = PreviewInstall(ctx.clientRoot, ctx.stageRoot, ctx.templateRoot, catalog);
    publishIntents = 0; ctx.testInterrupt = &InterruptSecond;
    batchResult = InstallBatch(ctx, catalog, approved, lock);
    if (batchResult.state != BatchInstallState::RolledBack || batchResult.applied != 1) {
        std::fprintf(stderr, "batch rollback: %s %d\n", batchResult.reason.c_str(), static_cast<int>(batchResult.state)); return 82;
    }
    GuardedPath batchOriginal, batchSecond;
    if (!batchOriginal.Open(ctx.clientRoot, L"d3d9.dll") || !batchSecond.Open(ctx.clientRoot, second.target) ||
        HashFile(batchOriginal.Path()).identity.sha256 != approved.items[0].existing.sha256 ||
        GetFileAttributesW(batchSecond.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 83;
    lock.Release();
    std::puts("member install: backups, stale preview, interruption recovery and multi-file partial rollback passed");
    return 0;
}
