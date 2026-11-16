#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <string>

namespace control_center {
enum class HashError { None, Open, Metadata, UnsupportedFile, Crypto, Read, Changed };
struct FileIdentity {
    std::array<std::uint8_t, 32> sha256{};
    DWORD volume = 0, indexHigh = 0, indexLow = 0;
    std::uint64_t size = 0;
};
struct HashResult {
    HashError error = HashError::None;
    DWORD win32Error = ERROR_SUCCESS;
    NTSTATUS cryptoStatus = 0;
    FileIdentity identity{};
    explicit operator bool() const { return error == HashError::None; }
};

// Hash an already open synchronous disk-file handle from offset zero. The
// caller owns the handle; on return its file position is unspecified.
inline HashResult HashOpenFile(HANDLE file) {
    HashResult result;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) {
        result.error = HashError::Metadata; result.win32Error = GetLastError();
        return result;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        GetFileType(file) != FILE_TYPE_DISK) {
        result.error = HashError::UnsupportedFile; return result;
    }
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) {
        result.error = HashError::Read; result.win32Error = GetLastError(); return result;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) { result.error = HashError::Crypto; result.cryptoStatus = status; }
    std::array<unsigned char, 65536> buffer{};
    std::uint64_t totalRead = 0;
    while (result.error == HashError::None) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            result.error = HashError::Read; result.win32Error = GetLastError(); break;
        }
        if (count == 0) break;
        totalRead += count;
        status = BCryptHashData(hash, buffer.data(), count, 0);
        if (status < 0) { result.error = HashError::Crypto; result.cryptoStatus = status; }
    }
    if (result.error == HashError::None) {
        status = BCryptFinishHash(hash, result.identity.sha256.data(),
                                  static_cast<ULONG>(result.identity.sha256.size()), 0);
        if (status < 0) { result.error = HashError::Crypto; result.cryptoStatus = status; }
        else {
            BY_HANDLE_FILE_INFORMATION after{};
            if (!GetFileInformationByHandle(file, &after)) {
                result.error = HashError::Metadata; result.win32Error = GetLastError();
            } else if (totalRead != ((static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) ||
                       info.nFileSizeHigh != after.nFileSizeHigh || info.nFileSizeLow != after.nFileSizeLow ||
                       info.ftLastWriteTime.dwHighDateTime != after.ftLastWriteTime.dwHighDateTime ||
                       info.ftLastWriteTime.dwLowDateTime != after.ftLastWriteTime.dwLowDateTime) {
                result.error = HashError::Changed;
            }
            result.identity.volume = info.dwVolumeSerialNumber;
            result.identity.indexHigh = info.nFileIndexHigh;
            result.identity.indexLow = info.nFileIndexLow;
            result.identity.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

// Parent components must be pinned separately. The returned identity is a
// snapshot and must be revalidated before any later path-based mutation.
inline HashResult HashFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        HashResult result; result.error = HashError::Open; result.win32Error = GetLastError(); return result;
    }
    auto result = HashOpenFile(file);
    CloseHandle(file);
    return result;
}
}
