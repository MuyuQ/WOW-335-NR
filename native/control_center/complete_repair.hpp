#pragma once

#include "recover_repair.hpp"

namespace control_center {
enum class RepairCompleteState { Completed, RecoveryRequired };
enum class RepairCompleteCheckpoint { MarkerPrepared, MarkerPublished, ReportPrepared, ReportPublished, JournalCompleted };
struct RepairCompleteResult {
    RepairCompleteState state = RepairCompleteState::RecoveryRequired;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
};

// The marker is the decision to retain repaired files. Its pending form also
// blocks rollback; recovery validates it and continues the commit.
inline RepairCompleteResult CompleteRepair(const std::wstring& clientRoot, const std::wstring& sourceOperationsRoot,
                                           const std::wstring& sourceOperation, const std::wstring& repairsRoot,
                                           const std::wstring& repairId, const std::vector<std::wstring>& protectedRoots,
                                           const CatalogResult& catalog, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                           , ProcessCheck (*testProcesses)() = nullptr,
                                             bool (*interrupt)(RepairCompleteCheckpoint) = nullptr
#endif
                                           ) {
    RepairCompleteResult result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) { result.reason = reason; result.win32Error = error; return result; };
    auto stopped = [&]() {
#ifdef CONTROL_CENTER_TESTING
        const auto process = testProcesses ? testProcesses() : CheckBlockingProcesses();
#else
        const auto process = CheckBlockingProcesses();
#endif
        return lock.IsFor(clientRoot) && ValidateClient(clientRoot, protectedRoots) && process.state == ProcessState::Stopped;
    };
    auto interrupted = [&](RepairCompleteCheckpoint point) {
#ifdef CONTROL_CENTER_TESTING
        return interrupt && interrupt(point);
#else
        (void)point; return false;
#endif
    };
    const auto plan = ReadTrustedRepairPlan(clientRoot, sourceOperationsRoot, sourceOperation, repairsRoot, repairId, protectedRoots, catalog, lock);
    if (!plan.valid || !stopped()) return fail("repair plan or client unavailable", plan.win32Error);
    std::wstring client, repairs;
    if (!CanonicalLocalPath(clientRoot, client) || !CanonicalLocalPath(repairsRoot, repairs)) return fail("invalid roots");
    const std::wstring work = repairs + L"\\" + repairId;
    GuardedPath root, marker, markerPending, report, reportPending, planPath;
    if (!root.Open(work, L"__root__") || !marker.Open(work, L"commit.json") || !markerPending.Open(work, L"commit.pending") ||
        !report.Open(work, L"result.json") || !reportPending.Open(work, L"result.pending") || !planPath.Open(work, L"plan.json"))
        return fail("repair commit path unavailable");
    auto journal = LoadJournal(work);
    if (!journal || !TrustedRepairCursor(journal.cursor, plan, repairId) || journal.cursor.Pending() || journal.cursor.Prepared() ||
        journal.cursor.RollingBack() || (journal.cursor.Terminal() && journal.cursor.TerminalKind() != JournalKind::Complete) ||
        journal.cursor.Applied().size() != plan.actions.size() || journal.cursor.AppliedIdentities().size() != plan.actions.size())
        return fail("repair journal not ready", journal.win32Error);
    auto verifyFiles = [&]() {
        if (!stopped()) return false;
        const auto fresh = ReadTrustedRepairPlan(clientRoot, sourceOperationsRoot, sourceOperation, repairsRoot, repairId, protectedRoots, catalog, lock);
        if (!fresh.valid || !SameIdentity(fresh.identity, plan.identity)) return false;
        for (const auto& action : plan.actions) {
            size_t position = journal.cursor.Applied().size();
            const auto expected = RepairJournalItem(action, repairId);
            for (size_t i = 0; i < journal.cursor.Applied().size(); ++i) if (SameJournalItem(journal.cursor.Applied()[i], expected)) {
                if (position != journal.cursor.Applied().size()) return false;
                position = i;
            }
            if (position == journal.cursor.Applied().size()) return false;
            GuardedPath target;
            if (!target.Open(client, action.member.target)) return false;
            const auto actual = HashFile(target.Path());
            if (!actual || HexSha256(actual.identity.sha256) != action.member.sha256 ||
                !SameJournalId(JournalId(actual.identity), journal.cursor.AppliedIdentities()[position])) return false;
            if (action.previouslyPresent) {
                GuardedPath saved;
                if (!saved.Open(work, L"modified/" + action.member.target)) return false;
                const auto copy = HashFile(saved.Path());
                if (!copy || copy.identity.sha256 != action.previous.sha256) return false;
            }
        }
        return true;
    };
    if (!verifyFiles()) return fail("repair files or saved modifications changed");
    std::string planBytes, id; DWORD error = ERROR_SUCCESS;
    if (!ReadJournalBytes(planPath, planBytes, error) || !WideUtf8(repairId, id)) return fail("repair plan unavailable", error);
    std::array<std::uint8_t, 32> planHash{};
    if (HashBytes(planBytes, planHash) < 0 || planHash != plan.identity.sha256) return fail("repair plan changed");
    const auto planJson = ParseJsonObject(planBytes);
    if (!planJson) return fail("repair plan invalid");
    const std::uint64_t next = journal.cursor.Next() - (journal.cursor.Terminal() ? 1 : 0);
    const Json markerJson = {{"schema", 1}, {"kind", "repair"}, {"repair_id", id}, {"plan_sha256", HexSha256(planHash)}, {"journal_next", next}};
    Json repaired = Json::array(), saved = Json::array();
    for (const auto& action : plan.actions) {
        std::string target;
        if (!WideUtf8(action.member.target, target)) return fail("invalid repair target");
        repaired.push_back(target);
        if (action.previouslyPresent) saved.push_back({{"target", target}, {"saved", "modified/" + target}});
    }
    const Json reportJson = {{"schema", 1}, {"kind", "repair"}, {"status", "completed"}, {"repair_id", id},
        {"plan_sha256", HexSha256(planHash)}, {"repaired", repaired}, {"saved_modified", saved}, {"preserved_config", planJson.value["preserved"]}};
    const std::string markerBytes = markerJson.dump() + "\n", reportBytes = reportJson.dump(2) + "\n";
    auto absent = [](const GuardedPath& path) { return GetFileAttributesW(path.Path().c_str()) == INVALID_FILE_ATTRIBUTES && MissingPathError(GetLastError()); };
    auto readExact = [](const GuardedPath& path, const std::string& expected, FileIdentity& identity, DWORD& error) {
        const auto hash = HashFile(path.Path()); std::string bytes; std::array<std::uint8_t, 32> digest{};
        if (!hash || !ReadJournalBytes(path, bytes, error) || bytes != expected || HashBytes(bytes, digest) < 0 || digest != hash.identity.sha256) return false;
        identity = hash.identity; return true;
    };
    auto publish = [&](const GuardedPath& temporary, const GuardedPath& final, const std::string& bytes,
                       RepairCompleteCheckpoint prepared, RepairCompleteCheckpoint published) {
        FileIdentity identity;
        if (!absent(final)) return absent(temporary) && readExact(final, bytes, identity, error);
        if (absent(temporary)) {
            const auto written = WriteNewVerified(temporary, bytes);
            if (!written) { error = written.win32Error; return false; }
            identity = written.identity;
        } else if (!readExact(temporary, bytes, identity, error)) return false;
        if (interrupted(prepared) || !verifyFiles()) return false;
        const auto renamed = RenameVerifiedNew(temporary, final, identity.sha256, &identity);
        if (!renamed) { error = renamed.win32Error; return false; }
        return !interrupted(published);
    };
    if (absent(marker) && (!absent(report) || !absent(reportPending) || journal.cursor.Terminal())) return fail("repair commit artifacts conflict");
    if (!publish(markerPending, marker, markerBytes, RepairCompleteCheckpoint::MarkerPrepared, RepairCompleteCheckpoint::MarkerPublished))
        return fail("repair commit marker requires recovery", error);
    if (!publish(reportPending, report, reportBytes, RepairCompleteCheckpoint::ReportPrepared, RepairCompleteCheckpoint::ReportPublished))
        return fail("repair report requires recovery", error);
    FileIdentity markerIdentity, reportIdentity;
    if (!readExact(marker, markerBytes, markerIdentity, error) || !readExact(report, reportBytes, reportIdentity, error) || !verifyFiles())
        return fail("repair commit changed", error);
    if (!journal.cursor.Terminal()) {
        const auto completed = PersistJournalEvent(work, journal.cursor, {journal.cursor.Next(), JournalKind::Complete, {}});
        if (!completed) return fail("cannot complete repair journal", completed.win32Error);
        journal = completed;
    }
    if (interrupted(RepairCompleteCheckpoint::JournalCompleted)) return fail("interrupted after repair completion");
    if (!verifyFiles()) return fail("repair files changed after commit");
    result.state = RepairCompleteState::Completed;
    return result;
}
}
