#pragma once

#include <array>
#include "file_identity.hpp"
#include "guarded_path.hpp"

namespace control_center {
enum class CopyError { None, InvalidPath, SourceOpen, SourceHash, SourceMismatch,
                       TargetOpen, Read, Write, Flush, TargetHash, TargetMismatch };
struct CopyResult {
    CopyError error = CopyError::None;
    DWORD win32Error = ERROR_SUCCESS;
    NTSTATUS cryptoStatus = 0;
    bool cleanupFailed = false;
    DWORD cleanupError = ERROR_SUCCESS;
    FileIdentity source{}, target{};
    explicit operator bool() const { return error == CopyError::None; }
};
struct CopyIo {
    decltype(&::ReadFile) read = &::ReadFile;
    decltype(&::WriteFile) write = &::WriteFile;
    decltype(&::FlushFileBuffers) flush = &::FlushFileBuffers;
    decltype(&::SetFileInformationByHandle) dispose = &::SetFileInformationByHandle;
};

// The caller holds both guards for this entire call. The destination must not
// exist. On failure, delete only the file created by this call, using its open
// handle; if cleanup fails report that it needs inspection.
inline CopyResult CopyVerifiedNew(const GuardedPath& source, const GuardedPath& target,
                                  const std::array<std::uint8_t, 32>& expected,
                                  const CopyIo& io = CopyIo{}) {
    CopyResult result;
    if (source.Path().empty() || target.Path().empty()) { result.error = CopyError::InvalidPath; return result; }
    HANDLE in = CreateFileW(source.Path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        result.error = CopyError::SourceOpen; result.win32Error = GetLastError(); return result;
    }
    auto inputHash = HashOpenFile(in);
    if (!inputHash) {
        result.error = CopyError::SourceHash; result.win32Error = inputHash.win32Error;
        result.cryptoStatus = inputHash.cryptoStatus; CloseHandle(in); return result;
    }
    result.source = inputHash.identity;
    if (inputHash.identity.sha256 != expected) { result.error = CopyError::SourceMismatch; CloseHandle(in); return result; }
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(in, start, nullptr, FILE_BEGIN)) {
        result.error = CopyError::Read; result.win32Error = GetLastError(); CloseHandle(in); return result;
    }
    HANDLE out = CreateFileW(target.Path().c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                             nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        result.error = CopyError::TargetOpen; result.win32Error = GetLastError(); CloseHandle(in); return result;
    }
    std::array<unsigned char, 65536> buffer{};
    while (result.error == CopyError::None) {
        DWORD count = 0;
        if (!io.read(in, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            result.error = CopyError::Read; result.win32Error = GetLastError(); break;
        }
        if (!count) break;
        DWORD offset = 0;
        while (offset < count) {
            DWORD written = 0;
            if (!io.write(out, buffer.data() + offset, count - offset, &written, nullptr)) {
                result.error = CopyError::Write; result.win32Error = GetLastError(); break;
            }
            if (!written) { result.error = CopyError::Write; result.win32Error = ERROR_WRITE_FAULT; break; }
            offset += written;
        }
    }
    if (result.error == CopyError::None && !io.flush(out)) {
        result.error = CopyError::Flush; result.win32Error = GetLastError();
    }
    if (result.error == CopyError::None) {
        auto outputHash = HashOpenFile(out);
        if (!outputHash) { result.error = CopyError::TargetHash; result.win32Error = outputHash.win32Error; result.cryptoStatus = outputHash.cryptoStatus; }
        else if (outputHash.identity.sha256 != expected || outputHash.identity.size != result.source.size) result.error = CopyError::TargetMismatch;
        else result.target = outputHash.identity;
    }
    if (result.error != CopyError::None) {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        if (!io.dispose(out, FileDispositionInfo, &disposition, sizeof(disposition))) {
            result.cleanupFailed = true; result.cleanupError = GetLastError();
        }
    }
    CloseHandle(out);
    CloseHandle(in);
    return result;
}
}
