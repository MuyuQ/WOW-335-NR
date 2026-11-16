#pragma once

#include "journal.hpp"
#include "install_preview.hpp"

namespace control_center {
enum class ObservedState { Missing, Candidate, Previous, Other, Unavailable };
enum class ReconcileDecision { NoPending, PrepareIncomplete, BeforeMutation, Applied, RestoreQuarantinedOriginal,
                               RollbackNotStarted, RolledBack, Conflict, Unavailable };
struct ReconcileResult {
    ReconcileDecision decision = ReconcileDecision::Unavailable;
    DWORD win32Error = ERROR_SUCCESS;
};
inline ObservedState ObserveJournalFile(const std::wstring& root, const std::wstring& relative,
                                        const JournalItem& item, const JournalFileId& candidateId,
                                        DWORD& error, bool requireIdentity = true) {
    GuardedPath path;
    if (!path.Open(root, relative)) { error = path.Error(); return ObservedState::Unavailable; }
    const auto hash = HashFile(path.Path());
    if (!hash) {
        if (hash.error == HashError::Open && MissingPathError(hash.win32Error)) return ObservedState::Missing;
        error = hash.win32Error; return ObservedState::Unavailable;
    }
    const auto hex = HexSha256(hash.identity.sha256);
    if (hex == item.candidateSha256 && (!requireIdentity || SameJournalId(JournalId(hash.identity), candidateId))) return ObservedState::Candidate;
    if (item.previouslyPresent && hex == item.previousSha256 &&
        (!requireIdentity || SameJournalId(JournalId(hash.identity), item.previousIdentity))) return ObservedState::Previous;
    return ObservedState::Other;
}

// Inspect a pending intent without mutating the client. The caller must retain
// an operation-wide lock and revalidate the same observations before acting.
// A conflict or inaccessible file never authorizes deletion or replacement.
inline ReconcileResult ReconcilePending(const std::wstring& clientRoot, const std::wstring& backupRoot,
                                        const JournalCursor& cursor) {
    ReconcileResult result;
    if (!cursor.Pending()) { result.decision = ReconcileDecision::NoPending; return result; }
    const auto& event = cursor.PendingEvent();
    if (event.kind == JournalKind::Prepare) { result.decision = ReconcileDecision::PrepareIncomplete; return result; }
    const auto& item = event.item;
    if (!ValidJournalItem(item)) { result.decision = ReconcileDecision::Conflict; return result; }
    const auto candidateId = cursor.PendingCandidateIdentity();
    if (!candidateId.known) { result.decision = ReconcileDecision::Conflict; return result; }
    const auto target = ObserveJournalFile(clientRoot, item.target, item, candidateId, result.win32Error);
    const auto staged = ObserveJournalFile(clientRoot, item.staged, item, candidateId, result.win32Error);
    if (target == ObservedState::Unavailable || staged == ObservedState::Unavailable) return result;
    ObservedState quarantine = ObservedState::Missing;
    if (item.previouslyPresent) {
        quarantine = ObserveJournalFile(clientRoot, item.quarantine, item, candidateId, result.win32Error);
        const auto backup = ObserveJournalFile(backupRoot, item.backup, item, candidateId, result.win32Error, false);
        if (quarantine == ObservedState::Unavailable || backup == ObservedState::Unavailable) return result;
        if (backup != ObservedState::Previous) { result.decision = ReconcileDecision::Conflict; return result; }
    }
    result.decision = ReconcileDecision::Conflict;
    if (event.kind == JournalKind::Intent) {
        if (!item.previouslyPresent) {
            if (target == ObservedState::Missing && staged == ObservedState::Candidate) result.decision = ReconcileDecision::BeforeMutation;
            else if (target == ObservedState::Candidate && staged == ObservedState::Missing) result.decision = ReconcileDecision::Applied;
        } else if (target == ObservedState::Previous && quarantine == ObservedState::Missing && staged == ObservedState::Candidate)
            result.decision = ReconcileDecision::BeforeMutation;
        else if (target == ObservedState::Missing && quarantine == ObservedState::Previous && staged == ObservedState::Candidate)
            result.decision = ReconcileDecision::RestoreQuarantinedOriginal;
        else if (target == ObservedState::Candidate && quarantine == ObservedState::Previous && staged == ObservedState::Missing)
            result.decision = ReconcileDecision::Applied;
    } else if (event.kind == JournalKind::RollbackIntent) {
        if (!item.previouslyPresent) {
            if (target == ObservedState::Candidate && staged == ObservedState::Missing) result.decision = ReconcileDecision::RollbackNotStarted;
            else if (target == ObservedState::Missing && staged == ObservedState::Missing) result.decision = ReconcileDecision::RolledBack;
        } else if (target == ObservedState::Candidate && quarantine == ObservedState::Previous && staged == ObservedState::Missing)
            result.decision = ReconcileDecision::RollbackNotStarted;
        else if (target == ObservedState::Missing && quarantine == ObservedState::Previous && staged == ObservedState::Candidate)
            result.decision = ReconcileDecision::RestoreQuarantinedOriginal;
        else if (target == ObservedState::Previous && quarantine == ObservedState::Missing && staged == ObservedState::Candidate)
            result.decision = ReconcileDecision::RolledBack;
    }
    return result;
}
}
