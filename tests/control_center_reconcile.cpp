#include "../native/control_center/reconcile.hpp"
#include "../native/control_center/copy_verified.hpp"
#include "../native/control_center/rename_verified.hpp"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1], client = root + L"\\reconcile-client", backup = root + L"\\reconcile-backup";
    const std::wstring badBackup = root + L"\\reconcile-bad-backup";
    if (!CreateDirectoryW(client.c_str(), nullptr) || !CreateDirectoryW(backup.c_str(), nullptr) ||
        !CreateDirectoryW(badBackup.c_str(), nullptr)) return 2;
    control_center::GuardedPath original, candidate, target, staged, quarantine, backupFile, badBackupFile;
    if (!original.Open(root, L"real/sample.bin") || !candidate.Open(root, L"real/occupied.bin") ||
        !target.Open(client, L"d3d9.dll") || !staged.Open(client, L"d3d9.dll.nr-stage") ||
        !quarantine.Open(client, L"d3d9.dll.nr-original") || !backupFile.Open(backup, L"d3d9.dll") ||
        !badBackupFile.Open(badBackup, L"d3d9.dll")) return 3;
    const auto before = control_center::HashFile(original.Path()), after = control_center::HashFile(candidate.Path());
    if (!before || !after || before.identity.sha256 == after.identity.sha256) return 4;
    const auto targetCopy = control_center::CopyVerifiedNew(original, target, before.identity.sha256);
    const auto originalBackup = control_center::CopyVerifiedNew(original, backupFile, before.identity.sha256);
    const auto changedBackup = control_center::CopyVerifiedNew(candidate, badBackupFile, after.identity.sha256);
    const auto stageCopy = control_center::CopyVerifiedNew(candidate, staged, after.identity.sha256);
    if (!targetCopy || !originalBackup || !changedBackup || !stageCopy) return 5;
    control_center::JournalItem item;
    item.target = L"d3d9.dll"; item.staged = L"d3d9.dll.nr-stage"; item.quarantine = L"d3d9.dll.nr-original";
    item.backup = L"d3d9.dll"; item.previouslyPresent = true;
    item.previousSha256 = control_center::HexSha256(before.identity.sha256);
    item.previousIdentity = control_center::JournalId(targetCopy.target);
    item.candidateSha256 = control_center::HexSha256(after.identity.sha256);
    control_center::JournalCursor cursor;
    using control_center::JournalKind;
    using control_center::ReconcileDecision;
    if (!cursor.Accept({1, JournalKind::Prepare, item}) ||
        control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::PrepareIncomplete ||
        !cursor.Accept({2, JournalKind::Prepared, item, ERROR_SUCCESS, control_center::JournalId(stageCopy.target)}) ||
        !cursor.Accept({3, JournalKind::Intent, item})) return 6;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::BeforeMutation) return 7;
    if (control_center::ReconcilePending(client, badBackup, cursor).decision != ReconcileDecision::Conflict) return 8;
    if (!control_center::RenameVerifiedNew(target, quarantine, before.identity.sha256)) return 9;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::RestoreQuarantinedOriginal) return 10;
    if (!control_center::RenameVerifiedNew(staged, target, after.identity.sha256)) return 11;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::Applied) return 12;
    if (!cursor.Accept({4, JournalKind::Applied, item}) || !cursor.Accept({5, JournalKind::RollbackIntent, item})) return 13;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::RollbackNotStarted) return 14;
    if (!control_center::RenameVerifiedNew(target, staged, after.identity.sha256)) return 15;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::RestoreQuarantinedOriginal) return 16;
    if (!control_center::RenameVerifiedNew(quarantine, target, before.identity.sha256)) return 17;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::RolledBack) return 18;
    if (!cursor.Accept({6, JournalKind::RollbackApplied, item})) return 19;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::NoPending) return 20;

    control_center::GuardedPath freshTarget, freshStaged;
    if (!freshTarget.Open(client, L"new.dll") || !freshStaged.Open(client, L"new.dll.nr-stage")) return 21;
    const auto freshCopy = control_center::CopyVerifiedNew(candidate, freshStaged, after.identity.sha256);
    if (!freshCopy) return 21;
    control_center::JournalItem fresh;
    fresh.target = L"new.dll"; fresh.staged = L"new.dll.nr-stage"; fresh.candidateSha256 = item.candidateSha256;
    cursor = control_center::JournalCursor{};
    if (!cursor.Accept({1, JournalKind::Prepare, fresh}) ||
        !cursor.Accept({2, JournalKind::Prepared, fresh, ERROR_SUCCESS, control_center::JournalId(freshCopy.target)}) ||
        !cursor.Accept({3, JournalKind::Intent, fresh})) return 22;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::BeforeMutation) return 23;
    if (!control_center::RenameVerifiedNew(freshStaged, freshTarget, after.identity.sha256)) return 24;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::Applied) return 25;
    if (!cursor.Accept({4, JournalKind::Applied, fresh}) || !cursor.Accept({5, JournalKind::RollbackIntent, fresh})) return 26;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::RollbackNotStarted) return 27;
    if (!DeleteFileW(freshTarget.Path().c_str())) return 28;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::RolledBack) return 29;
    if (!control_center::CopyVerifiedNew(candidate, freshTarget, after.identity.sha256)) return 32;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::Conflict) return 33;
    if (!DeleteFileW(freshTarget.Path().c_str())) return 34;
    if (!control_center::WriteNewVerified(freshTarget, "user-modified")) return 30;
    if (control_center::ReconcilePending(client, backup, cursor).decision != ReconcileDecision::Conflict) return 31;
    std::puts("reconcile: before, interrupted publish, applied, interrupted rollback, conflict and bad backup passed");
    return 0;
}
