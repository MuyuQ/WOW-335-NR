// R5 跨工具互斥探针：直接使用原生 control_center::ClientLock 获取与原生控制中心
// 完全一致的命名互斥量，并把实际互斥量名与客户端目录身份打印给 PowerShell 测试。
//
// 用法：
//   client-lock-probe.exe acquire <client-dir>   获取后立即释放，打印结果
//   client-lock-probe.exe hold <client-dir>      获取后保持进程存活，直到 stdin 关闭
//   client-lock-probe.exe abandon <client-dir>   获取后直接 ExitProcess（不释放，模拟异常结束）
//   client-lock-probe.exe keeper <client-dir>    只打开命名互斥量（不拥有），保持到 stdin 关闭
//
// keeper 用于让被遗弃的互斥量对象继续存活：若遗弃后没有任何打开句柄，内核会销毁
// 该对象，新的 CreateMutex 得到全新对象、看不到 WAIT_ABANDONED。真实维护场景里
// 由常驻进程持有句柄，这里用 keeper 精确复现该状态。
//
// 输出（stdout，ASCII）：
//   NAME=<mutex name>
//   VOL=<8 hex>  INDEXHIGH=<8 hex>  INDEXLOW=<8 hex>
//   STATUS=Acquired|Keeper|Busy|Abandoned|InvalidRoot|Create|Wait
// hold/keeper 模式在 STATUS 行之后等待 stdin EOF，随后正常释放并退出。
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "../native/control_center/client_lock.hpp"

namespace {

const char* ErrorName(control_center::ClientLockError error) {
    switch (error) {
        case control_center::ClientLockError::None: return "None";
        case control_center::ClientLockError::InvalidRoot: return "InvalidRoot";
        case control_center::ClientLockError::Create: return "Create";
        case control_center::ClientLockError::Busy: return "Busy";
        case control_center::ClientLockError::Abandoned: return "Abandoned";
        case control_center::ClientLockError::Wait: return "Wait";
    }
    return "Unknown";
}

bool PrintIdentity(const BY_HANDLE_FILE_INFORMATION& identity) {
    wchar_t name[96]{};
    if (swprintf_s(name, L"Local\\WOW335NR-Client-%08lX-%08lX-%08lX", identity.dwVolumeSerialNumber,
                   identity.nFileIndexHigh, identity.nFileIndexLow) <= 0) {
        std::printf("STATUS=InvalidRoot\n");
        std::fflush(stdout);
        return false;
    }
    std::printf("NAME=%ls\n", name);
    std::printf("VOL=%08lX\n", identity.dwVolumeSerialNumber);
    std::printf("INDEXHIGH=%08lX\n", identity.nFileIndexHigh);
    std::printf("INDEXLOW=%08lX\n", identity.nFileIndexLow);
    return true;
}

void PrintStatus(bool acquired, const control_center::ClientLock& lock) {
    if (acquired) {
        BY_HANDLE_FILE_INFORMATION identity{};
        if (lock.Identity(identity)) { PrintIdentity(identity); }
        std::printf("STATUS=Acquired\n");
    } else {
        std::printf("STATUS=%s\n", ErrorName(lock.Error()));
    }
    std::fflush(stdout);
}

// 保留命名互斥量对象（不获取所有权），让后续 WAIT_ABANDONED 可被观察。
int RunKeeper(const std::wstring& root) {
    control_center::GuardedPath guard;
    if (!guard.Open(root, L"__lock_root__")) { std::printf("STATUS=InvalidRoot\n"); return 1; }
    BY_HANDLE_FILE_INFORMATION identity{};
    if (!guard.ParentIdentity(identity)) { std::printf("STATUS=InvalidRoot\n"); return 1; }
    wchar_t name[96]{};
    if (swprintf_s(name, L"Local\\WOW335NR-Client-%08lX-%08lX-%08lX", identity.dwVolumeSerialNumber,
                   identity.nFileIndexHigh, identity.nFileIndexLow) <= 0) {
        std::printf("STATUS=InvalidRoot\n");
        return 1;
    }
    HANDLE handle = CreateMutexW(nullptr, FALSE, name);
    if (!handle) { std::printf("STATUS=Create\n"); std::fflush(stdout); return 1; }
    PrintIdentity(identity);
    std::printf("STATUS=Keeper\n");
    std::fflush(stdout);
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) { }
    CloseHandle(handle);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("STATUS=BadArguments\n");
        return 2;
    }
    const std::wstring mode = argv[1];
    const std::wstring root = argv[2];
    if (mode == L"keeper") { return RunKeeper(root); }
    control_center::ClientLock lock;
    const bool acquired = lock.Acquire(root);
    PrintStatus(acquired, lock);
    if (mode == L"acquire") {
        return acquired ? 0 : 3;
    }
    if (mode == L"hold") {
        if (!acquired) { return 3; }
        // 保持锁，直到父进程关闭 stdin；不解析内容，只等 EOF。
        char buffer[256];
        while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) { }
        return 0;
    }
    if (mode == L"abandon") {
        if (!acquired) { return 3; }
        // 模拟维护进程异常结束：不调用 ~ClientLock，让进程终止时由内核遗弃互斥量。
        std::fflush(stdout);
        ::ExitProcess(0);
    }
    std::printf("STATUS=BadMode\n");
    return 2;
}
