#include "../native/control_center/write_new.hpp"
#include <cstdio>

static BOOL WINAPI ShortWrite(HANDLE file, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) {
    return WriteFile(file, data, size > 3 ? 3 : size, written, overlapped);
}
static BOOL WINAPI ZeroWrite(HANDLE, LPCVOID, DWORD, LPDWORD written, LPOVERLAPPED) { *written = 0; return TRUE; }
static BOOL WINAPI CorruptWrite(HANDLE file, LPCVOID, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) {
    std::string modified(size, 'x');
    return WriteFile(file, modified.data(), size, written, overlapped);
}
static BOOL WINAPI FailedFlush(HANDLE) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
static BOOL WINAPI FailedCleanup(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1];
    control_center::GuardedPath normal, shortPath, badPath, flushPath, cleanupPath, occupied, invalid;
    if (!normal.Open(root, L"real/journal.bin") || !shortPath.Open(root, L"real/journal-short.bin") ||
        !badPath.Open(root, L"real/journal-corrupt.bin") || !flushPath.Open(root, L"real/journal-flush.bin") ||
        !cleanupPath.Open(root, L"real/journal-cleanup.bin") || !occupied.Open(root, L"real/occupied.bin")) return 2;
    const std::string content = std::string("abc\0", 4) + std::string(65537, 'y');
    auto result = control_center::WriteNewVerified(normal, content);
    if (!result || result.identity.size != content.size()) return 3;
    const auto actual = control_center::HashFile(normal.Path());
    if (!actual || actual.identity.sha256 != result.identity.sha256) return 4;
    control_center::CopyIo io; io.write = &ShortWrite;
    result = control_center::WriteNewVerified(shortPath, "short-write-data", io);
    if (!result || result.identity.size != 16) return 5;
    io.write = &CorruptWrite;
    result = control_center::WriteNewVerified(badPath, "expected", io);
    if (result.error != control_center::WriteNewError::Verify || result.cleanupFailed ||
        GetFileAttributesW(badPath.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 6;
    io = control_center::CopyIo{}; io.flush = &FailedFlush;
    result = control_center::WriteNewVerified(flushPath, "data", io);
    if (result.error != control_center::WriteNewError::Flush || result.cleanupFailed ||
        GetFileAttributesW(flushPath.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 7;
    io.write = &ZeroWrite; io.dispose = &FailedCleanup;
    result = control_center::WriteNewVerified(cleanupPath, "data", io);
    if (result.error != control_center::WriteNewError::Write || !result.cleanupFailed ||
        result.cleanupError != ERROR_ACCESS_DENIED || GetFileAttributesW(cleanupPath.Path().c_str()) == INVALID_FILE_ATTRIBUTES) return 8;
    const auto before = control_center::HashFile(occupied.Path());
    result = control_center::WriteNewVerified(occupied, "overwrite forbidden");
    const auto after = control_center::HashFile(occupied.Path());
    if (result.error != control_center::WriteNewError::Open || !before || !after || before.identity.sha256 != after.identity.sha256) return 9;
    if (control_center::WriteNewVerified(invalid, "x").error != control_center::WriteNewError::InvalidPath) return 10;
    std::puts("write new: readback, short write, corruption, flush, cleanup and occupied cases passed");
    return 0;
}
