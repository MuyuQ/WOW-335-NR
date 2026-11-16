#pragma once

#include "recover_operation.hpp"

namespace control_center {
enum class BatchInstallState { ReadyToFinalize, AlreadyCurrent, PreconditionFailed, RolledBack, RecoveryRequired };
struct BatchInstallResult {
    BatchInstallState state = BatchInstallState::PreconditionFailed;
    std::string reason;
    DWORD win32Error = ERROR_SUCCESS;
    size_t applied = 0;
    RecoveryResult recovery;
};

// The operation directory must be new and the caller must first discover and
// resolve all unfinished operations for this client. ReadyToFinalize does not
// mean installation succeeded: both records still require durable publication.
inline BatchInstallResult InstallBatch(const MemberInstallContext& context, const CatalogResult& catalog,
                                       const InstallPreview& approved, const ClientLock& lock) {
    BatchInstallResult result;
    auto fail = [&result](const char* reason) { result.reason = reason; return result; };
    if (!catalog || catalog.members.empty() || !approved.filesReady || approved.items.size() != catalog.members.size() ||
        !ValidOperationId(context.operationId) || !lock.IsFor(context.clientRoot)) return fail("invalid approved preview or lock");
    const auto client = ValidateClient(context.clientRoot, context.protectedRoots);
    if (!client || MemberProcesses(context).state != ProcessState::Stopped) return fail("client or process state prevents installation");
    std::wstring journalRoot;
    if (!CanonicalLocalPath(context.journalRoot, journalRoot) || WithinRoot(journalRoot, client.root) ||
        WithinRoot(client.root, journalRoot)) return fail("invalid operation directory");
    GuardedPath journalGuard;
    if (!journalGuard.Open(journalRoot, L"__journal_root__")) return fail("operation directory unavailable");
    const auto journal = LoadJournal(journalRoot);
    if (!journal || journal.cursor.Next() != 1) return fail("operation directory is not new");
    const auto current = PreviewInstall(client.root, context.stageRoot, context.templateRoot, catalog);
    if (!current.filesReady || current.items.size() != approved.items.size()) return fail("preview no longer valid");
    for (size_t i = 0; i < current.items.size(); ++i) {
        const auto& expected = approved.items[i];
        const auto& actual = current.items[i];
        const auto& trusted = catalog.members[i];
        if (!SameTarget(expected.member.target, trusted.target) || !SameTarget(expected.member.source, trusted.source) ||
            expected.member.sourceKind != trusted.sourceKind || expected.member.sha256 != trusted.sha256 ||
            expected.member.required != trusted.required || expected.member.replacesOriginal != trusted.replacesOriginal ||
            actual.state != expected.state ||
            (actual.state != PreviewState::MissingOptional && !SameIdentity(expected.source, actual.source)) ||
            ((actual.state == PreviewState::Same || actual.state == PreviewState::Present) &&
                !SameIdentity(expected.existing, actual.existing))) return fail("approved preview changed");
    }
    for (const auto& item : approved.items) {
        const auto installed = InstallMember(context, catalog, item, lock);
        if (installed.state == MemberInstallState::Applied) { ++result.applied; continue; }
        if (installed.state == MemberInstallState::NoChange) continue;
        result.reason = installed.reason; result.win32Error = installed.win32Error;
        const auto interrupted = LoadJournal(journalRoot);
        if (!interrupted) { result.state = BatchInstallState::RecoveryRequired; return result; }
        if (interrupted.cursor.Next() == 1) {
            result.state = installed.state == MemberInstallState::RecoveryRequired ? BatchInstallState::RecoveryRequired : BatchInstallState::PreconditionFailed;
            return result;
        }
        result.recovery = RecoverOperation(context, catalog, lock);
        result.state = result.recovery.state == RecoveryState::Recovered ? BatchInstallState::RolledBack : BatchInstallState::RecoveryRequired;
        return result;
    }
    // A later repair must also be able to find unchanged inherited members.
    // Store a verified copy of every available source in the operation, not
    // just the members replaced by this particular batch.
    if (result.applied) for (const auto& item : approved.items) {
        if (item.state == PreviewState::MissingOptional) continue;
        GuardedPath source, persistent;
        const auto& sourceRoot = item.member.sourceKind == "template" ? context.templateRoot : context.stageRoot;
        bool valid = source.Open(sourceRoot, item.member.source) &&
                     persistent.Open(journalRoot, L"stage/" + item.member.target, true);
        DWORD error = valid ? ERROR_SUCCESS : (source.Error() ? source.Error() : persistent.Error());
        if (valid) {
            const auto existing = HashFile(persistent.Path());
            if (existing) valid = existing.identity.sha256 == item.source.sha256;
            else if (existing.error == HashError::Open && MissingPathError(existing.win32Error)) {
                const auto copied = CopyVerifiedNew(source, persistent, item.source.sha256);
                valid = copied && SameIdentity(copied.source, item.source);
                error = copied.win32Error;
            } else { valid = false; error = existing.win32Error; }
        }
        if (!valid) {
            result.reason = "could not preserve repair source"; result.win32Error = error;
            result.recovery = RecoverOperation(context, catalog, lock);
            result.state = result.recovery.state == RecoveryState::Recovered ? BatchInstallState::RolledBack : BatchInstallState::RecoveryRequired;
            return result;
        }
    }
    result.state = result.applied ? BatchInstallState::ReadyToFinalize : BatchInstallState::AlreadyCurrent;
    return result;
}
}
