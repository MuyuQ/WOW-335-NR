#define CONTROL_CENTER_TESTING
#include "../native/control_center/uninstall_plan_read.hpp"
#include "../native/control_center/coordinated_uninstall.hpp"
#include "../native/control_center/write_new.hpp"
#include <cstdio>

using namespace control_center;
static ProcessCheck Stopped() { ProcessCheck result; result.state = ProcessState::Stopped; return result; }
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1], client = root + L"\\uninstall-preview-client";
    if (!CreateDirectoryW(client.c_str(), nullptr) ||
        !CopyFileW((root + L"\\client-x86\\wow.exe").c_str(), (client + L"\\wow.exe").c_str(), TRUE)) return 2;
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 3;
    const auto installedHash = HashFile(source.Path());
    std::array<std::uint8_t, 32> oldHash{};
    if (!installedHash || HashBytes("original binary", oldHash) < 0) return 4;
    CatalogResult catalog; Json entries = Json::array(), manifestEntries = Json::array();
    const char* names[] = {"d3d9.dll", "host64/new.dll", "host64/missing.dll", "host64/restored.dll"};
    for (size_t index = 0; index < 4; ++index) {
        CatalogMember member;
        if (!Utf8Wide(names[index], member.target)) return 5;
        member.source = L"real/occupied.bin"; member.sourceKind = "stage"; member.sha256 = HexSha256(installedHash.identity.sha256);
        member.replacesOriginal = index == 0;
        const bool wasPresent = index == 0 || index == 3;
        catalog.members.push_back(member);
        entries.push_back({{"target", names[index]}, {"sha256", member.sha256}, {"replaces_original", member.replacesOriginal},
            {"pre_state", wasPresent ? "present" : "absent"},
            {"previous_sha256", wasPresent ? Json(HexSha256(oldHash)) : Json(nullptr)},
            {"backup", wasPresent ? Json(names[index]) : Json(nullptr)}});
        manifestEntries.push_back({{"target", names[index]}, {"source_kind", "stage"}, {"folder", "host64"}, {"purpose", "fixture"},
                                   {"replaces_original", member.replacesOriginal}, {"required", true}});
    }
    const Json residues = Json::array({{{"path", "host64/logs"}, {"kind", "tree"}, {"policy", "delete"}}});
    const Json manifest = {{"schema", 1}, {"updated", "fixture"}, {"entries", manifestEntries}, {"residue", residues}};
    std::string clientText;
    if (!WideUtf8(root + L"\\old-client-location", clientText)) return 6;
    Json recordedResidue = residues;
    recordedResidue.push_back({{"path", "host64/unlisted"}, {"kind", "tree"}, {"policy", "delete"}});
    Json record = {{"schema", 1}, {"status", "installed"}, {"client", clientText}, {"entries", entries}, {"residue", recordedResidue}};
    GuardedPath recordPath, restore, changed, restored, backup;
    if (!recordPath.Open(client, L"host64/dlss5-install-record.json", true) || !WriteNewVerified(recordPath, record.dump()) ||
        !restore.Open(client, L"d3d9.dll") || !WriteNewVerified(restore, "modified installed file") ||
        !changed.Open(client, L"host64/new.dll") || !WriteNewVerified(changed, "modified new file") ||
        !restored.Open(client, L"host64/restored.dll") || !WriteNewVerified(restored, "original binary") ||
        !backup.Open(client, L"host64/dlss5-backup/d3d9.dll", true) || !WriteNewVerified(backup, "original binary")) return 7;
    ClientLock lock;
    if (!lock.Acquire(client)) return 8;
    auto preview = PreviewUninstall(client, {}, catalog, manifest, lock);
    if (!preview.valid || !preview.canUninstall || preview.items.size() != 4 || preview.residue.size() != 2 ||
        preview.items[0].state != UninstallItemState::RestoreOriginal || !preview.items[0].saveCurrent ||
        preview.items[1].state != UninstallItemState::DeleteModified || !preview.items[1].saveCurrent ||
        preview.items[2].state != UninstallItemState::AlreadyAbsent || preview.items[3].state != UninstallItemState::AlreadyRestored ||
        !preview.residue[0].matchesManifest || preview.residue[1].matchesManifest) {
        std::fprintf(stderr, "preview valid=%d allowed=%d items=%zu residue=%zu error=%s win32=%lu\n", preview.valid,
                     preview.canUninstall, preview.items.size(), preview.residue.size(), preview.error.c_str(), preview.win32Error);
        for (const auto& item : preview.items) std::fprintf(stderr, "state=%d save=%d win32=%lu\n", static_cast<int>(item.state), item.saveCurrent, item.win32Error);
        return 9;
    }
    const auto approved = preview;
    const std::wstring jobs = root + L"\\uninstall-preview-jobs", operations = root + L"\\uninstall-preview-operations",
        repairs = root + L"\\uninstall-preview-repairs", id = L"0123456789abcdef0123456789abcdef";
    if (!CreateDirectoryW(jobs.c_str(), nullptr) || !CreateDirectoryW(operations.c_str(), nullptr) ||
        !CreateDirectoryW(repairs.c_str(), nullptr)) return 20;
    if (!RecoverBeforeWork(operations, repairs, jobs, operations, client, {}, catalog, manifest, lock).ready ||
        RecoverBeforeWork(operations, operations, jobs, operations, client, {}, catalog, manifest, lock).ready) return 37;
    HANDLE change = CreateFileW(changed.Path().c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (change == INVALID_HANDLE_VALUE) return 21;
    DWORD appended = 0; const bool changedOk = WriteFile(change, "x", 1, &appended, nullptr) && appended == 1; CloseHandle(change);
    if (!changedOk || PrepareUninstallPlan(client, jobs, id, {}, catalog, manifest, approved, lock).state != UninstallPlanState::PreconditionFailed ||
        GetFileAttributesW((jobs + L"\\" + id).c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError())) return 22;
    change = CreateFileW(changed.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (change == INVALID_HANDLE_VALUE) return 23;
    DWORD resetBytes = 0; const char initial[] = "modified new file";
    const bool reset = WriteFile(change, initial, sizeof(initial) - 1, &resetBytes, nullptr) && resetBytes == sizeof(initial) - 1; CloseHandle(change);
    if (!reset) return 24;
    const auto start = StartCoordinatedUninstall(operations, repairs, jobs, operations, client, id, {}, catalog, manifest, approved, lock);
    if (!start.recovery.ready || !start.attemptedPlan) return 43;
    const auto plan = start.plan;
    GuardedPath planPath, sessionPath;
    std::string planBytes, sessionBytes; DWORD error = ERROR_SUCCESS;
    if (plan.state != UninstallPlanState::Ready || plan.items != 4 || !planPath.Open(jobs + L"\\" + id, L"plan.json") ||
        !sessionPath.Open(jobs + L"\\" + id, L"session.json") || !ReadJournalBytes(planPath, planBytes, error) ||
        !ReadJournalBytes(sessionPath, sessionBytes, error)) return 25;
    const auto parsedPlan = ParseJsonObject(planBytes), parsedSession = ParseJsonObject(sessionBytes);
    if (!parsedPlan || !parsedSession || parsedPlan.value["items"].size() != 4 || parsedPlan.value["items"][0]["state"] != "restore" ||
        parsedPlan.value["items"][1]["state"] != "delete_modified" || parsedPlan.value["residue"][1]["trusted"] != false ||
        parsedSession.value["plan_sha256"] != HexSha256(plan.identity.sha256) ||
        PrepareUninstallPlan(client, jobs, id, {}, catalog, manifest, approved, lock).state != UninstallPlanState::PreconditionFailed ||
        !SameIdentity(HashFile(restore.Path()).identity, approved.items[0].target) ||
        !SameIdentity(HashFile(changed.Path()).identity, approved.items[1].target) ||
        !SameIdentity(HashFile(backup.Path()).identity, approved.items[0].backup) ||
        !SameIdentity(HashFile(recordPath.Path()).identity, approved.clientRecord)) return 26;
    auto trusted = ReadTrustedUninstallPlan(client, jobs, id, {}, catalog, manifest, lock);
    if (!trusted.valid || trusted.items.size() != 4 || trusted.items[0].state != UninstallItemState::RestoreOriginal ||
        trusted.items[1].state != UninstallItemState::DeleteModified || !trusted.items[0].targetPresent ||
        !SameIdentity(trusted.items[0].backup, approved.items[0].backup)) return 27;
    auto scan = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
    if (!scan.ok || !scan.blocksNewOperation || scan.uninstalls.size() != 1 ||
        scan.uninstalls[0].state != UninstallScanState::Incomplete ||
        !SameIdentity(HashFile(restore.Path()).identity, approved.items[0].target)) return 38;
    GuardedPath unexpectedFinal;
    if (!unexpectedFinal.Open(jobs + L"\\" + id, L"result.pending") || !WriteNewVerified(unexpectedFinal, "unknown")) return 45;
    scan = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
    if (!scan.ok || !scan.blocksNewOperation || scan.uninstalls.size() != 1 || scan.uninstalls[0].state != UninstallScanState::Unknown ||
        RecoverBeforeWork(operations, repairs, jobs, operations, client, {}, catalog, manifest, lock, &Stopped).ready ||
        !DeleteFileW(unexpectedFinal.Path().c_str())) return 46;
    const std::wstring other = root + L"\\uninstall-preview-other-client";
    if (!CreateDirectoryW(other.c_str(), nullptr)) return 39;
    ClientLock otherLock;
    if (!otherLock.Acquire(other)) return 40;
    const auto otherScan = ScanClientUninstalls(jobs, other, {}, catalog, manifest, otherLock);
    if (!otherScan.ok || otherScan.blocksNewOperation || !otherScan.uninstalls.empty()) return 41;
    auto differentManifest = manifest; differentManifest["updated"] = "changed";
    if (ReadTrustedUninstallPlan(client, jobs, id, {}, catalog, differentManifest, lock).valid) return 28;
    change = CreateFileW(planPath.Path().c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (change == INVALID_HANDLE_VALUE) return 29;
    appended = 0; const bool appendedPlan = WriteFile(change, " ", 1, &appended, nullptr) && appended == 1; CloseHandle(change);
    if (!appendedPlan || ReadTrustedUninstallPlan(client, jobs, id, {}, catalog, manifest, lock).valid) return 30;
    scan = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
    const auto blocked = RecoverBeforeWork(operations, repairs, jobs, operations, client, {}, catalog, manifest, lock, &Stopped);
    if (!scan.ok || !scan.blocksNewOperation || scan.uninstalls.size() != 1 || scan.uninstalls[0].state != UninstallScanState::Unknown || blocked.ready) return 42;
    change = CreateFileW(planPath.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (change == INVALID_HANDLE_VALUE) return 31;
    appended = 0; const bool restoredPlan = WriteFile(change, planBytes.data(), static_cast<DWORD>(planBytes.size()), &appended, nullptr) && appended == planBytes.size();
    CloseHandle(change);
    if (!restoredPlan || !ReadTrustedUninstallPlan(client, jobs, id, {}, catalog, manifest, lock).valid) return 32;
    auto overwrite = [](const std::wstring& path, const std::string& contents) {
        HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return false;
        DWORD count = 0; const bool ok = WriteFile(output, contents.data(), static_cast<DWORD>(contents.size()), &count, nullptr) && count == contents.size();
        CloseHandle(output); return ok;
    };
    auto tamperedPlan = parsedPlan.value; tamperedPlan["items"][0]["state"] = "delete_modified";
    if (!overwrite(planPath.Path(), tamperedPlan.dump() + "\n")) return 34;
    const auto tamperedHash = HashFile(planPath.Path());
    if (!tamperedHash) return 35;
    auto tamperedSession = parsedSession.value;
    tamperedSession["plan_sha256"] = HexSha256(tamperedHash.identity.sha256);
    tamperedSession["plan_identity"] = JournalIdJson(JournalId(tamperedHash.identity));
    if (!overwrite(sessionPath.Path(), tamperedSession.dump() + "\n") ||
        ReadTrustedUninstallPlan(client, jobs, id, {}, catalog, manifest, lock).valid ||
        !overwrite(planPath.Path(), planBytes) || !overwrite(sessionPath.Path(), sessionBytes) ||
        !ReadTrustedUninstallPlan(client, jobs, id, {}, catalog, manifest, lock).valid) return 36;
    const auto reconciled = RecoverBeforeWork(operations, repairs, jobs, operations, client, {}, catalog, manifest, lock, &Stopped);
    scan = ScanClientUninstalls(jobs, client, {}, catalog, manifest, lock);
    if (!reconciled.ready || !reconciled.recoveredUninstall || !scan.ok || scan.blocksNewOperation || scan.uninstalls.size() != 1 ||
        scan.uninstalls[0].state != UninstallScanState::RolledBack ||
        !SameIdentity(HashFile(restore.Path()).identity, approved.items[0].target)) return 44;
    HANDLE file = CreateFileW(backup.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 10;
    DWORD written = 0; const bool ok = WriteFile(file, "wrong", 5, &written, nullptr) && written == 5; CloseHandle(file);
    if (!ok) return 11;
    preview = PreviewUninstall(client, {}, catalog, manifest, lock);
    if (!preview.valid || preview.canUninstall || preview.items[0].state != UninstallItemState::BackupMismatch ||
        !ReadTrustedUninstallPlan(client, jobs, id, {}, catalog, manifest, lock).valid) return 12;
    if (!DeleteFileW(backup.Path().c_str())) return 15;
    preview = PreviewUninstall(client, {}, catalog, manifest, lock);
    if (!preview.valid || preview.canUninstall || preview.items[0].state != UninstallItemState::MissingBackup ||
        !WriteNewVerified(backup, "original binary")) return 16;
    auto writeRecord = [&]() {
        HANDLE output = CreateFileW(recordPath.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return false;
        const auto bytes = record.dump(); DWORD count = 0;
        const bool success = WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size();
        CloseHandle(output); return success;
    };
    record["entries"][0]["previous_sha256"] = nullptr;
    if (!writeRecord()) return 17;
    if (ReadTrustedUninstallPlan(client, jobs, id, {}, catalog, manifest, lock).valid) return 33;
    preview = PreviewUninstall(client, {}, catalog, manifest, lock);
    if (!preview.valid || preview.canUninstall || preview.items[0].state != UninstallItemState::UnverifiedBackup) return 18;
    record["residue"][1]["path"] = "WTF";
    if (!writeRecord() || PreviewUninstall(client, {}, catalog, manifest, lock).valid) return 19;
    record["residue"][1]["path"] = "host64/unlisted";
    record["entries"][0]["backup"] = "host64/new.dll";
    if (!writeRecord() || PreviewUninstall(client, {}, catalog, manifest, lock).valid ||
        PreviewUninstall(client, {client}, catalog, manifest, lock).valid) return 14;
    std::puts("uninstall preview: modified files, missing/restored files, backup and residue authorization passed");
    return 0;
}
