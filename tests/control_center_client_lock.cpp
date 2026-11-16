#include "../native/control_center/client_lock.hpp"
#include <cstdio>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1];
    control_center::ClientLock first;
    if (!first.Acquire(root) || !first.IsFor(root) || first.IsFor(root + L"\\real")) return 2;
    bool busy = false, crossThreadRejected = false;
    std::thread contender([&] {
        crossThreadRejected = !first.IsFor(root);
        control_center::ClientLock second;
        busy = !second.Acquire(root) && second.Error() == control_center::ClientLockError::Busy;
    });
    contender.join();
    if (!busy || !crossThreadRejected) return 3;
    first.Release();
    bool acquired = false;
    std::thread successor([&] {
        control_center::ClientLock next;
        acquired = next.Acquire(root) && next.IsFor(root);
    });
    successor.join();
    if (!acquired) return 4;
    control_center::ClientLock* abandoned = nullptr;
    std::thread owner([&] {
        auto candidate = std::make_unique<control_center::ClientLock>();
        if (candidate->Acquire(root)) abandoned = candidate.release();
    });
    owner.join();
    if (!abandoned) return 5;
    control_center::ClientLock recovery;
    const bool rejected = !recovery.Acquire(root) && recovery.Error() == control_center::ClientLockError::Abandoned;
    delete abandoned;
    if (!rejected) return 6;
    if (!recovery.Acquire(root)) return 7;
    std::puts("client lock: contention, release and abandoned-operation detection passed");
    return 0;
}
