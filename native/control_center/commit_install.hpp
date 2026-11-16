#pragma once

#include "commit_validate.hpp"

namespace control_center {
enum class CommitState { Completed, RecoveryRequired };
enum class CommitCheckpoint { MarkerPrepared, MarkerPublished, TransactionPrepared, TransactionPublished, RecordCompleted, MembersCompleted };
struct CommitResult {
    CommitState state = CommitState::RecoveryRequired;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
};

// A published commit marker is the decision point. Thereafter recovery only
// finishes the transaction and never rolls the installed files back.
inline CommitResult CommitInstall(const MemberInstallContext& context, const CatalogResult& catalog,
                                  const Json& manifest, const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                  , bool (*interrupt)(CommitCheckpoint) = nullptr
#endif
                                  ) {
    CommitResult result;
    auto fail = [&result](std::string reason, DWORD error = ERROR_SUCCESS) { result.reason = std::move(reason); result.win32Error = error; return result; };
    auto interrupted = [&](CommitCheckpoint point) {
#ifdef CONTROL_CENTER_TESTING
        return interrupt && interrupt(point);
#else
        (void)point; return false;
#endif
    };
    const auto validation = ValidateCommit(context, catalog, manifest, lock);
    if (!validation) return fail(validation.error, validation.win32Error);
    std::wstring operation;
    if (!CanonicalLocalPath(context.journalRoot, operation)) return fail("invalid operation path");
    GuardedPath root, marker, pendingMarker, transaction, pendingTransaction;
    if (!root.Open(operation, L"__journal_root__") || !marker.Open(operation, L"commit.json") ||
        !pendingMarker.Open(operation, L"commit.pending") || !transaction.Open(operation, L"transaction.json") ||
        !pendingTransaction.Open(operation, L"transaction.pending")) return fail("commit path unavailable");
    const std::uint64_t recordNext = validation.recordJournal.cursor.Next() - (validation.recordJournal.cursor.Terminal() ? 1 : 0);
    const Json markerJson = {{"schema", 1}, {"operation_id", validation.operationId}, {"catalog_sha256", validation.catalogDigest},
        {"plan_sha256", HexSha256(validation.planIdentity.sha256)}, {"client_record_identity", JournalIdJson(JournalId(validation.recordIdentity))},
        {"member_next", validation.membersBeforeComplete.Next()}, {"record_next", recordNext}};
    const std::string markerBytes = markerJson.dump() + "\n";
    std::array<std::uint8_t, 32> markerHash{}, transactionHash{};
    if (HashBytes(markerBytes, markerHash) < 0 || HashBytes(validation.transactionBytes, transactionHash) < 0) return fail("cannot hash commit data");
    auto checkBytes = [](const GuardedPath& path, const std::string& expected, FileIdentity& identity, DWORD& error) {
        const auto hash = HashFile(path.Path());
        if (!hash) { error = hash.win32Error; return false; }
        std::string bytes;
        if (!ReadJournalBytes(path, bytes, error) || bytes != expected) return false;
        std::array<std::uint8_t, 32> computed{};
        if (HashBytes(bytes, computed) < 0 || computed != hash.identity.sha256) return false;
        identity = hash.identity; return true;
    };
    auto absent = [](const GuardedPath& path) {
        return GetFileAttributesW(path.Path().c_str()) == INVALID_FILE_ATTRIBUTES && MissingPathError(GetLastError());
    };
    FileIdentity markerIdentity; DWORD error = ERROR_SUCCESS;
    if (absent(marker)) {
        if (validation.members.cursor.Terminal() || validation.recordJournal.cursor.Terminal() || !absent(transaction) || !absent(pendingTransaction))
            return fail("commit artifacts exist before decision");
        FileIdentity tempIdentity;
        if (absent(pendingMarker)) {
            const auto write = WriteNewVerified(pendingMarker, markerBytes);
            if (!write) return fail("cannot prepare commit marker", write.win32Error);
            tempIdentity = write.identity;
        } else if (!checkBytes(pendingMarker, markerBytes, tempIdentity, error)) return fail("pending commit marker differs", error);
        if (interrupted(CommitCheckpoint::MarkerPrepared)) return fail("interrupted before commit decision");
        const auto revalidated = ValidateCommit(context, catalog, manifest, lock);
        if (!revalidated || !SameIdentity(revalidated.planIdentity, validation.planIdentity) ||
            !SameIdentity(revalidated.recordIdentity, validation.recordIdentity)) return fail("inputs changed before commit");
        const auto moved = RenameVerifiedNew(pendingMarker, marker, markerHash, &tempIdentity);
        if (!moved) return fail("cannot publish commit marker", moved.win32Error);
        markerIdentity = moved.identity;
        if (interrupted(CommitCheckpoint::MarkerPublished)) return fail("interrupted after commit decision");
    } else if (!checkBytes(marker, markerBytes, markerIdentity, error) || !absent(pendingMarker)) return fail("commit marker differs", error);
    const auto afterMarker = ValidateCommit(context, catalog, manifest, lock);
    if (!afterMarker || !SameIdentity(afterMarker.planIdentity, validation.planIdentity) ||
        !SameIdentity(afterMarker.recordIdentity, validation.recordIdentity)) return fail("commit inputs changed");
    if (absent(transaction)) {
        FileIdentity tempIdentity;
        if (absent(pendingTransaction)) {
            const auto write = WriteNewVerified(pendingTransaction, validation.transactionBytes);
            if (!write) return fail("cannot prepare transaction record", write.win32Error);
            tempIdentity = write.identity;
        } else if (!checkBytes(pendingTransaction, validation.transactionBytes, tempIdentity, error)) return fail("pending transaction differs", error);
        if (interrupted(CommitCheckpoint::TransactionPrepared)) return fail("interrupted while preparing transaction");
        FileIdentity markerNow;
        if (!checkBytes(marker, markerBytes, markerNow, error) || !SameIdentity(markerNow, markerIdentity)) return fail("commit marker changed", error);
        const auto moved = RenameVerifiedNew(pendingTransaction, transaction, transactionHash, &tempIdentity);
        if (!moved) return fail("cannot publish transaction record", moved.win32Error);
        if (interrupted(CommitCheckpoint::TransactionPublished)) return fail("interrupted after transaction publication");
    } else {
        FileIdentity identity;
        if (!checkBytes(transaction, validation.transactionBytes, identity, error) || !absent(pendingTransaction)) return fail("transaction record differs", error);
    }
    auto decisionStillValid = [&]() {
        FileIdentity currentMarker, currentTransaction;
        if (!absent(pendingMarker) || !absent(pendingTransaction) ||
            !checkBytes(marker, markerBytes, currentMarker, error) || !SameIdentity(currentMarker, markerIdentity) ||
            !checkBytes(transaction, validation.transactionBytes, currentTransaction, error)) return false;
        const auto current = ValidateCommit(context, catalog, manifest, lock);
        return current && SameIdentity(current.planIdentity, validation.planIdentity) &&
            SameIdentity(current.recordIdentity, validation.recordIdentity);
    };
    if (!decisionStillValid()) return fail("commit state changed", error);
    auto metadata = LoadJournal(operation + L"\\record-publication");
    if (!metadata) return fail("record journal unavailable", metadata.win32Error);
    if (!metadata.cursor.Terminal()) {
        const auto next = PersistJournalEvent(operation + L"\\record-publication", metadata.cursor, {metadata.cursor.Next(), JournalKind::Complete, {}});
        if (!next) return fail("cannot complete record journal", next.win32Error);
    } else if (metadata.cursor.TerminalKind() != JournalKind::Complete) return fail("record journal has incompatible terminal");
    if (interrupted(CommitCheckpoint::RecordCompleted)) return fail("interrupted after record completion");
    if (!decisionStillValid()) return fail("commit state changed", error);
    auto members = LoadJournal(operation);
    if (!members) return fail("member journal unavailable", members.win32Error);
    if (!members.cursor.Terminal()) {
        const auto next = PersistJournalEvent(operation, members.cursor, {members.cursor.Next(), JournalKind::Complete, {}});
        if (!next) return fail("cannot complete member journal", next.win32Error);
    } else if (members.cursor.TerminalKind() != JournalKind::Complete) return fail("member journal has incompatible terminal");
    if (interrupted(CommitCheckpoint::MembersCompleted)) return fail("interrupted after member completion");
    if (!decisionStillValid()) return fail("commit state changed", error);
    const auto final = ValidateCommit(context, catalog, manifest, lock);
    if (!final || !final.members.cursor.Terminal() || !final.recordJournal.cursor.Terminal()) return fail("completed transaction failed validation");
    result.state = CommitState::Completed;
    return result;
}
}
