#include "../native/control_center/delete_verified.hpp"
#include "../native/control_center/write_new.hpp"
#include <cstdio>

static BOOL WINAPI DenyDelete(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1];
    control_center::GuardedPath file, denied, missing, invalid;
    if (!file.Open(root, L"real/delete-test.bin") || !denied.Open(root, L"real/delete-denied.bin") ||
        !missing.Open(root, L"real/delete-missing.bin")) return 2;
    const auto created = control_center::WriteNewVerified(file, "owned");
    const auto protectedFile = control_center::WriteNewVerified(denied, "user");
    if (!created || !protectedFile) return 3;
    auto wrong = created.identity.sha256; wrong[0] ^= 1;
    auto result = control_center::DeleteVerified(file, wrong);
    if (result.error != control_center::DeleteError::Mismatch || GetFileAttributesW(file.Path().c_str()) == INVALID_FILE_ATTRIBUTES) return 4;
    auto wrongId = created.identity; ++wrongId.indexLow;
    result = control_center::DeleteVerified(file, created.identity.sha256, &wrongId);
    if (result.error != control_center::DeleteError::Mismatch || GetFileAttributesW(file.Path().c_str()) == INVALID_FILE_ATTRIBUTES) return 5;
    control_center::CopyIo io; io.dispose = &DenyDelete;
    result = control_center::DeleteVerified(denied, protectedFile.identity.sha256, nullptr, io);
    const auto after = control_center::HashFile(denied.Path());
    if (result.error != control_center::DeleteError::Delete || result.win32Error != ERROR_ACCESS_DENIED ||
        !after || after.identity.sha256 != protectedFile.identity.sha256) return 6;
    result = control_center::DeleteVerified(file, created.identity.sha256, &created.identity);
    if (!result || !result.dispositionSet || GetFileAttributesW(file.Path().c_str()) != INVALID_FILE_ATTRIBUTES) return 7;
    if (control_center::DeleteVerified(missing, created.identity.sha256).error != control_center::DeleteError::Open) return 8;
    if (control_center::DeleteVerified(invalid, created.identity.sha256).error != control_center::DeleteError::InvalidPath) return 9;
    std::puts("verified delete: hash, identity, denied delete and preservation passed");
    return 0;
}
