#include "../native/control_center/journal.hpp"
#include <cstdio>

static BOOL WINAPI FailJournalWrite(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED) {
    SetLastError(ERROR_DISK_FULL); return FALSE;
}
static BOOL WINAPI FailJournalCleanup(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD) {
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
}
static bool MakeDir(const std::wstring& path) { return CreateDirectoryW(path.c_str(), nullptr) != FALSE; }

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1], journal = root + L"\\journal", broken = root + L"\\journal-broken";
    const std::wstring multiple = root + L"\\journal-multiple";
    if (!MakeDir(journal) || !MakeDir(broken) || !MakeDir(multiple)) return 2;
    control_center::JournalItem first;
    first.target = L"d3d9.dll"; first.backup = L"d3d9.dll"; first.previouslyPresent = true;
    first.staged = L"d3d9.dll.nr-stage"; first.quarantine = L"d3d9.dll.nr-original";
    first.candidateSha256.assign(64, 'a'); first.previousSha256.assign(64, 'b');
    first.previousIdentity = {1, 2, 3, 4, true};
    control_center::JournalItem second;
    second.target = L"host64/new.dll"; second.staged = L"host64/new.dll.nr-stage"; second.candidateSha256.assign(64, 'c');
    const control_center::JournalFileId firstStage{1, 2, 4, 100, true}, secondStage{1, 2, 5, 200, true};
    using control_center::JournalKind;
    control_center::JournalCursor cursor;
    control_center::JournalEvent event{1, JournalKind::Prepare, first};
    auto invalid = event; invalid.sequence = 2;
    if (control_center::PersistJournalEvent(journal, cursor, invalid)) return 3;
    auto persisted = control_center::PersistJournalEvent(journal, cursor, event);
    if (!persisted || !persisted.cursor.Pending() || persisted.cursor.Next() != 2) return 4;
    if (control_center::PersistJournalEvent(journal, cursor, event).error != control_center::JournalError::Write) return 5;
    auto replay = control_center::LoadJournal(journal);
    if (!replay || !replay.cursor.Pending() || replay.cursor.PendingEvent().item.target != first.target) return 6;
    cursor = replay.cursor;
    invalid = {2, JournalKind::Prepared, second, ERROR_SUCCESS, secondStage};
    if (control_center::PersistJournalEvent(journal, cursor, invalid)) return 7;
    event = {2, JournalKind::Prepared, first, ERROR_SUCCESS, firstStage}; persisted = control_center::PersistJournalEvent(journal, cursor, event);
    if (!persisted || persisted.cursor.Pending() || !persisted.cursor.Prepared()) return 8;
    cursor = persisted.cursor;
    event = {3, JournalKind::Intent, first}; persisted = control_center::PersistJournalEvent(journal, cursor, event);
    if (!persisted) return 9;
    cursor = persisted.cursor;
    event = {4, JournalKind::Applied, first}; persisted = control_center::PersistJournalEvent(journal, cursor, event);
    if (!persisted || persisted.cursor.Pending() || persisted.cursor.Applied().size() != 1) return 10;
    cursor = persisted.cursor;
    const control_center::JournalEvent remainder[] = {
        {5, JournalKind::Prepare, second}, {6, JournalKind::Prepared, second, ERROR_SUCCESS, secondStage},
        {7, JournalKind::Intent, second}, {8, JournalKind::CancelIntent, second}
    };
    for (const auto& step : remainder) {
        persisted = control_center::PersistJournalEvent(journal, cursor, step);
        if (!persisted) return 25;
        cursor = persisted.cursor;
    }
    event = {9, JournalKind::RollbackIntent, first}; persisted = control_center::PersistJournalEvent(journal, cursor, event);
    if (!persisted || !persisted.cursor.RollingBack()) return 11;
    cursor = persisted.cursor;
    event = {10, JournalKind::RollbackApplied, first}; persisted = control_center::PersistJournalEvent(journal, cursor, event);
    if (!persisted || !persisted.cursor.Applied().empty()) return 12;
    cursor = persisted.cursor;
    event = {11, JournalKind::RolledBack, {}}; persisted = control_center::PersistJournalEvent(journal, cursor, event);
    if (!persisted || !persisted.cursor.Terminal()) return 13;
    replay = control_center::LoadJournal(journal);
    if (!replay || !replay.cursor.Terminal() || replay.cursor.Next() != 12 || !replay.cursor.Applied().empty()) return 14;
    if (control_center::PersistJournalEvent(journal, replay.cursor, {12, JournalKind::Prepare, first})) return 15;
    control_center::JournalCursor blank;
    control_center::CopyIo io; io.write = &FailJournalWrite; io.dispose = &FailJournalCleanup;
    persisted = control_center::PersistJournalEvent(broken, blank, {1, JournalKind::Prepare, first}, io);
    if (persisted.error != control_center::JournalError::Write || !persisted.cleanupFailed || persisted.cursor.Next() != 1) return 16;
    replay = control_center::LoadJournal(broken);
    if (replay.error != control_center::JournalError::InvalidFile) return 17;
    if (control_center::LoadJournal(root + L"\\link").error != control_center::JournalError::UnsafeRoot) return 18;
    control_center::JournalEvent decoded;
    if (control_center::DecodeJournalEvent("{\"schema\":1,\"schema\":1}", decoded)) return 19;
    const control_center::JournalEvent sequence[] = {
        {1, JournalKind::Prepare, first}, {2, JournalKind::Prepared, first, ERROR_SUCCESS, firstStage},
        {3, JournalKind::Intent, first}, {4, JournalKind::Applied, first},
        {5, JournalKind::Prepare, second}, {6, JournalKind::Prepared, second, ERROR_SUCCESS, secondStage},
        {7, JournalKind::Intent, second}, {8, JournalKind::Applied, second},
        {9, JournalKind::RollbackIntent, second}, {10, JournalKind::RollbackFailed, second, ERROR_ACCESS_DENIED},
        {11, JournalKind::RollbackIntent, first}, {12, JournalKind::RollbackApplied, first},
    };
    cursor = control_center::JournalCursor{};
    for (const auto& step : sequence) {
        persisted = control_center::PersistJournalEvent(multiple, cursor, step);
        if (!persisted) return 20;
        cursor = persisted.cursor;
    }
    if (cursor.Unresolved().size() != 1 || cursor.Unresolved()[0].failureCode != ERROR_ACCESS_DENIED ||
        !cursor.Applied().empty()) return 21;
    if (control_center::PersistJournalEvent(multiple, cursor, {13, JournalKind::RolledBack, {}})) return 22;
    persisted = control_center::PersistJournalEvent(multiple, cursor, {13, JournalKind::RollbackIncomplete, {}});
    if (!persisted || !persisted.cursor.Terminal()) return 23;
    replay = control_center::LoadJournal(multiple);
    if (!replay || !replay.cursor.Terminal() || replay.cursor.Unresolved().size() != 1 ||
        replay.cursor.Unresolved()[0].failureCode != ERROR_ACCESS_DENIED) return 24;
    std::puts("journal: durable append, replay, pending intent, rollback, partial failure and torn checkpoint passed");
    return 0;
}
