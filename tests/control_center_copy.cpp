#include "../native/control_center/copy_verified.hpp"
#include <cstdio>
#include <string>

static BOOL WINAPI FailWrite(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED) {
    SetLastError(ERROR_DISK_FULL); return FALSE;
}
static BOOL WINAPI PartialWrite(HANDLE file, LPCVOID buffer, DWORD count, LPDWORD written, LPOVERLAPPED overlapped) {
    return WriteFile(file, buffer, count > 2 ? 2 : count, written, overlapped);
}
static BOOL WINAPI FailFlush(HANDLE) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
static BOOL WINAPI FailDispose(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD) {
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1];
    control_center::GuardedPath source, target, occupied, absent, invalid;
    if (!source.Open(root, L"real/sample.bin") || !target.Open(root, L"real/copy.bin") ||
        !occupied.Open(root, L"real/occupied.bin") || !absent.Open(root, L"real/absent.bin")) return 2;
    const auto identity = control_center::HashFile(source.Path());
    if (!identity) return 3;
    auto wrong = identity.identity.sha256;
    wrong[0] ^= 1;
    auto result = control_center::CopyVerifiedNew(source, target, wrong);
    if (result.error != control_center::CopyError::SourceMismatch || GetFileAttributesW(target.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 4;
    result = control_center::CopyVerifiedNew(source, target, identity.identity.sha256);
    if (!result || result.target.sha256 != identity.identity.sha256 || result.target.size != 3) return 5;
    result = control_center::CopyVerifiedNew(source, target, identity.identity.sha256);
    if (result.error != control_center::CopyError::TargetOpen || result.win32Error != ERROR_FILE_EXISTS) return 6;
    const auto copied = control_center::HashFile(target.Path());
    if (!copied || copied.identity.sha256 != identity.identity.sha256) return 7;
    HANDLE file = CreateFileW(occupied.Path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 8;
    DWORD written = 0;
    if (!WriteFile(file, "keep", 4, &written, nullptr) || written != 4) { CloseHandle(file); return 9; }
    CloseHandle(file);
    const auto before = control_center::HashFile(occupied.Path());
    result = control_center::CopyVerifiedNew(source, occupied, identity.identity.sha256);
    const auto after = control_center::HashFile(occupied.Path());
    if (result.error != control_center::CopyError::TargetOpen || !before || !after || before.identity.sha256 != after.identity.sha256) return 10;
    result = control_center::CopyVerifiedNew(absent, occupied, identity.identity.sha256);
    if (result.error != control_center::CopyError::SourceOpen) return 11;
    result = control_center::CopyVerifiedNew(invalid, target, identity.identity.sha256);
    if (result.error != control_center::CopyError::InvalidPath) return 12;
    control_center::GuardedPath partial, failedWrite, failedFlush, failedCleanup;
    if (!partial.Open(root, L"real/partial.bin") || !failedWrite.Open(root, L"real/write-fail.bin") ||
        !failedFlush.Open(root, L"real/flush-fail.bin") || !failedCleanup.Open(root, L"real/cleanup-fail.bin")) return 13;
    control_center::CopyIo io;
    io.write = &PartialWrite;
    result = control_center::CopyVerifiedNew(source, partial, identity.identity.sha256, io);
    if (!result || result.target.sha256 != identity.identity.sha256) return 14;
    io.write = &FailWrite;
    result = control_center::CopyVerifiedNew(source, failedWrite, identity.identity.sha256, io);
    if (result.error != control_center::CopyError::Write || result.win32Error != ERROR_DISK_FULL || result.cleanupFailed ||
        GetFileAttributesW(failedWrite.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 15;
    io = control_center::CopyIo{};
    io.flush = &FailFlush;
    result = control_center::CopyVerifiedNew(source, failedFlush, identity.identity.sha256, io);
    if (result.error != control_center::CopyError::Flush || result.cleanupFailed ||
        GetFileAttributesW(failedFlush.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 16;
    io.write = &FailWrite;
    io.dispose = &FailDispose;
    result = control_center::CopyVerifiedNew(source, failedCleanup, identity.identity.sha256, io);
    if (result.error != control_center::CopyError::Write || !result.cleanupFailed || result.cleanupError != ERROR_ACCESS_DENIED ||
        GetFileAttributesW(failedCleanup.Path().c_str()) == INVALID_FILE_ATTRIBUTES) return 17;
    if (control_center::HashFile(occupied.Path()).identity.sha256 != before.identity.sha256) return 18;
    std::puts("verified copy: success, mismatch, occupied, short write, disk full, flush and cleanup failure passed");
    return 0;
}
