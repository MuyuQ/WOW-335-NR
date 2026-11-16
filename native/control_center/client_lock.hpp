#pragma once

#include <windows.h>
#include <cwchar>
#include <memory>
#include <string>
#include "guarded_path.hpp"

namespace control_center {
enum class ClientLockError { None, InvalidRoot, Create, Busy, Abandoned, Wait };
class ClientLock {
    HANDLE mutex_ = nullptr;
    DWORD ownerThread_ = 0;
    std::unique_ptr<GuardedPath> rootGuard_;
    BY_HANDLE_FILE_INFORMATION identity_{};
    ClientLockError error_ = ClientLockError::None;
    DWORD win32Error_ = ERROR_SUCCESS;
public:
    ClientLock() = default;
    ClientLock(const ClientLock&) = delete;
    ClientLock& operator=(const ClientLock&) = delete;
    ~ClientLock() { Release(); }
    void Release() {
        if (mutex_) { ReleaseMutex(mutex_); CloseHandle(mutex_); mutex_ = nullptr; }
        rootGuard_.reset();
        identity_ = {};
        ownerThread_ = 0;
    }
    ClientLockError Error() const { return error_; }
    DWORD Win32Error() const { return win32Error_; }
    bool Identity(BY_HANDLE_FILE_INFORMATION& output) const {
        if (!mutex_ || ownerThread_ != GetCurrentThreadId()) return false;
        output = identity_;
        return true;
    }
    bool IsFor(const std::wstring& root) const {
        GuardedPath guard;
        BY_HANDLE_FILE_INFORMATION info{};
        return mutex_ && ownerThread_ == GetCurrentThreadId() && guard.Open(root, L"__lock_root__") && guard.ParentIdentity(info) &&
            info.dwVolumeSerialNumber == identity_.dwVolumeSerialNumber &&
            info.nFileIndexHigh == identity_.nFileIndexHigh && info.nFileIndexLow == identity_.nFileIndexLow;
    }
    // A lock must remain on the thread that acquired it. Abandoned operations
    // require recovery, not a fresh mutation on this acquisition attempt.
    bool Acquire(const std::wstring& root) {
        Release(); error_ = ClientLockError::None; win32Error_ = ERROR_SUCCESS;
        auto guard = std::make_unique<GuardedPath>();
        if (!guard->Open(root, L"__lock_root__")) {
            error_ = ClientLockError::InvalidRoot; win32Error_ = guard->Error(); return false;
        }
        if (!guard->ParentIdentity(identity_)) { error_ = ClientLockError::InvalidRoot; win32Error_ = GetLastError(); return false; }
        wchar_t name[96]{};
        if (swprintf_s(name, L"Local\\WOW335NR-Client-%08lX-%08lX-%08lX", identity_.dwVolumeSerialNumber,
                       identity_.nFileIndexHigh, identity_.nFileIndexLow) < 0) {
            error_ = ClientLockError::InvalidRoot; return false;
        }
        HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
        if (!mutex) { error_ = ClientLockError::Create; win32Error_ = GetLastError(); return false; }
        const DWORD wait = WaitForSingleObject(mutex, 0);
        if (wait != WAIT_OBJECT_0) {
            if (wait == WAIT_ABANDONED) { ReleaseMutex(mutex); error_ = ClientLockError::Abandoned; }
            else if (wait == WAIT_TIMEOUT) error_ = ClientLockError::Busy;
            else { error_ = ClientLockError::Wait; win32Error_ = GetLastError(); }
            CloseHandle(mutex); return false;
        }
        mutex_ = mutex;
        ownerThread_ = GetCurrentThreadId();
        rootGuard_ = std::move(guard);
        return true;
    }
};
}
