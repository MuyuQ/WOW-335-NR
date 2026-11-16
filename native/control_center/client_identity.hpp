#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <string>
#include <vector>
#include "guarded_path.hpp"

namespace control_center {
enum class ClientError { None, InvalidRoot, ProtectedRoot, MissingOrUnreadable, UnsupportedFile, InvalidPe, NotX86 };
struct ClientResult {
    ClientError error = ClientError::None;
    DWORD win32Error = ERROR_SUCCESS;
    DWORD volume = 0, indexHigh = 0, indexLow = 0;
    std::wstring root;
    explicit operator bool() const { return error == ClientError::None; }
};
inline bool WithinRoot(const std::wstring& path, const std::wstring& root) {
    if (root.empty() || path.size() < root.size()) return false;
    if (CompareStringOrdinal(path.c_str(), static_cast<int>(root.size()), root.c_str(),
                             static_cast<int>(root.size()), TRUE) != CSTR_EQUAL) return false;
    return path.size() == root.size() || root.back() == L'\\' || path[root.size()] == L'\\';
}

// A snapshot used for selection and display. Before a write operation callers
// must repeat this check while retaining the relevant directory/file guards.
inline ClientResult ValidateClient(const std::wstring& root, const std::vector<std::wstring>& protectedRoots) {
    ClientResult result;
    if (!CanonicalLocalPath(root, result.root)) { result.error = ClientError::InvalidRoot; return result; }
    for (const auto& protectedRoot : protectedRoots) {
        std::wstring normalized;
        if (!CanonicalLocalPath(protectedRoot, normalized)) { result.error = ClientError::InvalidRoot; return result; }
        if (WithinRoot(result.root, normalized)) { result.error = ClientError::ProtectedRoot; return result; }
    }
    GuardedPath guard;
    if (!guard.Open(result.root, L"wow.exe")) {
        result.error = ClientError::InvalidRoot; result.win32Error = guard.Error(); return result;
    }
    HANDLE file = CreateFileW(guard.Path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.error = ClientError::MissingOrUnreadable; result.win32Error = GetLastError(); return result;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) {
        result.error = ClientError::MissingOrUnreadable; result.win32Error = GetLastError(); CloseHandle(file); return result;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || GetFileType(file) != FILE_TYPE_DISK) {
        result.error = ClientError::UnsupportedFile; CloseHandle(file); return result;
    }
    LARGE_INTEGER size{};
    IMAGE_DOS_HEADER dos{};
    DWORD count = 0;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < static_cast<LONGLONG>(sizeof(dos)) ||
        !ReadFile(file, &dos, sizeof(dos), &count, nullptr) || count != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<LONGLONG>(dos.e_lfanew) > size.QuadPart - 6) {
        result.error = ClientError::InvalidPe; CloseHandle(file); return result;
    }
    LARGE_INTEGER offset{}; offset.QuadPart = dos.e_lfanew;
    unsigned char header[6]{};
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) ||
        !ReadFile(file, header, sizeof(header), &count, nullptr) || count != sizeof(header) ||
        header[0] != 'P' || header[1] != 'E' || header[2] != 0 || header[3] != 0) {
        result.error = ClientError::InvalidPe; CloseHandle(file); return result;
    }
    const WORD machine = static_cast<WORD>(header[4] | (static_cast<WORD>(header[5]) << 8));
    if (machine != IMAGE_FILE_MACHINE_I386) result.error = ClientError::NotX86;
    else { result.volume = info.dwVolumeSerialNumber; result.indexHigh = info.nFileIndexHigh; result.indexLow = info.nFileIndexLow; }
    CloseHandle(file);
    return result;
}

inline bool IsBlockingProcessName(const wchar_t* name) {
    return _wcsicmp(name, L"wow.exe") == 0 || _wcsicmp(name, L"wowclassic.exe") == 0 ||
           _wcsicmp(name, L"patchmenu.exe") == 0 || _wcsicmp(name, L"AIO DLSS5 32-bit Wrapper.exe") == 0;
}
enum class ProcessState { Stopped, Running, Unknown };
struct ProcessCheck { ProcessState state = ProcessState::Unknown; DWORD win32Error = ERROR_SUCCESS; std::wstring name; };
// Match the old installer's global stop requirement. Enumeration failure must
// not be interpreted as proof that every relevant process has stopped.
inline ProcessCheck CheckBlockingProcesses() {
    ProcessCheck result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { result.win32Error = GetLastError(); return result; }
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        result.win32Error = GetLastError(); CloseHandle(snapshot); return result;
    }
    do {
        if (IsBlockingProcessName(entry.szExeFile)) {
            result.state = ProcessState::Running; result.name = entry.szExeFile; CloseHandle(snapshot); return result;
        }
    } while (Process32NextW(snapshot, &entry));
    const DWORD error = GetLastError();
    CloseHandle(snapshot);
    if (error == ERROR_NO_MORE_FILES) result.state = ProcessState::Stopped;
    else result.win32Error = error;
    return result;
}
}
