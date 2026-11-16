#include "../native/control_center/config_wtf.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace control_center;

int gFailures = 0;

void Fail(const char* what) {
    std::fprintf(stderr, "config.wtf test failed: %s\n", what);
    ++gFailures;
}

void Check(bool condition, const char* what) {
    if (!condition) Fail(what);
}

std::wstring Join(const std::wstring& base, const wchar_t* tail) {
    if (base.empty()) return tail;
    std::wstring path = base;
    if (path.back() != L'\\') path.push_back(L'\\');
    path += tail;
    return path;
}

bool MakeDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteBytes(const std::wstring& path, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size();
    CloseHandle(file);
    return ok;
}

bool ReadBytes(const std::wstring& path, std::string& bytes) {
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) bytes.append(buffer, read);
    CloseHandle(file);
    return true;
}

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

size_t CountOccurrences(const std::string& text, const char* needle) {
    size_t count = 0;
    size_t position = 0;
    const size_t length = std::strlen(needle);
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += length;
    }
    return count;
}

std::wstring MakeClient(const std::wstring& root, const wchar_t* name, const std::string& configBytes,
                        bool withConfig = true) {
    const std::wstring client = Join(root, name);
    MakeDirectory(client);
    MakeDirectory(Join(client, L"WTF"));
    if (withConfig) WriteBytes(Join(client, L"WTF\\Config.wtf"), configBytes);
    return client;
}

void TestMissingFile(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"missing", {}, false);
    const auto cap = ReadConfigFpsCap(client);
    Check(!cap.exists && cap.front == -1 && cap.back == -1, "missing config must read as absent");
    const auto saved = SaveConfigFpsCap(client, 60, 30);
    Check(!saved && saved.error == ConfigWtfWriteError::MissingFile, "save without config must fail");
}

void TestAnsiLfPreservingBytes(const std::wstring& root) {
    // GBK comment bytes and LF endings: every non-digit byte must survive untouched.
    std::string original;
    original += "\xEF\xBB\xBF";  // deliberately a UTF-8 BOM followed by ANSI text
    original += "SET locale \"zhCN\"\r\n";
    original += "\xB7\xFE\xCE\xF1\xC6\xF7\xD7\xA2\xCA\xCD\r\n";  // GBK comment text
    original += "SET maxFPS 45\r\n";
    original += "SET maxFPSBK \"30\"\n";
    original += "SET gxResolution \"1920x1080\"\r\n";
    const std::wstring client = MakeClient(root, L"ansi", original);
    const auto cap = ReadConfigFpsCap(client);
    Check(cap.exists && cap.readable && cap.front == 45 && cap.back == 30, "initial caps must be parsed");

    const auto saved = SaveConfigFpsCap(client, 60, 120);
    Check(static_cast<bool>(saved), "save with quotes/lf must succeed");
    if (!saved) return;
    std::string updated;
    Check(ReadBytes(Join(client, L"WTF\\Config.wtf"), updated), "written config must be readable");
    Check(updated.size() > 3 && static_cast<unsigned char>(updated[0]) == 0xEF &&
              static_cast<unsigned char>(updated[1]) == 0xBB &&
              static_cast<unsigned char>(updated[2]) == 0xBF,
          "BOM must be preserved");
    Check(Contains(updated, "SET maxFPS 60\r\n"), "unquoted value must keep its style");
    Check(Contains(updated, "SET maxFPSBK \"120\"\n"), "quoted value must keep its style and LF ending");
    Check(Contains(updated, "SET gxResolution \"1920x1080\"\r\n"), "unrelated lines must be untouched");
    Check(Contains(updated, "\xB7\xFE\xCE\xF1\xC6\xF7\xD7\xA2\xCA\xCD\r\n"), "ANSI bytes must be preserved");
    Check(!Contains(updated, "\"60\""), "quoted style must not be invented for unquoted lines");

    std::string backup;
    Check(ReadBytes(saved.backupPath, backup), "backup must exist");
    Check(backup == original, "backup must contain the original bytes");
}

void TestAppendMissingKeys(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"append", "SET gxResolution \"1280x720\"");
    const auto saved = SaveConfigFpsCap(client, 0, 999);
    Check(static_cast<bool>(saved), "append save must succeed");
    if (!saved) return;
    std::string updated;
    Check(ReadBytes(Join(client, L"WTF\\Config.wtf"), updated), "appended config must be readable");
    Check(Contains(updated, "SET gxResolution \"1280x720\"\r\n"), "missing final newline must be repaired with CRLF");
    Check(Contains(updated, "SET maxFPS \"0\"\r\n"), "front cap must be appended with quotes and CRLF");
    Check(Contains(updated, "SET maxFPSBK \"999\"\r\n"), "back cap must be appended with quotes and CRLF");
    const auto cap = ReadConfigFpsCap(client);
    Check(cap.front == 0 && cap.back == 999, "appended caps must be read back");

    const auto emptyClient = MakeClient(root, L"empty", "");
    const auto emptySaved = SaveConfigFpsCap(emptyClient, 30, 20);
    Check(static_cast<bool>(emptySaved), "save into empty file must succeed");
    std::string emptyUpdated;
    ReadBytes(Join(emptyClient, L"WTF\\Config.wtf"), emptyUpdated);
    Check(emptyUpdated == "SET maxFPS \"30\"\r\nSET maxFPSBK \"20\"\r\n",
          "empty file must receive exactly the two appended lines");
}

