#pragma once

#include "uninstall_finalize.hpp"

namespace control_center {
// A verified rollback or cancellation unblocks writes. AwaitingFinalization and
// Finalizing still need the forward finalization, while Finalized is complete
// and therefore does not block new transactions.
enum class UninstallScanState { Incomplete, RolledBack, AwaitingFinalization, Finalizing, Finalized, Unknown };
struct ScannedUninstall { std::wstring id, directory; UninstallScanState state = UninstallScanState::Unknown; };
struct UninstallScanResult {
    bool ok = false, blocksNewOperation = true;
    DWORD win32Error = ERROR_SUCCESS;
    std::string error;
    std::vector<ScannedUninstall> uninstalls;
};

// Only a verified rollback or cancellation unblocks writes. Member completion
// still needs record and residue finalization and therefore remains blocked.
inline UninstallScanResult ScanClientUninstalls(const std::wstring& uninstallRoot, const std::wstring& clientRoot,
                                                 const std::vector<std::wstring>& protectedRoots, const CatalogResult& catalog,
                                                 const Json& manifest, const ClientLock& lock) {
    UninstallScanResult result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) { result.error = reason; result.win32Error = error; return result; };
    BY_HANDLE_FILE_INFORMATION clientId{};
    if (!lock.IsFor(clientRoot) || !lock.Identity(clientId) || !ManifestMatchesCatalog(manifest, catalog)) return fail("invalid context");
    std::wstring root, client;
    GuardedPath rootGuard;
    if (!CanonicalLocalPath(uninstallRoot, root) || !CanonicalLocalPath(clientRoot, client) || WithinRoot(root, client) || WithinRoot(client, root) ||
        !rootGuard.Open(root, L"__root__")) return fail("uninstall root unavailable", rootGuard.Error());
    WIN32_FIND_DATAW entry{};
    HANDLE finder = FindFirstFileW((root + L"\\*").c_str(), &entry);
    if (finder == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) { result.ok = true; result.blocksNewOperation = false; return result; }
        return fail("uninstall enumeration failed", error);
    }
    result.blocksNewOperation = false;
    do {
        const std::wstring name = entry.cFileName;
        if (name == L"." || name == L"..") continue;
        ScannedUninstall found; found.id = name; found.directory = root + L"\\" + name;
        auto unknown = [&]() { result.blocksNewOperation = true; result.uninstalls.push_back(found); };
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !ValidOperationId(name)) {
            unknown(); continue;
        }
        GuardedPath planPath, sessionPath, pending;
        if (!planPath.Open(found.directory, L"plan.json") || !sessionPath.Open(found.directory, L"session.json") ||
            !pending.Open(found.directory, L"plan.pending")) { unknown(); continue; }
        if (GetFileAttributesW(pending.Path().c_str()) != INVALID_FILE_ATTRIBUTES || !MissingPathError(GetLastError())) { unknown(); continue; }
        std::string planBytes, sessionBytes, id, planId, kind, catalogHash, manifestHash, clientText; DWORD error = ERROR_SUCCESS;
        if (!ReadJournalBytes(planPath, planBytes, error) || !ReadJournalBytes(sessionPath, sessionBytes, error)) { unknown(); continue; }
        const auto plan = ParseJsonObject(planBytes), session = ParseJsonObject(sessionBytes);
        const auto actual = HashFile(planPath.Path()); std::array<std::uint8_t, 32> digest{};
        if (!plan || !session || !actual || HashBytes(planBytes, digest) < 0 || digest != actual.identity.sha256 || !WideUtf8(name, id) ||
            !StringField(plan.value, "kind", kind) || kind != "uninstall" || !StringField(plan.value, "uninstall_id", planId) || planId != id ||
            !StringField(plan.value, "client", clientText) ||
            !StringField(plan.value, "catalog_sha256", catalogHash) || !ValidSha256(catalogHash) ||
            !StringField(plan.value, "manifest_sha256", manifestHash) || !ValidSha256(manifestHash) ||
            !plan.value.contains("schema") || plan.value["schema"] != 1 || !plan.value.contains("client_volume") ||
            !plan.value["client_volume"].is_number_unsigned() || !plan.value.contains("client_index_high") ||
            !plan.value["client_index_high"].is_number_unsigned() || !plan.value.contains("client_index_low") ||
            !plan.value["client_index_low"].is_number_unsigned() || plan.value["client_volume"] > MAXDWORD ||
            plan.value["client_index_high"] > MAXDWORD || plan.value["client_index_low"] > MAXDWORD) { unknown(); continue; }
        const Json expectedSession = {{"schema", 1}, {"kind", "uninstall_session"}, {"uninstall_id", id},
            {"client_volume", plan.value["client_volume"]}, {"client_index_high", plan.value["client_index_high"]},
            {"client_index_low", plan.value["client_index_low"]}, {"catalog_sha256", catalogHash}, {"manifest_sha256", manifestHash},
            {"plan_sha256", HexSha256(digest)}, {"plan_identity", JournalIdJson(JournalId(actual.identity))}};
        if (session.value != expectedSession) { unknown(); continue; }
        std::wstring recordedClient, canonicalClient;
        if (!Utf8Wide(clientText, recordedClient) || !CanonicalLocalPath(recordedClient, canonicalClient)) { unknown(); continue; }
        if (plan.value["client_volume"] != clientId.dwVolumeSerialNumber || plan.value["client_index_high"] != clientId.nFileIndexHigh ||
            plan.value["client_index_low"] != clientId.nFileIndexLow) {
            if (CompareStringOrdinal(canonicalClient.c_str(), -1, client.c_str(), -1, TRUE) == CSTR_EQUAL) unknown();
            continue;
        }
        // A directory that already carries finalization state can no longer be
        // validated through the client record: the record is deleted last. Check
        // the finalization markers instead and treat a completed one as final.
        auto artifactPresent = [&](const wchar_t* artifactName, bool& present) {
            GuardedPath path;
            if (!path.Open(found.directory, artifactName)) return false;
            const DWORD attributes = GetFileAttributesW(path.Path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) { present = true; return true; }
            present = false;
            return MissingPathError(GetLastError());
        };
        bool hasResult = false, hasResultPending = false, hasCommit = false, hasCommitPending = false;
        bool hasFinalization = false, hasSnapshot = false, hasPublication = false;
        if (!artifactPresent(L"result.json", hasResult) || !artifactPresent(L"result.pending", hasResultPending) ||
            !artifactPresent(L"commit.json", hasCommit) || !artifactPresent(L"commit.pending", hasCommitPending) ||
            !artifactPresent(L"finalization-plan.json", hasFinalization) || !artifactPresent(L"record.snapshot", hasSnapshot) ||
            !artifactPresent(L"record-publication", hasPublication)) { unknown(); continue; }
        if (hasPublication) { unknown(); continue; }
        const bool decided = hasResult || hasResultPending || hasCommit || hasCommitPending;
        if (decided || (hasFinalization && hasSnapshot)) {
            const auto journalPlan = RebuildUninstallJournalPlan(plan.value, actual.identity);
            if (!journalPlan.valid || journalPlan.items.empty()) { unknown(); continue; }
            const auto journal = LoadUninstallJournal(found.directory, journalPlan);
            if (!journal) { unknown(); continue; }
            const bool membersDone = journal.cursor.Terminal() &&
                journal.cursor.TerminalKind() == UninstallJournalKind::MembersDone;
            const bool appliedAll = !journal.cursor.Terminal() &&
                journal.cursor.Applied().size() == journalPlan.items.size();
            auto jsonMatches = [&](const wchar_t* artifactName, const char* expectedKind) {
                GuardedPath path;
                if (!path.Open(found.directory, artifactName)) return false;
                std::string bytes, kindText, idText, planText;
                DWORD readError = ERROR_SUCCESS;
                if (!ReadJournalBytes(path, bytes, readError)) return false;
                const auto document = ParseJsonObject(bytes);
                return document && document.value.contains("schema") && document.value["schema"] == 1 &&
                    StringField(document.value, "kind", kindText) && kindText == expectedKind &&
                    StringField(document.value, "uninstall_id", idText) && idText == id &&
                    StringField(document.value, "plan_sha256", planText) && planText == HexSha256(digest);
            };
            // A published result.json is written only after the client record was removed,
            // so its presence alone proves the uninstall is complete. A record that exists
            // now belongs to a newer install and does not reopen this finalization.
            if (membersDone && hasResult && !hasResultPending && hasCommit && !hasCommitPending &&
                jsonMatches(L"result.json", "uninstall_result") && jsonMatches(L"commit.json", "uninstall_finalization_commit")) {
                found.state = UninstallScanState::Finalized;
                result.uninstalls.push_back(std::move(found));
                continue;
            }
            if ((membersDone || appliedAll) && !decided) {
                found.state = UninstallScanState::AwaitingFinalization;
            } else if (membersDone || appliedAll) {
                found.state = UninstallScanState::Finalizing;
            } else { unknown(); continue; }
            result.blocksNewOperation = true;
            result.uninstalls.push_back(std::move(found));
            continue;
        }
        const auto trusted = ReadTrustedUninstallPlan(client, root, name, protectedRoots, catalog, manifest, lock);
        if (!trusted.valid || !NoUninstallFinalState(found.directory)) { unknown(); continue; }
        const auto journal = LoadUninstallJournal(found.directory, trusted);
        if (!journal) { unknown(); continue; }
        if (journal.cursor.Terminal()) {
            if ((journal.cursor.TerminalKind() == UninstallJournalKind::Cancelled || journal.cursor.TerminalKind() == UninstallJournalKind::RolledBack) &&
                journal.cursor.Unresolved().empty()) found.state = UninstallScanState::RolledBack;
            else if (journal.cursor.TerminalKind() == UninstallJournalKind::MembersDone) found.state = UninstallScanState::AwaitingFinalization;
            else { unknown(); continue; }
        } else found.state = UninstallScanState::Incomplete;
        if (found.state != UninstallScanState::RolledBack) result.blocksNewOperation = true;
        result.uninstalls.push_back(std::move(found));
    } while (FindNextFileW(finder, &entry));
    const DWORD error = GetLastError(); FindClose(finder);
    if (error != ERROR_NO_MORE_FILES) return fail("uninstall enumeration failed", error);
    result.ok = true; return result;
}
}
