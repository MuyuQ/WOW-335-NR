#include "../native/control_center/file_identity.hpp"
#include <cstdio>
#include <string>

static std::string Hex(const std::array<std::uint8_t, 32>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) { result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]); }
    return result;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring base = std::wstring(argv[1]) + L"\\";
    const std::wstring test = base + L"哈希 sample.bin";
    HANDLE file = CreateFileW(test.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 2;
    DWORD written = 0;
    if (!WriteFile(file, "abc", 3, &written, nullptr) || written != 3) { CloseHandle(file); return 3; }
    if (control_center::HashFile(test).error != control_center::HashError::Open) { CloseHandle(file); return 4; }
    CloseHandle(file);
    auto result = control_center::HashFile(test);
    if (!result || result.identity.size != 3 ||
        Hex(result.identity.sha256) != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") return 5;
    if (control_center::HashFile(base + L"missing.bin").error != control_center::HashError::Open) return 6;
    const auto directory = control_center::HashFile(argv[1]);
    if (directory) return 7;
    const std::wstring empty = base + L"empty.bin";
    file = CreateFileW(empty.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 8;
    CloseHandle(file);
    result = control_center::HashFile(empty);
    if (!result || result.identity.size != 0 ||
        Hex(result.identity.sha256) != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") return 9;
    const std::wstring large = base + L"large.bin";
    file = CreateFileW(large.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 10;
    const std::string content(65537, 'x');
    if (!WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) || written != content.size()) {
        CloseHandle(file); return 11;
    }
    CloseHandle(file);
    result = control_center::HashFile(large);
    if (!result || result.identity.size != content.size() ||
        Hex(result.identity.sha256) != "1abe08ebecf1c18cab71f6fe28aaddf20268f85bad78bb9a72f88ca47c874662") return 12;
    std::puts("file identity: known digest, empty, multi-block, locked, missing and directory cases passed");
    return 0;
}
