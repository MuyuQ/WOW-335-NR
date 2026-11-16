#pragma once

#include <windows.h>
#include <cstddef>
#include <cstring>
#include <vector>
#include "file_identity.hpp"
#include "guarded_path.hpp"

namespace control_center {
enum class RenameError { None, InvalidPath, Open, Hash, Mismatch, Rename, Verify };
struct RenameResult {
    RenameError error = RenameError::None;
    DWORD win32Error = ERROR_SUCCESS;
    NTSTATUS cryptoStatus = 0;
    FileIdentity identity{};
    bool renamed = false;
    explicit operator bool() const { return error == RenameError::None; }
};

// Rename the exact opened file, never replacing an existing target. Both
// directory guards must remain alive for the whole operation. Cross-volume
// moves fail rather than falling back to an unverified copy-and-delete.
inline RenameResult RenameVerifiedNew(const GuardedPath& source, const GuardedPath& target,
                                      const std::array<std::uint8_t, 32>& expected,
                                      const FileIdentity* expectedIdentity = nullptr) {
    RenameResult result;
    if (source.Path().empty() || target.Path().empty()) { result.error = RenameError::InvalidPath; return result; }
    HANDLE file = CreateFileW(source.Path().c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { result.error = RenameError::Open; result.win32Error = GetLastError(); return result; }
    const auto before = HashOpenFile(file);
    if (!before) {
        result.error = RenameError::Hash; result.win32Error = before.win32Error; result.cryptoStatus = before.cryptoStatus;
        CloseHandle(file); return result;
    }
    if (before.identity.sha256 != expected || (expectedIdentity &&
        (before.identity.volume != expectedIdentity->volume || before.identity.indexHigh != expectedIdentity->indexHigh ||
         before.identity.indexLow != expectedIdentity->indexLow))) {
        result.error = RenameError::Mismatch; CloseHandle(file); return result;
    }
    const size_t bytes = target.Path().size() * sizeof(wchar_t);
    if (bytes > MAXDWORD - offsetof(FILE_RENAME_INFO, FileName) - sizeof(wchar_t)) {
        result.error = RenameError::InvalidPath; CloseHandle(file); return result;
    }
    // Windows may inspect the DOS path as a terminated string while translating
    // it. Reserve the terminator even though FileNameLength excludes it.
    const size_t total = offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t);
    std::vector<std::max_align_t> storage((total + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    std::memset(info, 0, total);
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(info->FileName, target.Path().data(), bytes);
    if (!SetFileInformationByHandle(file, FileRenameInfo, info, static_cast<DWORD>(total))) {
        result.error = RenameError::Rename; result.win32Error = GetLastError(); CloseHandle(file); return result;
    }
    result.renamed = true;
    const auto after = HashOpenFile(file);
    if (!after || after.identity.sha256 != expected || after.identity.volume != before.identity.volume ||
        after.identity.indexHigh != before.identity.indexHigh || after.identity.indexLow != before.identity.indexLow) {
        result.error = RenameError::Verify; result.win32Error = after.win32Error; result.cryptoStatus = after.cryptoStatus;
    } else result.identity = after.identity;
    CloseHandle(file);
    return result;
}
}
