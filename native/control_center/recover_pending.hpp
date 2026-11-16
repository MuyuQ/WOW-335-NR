#pragma once

#include "commit_install.hpp"
#include "recover_client_record.hpp"
#include "scan_operations.hpp"

namespace control_center {
enum class PendingRecoveryState { NoWork, RolledBack, Committed, NeedsAttention };
struct PendingRecoveryResult {
    PendingRecoveryState state = PendingRecoveryState::NeedsAttention;
    std::wstring operationId;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    RecoveryResult members;
};

// The client lock remains held for scan, decision and recovery. More than one
// unfinished operation is deliberately not ordered or modified automatically.
inline PendingRecoveryResult RecoverPendingOperations(const std::wstring& operationsRoot, const std::wstring& clientRoot,
                                                        const std::vector<std::wstring>& protectedRoots,
                                                        const CatalogResult& catalog, const Json& manifest,
                                                        const ClientLock& lock
#ifdef CONTROL_CENTER_TESTING
                                                        , ProcessCheck (*testProcesses)() = nullptr
#endif
                                                        ) {
    PendingRecoveryResult result;
    auto fail = [&result](std::string reason, DWORD error = ERROR_SUCCESS) {
        result.state = PendingRecoveryState::NeedsAttention; result.reason = std::move(reason); result.win32Error = error; return result;
    };
    if (!lock.IsFor(clientRoot) || !catalog) return fail("invalid client lock or catalog");
    const auto scan = ScanClientOperations(operationsRoot, lock, catalog);
    if (!scan.ok) return fail("operation scan failed", scan.win32Error);
    const ScannedOperation* pending = nullptr;
    for (const auto& item : scan.operations) {
        if (item.state == OperationScanState::Unknown) return fail("unknown operation requires inspection");
        if (item.state != OperationScanState::Incomplete) continue;
        if (pending) return fail("multiple unfinished operations require inspection");
        pending = &item;
    }
    if (!pending) { result.state = PendingRecoveryState::NoWork; return result; }
    result.operationId = pending->id;
    MemberInstallContext context;
    context.clientRoot = clientRoot; context.journalRoot = pending->directory; context.operationId = pending->id;
    context.protectedRoots = protectedRoots; context.stageRoot = operationsRoot; context.templateRoot = operationsRoot;
#ifdef CONTROL_CENTER_TESTING
    context.testProcesses = testProcesses;
#endif
    const auto client = ValidateClient(clientRoot, protectedRoots);
    if (!client || MemberProcesses(context).state != ProcessState::Stopped) return fail("client or process state prevents recovery");
    GuardedPath publication, marker, pendingMarker, transaction, pendingTransaction;
    if (!publication.Open(context.journalRoot, L"record-publication") || !marker.Open(context.journalRoot, L"commit.json") ||
        !pendingMarker.Open(context.journalRoot, L"commit.pending") || !transaction.Open(context.journalRoot, L"transaction.json") ||
        !pendingTransaction.Open(context.journalRoot, L"transaction.pending")) return fail("operation paths unavailable");
    auto presence = [](const GuardedPath& path) -> int {
        if (GetFileAttributesW(path.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 1;
        return MissingPathError(GetLastError()) ? 0 : -1;
    };
    const int hasPublication = presence(publication), hasMarker = presence(marker), hasPendingMarker = presence(pendingMarker),
              hasTransaction = presence(transaction), hasPendingTransaction = presence(pendingTransaction);
    if (hasPublication < 0 || hasMarker < 0 || hasPendingMarker < 0 || hasTransaction < 0 || hasPendingTransaction < 0)
        return fail("cannot inspect operation state");
    if (hasPublication || hasMarker || hasPendingMarker || hasTransaction || hasPendingTransaction) {
        GuardedPath plan;
        std::string bytes, operationText, stageText; DWORD error = ERROR_SUCCESS;
        if (!plan.Open(context.journalRoot, L"finalization-plan.json") || !ReadJournalBytes(plan, bytes, error))
            return fail("finalization plan unavailable", error);
        const auto parsed = ParseJsonObject(bytes);
        if (!parsed || !StringField(parsed.value, "operation_record", operationText)) return fail("invalid finalization plan");
        const auto operationRecord = ReadOperationRecord(operationText);
        if (!operationRecord || !StringField(operationRecord.original, "stage", stageText) ||
            !Utf8Wide(stageText, context.stageRoot)) return fail("invalid operation stage");
        std::wstring stage, current;
        if (!CanonicalLocalPath(context.stageRoot, stage) || !CanonicalLocalPath(clientRoot, current) ||
            CompareStringOrdinal(operationRecord.client.c_str(), -1, current.c_str(), -1, TRUE) != CSTR_EQUAL)
            return fail("operation paths do not match client");
        context.stageRoot = stage; context.templateRoot = stage;
    }
    if (hasMarker || hasPendingMarker) {
        const auto commit = CommitInstall(context, catalog, manifest, lock);
        if (commit.state != CommitState::Completed) return fail(commit.reason, commit.win32Error);
        result.state = PendingRecoveryState::Committed;
    } else {
        if (hasTransaction || hasPendingTransaction) return fail("transaction record has no commit decision");
        if (hasPublication && !RecordRollbackAllowsMemberRecovery(context.journalRoot, client.root, context, catalog, lock)) {
            const auto record = RecoverClientRecord(context, catalog, manifest, lock);
            if (record.state != RecordRecoveryState::Recovered && record.state != RecordRecoveryState::AlreadyTerminal)
                return fail(record.reason, record.win32Error);
        }
        result.members = RecoverOperation(context, catalog, lock);
        if (result.members.state != RecoveryState::Recovered && result.members.state != RecoveryState::AlreadyTerminal &&
            result.members.state != RecoveryState::NoWork) return fail(result.members.reason, result.members.win32Error);
        result.state = PendingRecoveryState::RolledBack;
    }
    const auto after = ScanClientOperations(operationsRoot, lock, catalog);
    if (!after.ok || after.blocksNewOperation) return fail("recovery left unresolved operation state", after.win32Error);
    return result;
}
}
