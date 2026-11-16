#pragma once

#include "copy_verified.hpp"

namespace control_center {
enum class DeleteError { None, InvalidPath, Open, Hash, Mismatch, Delete, Verify };
struct DeleteResult {
    DeleteError error = DeleteError::None;
    DWORD win32Error = ERROR_SUCCESS;
    NTSTATUS cryptoStatus = 0;
    bool dispositionSet = false;
    explicit operator bool() const { return error == DeleteError::None; }
};

// Request deletion of the exact opened file only after verifying its bytes and
// optional identity. Never follow the final reparse point or delete a directory.
inline DeleteResult DeleteVerified(const GuardedPath& path, const std::array<std::uint8_t, 32>& expected,
                                   const FileIdentity* expectedIdentity = nullptr, const CopyIo& io = CopyIo{}) {
    DeleteResult result;
    if (path.Path().empty()) { result.error = DeleteError::InvalidPath; return result; }
    HANDLE file = CreateFileW(path.Path().c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { result.error = DeleteError::Open; result.win32Error = GetLastError(); return result; }
    const auto hash = HashOpenFile(file);
    if (!hash) {
        result.error = DeleteError::Hash; result.win32Error = hash.win32Error; result.cryptoStatus = hash.cryptoStatus;
        CloseHandle(file); return result;
    }
    if (hash.identity.sha256 != expected || (expectedIdentity &&
        (hash.identity.volume != expectedIdentity->volume || hash.identity.indexHigh != expectedIdentity->indexHigh ||
         hash.identity.indexLow != expectedIdentity->indexLow))) {
        result.error = DeleteError::Mismatch; CloseHandle(file); return result;
    }
    FILE_DISPOSITION_INFO disposition{}; disposition.DeleteFile = TRUE;
    if (!io.dispose(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
        result.error = DeleteError::Delete; result.win32Error = GetLastError(); CloseHandle(file); return result;
    }
    result.dispositionSet = true;
    CloseHandle(file);
    if (GetFileAttributesW(path.Path().c_str()) != INVALID_FILE_ATTRIBUTES) {
        result.error = DeleteError::Verify; return result;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        result.error = DeleteError::Verify; result.win32Error = error;
    }
    return result;
}
}
