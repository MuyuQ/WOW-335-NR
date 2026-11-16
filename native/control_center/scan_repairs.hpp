#pragma once

#include "complete_repair.hpp"

namespace control_center {
enum class RepairScanState { Incomplete, Completed, RolledBack, Unknown };
struct ScannedRepair { std::wstring id, directory, sourceOperation; RepairScanState state = RepairScanState::Unknown; };
struct RepairScanResult {
    bool ok = false, blocksNewOperation = true;
    DWORD win32Error = ERROR_SUCCESS;
    std::string error;
    std::vector<ScannedRepair> repairs;
};

// Classification is read-only. Unknown files, mismatched active catalogs and
// malformed records block new work rather than authorizing a guessed recovery.
inline RepairScanResult ScanClientRepairs(const std::wstring& repairsRoot, const std::wstring& clientRoot,
                                           const std::wstring& sourceOperationsRoot, const std::vector<std::wstring>& protectedRoots,
                                           const CatalogResult& catalog, const ClientLock& lock) {
    RepairScanResult result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) { result.error = reason; result.win32Error = error; return result; };
    BY_HANDLE_FILE_INFORMATION clientId{};
    if (!catalog || !lock.IsFor(clientRoot) || !lock.Identity(clientId)) return fail("client lock unavailable");
    std::wstring root, client;
    GuardedPath rootGuard;
    if (!CanonicalLocalPath(repairsRoot, root) || !CanonicalLocalPath(clientRoot, client) ||
        WithinRoot(root, client) || WithinRoot(client, root) || !rootGuard.Open(root, L"__root__"))
        return fail("repair root unavailable", rootGuard.Error());
    WIN32_FIND_DATAW entry{};
    HANDLE finder = FindFirstFileW((root + L"\\*").c_str(), &entry);
    if (finder == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) { result.ok = true; result.blocksNewOperation = false; return result; }
        return fail("repair enumeration failed", error);
    }
    result.blocksNewOperation = false;
    do {
        const std::wstring name = entry.cFileName;
        if (name == L"." || name == L"..") continue;
        ScannedRepair found; found.id = name; found.directory = root + L"\\" + name;
        auto unknown = [&]() { found.state = RepairScanState::Unknown; result.blocksNewOperation = true; result.repairs.push_back(found); };
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !ValidOperationId(name)) {
            unknown(); continue;
        }
        GuardedPath planPath, sessionPath, pendingPlan, marker, pendingMarker, report, pendingReport, transaction, pendingTransaction;
        if (!planPath.Open(found.directory, L"plan.json") || !sessionPath.Open(found.directory, L"session.json") ||
            !pendingPlan.Open(found.directory, L"plan.pending") || !marker.Open(found.directory, L"commit.json") ||
            !pendingMarker.Open(found.directory, L"commit.pending") || !report.Open(found.directory, L"result.json") ||
            !pendingReport.Open(found.directory, L"result.pending") || !transaction.Open(found.directory, L"transaction.json") ||
            !pendingTransaction.Open(found.directory, L"transaction.pending")) { unknown(); continue; }
        auto presence = [](const GuardedPath& path) -> int {
            if (GetFileAttributesW(path.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 1;
            return MissingPathError(GetLastError()) ? 0 : -1;
        };
        const int hasPendingPlan = presence(pendingPlan), hasMarker = presence(marker), hasPendingMarker = presence(pendingMarker),
                  hasReport = presence(report), hasPendingReport = presence(pendingReport);
        if (hasPendingPlan != 0 || hasMarker < 0 || hasPendingMarker < 0 || hasReport < 0 || hasPendingReport < 0 ||
            presence(transaction) != 0 || presence(pendingTransaction) != 0) { unknown(); continue; }
        std::string planBytes, sessionBytes; DWORD error = ERROR_SUCCESS;
        if (!ReadJournalBytes(planPath, planBytes, error) || !ReadJournalBytes(sessionPath, sessionBytes, error)) { unknown(); continue; }
        const auto plan = ParseJsonObject(planBytes);
        const auto session = ParseJsonObject(sessionBytes);
        const auto planHash = HashFile(planPath.Path());
        std::array<std::uint8_t, 32> digest{};
        std::string id, clientText, catalogHash, planId, planClient, planSource, planCatalog, kind;
        if (!plan || !session || !planHash || HashBytes(planBytes, digest) < 0 || digest != planHash.identity.sha256 ||
            !WideUtf8(name, id) || !WideUtf8(client, clientText) || !CatalogDigest(catalog, catalogHash) ||
            !StringField(plan.value, "kind", kind) || kind != "repair" || !StringField(plan.value, "repair_id", planId) || planId != id ||
            !StringField(plan.value, "client", planClient) || !StringField(plan.value, "source_operation", planSource) ||
            !StringField(plan.value, "catalog_sha256", planCatalog) || !ValidSha256(planCatalog) ||
            !plan.value.contains("schema") || plan.value["schema"] != 1 || !plan.value.contains("client_volume") ||
            !plan.value["client_volume"].is_number_unsigned() || !plan.value.contains("client_index_high") ||
            !plan.value["client_index_high"].is_number_unsigned() || !plan.value.contains("client_index_low") ||
            !plan.value["client_index_low"].is_number_unsigned() || plan.value["client_volume"] > MAXDWORD ||
            plan.value["client_index_high"] > MAXDWORD || plan.value["client_index_low"] > MAXDWORD) { unknown(); continue; }
        const Json expectedSession = {{"schema", 1}, {"kind", "repair_session"}, {"repair_id", id},
            {"client_volume", plan.value["client_volume"]}, {"client_index_high", plan.value["client_index_high"]},
            {"client_index_low", plan.value["client_index_low"]}, {"catalog_sha256", planCatalog},
            {"plan_sha256", HexSha256(digest)}, {"plan_identity", JournalIdJson(JournalId(planHash.identity))}};
        if (session.value != expectedSession) { unknown(); continue; }
        if (plan.value["client_volume"] != clientId.dwVolumeSerialNumber || plan.value["client_index_high"] != clientId.nFileIndexHigh ||
            plan.value["client_index_low"] != clientId.nFileIndexLow) continue;
        if (planClient != clientText || !Utf8Wide(planSource, found.sourceOperation)) { unknown(); continue; }
        const auto actions = plan.value.find("actions"), preserved = plan.value.find("preserved");
        if (actions == plan.value.end() || !actions->is_array() || actions->empty() || preserved == plan.value.end() || !preserved->is_array()) { unknown(); continue; }
        std::vector<JournalItem> expectedItems;
        Json repaired = Json::array(), saved = Json::array();
        bool valid = true;
        for (const auto& raw : *actions) {
            std::string targetText, candidate, previousHash; std::wstring target; JournalFileId sourceId{}, previousId{};
            if (!raw.is_object() || raw.size() != 5 || !StringField(raw, "target", targetText) || !Utf8Wide(targetText, target) ||
                !ValidMemberPath(target) || !StringField(raw, "sha256", candidate) || !ValidSha256(candidate) ||
                !raw.contains("source_identity") || !DecodeJournalId(raw["source_identity"], sourceId) ||
                !raw.contains("previous_identity") || !raw.contains("previous_sha256")) { valid = false; break; }
            JournalItem item; item.target = target; item.candidateSha256 = candidate; item.staged = target + L".nr-repair-" + name + L".stage";
            if (!raw["previous_identity"].is_null() || !raw["previous_sha256"].is_null()) {
                if (IsProtectedRepairConfig(target) || !DecodeJournalId(raw["previous_identity"], previousId) ||
                    !StringField(raw, "previous_sha256", previousHash) || !ValidSha256(previousHash)) { valid = false; break; }
                item.previouslyPresent = true; item.previousSha256 = previousHash; item.previousIdentity = previousId;
                item.backup = target; item.quarantine = target + L".nr-repair-" + name + L".original";
                saved.push_back({{"target", targetText}, {"saved", "modified/" + targetText}});
            }
            if (!ValidJournalItem(item)) { valid = false; break; }
            for (const auto& old : expectedItems) if (SameTarget(old.target, target)) valid = false;
            expectedItems.push_back(std::move(item)); repaired.push_back(targetText);
        }
        for (const auto& raw : *preserved) {
            std::wstring target;
            if (!raw.is_string() || !Utf8Wide(raw.get<std::string>(), target) || !IsProtectedRepairConfig(target)) valid = false;
        }
        const auto journal = LoadJournal(found.directory);
        if (!valid || !journal) { unknown(); continue; }
        for (const auto& seen : journal.cursor.Seen()) {
            bool matched = false;
            for (const auto& expected : expectedItems) if (SameJournalItem(seen, expected)) { matched = true; break; }
            if (!matched) valid = false;
        }
        if (!valid) { unknown(); continue; }
        if (journal.cursor.Terminal() && (journal.cursor.TerminalKind() == JournalKind::Cancelled || journal.cursor.TerminalKind() == JournalKind::RolledBack) &&
            !hasMarker && !hasPendingMarker && !hasReport && !hasPendingReport && journal.cursor.Unresolved().empty()) found.state = RepairScanState::RolledBack;
        else if (journal.cursor.Terminal() && journal.cursor.TerminalKind() == JournalKind::Complete && hasMarker && hasReport && !hasPendingMarker && !hasPendingReport &&
                 journal.cursor.Applied().size() == expectedItems.size()) {
            std::string markerBytes, reportBytes;
            if (!ReadJournalBytes(marker, markerBytes, error) || !ReadJournalBytes(report, reportBytes, error)) { unknown(); continue; }
            const auto markerJson = ParseJsonObject(markerBytes), reportJson = ParseJsonObject(reportBytes);
            const Json expectedMarker = {{"schema", 1}, {"kind", "repair"}, {"repair_id", id},
                {"plan_sha256", HexSha256(digest)}, {"journal_next", journal.cursor.Next() - 1}};
            const Json expectedReport = {{"schema", 1}, {"kind", "repair"}, {"status", "completed"}, {"repair_id", id},
                {"plan_sha256", HexSha256(digest)}, {"repaired", repaired}, {"saved_modified", saved}, {"preserved_config", *preserved}};
            if (!markerJson || !reportJson || markerJson.value != expectedMarker || reportJson.value != expectedReport) { unknown(); continue; }
            found.state = RepairScanState::Completed;
        } else {
            if (planCatalog != catalogHash || (hasMarker && hasPendingMarker) || (!hasMarker && (hasReport || hasPendingReport))) { unknown(); continue; }
            const auto trusted = ReadTrustedRepairPlan(client, sourceOperationsRoot, found.sourceOperation, root, name, protectedRoots, catalog, lock);
            if (!trusted.valid || !SameIdentity(trusted.identity, planHash.identity)) { unknown(); continue; }
            found.state = RepairScanState::Incomplete;
        }
        if (found.state == RepairScanState::Incomplete) result.blocksNewOperation = true;
        result.repairs.push_back(std::move(found));
    } while (FindNextFileW(finder, &entry));
    const DWORD error = GetLastError();
    FindClose(finder);
    if (error != ERROR_NO_MORE_FILES) return fail("repair enumeration failed", error);
    result.ok = true;
    return result;
}
}
