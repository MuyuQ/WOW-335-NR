#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include "member_path.hpp"

namespace control_center {

inline bool CanonicalLocalPath(const std::wstring& input, std::wstring& output) {
    if (input.size() < 3 || !((input[0] >= L'A' && input[0] <= L'Z') || (input[0] >= L'a' && input[0] <= L'z')) ||
        input[1] != L':' || (input[2] != L'\\' && input[2] != L'/')) return false;
    std::wstring part;
    for (size_t i = 3; i <= input.size(); ++i) {
        const wchar_t c = i == input.size() ? L'\\' : input[i];
        if (c == L'\\' || c == L'/') {
            if (part.empty()) {
                if (i != input.size() || (i > 3 && input[i - 1] != L'\\' && input[i - 1] != L'/')) return false;
            } else if (!ValidPathComponent(part)) return false;
            part.clear();
        } else part.push_back(c);
    }
    const DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (!needed) return false;
    std::vector<wchar_t> buffer(needed);
    const DWORD count = GetFullPathNameW(input.c_str(), needed, buffer.data(), nullptr);
    if (!count || count >= needed) return false;
    output.assign(buffer.data(), count);
    while (output.size() > 3 && output.back() == L'\\') output.pop_back();
    return output.size() >= 3 && output[1] == L':' && output[2] == L'\\';
}

// Keeps each existing ancestor open without FILE_SHARE_DELETE so that a later
// path-based operation cannot be redirected by renaming an opened directory.
// With createParents enabled it may create missing member-parent directories;
// the root must already exist. The final file itself is not validated here.
class GuardedPath {
    std::vector<HANDLE> directories_;
    std::wstring path_;
    DWORD error_ = ERROR_SUCCESS;
    void Reset() {
        for (auto it = directories_.rbegin(); it != directories_.rend(); ++it) CloseHandle(*it);
        directories_.clear();
        path_.clear();
    }
    bool PinDirectory(const std::wstring& path) {
        HANDLE h = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) { error_ = GetLastError(); return false; }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h, &info)) {
            error_ = GetLastError(); CloseHandle(h); return false;
        }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || GetFileType(h) != FILE_TYPE_DISK) {
            error_ = ERROR_CANT_ACCESS_FILE; CloseHandle(h); return false;
        }
        directories_.push_back(h);
        return true;
    }
public:
    GuardedPath() = default;
    GuardedPath(const GuardedPath&) = delete;
    GuardedPath& operator=(const GuardedPath&) = delete;
    ~GuardedPath() { Reset(); }
    DWORD Error() const { return error_; }
    const std::wstring& Path() const { return path_; }
    bool ParentIdentity(BY_HANDLE_FILE_INFORMATION& info) const {
        return !path_.empty() && !directories_.empty() && GetFileInformationByHandle(directories_.back(), &info) != FALSE;
    }

    bool Open(const std::wstring& root, const std::wstring& member, bool createParents = false) {
        Reset(); error_ = ERROR_SUCCESS;
        std::wstring full;
        if (!ValidMemberPath(member) || !CanonicalLocalPath(root, full)) { error_ = ERROR_INVALID_NAME; return false; }
        std::wstring current = full.substr(0, 3);
        if (!PinDirectory(current)) { Reset(); return false; }
        size_t at = 3;
        while (at < full.size()) {
            const size_t end = full.find(L'\\', at);
            const std::wstring part = full.substr(at, end == std::wstring::npos ? end : end - at);
            if (!part.empty()) {
                if (current.back() != L'\\') current += L'\\';
                current += part;
                if (!PinDirectory(current)) { Reset(); return false; }
            }
            if (end == std::wstring::npos) break;
            at = end + 1;
        }
        std::wstring normalized = member;
        for (auto& c : normalized) if (c == L'/') c = L'\\';
        at = 0;
        for (;;) {
            const size_t end = normalized.find(L'\\', at);
            if (end == std::wstring::npos) break;
            if (current.back() != L'\\') current += L'\\';
            current += normalized.substr(at, end - at);
            if (!PinDirectory(current)) {
                if (!createParents || (error_ != ERROR_FILE_NOT_FOUND && error_ != ERROR_PATH_NOT_FOUND)) {
                    Reset(); return false;
                }
                if (!CreateDirectoryW(current.c_str(), nullptr)) {
                    const DWORD createError = GetLastError();
                    if (createError != ERROR_ALREADY_EXISTS) { error_ = createError; Reset(); return false; }
                }
                if (!PinDirectory(current)) { Reset(); return false; }
            }
            at = end + 1;
        }
        if (current.back() != L'\\') current += L'\\';
        path_ = current + normalized.substr(at);
        error_ = ERROR_SUCCESS;
        return true;
    }
};
}
