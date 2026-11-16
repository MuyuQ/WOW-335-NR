#include "../native/control_center/guarded_path.hpp"
#include "../native/control_center/file_identity.hpp"
#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1];
    const std::wstring real = root + L"\\real";
    const std::wstring renamed = root + L"\\renamed";
    const std::wstring file = real + L"\\sample.bin";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 2;
    DWORD written = 0;
    if (!WriteFile(h, "abc", 3, &written, nullptr) || written != 3) { CloseHandle(h); return 3; }
    CloseHandle(h);
    {
        control_center::GuardedPath guard;
        if (!guard.Open(root, L"real/sample.bin") || !control_center::HashFile(guard.Path())) return 4;
        if (MoveFileExW(real.c_str(), renamed.c_str(), 0)) return 5;
        if (GetLastError() != ERROR_SHARING_VIOLATION && GetLastError() != ERROR_ACCESS_DENIED) return 6;
    }
    if (!MoveFileExW(real.c_str(), renamed.c_str(), 0)) return 7;
    if (!MoveFileExW(renamed.c_str(), real.c_str(), 0)) return 8;
    control_center::GuardedPath guard;
    if (guard.Open(root, L"link/sample.bin")) return 9;
    if (guard.Open(root + L"\\link", L"sample.bin")) return 10;
    if (guard.Open(root, L"../escape")) return 11;
    if (guard.Open(root, L"missing/sample.bin")) return 12;
    if (guard.Open(L"\\\\server\\share", L"sample.bin")) return 13;
    if (guard.Open(root, L"real/sample.bin/other")) return 14;
    if (!guard.Open(root, L"real/sample.bin")) return 15;
    const std::wstring hiddenNull = root + std::wstring(L"\0hidden", 7);
    if (guard.Open(hiddenNull, L"sample.bin")) return 16;
    if (!guard.Open(root, L"created/deep/file.bin", true) || guard.Error() != ERROR_SUCCESS) return 17;
    if (GetFileAttributesW((root + L"\\created\\deep").c_str()) == INVALID_FILE_ATTRIBUTES) return 18;
    if (guard.Open(root, L"link/new/file.bin", true)) return 19;
    if (guard.Open(root + L"\\absent-root", L"file.bin", true)) return 20;
    std::puts("guarded path: junction, traversal, missing parent, file parent and rename lock passed");
    return 0;
}
