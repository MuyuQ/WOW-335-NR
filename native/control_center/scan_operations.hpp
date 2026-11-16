#pragma once

#include "recover_operation.hpp"
#include "operation_record.hpp"

namespace control_center {
enum class OperationScanState { Empty, Incomplete, Completed, RolledBack, Unknown };
struct ScannedOperation {
    std::wstring id, directory;
    OperationScanState state = OperationScanState::Unknown;
};
struct OperationScanResult {
    bool ok = false, blocksNewOperation = true;
    DWORD win32Error = ERROR_SUCCESS;
    std::string error;
    std::vector<ScannedOperation> operations;
};

// This classifies operation directories only; it never grants permission to
// mutate a client. Unknown or unreadable entries block a new write operation.
inline OperationScanResult ScanClientOperations(const std::wstring& operationsRoot, const ClientLock& lock,
                                                 const CatalogResult& catalog) {
    OperationScanResult result;
    auto fail = [&result](const char* reason, DWORD error = ERROR_SUCCESS) {
        result.error = reason; result.win32Error = error; return result;
    };
    BY_HANDLE_FILE_INFORMATION clientId{};
    if (!catalog || !lock.Identity(clientId)) return fail("client lock unavailable");
    std::wstring root;
    GuardedPath rootGuard;
    if (!CanonicalLocalPath(operationsRoot, root) || !rootGuard.Open(root, L"__root__")) return fail("operations root unavailable", rootGuard.Error());
    WIN32_FIND_DATAW entry{};
    HANDLE finder = FindFirstFileW((root + L"\\*").c_str(), &entry);
    if (finder == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) { result.ok = true; result.blocksNewOperation = false; return result; }
        return fail("operations enumeration failed", error);
    }
    result.blocksNewOperation = false;
    do {
        const std::wstring name = entry.cFileName;
        if (name == L"." || name == L"..") continue;
        ScannedOperation found; found.id = name; found.directory = root + L"\\" + name;
        auto unknown = [&]() { found.state = OperationScanState::Unknown; result.blocksNewOperation = true; result.operations.push_back(found); };
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !ValidOperationId(name)) {
            unknown(); continue;
        }
        GuardedPath session;
        if (!session.Open(found.directory, L"session.json")) { unknown(); continue; }
        std::string bytes; DWORD error = ERROR_SUCCESS;
        if (!ReadJournalBytes(session, bytes, error)) { unknown(); continue; }
        const auto parsed = ParseJsonObject(bytes);
        std::string id, digest, expectedId, expectedDigest;
        if (!parsed || !WideUtf8(name, expectedId) || !CatalogDigest(catalog, expectedDigest) ||
            !StringField(parsed.value, "operation_id", id) || id != expectedId || !StringField(parsed.value, "catalog_sha256", digest) ||
            !parsed.value.contains("schema") || parsed.value["schema"] != 1 ||
            !parsed.value.contains("client_volume") || !parsed.value["client_volume"].is_number_unsigned() ||
            !parsed.value.contains("client_index_high") || !parsed.value["client_index_high"].is_number_unsigned() ||
            !parsed.value.contains("client_index_low") || !parsed.value["client_index_low"].is_number_unsigned() ||
            parsed.value["client_volume"] > MAXDWORD || parsed.value["client_index_high"] > MAXDWORD ||
            parsed.value["client_index_low"] > MAXDWORD || !ValidSha256(digest)) { unknown(); continue; }
        if (parsed.value["client_volume"] != clientId.dwVolumeSerialNumber ||
            parsed.value["client_index_high"] != clientId.nFileIndexHigh || parsed.value["client_index_low"] != clientId.nFileIndexLow) continue;
        const auto prior = ReadSessionPriorRecord(found.directory);
        if (!BindJournalSessionDigest(found.directory, name, lock, digest, prior, true)) {
            unknown(); continue;
        }
        const auto journal = LoadJournal(found.directory);
        const bool sameCatalog = digest == expectedDigest;
        if (!journal || (sameCatalog && !TrustedJournalCursor(journal.cursor, catalog, name)) ||
            (!sameCatalog && (!journal.cursor.Terminal() ||
             (journal.cursor.TerminalKind() != JournalKind::Complete && journal.cursor.TerminalKind() != JournalKind::RolledBack &&
              journal.cursor.TerminalKind() != JournalKind::Cancelled)))) { unknown(); continue; }
        GuardedPath publication, marker, pending, transaction, pendingTransaction, pendingPlan;
        if (!publication.Open(found.directory, L"record-publication") || !marker.Open(found.directory, L"commit.json") ||
            !pending.Open(found.directory, L"commit.pending") || !transaction.Open(found.directory, L"transaction.json") ||
            !pendingTransaction.Open(found.directory, L"transaction.pending") || !pendingPlan.Open(found.directory, L"finalization-plan.pending")) { unknown(); continue; }
        auto presence = [](const GuardedPath& path) -> int {
            if (GetFileAttributesW(path.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 1;
            return MissingPathError(GetLastError()) ? 0 : -1;
        };
        const int hasPublication = presence(publication), hasMarker = presence(marker), hasPending = presence(pending), hasTransaction = presence(transaction);
        const int hasPendingTransaction = presence(pendingTransaction), hasPendingPlan = presence(pendingPlan);
        if (hasPublication < 0 || hasMarker < 0 || hasPending < 0 || hasTransaction < 0 || hasPendingTransaction < 0 || hasPendingPlan < 0) { unknown(); continue; }
        if (hasPendingTransaction || hasPendingPlan) found.state = OperationScanState::Incomplete;
        else if (journal.cursor.Terminal() && (journal.cursor.TerminalKind() == JournalKind::Cancelled || journal.cursor.TerminalKind() == JournalKind::RolledBack) &&
            !hasMarker && !hasPending && !hasTransaction) {
            if (hasPublication) {
                const auto recordJournal = LoadJournal(found.directory + L"\\record-publication");
                GuardedPath plan;
                std::string planBytes, candidate, candidateDigest, planOperation, planCatalog, planSession;
                if (!recordJournal || !recordJournal.cursor.Terminal() || !recordJournal.cursor.Unresolved().empty() ||
                    (recordJournal.cursor.TerminalKind() != JournalKind::Cancelled && recordJournal.cursor.TerminalKind() != JournalKind::RolledBack) ||
                    !plan.Open(found.directory, L"finalization-plan.json") || !ReadJournalBytes(plan, planBytes, error)) { unknown(); continue; }
                const auto planJson = ParseJsonObject(planBytes);
                const auto sessionHash = HashFile(session.Path());
                std::array<std::uint8_t, 32> candidateHash{};
                if (!planJson || !sessionHash || !StringField(planJson.value, "operation_id", planOperation) || planOperation != id ||
                    !StringField(planJson.value, "catalog_sha256", planCatalog) || planCatalog != digest ||
                    !StringField(planJson.value, "session_sha256", planSession) || planSession != HexSha256(sessionHash.identity.sha256) ||
                    !StringField(planJson.value, "client_record", candidate) || !ReadClientRecord(candidate) ||
                    !StringField(planJson.value, "client_record_sha256", candidateDigest) || HashBytes(candidate, candidateHash) < 0 ||
                    candidateDigest != HexSha256(candidateHash)) { unknown(); continue; }
                JournalItem expectedRecord;
                expectedRecord.target = L"host64/dlss5-install-record.json";
                expectedRecord.staged = expectedRecord.target + L".nr-" + name + L".stage";
                expectedRecord.candidateSha256 = candidateDigest;
                expectedRecord.previouslyPresent = prior.state == CapturedRecordState::Present;
                if (expectedRecord.previouslyPresent) {
                    expectedRecord.previousSha256 = HexSha256(prior.identity.sha256); expectedRecord.previousIdentity = JournalId(prior.identity);
                    expectedRecord.backup = expectedRecord.target; expectedRecord.quarantine = expectedRecord.target + L".nr-" + name + L".original";
                }
                bool trustedRecord = ValidJournalItem(expectedRecord);
                for (const auto& item : recordJournal.cursor.Seen()) if (!SameJournalItem(item, expectedRecord)) trustedRecord = false;
                if (!trustedRecord) { unknown(); continue; }
            }
            found.state = OperationScanState::RolledBack;
        }
        else if (journal.cursor.Terminal() && journal.cursor.TerminalKind() == JournalKind::Complete && hasPublication && hasMarker && hasTransaction && !hasPending) {
            const auto recordJournal = LoadJournal(found.directory + L"\\record-publication");
            GuardedPath plan;
            std::string planBytes, transactionBytes, markerBytes;
            if (!recordJournal || !recordJournal.cursor.Terminal() || recordJournal.cursor.TerminalKind() != JournalKind::Complete ||
                recordJournal.cursor.Applied().size() != 1 || recordJournal.cursor.AppliedIdentities().size() != 1 ||
                !plan.Open(found.directory, L"finalization-plan.json") || !ReadJournalBytes(plan, planBytes, error) ||
                !ReadJournalBytes(transaction, transactionBytes, error) || !ReadJournalBytes(marker, markerBytes, error)) { unknown(); continue; }
            const auto planJson = ParseJsonObject(planBytes), markerJson = ParseJsonObject(markerBytes);
            std::array<std::uint8_t, 32> planHash{}, transactionHash{}, candidateHash{};
            std::string recordedTransaction, recordedTransactionHash, planOperation, planCatalog, planSession, candidate, candidateDigest;
            const auto sessionHash = HashFile(session.Path());
            if (!planJson || !markerJson || !sessionHash || HashBytes(planBytes, planHash) < 0 || HashBytes(transactionBytes, transactionHash) < 0 ||
                !StringField(planJson.value, "operation_id", planOperation) || planOperation != id ||
                !StringField(planJson.value, "catalog_sha256", planCatalog) || planCatalog != digest ||
                !StringField(planJson.value, "session_sha256", planSession) || planSession != HexSha256(sessionHash.identity.sha256) ||
                !StringField(planJson.value, "operation_record", recordedTransaction) || recordedTransaction != transactionBytes ||
                !StringField(planJson.value, "operation_record_sha256", recordedTransactionHash) || recordedTransactionHash != HexSha256(transactionHash) ||
                !StringField(planJson.value, "client_record", candidate) || !ReadClientRecord(candidate) ||
                !StringField(planJson.value, "client_record_sha256", candidateDigest) || HashBytes(candidate, candidateHash) < 0 ||
                candidateDigest != HexSha256(candidateHash) ||
                !ReadOperationRecord(transactionBytes)) { unknown(); continue; }
            JournalItem expectedRecord;
            expectedRecord.target = L"host64/dlss5-install-record.json";
            expectedRecord.staged = expectedRecord.target + L".nr-" + name + L".stage";
            expectedRecord.candidateSha256 = candidateDigest;
            expectedRecord.previouslyPresent = prior.state == CapturedRecordState::Present;
            if (expectedRecord.previouslyPresent) {
                expectedRecord.previousSha256 = HexSha256(prior.identity.sha256); expectedRecord.previousIdentity = JournalId(prior.identity);
                expectedRecord.backup = expectedRecord.target; expectedRecord.quarantine = expectedRecord.target + L".nr-" + name + L".original";
            }
            bool trustedRecord = ValidJournalItem(expectedRecord);
            for (const auto& item : recordJournal.cursor.Seen()) if (!SameJournalItem(item, expectedRecord)) trustedRecord = false;
            if (!trustedRecord || !SameJournalItem(recordJournal.cursor.Applied()[0], expectedRecord)) { unknown(); continue; }
            const Json expectedMarker = {{"schema", 1}, {"operation_id", id}, {"catalog_sha256", digest},
                {"plan_sha256", HexSha256(planHash)}, {"client_record_identity", JournalIdJson(recordJournal.cursor.AppliedIdentities()[0])},
                {"member_next", journal.cursor.Next() - 1}, {"record_next", recordJournal.cursor.Next() - 1}};
            if (markerJson.value != expectedMarker) { unknown(); continue; }
            found.state = OperationScanState::Completed;
        } else if (journal.cursor.Next() == 1 && !hasPublication && !hasMarker && !hasPending && !hasTransaction)
            found.state = OperationScanState::Empty;
        else found.state = OperationScanState::Incomplete;
        if (found.state == OperationScanState::Incomplete) result.blocksNewOperation = true;
        result.operations.push_back(std::move(found));
    } while (FindNextFileW(finder, &entry));
    const DWORD error = GetLastError();
    FindClose(finder);
    if (error != ERROR_NO_MORE_FILES) return fail("operations enumeration failed", error);
    result.ok = true;
    return result;
}
}
