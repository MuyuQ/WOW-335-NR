#pragma once
#include <windows.h>
#include <atomic>

namespace nr_shutdown {
// Admission and rundown share the same lock, so closing cannot miss an entrant.
// Reservations also cover queued work which has not started executing yet.
class Admission {
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::atomic<bool> closing_{false};
    std::atomic<unsigned> active_{0};
public:
    bool Enter() {
        AcquireSRWLockShared(&lock_);
        const bool admitted = !closing_.load(std::memory_order_relaxed);
        if (admitted) active_.fetch_add(1, std::memory_order_relaxed);
        ReleaseSRWLockShared(&lock_);
        return admitted;
    }
    void Leave() { active_.fetch_sub(1, std::memory_order_release); }
    void Close() {
        AcquireSRWLockExclusive(&lock_);
        closing_.store(true, std::memory_order_release);
        ReleaseSRWLockExclusive(&lock_);
    }
    bool Closing() const { return closing_.load(std::memory_order_acquire); }
    unsigned Active() const { return active_.load(std::memory_order_acquire); }
};
class Lease {
    Admission *owner_;
public:
    explicit Lease(Admission &owner) : owner_(owner.Enter() ? &owner : nullptr) {}
    ~Lease() { if (owner_) owner_->Leave(); }
    explicit operator bool() const { return owner_ != nullptr; }
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
};
struct Reservation {
    Admission &owner;
    ~Reservation() { owner.Leave(); }
};
class Deadline {
    ULONGLONG end_;
public:
    explicit Deadline(DWORD timeout) : end_(GetTickCount64() + timeout) {}
    DWORD Remaining() const {
        const auto now = GetTickCount64();
        return now >= end_ ? 0 : static_cast<DWORD>(end_ - now);
    }
};
inline bool WaitIdle(const Admission &admission, const Deadline &deadline, unsigned owned_entries = 0) {
    while (admission.Active() > owned_entries) {
        if (!deadline.Remaining()) return false;
        Sleep(1);
    }
    return true;
}
inline DWORD Join(HANDLE thread, const Deadline &deadline) {
    // No handles or events are closed here. The owner releases them only after
    // every join and GPU completion check has succeeded.
    if (!thread) return ERROR_SUCCESS;
    const DWORD result = WaitForSingleObject(thread, deadline.Remaining());
    if (result == WAIT_OBJECT_0) return ERROR_SUCCESS;
    if (result == WAIT_TIMEOUT) return ERROR_TIMEOUT;
    const DWORD error = GetLastError();
    return error ? error : ERROR_GEN_FAILURE;
}
}