void TestDuplicateKeysAndCaseInsensitivity(const std::wstring& root) {
    std::string original;
    original += "  set MAXFPS  30 \r\n";
    original += "SET MaxFPS \"40\"\r\n";
    const std::wstring client = MakeClient(root, L"duplicate", original);
    const auto cap = ReadConfigFpsCap(client);
    Check(cap.front == 30, "first matching key wins for reading");
    const auto saved = SaveConfigFpsCap(client, 55, 25);
    Check(static_cast<bool>(saved), "duplicate save must succeed");
    if (!saved) return;
    std::string updated;
    ReadBytes(Join(client, L"WTF\\Config.wtf"), updated);
    Check(CountOccurrences(updated, "55") == 2, "every duplicate line is rewritten");
    Check(Contains(updated, "  set MAXFPS  55 \r\n"), "leading whitespace and key case must be preserved");
    Check(Contains(updated, "SET MaxFPS \"55\"\r\n"), "quoted duplicate must keep quotes");
    Check(Contains(updated, "SET maxFPSBK \"25\"\r\n"), "missing back key is appended");
}

void TestRangeValidation(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"range", "SET maxFPS 30\r\n");
    const auto negative = SaveConfigFpsCap(client, -1, 30);
    const auto tooLarge = SaveConfigFpsCap(client, 30, 1000);
    Check(!negative && negative.error == ConfigWtfWriteError::InvalidRange, "negative front must be rejected");
    Check(!tooLarge && tooLarge.error == ConfigWtfWriteError::InvalidRange, "oversized back must be rejected");
    const auto cap = ReadConfigFpsCap(client);
    Check(cap.front == 30, "rejected saves must not touch the file");
}

void TestLockedFile(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"locked", "SET maxFPS 30\r\n");
    const std::wstring path = Join(client, L"WTF\\Config.wtf");
    HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "test must be able to open the config file");
    if (lock != INVALID_HANDLE_VALUE) {
        const auto saved = SaveConfigFpsCap(client, 60, 40);
        Check(!saved, "save must fail while another process holds the file");
        CloseHandle(lock);
    }
    const auto cap = ReadConfigFpsCap(client);
    Check(cap.front == 30, "failed save must not modify the file");
}

void TestBackupNamesDoNotCollide(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"backup", "SET maxFPS 30\r\nSET maxFPSBK 20\r\n");
    const auto first = SaveConfigFpsCap(client, 31, 21);
    const auto second = SaveConfigFpsCap(client, 32, 22);
    Check(static_cast<bool>(first) && static_cast<bool>(second), "two immediate saves must both succeed");
    if (!first || !second) return;
    Check(first.backupPath != second.backupPath, "backup names must not collide");
    Check(GetFileAttributesW(first.backupPath.c_str()) != INVALID_FILE_ATTRIBUTES, "first backup must exist");
    Check(GetFileAttributesW(second.backupPath.c_str()) != INVALID_FILE_ATTRIBUTES, "second backup must exist");
}

void TestGameRunningGate(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"game", "SET maxFPS 30\r\n");
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        Fail("cannot locate the system directory for the running-process check");
        return;
    }
    const std::wstring source = Join(systemDirectory, L"cmd.exe");
    const std::wstring fakeGame = Join(client, L"wow.exe");
    if (!CopyFileW(source.c_str(), fakeGame.c_str(), FALSE)) {
        Fail("cannot copy cmd.exe into the fixture");
        return;
    }
    Check(!IsClientGameRunning(client), "no game process must be reported before launch");
    std::wstring command = L"\"" + fakeGame + L"\" /c ping -n 20 127.0.0.1 >nul";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        Fail("cannot start the fake game process");
        return;
    }
    Sleep(400);
    const bool running = IsClientGameRunning(client);
    Check(running, "a process whose image path equals this client's wow.exe must count as running");
    const auto saved = SaveConfigFpsCap(client, 60, 40);
    Check(!saved && saved.error == ConfigWtfWriteError::GameRunning, "save must be refused while the game runs");
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Sleep(200);
    Check(!IsClientGameRunning(client), "terminated game process must no longer count as running");
    const auto after = SaveConfigFpsCap(client, 60, 40);
    Check(static_cast<bool>(after), "save must succeed after the game exits");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: control_center_config_wtf <output-root>\n");
        return 1;
    }
    const std::wstring root = Join(argv[1], L"config-wtf");
    MakeDirectory(root);
    TestMissingFile(root);
    TestAnsiLfPreservingBytes(root);
    TestAppendMissingKeys(root);
    TestDuplicateKeysAndCaseInsensitivity(root);
    TestRangeValidation(root);
    TestLockedFile(root);
    TestBackupNamesDoNotCollide(root);
    TestGameRunningGate(root);
    if (gFailures != 0) {
        std::fwprintf(stderr, L"config.wtf: %d failures\n", gFailures);
        return 1;
    }
    std::printf("config.wtf: fps cap read/write, encoding preservation, backup and running-process gate passed\n");
    return 0;
}
