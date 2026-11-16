#pragma once

#include "client_record.hpp"
#include "install_preview.hpp"
#include "journal.hpp"

namespace control_center {
enum class CapturedRecordState { Absent, Present, Invalid, Unavailable };
struct CapturedClientRecord {
    CapturedRecordState state = CapturedRecordState::Unavailable;
    DWORD win32Error = ERROR_SUCCESS;
    std::string bytes;
    FileIdentity identity;
    ClientRecord record;
};

// Capture the existing record without following directory or file links.
// The caller must revalidate the returned identity before replacing the file.
inline CapturedClientRecord CaptureClientRecord(const std::wstring& clientRoot) {
    CapturedClientRecord result;
    GuardedPath path;
    if (!path.Open(clientRoot, L"host64/dlss5-install-record.json")) {
        if (MissingPathError(path.Error())) result.state = CapturedRecordState::Absent;
        else result.win32Error = path.Error();
        return result;
    }
    if (!ReadJournalBytes(path, result.bytes, result.win32Error)) {
        if (MissingPathError(result.win32Error)) result.state = CapturedRecordState::Absent;
        return result;
    }
    const auto actual = HashFile(path.Path());
    std::array<std::uint8_t, 32> fromBytes{};
    if (!actual || HashBytes(result.bytes, fromBytes) < 0 || actual.identity.sha256 != fromBytes) {
        result.win32Error = actual.win32Error; return result;
    }
    result.record = ReadClientRecord(result.bytes);
    if (!result.record) { result.state = CapturedRecordState::Invalid; return result; }
    result.identity = actual.identity;
    result.state = CapturedRecordState::Present;
    return result;
}
}
