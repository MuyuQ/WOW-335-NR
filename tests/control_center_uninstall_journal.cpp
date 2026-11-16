#include "../native/control_center/uninstall_journal.hpp"
#include <cstdio>

using namespace control_center;
static BOOL WINAPI FailWrite(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED) { SetLastError(ERROR_DISK_FULL); return FALSE; }
static BOOL WINAPI FailCleanup(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1], journal = root + L"\\uninstall-journal", broken = root + L"\\uninstall-journal-broken",
                       complete = root + L"\\uninstall-journal-complete";
    if (!CreateDirectoryW(journal.c_str(), nullptr) || !CreateDirectoryW(broken.c_str(), nullptr) || !CreateDirectoryW(complete.c_str(), nullptr)) return 2;
    TrustedUninstallPlan plan; plan.valid = true; plan.identity.sha256.fill(0x12);
    TrustedUninstallItem first; first.state = UninstallItemState::RestoreOriginal; first.saveCurrent = true;
    TrustedUninstallItem skipped; skipped.state = UninstallItemState::AlreadyAbsent;
    TrustedUninstallItem second; second.state = UninstallItemState::DeleteModified; second.saveCurrent = true;
    plan.items = {first, skipped, second};
    const JournalFileId staged{1, 2, 3, 4, true}, saved1{1, 2, 4, 5, true}, saved2{1, 2, 5, 6, true};
    using K = UninstallJournalKind;
    UninstallJournalCursor cursor(plan);
    auto foreignPlan = plan; foreignPlan.identity.sha256.fill(0x13);
    if (PersistUninstallJournal(journal, foreignPlan, cursor, {1, K::Prepare, 0})) return 17;
    foreignPlan = plan; foreignPlan.items[0].saveCurrent = false;
    if (PersistUninstallJournal(journal, foreignPlan, cursor, {1, K::Prepare, 0})) return 18;
    if (PersistUninstallJournal(journal, plan, cursor, {1, K::MembersDone, 0})) return 19;
    if (PersistUninstallJournal(journal, plan, cursor, {1, K::Prepare, 2})) return 3;
    auto out = PersistUninstallJournal(journal, plan, cursor, {1, K::Prepare, 0});
    if (!out || !out.cursor.Preparing()) return 4;
    cursor = out.cursor;
    if (PersistUninstallJournal(journal, plan, cursor, {2, K::Prepared, 0, {}, saved1})) return 5;
    out = PersistUninstallJournal(journal, plan, cursor, {2, K::Prepared, 0, staged, saved1});
    if (!out || !out.cursor.Prepared()) return 6;
    cursor = out.cursor;
    out = PersistUninstallJournal(journal, plan, cursor, {3, K::Intent, 0});
    if (!out || !out.cursor.IntentPending()) return 7;
    auto loaded = LoadUninstallJournal(journal, plan);
    if (!loaded || !loaded.cursor.IntentPending() || loaded.cursor.Current() != 0 || !SameJournalId(loaded.cursor.Staged(), staged) ||
        !SameJournalId(loaded.cursor.Saved(), saved1)) return 8;
    cursor = loaded.cursor;
    const UninstallJournalEvent events[] = {
        {4, K::Applied, 0}, {5, K::Prepare, 2}, {6, K::Prepared, 2, {}, saved2}, {7, K::Intent, 2}, {8, K::Applied, 2},
        {9, K::RollbackIntent, 2}, {10, K::RollbackFailed, 2, {}, {}, ERROR_ACCESS_DENIED},
        {11, K::RollbackIntent, 0}, {12, K::RollbackApplied, 0}
    };
    for (const auto& event : events) {
        out = PersistUninstallJournal(journal, plan, cursor, event);
        if (!out) return 9;
        cursor = out.cursor;
    }
    if (cursor.Unresolved().size() != 1 || !cursor.Applied().empty() ||
        PersistUninstallJournal(journal, plan, cursor, {13, K::RolledBack, 0})) return 10;
    out = PersistUninstallJournal(journal, plan, cursor, {13, K::RollbackIncomplete, 0});
    if (!out || !out.cursor.Terminal()) return 11;
    loaded = LoadUninstallJournal(journal, plan);
    if (!loaded || !loaded.cursor.Terminal() || loaded.cursor.TerminalKind() != K::RollbackIncomplete || loaded.cursor.Unresolved().size() != 1) return 12;
    auto differentPlan = plan; differentPlan.identity.sha256.fill(0x13);
    if (LoadUninstallJournal(journal, differentPlan).error != JournalError::InvalidFile) return 13;
    UninstallJournalCursor fresh(plan); CopyIo io; io.write = &FailWrite; io.dispose = &FailCleanup;
    out = PersistUninstallJournal(broken, plan, fresh, {1, K::Prepare, 0}, io);
    if (out.error != JournalError::Write || !out.cleanupFailed || LoadUninstallJournal(broken, plan).error != JournalError::InvalidFile) return 14;
    UninstallJournalCursor completed(plan);
    const UninstallJournalEvent done[] = {
        {1, K::Prepare, 0}, {2, K::Prepared, 0, staged, saved1}, {3, K::Intent, 0}, {4, K::Applied, 0},
        {5, K::Prepare, 2}, {6, K::Prepared, 2, {}, saved2}, {7, K::Intent, 2}, {8, K::Applied, 2}, {9, K::MembersDone, 0}
    };
    for (const auto& event : done) {
        out = PersistUninstallJournal(complete, plan, completed, event);
        if (!out) return 15;
        completed = out.cursor;
    }
    loaded = LoadUninstallJournal(complete, plan);
    if (!loaded || loaded.cursor.TerminalKind() != K::MembersDone || loaded.cursor.Applied().size() != 2) return 16;
    std::puts("uninstall journal: plan binding, replay, ordering, rollback failure and torn write passed");
    return 0;
}
