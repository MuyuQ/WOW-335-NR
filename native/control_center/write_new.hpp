#pragma once

#include <algorithm>
#include <array>
#include <string>
#include "copy_verified.hpp"

namespace control_center {
enum class WriteNewError { None, InvalidPath, Crypto, Open, Write, Flush, Verify };
struct WriteNewResult {
    WriteNewError error = WriteNewError::None;
    DWORD win32Error = ERROR_SUCCESS, cleanupError = ERROR_SUCCESS;
    NTSTATUS cryptoStatus = 0;
    bool cleanupFailed = false;
    FileIdentity identity{};
    explicit operator bool() const { return error == WriteNewError::None; }
};
inline NTSTATUS HashBytes(const std::string& bytes, std::array<std::uint8_t, 32>& digest) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    size_t offset = 0;
    while (status >= 0 && offset < bytes.size()) {
        const auto count = static_cast<ULONG>((std::min)(bytes.size() - offset, static_cast<size_t>(65536)));
        status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data() + offset)), count, 0);
        offset += count;
    }
    if (status >= 0) status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return status;
}

// Create a new file, flush it and re-read it through the same handle. Existing
// paths are never replaced; failure cleanup only addresses the created handle.
inline WriteNewResult WriteNewVerified(const GuardedPath& path, const std::string& bytes,
                                       const CopyIo& io = CopyIo{}) {
    WriteNewResult result;
    if (path.Path().empty()) { result.error = WriteNewError::InvalidPath; return result; }
    std::array<std::uint8_t, 32> expected{};
    const NTSTATUS status = HashBytes(bytes, expected);
    if (status < 0) { result.error = WriteNewError::Crypto; result.cryptoStatus = status; return result; }
    HANDLE file = CreateFileW(path.Path().c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { result.error = WriteNewError::Open; result.win32Error = GetLastError(); return result; }
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = static_cast<DWORD>((std::min)(bytes.size() - offset, static_cast<size_t>(65536)));
        DWORD written = 0;
        if (!io.write(file, bytes.data() + offset, count, &written, nullptr)) {
            result.error = WriteNewError::Write; result.win32Error = GetLastError(); break;
        }
        if (!written || written > count) { result.error = WriteNewError::Write; result.win32Error = ERROR_WRITE_FAULT; break; }
        offset += written;
    }
    if (result.error == WriteNewError::None && !io.flush(file)) {
        result.error = WriteNewError::Flush; result.win32Error = GetLastError();
    }
    if (result.error == WriteNewError::None) {
        const auto check = HashOpenFile(file);
        if (!check || check.identity.sha256 != expected || check.identity.size != bytes.size()) {
            result.error = WriteNewError::Verify; result.win32Error = check.win32Error; result.cryptoStatus = check.cryptoStatus;
        } else result.identity = check.identity;
    }
    if (result.error != WriteNewError::None) {
        FILE_DISPOSITION_INFO disposition{}; disposition.DeleteFile = TRUE;
        if (!io.dispose(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
            result.cleanupFailed = true; result.cleanupError = GetLastError();
        }
    }
    CloseHandle(file);
    return result;
}
}
