#define CONTROL_CENTER_TESTING
#include "../native/control_center/final_records.hpp"
#include "../native/control_center/finalization_plan.hpp"
#include "../native/control_center/publish_client_record.hpp"
#include "../native/control_center/recover_operation.hpp"
#include "../native/control_center/recover_client_record.hpp"
#include "../native/control_center/commit_validate.hpp"
#include <cstdio>

static control_center::ProcessCheck Stopped() {
    control_center::ProcessCheck result; result.state = control_center::ProcessState::Stopped; return result;
}
static bool StopAfterRecordPublish(control_center::RecordPublishCheckpoint point) {
    return point == control_center::RecordPublishCheckpoint::Published;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    using namespace control_center;
    const std::wstring root = argv[1];
    MemberInstallContext context;
    context.clientRoot = root + L"\\member-client-3"; context.journalRoot = root + L"\\member-journal-3";
    context.stageRoot = root; context.templateRoot = root; context.operationId = L"00112233445566778899aabbccddeeff";
    context.testProcesses = &Stopped;
    GuardedPath source;
    if (!source.Open(root, L"real/occupied.bin")) return 2;
    const auto sourceHash = HashFile(source.Path());
    if (!sourceHash) return 3;
    CatalogMember member;
    member.target = L"d3d9.dll"; member.source = L"real/occupied.bin"; member.sourceKind = "stage";
    member.sha256 = HexSha256(sourceHash.identity.sha256); member.replacesOriginal = true;
    CatalogResult catalog; catalog.members.push_back(member);
    Json manifest = {{"schema", 1}, {"updated", "fixture"},
        {"entries", Json::array({{{"target", "d3d9.dll"}, {"source_kind", "stage"}, {"folder", "root"},
                                  {"purpose", "fixture"}, {"replaces_original", true}, {"required", true}}})},
        {"residue", Json::array({{{"path", "WTF/Config.wtf"}, {"kind", "file"}, {"policy", "report"}}})}};
    const auto snapshot = PreviewInstall(context.clientRoot, root, root, catalog);
    const auto journal = LoadJournal(context.journalRoot);
    if (!snapshot.filesReady || !journal || journal.cursor.Applied().size() != 1) return 4;
    const auto built = BuildFinalRecords(context, catalog, manifest, snapshot, journal.cursor, nullptr, "2026-11-14T00:00:00Z");
    if (!built) { std::fprintf(stderr, "final record: %s\n", built.error.c_str()); return 5; }
    const auto client = ReadClientRecord(built.clientRecord);
    const auto operation = ReadOperationRecord(built.operationRecord);
    if (!client || !operation || client.entries.size() != 1 || !client.entries[0].previouslyPresent ||
        operation.entries.size() != 1 || operation.applied.size() != 1 || operation.status != "completed") return 6;
    Json oldJson = client.original;
    oldJson["entries"][0]["sha256"] = std::string(64, 'd');
    oldJson["entries"][0]["previous_sha256"] = std::string(64, 'e');
    const auto old = ReadClientRecord(oldJson.dump());
    if (!old) return 7;
    const auto merged = BuildFinalRecords(context, catalog, manifest, snapshot, journal.cursor, &old, "2026-11-14T00:00:00Z");
    if (!merged) return 8;
    const auto mergedClient = ReadClientRecord(merged.clientRecord);
    const auto mergedOperation = ReadOperationRecord(merged.operationRecord);
    if (!mergedClient || !mergedOperation || mergedClient.entries[0].sha256 != std::string(64, 'd') ||
        mergedClient.entries[0].previousSha256 != std::string(64, 'e') ||
        mergedOperation.original["previous_client_record"] != old.original) return 9;
    auto badSnapshot = snapshot;
    badSnapshot.items[0].existing.sha256[0] ^= 1;
    if (BuildFinalRecords(context, catalog, manifest, badSnapshot, journal.cursor, nullptr, "2026-11-14T00:00:00Z")) return 10;
    auto badManifest = manifest;
    badManifest["residue"][0]["policy"] = "delete";
    if (BuildFinalRecords(context, catalog, badManifest, snapshot, journal.cursor, nullptr, "2026-11-14T00:00:00Z")) return 11;
    badManifest = manifest;
    badManifest["entries"][0]["source_kind"] = "template";
    if (BuildFinalRecords(context, catalog, badManifest, snapshot, journal.cursor, nullptr, "2026-11-14T00:00:00Z")) return 12;
    auto badContext = context; badContext.journalRoot = context.clientRoot + L"\\nested";
    if (BuildFinalRecords(badContext, catalog, manifest, snapshot, journal.cursor, nullptr, "2026-11-14T00:00:00Z")) return 13;
    if (BuildFinalRecords(context, catalog, manifest, snapshot, journal.cursor, nullptr, "invalid")) return 14;
    if (ValidUtcTimestamp("2026-02-29T00:00:00Z") || ValidUtcTimestamp("2026-01-01T24:00:00Z") ||
        ValidUtcTimestamp("2026-01-01T00:00:00.Z") || !ValidUtcTimestamp("2024-02-29T23:59:59.1234567Z")) return 39;
    ClientLock lock;
    if (!lock.Acquire(context.clientRoot)) return 15;
    const auto prepared = PrepareFinalization(context, catalog, manifest, lock, "2026-11-14T00:00:00Z");
    if (prepared.state != FinalizationPlanState::Ready) {
        std::fprintf(stderr, "prepare finalization: %s %lu\n", prepared.reason.c_str(), prepared.win32Error); return 16;
    }
    const auto repeated = PrepareFinalization(context, catalog, manifest, lock, "2026-11-14T00:00:00Z");
    if (repeated.state != FinalizationPlanState::Ready || !SameIdentity(prepared.identity, repeated.identity)) return 17;
    if (PrepareFinalization(context, catalog, manifest, lock, "2026-11-14T00:00:01Z").state != FinalizationPlanState::RecoveryRequired) return 18;
    GuardedPath plan;
    if (!plan.Open(context.journalRoot, L"finalization-plan.json")) return 19;
    std::string planBytes; DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(plan, planBytes, error)) return 20;
    const auto parsedPlan = ParseJsonObject(planBytes);
    if (!parsedPlan || parsedPlan.value["client_record"] != built.clientRecord ||
        parsedPlan.value["operation_record"] != built.operationRecord) return 21;
    GuardedPath pending;
    if (!pending.Open(context.journalRoot, L"finalization-plan.pending") ||
        !MoveFileW(plan.Path().c_str(), pending.Path().c_str())) return 27;
    if (PrepareFinalization(context, catalog, manifest, lock, "2026-11-14T00:00:00Z").state != FinalizationPlanState::RecoveryRequired ||
        !HashFile(pending.Path()) || !MoveFileW(pending.Path().c_str(), plan.Path().c_str())) return 28;
    const auto published = PublishClientRecord(context, catalog, manifest, lock, "2026-11-14T00:00:00Z");
    if (published.state != RecordPublishState::Published) {
        std::fprintf(stderr, "publish record: %s %lu\n", published.reason.c_str(), published.win32Error); return 29;
    }
    GuardedPath publishedPath;
    std::string publishedBytes;
    if (!publishedPath.Open(context.clientRoot, L"host64/dlss5-install-record.json") ||
        !ReadJournalBytes(publishedPath, publishedBytes, error) || publishedBytes != built.clientRecord) return 30;
    const auto metadata = LoadJournal(context.journalRoot + L"\\record-publication");
    if (!metadata || metadata.cursor.Applied().size() != 1 || metadata.cursor.Terminal() ||
        RecoverOperation(context, catalog, lock).state != RecoveryState::RecoveryRequired) return 31;
    const auto ready = ValidateCommit(context, catalog, manifest, lock);
    if (!ready || ready.transactionBytes != built.operationRecord) {
        std::fprintf(stderr, "commit validation: %s\n", ready.error.c_str()); return 37;
    }
    GuardedPath installed;
    if (!installed.Open(context.clientRoot, L"d3d9.dll")) return 22;
    HANDLE changed = CreateFileW(installed.Path().c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (changed == INVALID_HANDLE_VALUE) return 23;
    DWORD written = 0;
    if (!WriteFile(changed, "changed", 7, &written, nullptr) || written != 7) { CloseHandle(changed); return 24; }
    CloseHandle(changed);
    if (PrepareFinalization(context, catalog, manifest, lock, "2026-11-14T00:00:00Z").state != FinalizationPlanState::RecoveryRequired) return 25;
    if (ValidateCommit(context, catalog, manifest, lock)) return 38;
    if (!HashFile(plan.Path()) || HashFile(plan.Path()).identity.sha256 != prepared.identity.sha256) return 26;
    lock.Release();
    auto interrupted = context;
    interrupted.clientRoot = root + L"\\member-client-1";
    interrupted.journalRoot = root + L"\\member-journal-1";
    if (!lock.Acquire(interrupted.clientRoot)) return 32;
    const auto stoppedPublish = PublishClientRecord(interrupted, catalog, manifest, lock, "2026-11-14T00:00:00Z", &StopAfterRecordPublish);
    if (stoppedPublish.state != RecordPublishState::RecoveryRequired) return 33;
    const auto interruptedJournal = LoadJournal(interrupted.journalRoot + L"\\record-publication");
    if (!interruptedJournal || !interruptedJournal.cursor.Pending() ||
        ReconcilePending(interrupted.clientRoot, interrupted.journalRoot + L"\\record-backup", interruptedJournal.cursor).decision != ReconcileDecision::Applied ||
        RecoverOperation(interrupted, catalog, lock).state != RecoveryState::RecoveryRequired) return 34;
    const auto recoveredRecord = RecoverClientRecord(interrupted, catalog, manifest, lock);
    if (recoveredRecord.state != RecordRecoveryState::Recovered) {
        std::fprintf(stderr, "recover record: %s %lu\n", recoveredRecord.reason.c_str(), recoveredRecord.win32Error); return 35;
    }
    GuardedPath missingRecord;
    if (!missingRecord.Open(interrupted.clientRoot, L"host64/dlss5-install-record.json") ||
        GetFileAttributesW(missingRecord.Path().c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError()) ||
        RecoverClientRecord(interrupted, catalog, manifest, lock).state != RecordRecoveryState::AlreadyTerminal) return 36;
    lock.Release();
    std::puts("final records: legacy schemas, prior snapshot, durable plan, conflicts and stale files passed");
    return 0;
}
