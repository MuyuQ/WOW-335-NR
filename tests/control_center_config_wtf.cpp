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

// SaveConfigFpsCap writes its temporary file next to Config.wtf; these helpers let the
// beforeCommit hook prove the temporary file already exists while the target is untouched.
int CountTemporaryFiles(const std::wstring& configPath) {
    const size_t slash = configPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return 0;
    const std::wstring pattern = configPath.substr(0, slash) + L"\\Config.wtf.nr-*.tmp";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do {
        ++count;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return count;
}

bool ReadTemporaryFileBytes(const std::wstring& configPath, std::string& bytes) {
    bytes.clear();
    const size_t slash = configPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    const std::wstring directory = configPath.substr(0, slash);
    const std::wstring pattern = directory + L"\\Config.wtf.nr-*.tmp";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    const std::wstring temporary = directory + L"\\" + data.cFileName;
    FindClose(find);
    return ReadBytes(temporary, bytes);
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

void TestReorderedAndInterleavedKeys(const std::wstring& root) {
    const std::string inputs[] = {
        "SET maxFPSBK \"9\"\r\nSET maxFPS \"30\"\r\nSET locale \"zhCN\"\r\n",
        "SET maxFPS \"9\"\nSET maxFPSBK \"7\"\nSET maxFPS \"30\"\nSET maxFPSBK \"40\"\nSET locale \"zhCN\"\n"
    };
    for (size_t i = 0; i < 2; ++i) {
        const auto client = MakeClient(root, (L"reordered-" + std::to_wstring(i)).c_str(), inputs[i]);
        const auto saved = SaveConfigFpsCap(client, 60, 120);
        Check(static_cast<bool>(saved), "reordered/interleaved key save");
        const auto caps = ReadConfigFpsCap(client);
        Check(caps.front == 60 && caps.back == 120, "reordered caps readback");
        std::string bytes; ReadBytes(Join(client, L"WTF\\Config.wtf"), bytes);
        std::string expected = inputs[i];
        // Verify every duplicate and unrelated byte, including newline style.
        if (i == 0) expected = "SET maxFPSBK \"120\"\r\nSET maxFPS \"60\"\r\nSET locale \"zhCN\"\r\n";
        else expected = "SET maxFPS \"60\"\nSET maxFPSBK \"120\"\nSET maxFPS \"60\"\nSET maxFPSBK \"120\"\nSET locale \"zhCN\"\n";
        Check(bytes == expected, "only value spans may change");
    }
}

// 替换后的并发写者模拟：hooks.afterWrite 在本进程原子替换之后、回读校验之前被调用，
// 测试在这里改写文件，等价于另一个进程刚好在我们提交之后的窗口里写入。
struct ConcurrentWriteFixture {
    std::wstring path;
    std::string injected;
    int injectAttempt = 0;  // 0 = 每一轮都注入
    int calls = 0;
    std::vector<std::string> afterWrite;
};

void InjectConcurrentWrite(const std::wstring& path, int attempt, void* context) {
    auto* fixture = static_cast<ConcurrentWriteFixture*>(context);
    if (fixture == nullptr) return;
    ++fixture->calls;
    std::string bytes;
    if (!ReadBytes(path, bytes)) return;
    fixture->afterWrite.push_back(bytes);
    if (fixture->injectAttempt != 0 && fixture->injectAttempt != attempt) return;
    bytes += fixture->injected;
    WriteBytes(path, bytes);
}

void TestConcurrentWriterRetry(const std::wstring& root) {
    // 第一轮回读看到并发写者的修改 -> 重新读取最新内容、保留其无关设置后再写入并校验。
    const std::string initial = "SET maxFPS \"30\"\r\nSET maxFPSBK \"20\"\r\nSET locale \"zhCN\"\r\n";
    const std::wstring client = MakeClient(root, L"concurrent-retry", initial);
    ConcurrentWriteFixture fixture;
    fixture.path = Join(client, L"WTF\\Config.wtf");
    fixture.injected = "SET farclip \"777\"\r\n";
    fixture.injectAttempt = 1;
    ConfigWtfWriteHooks hooks;
    hooks.afterWrite = InjectConcurrentWrite;
    hooks.context = &fixture;
    const auto saved = SaveConfigFpsCap(client, 60, 40, hooks);
    Check(static_cast<bool>(saved), "save must succeed after retrying around a concurrent write");
    Check(fixture.calls == 2, "the concurrent write must be observed once, then the retry verifies");
    std::string bytes;
    Check(ReadBytes(fixture.path, bytes), "retried save must leave a readable config");
    Check(Contains(bytes, "SET farclip \"777\"\r\n"), "the concurrent writer's unrelated setting must be preserved");
    Check(Contains(bytes, "SET locale \"zhCN\"\r\n"), "unrelated settings from the original file must be preserved");
    Check(Contains(bytes, "SET maxFPS \"60\"\r\n") && Contains(bytes, "SET maxFPSBK \"40\"\r\n"),
          "both fps values must be applied after the retry");
    const auto cap = ReadConfigFpsCap(client);
    Check(cap.front == 60 && cap.back == 40, "retried save caps readback");
    Check(!saved.backupPath.empty(), "retried save must report a backup path");
}

void TestPersistentAfterWriteWriterLeavesTarget(const std::wstring& root) {
    // 每一轮回读都与期望不符：3 轮后失败，但当前文件保留我们最后一轮写入后对方追加的
    // 内容，不得用旧备份回填；备份对应最后一轮读取，可供人工恢复。
    const std::string initial = "SET maxFPS \"30\"\r\nSET maxFPSBK \"20\"\r\n";
    const std::wstring client = MakeClient(root, L"after-write-persistent", initial);
    ConcurrentWriteFixture fixture;
    fixture.path = Join(client, L"WTF\\Config.wtf");
    fixture.injected = "SET farclip \"555\"\r\n";
    fixture.injectAttempt = 0;
    ConfigWtfWriteHooks hooks;
    hooks.afterWrite = InjectConcurrentWrite;
    hooks.context = &fixture;
    const auto saved = SaveConfigFpsCap(client, 60, 40, hooks);
    Check(!saved, "persistent afterWrite conflicts must fail");
    Check(saved.error == ConfigWtfWriteError::VerifyFront,
          "persistent afterWrite conflicts must surface the verification error");
    Check(saved.detail.find("concurrent writer detected") != std::string::npos,
          "the afterWrite failure detail must name the concurrent writer");
    Check(fixture.calls == kConfigWtfWriteAttempts, "the afterWrite retry loop must be bounded to 3 attempts");
    if (fixture.afterWrite.size() != static_cast<size_t>(kConfigWtfWriteAttempts)) {
        Fail("afterWrite snapshots must be captured on every attempt");
        return;
    }
    std::string bytes;
    Check(ReadBytes(fixture.path, bytes), "failed save must leave the current config readable");
    Check(bytes == fixture.afterWrite[2] + fixture.injected,
          "the target must keep the last written content instead of the stale backup");
    Check(bytes != initial, "the failed save must not restore the old snapshot");
    Check(saved.backupPath != std::wstring(), "a recoverable backup must be kept on persistent conflict");
    Check(saved.backupAttempt == kConfigWtfWriteAttempts, "the backup must be tagged with the last read");
    if (!saved.backupPath.empty()) {
        std::string backup;
        Check(ReadBytes(saved.backupPath, backup), "the recoverable backup must be readable");
        Check(backup == fixture.afterWrite[1] + fixture.injected, "the backup must hold the last read snapshot");
    }
}

// 替换前并发写者模拟：hooks.beforeCommit 在本进程临时文件写好、尚未替换目标时被同步
// 调用，测试在这里改写目标文件，确定性地覆盖“读取后、替换前”的时序（不依赖 sleep）。
struct BeforeCommitFixture {
    std::string payload;    // 非空时追加到目标文件，模拟另一写者已提交的内容
    std::string replaceFrom;  // 非空时把目标文件里的这段替换为 replaceTo，模拟另一写者改写已有行
    std::string replaceTo;
    int injectAttempt = 0;  // 0 = 每一轮都注入；>0 = 只在该轮注入
    int calls = 0;
    std::vector<int> attempts;
    std::vector<std::string> targetSeen;     // 注入前读到的目标文件内容
    std::vector<std::string> temporarySeen;  // 注入时已写好的临时文件内容
    std::vector<bool> temporaryPresent;
};

void InjectBeforeCommit(const std::wstring& path, int attempt, void* context) {
    auto* fixture = static_cast<BeforeCommitFixture*>(context);
    if (fixture == nullptr) return;
    ++fixture->calls;
    fixture->attempts.push_back(attempt);
    std::string target;
    if (!ReadBytes(path, target)) return;
    fixture->targetSeen.push_back(target);
    std::string temporary;
    const bool found = ReadTemporaryFileBytes(path, temporary);
    fixture->temporaryPresent.push_back(found);
    fixture->temporarySeen.push_back(found ? temporary : std::string());
    if (fixture->payload.empty() && fixture->replaceFrom.empty()) return;  // 只观察顺序，不注入
    if (fixture->injectAttempt != 0 && fixture->injectAttempt != attempt) return;
    if (!fixture->replaceFrom.empty()) {
        const size_t at = target.find(fixture->replaceFrom);
        if (at == std::string::npos) return;
        target.replace(at, fixture->replaceFrom.size(), fixture->replaceTo);
    } else {
        target += fixture->payload;
    }
    WriteBytes(path, target);
}

void TestBeforeCommitHookOrdering(const std::wstring& root) {
    // 钩子在“临时文件已写好、目标文件尚未替换”时被同步调用：此时临时文件应已含新值，
    // 目标文件仍是旧内容。这里不注入任何写者，只断言顺序，因此不依赖 sleep 竞态。
    const std::string initial = "SET locale \"zhCN\"\r\nSET maxFPS \"30\"\r\nSET maxFPSBK \"20\"\r\n";
    const std::string expected = "SET locale \"zhCN\"\r\nSET maxFPS \"60\"\r\nSET maxFPSBK \"40\"\r\n";
    const std::wstring client = MakeClient(root, L"before-commit-order", initial);
    const std::wstring path = Join(client, L"WTF\\Config.wtf");
    BeforeCommitFixture fixture;
    ConfigWtfWriteHooks hooks;
    hooks.beforeCommit = InjectBeforeCommit;
    hooks.context = &fixture;
    const auto saved = SaveConfigFpsCap(client, 60, 40, hooks);
    Check(static_cast<bool>(saved), "save with an observing beforeCommit hook must succeed");
    Check(fixture.calls == 1, "beforeCommit must run exactly once for a single-round save");
    if (fixture.calls != 1) return;
    Check(fixture.attempts[0] == 1, "beforeCommit must report the 1-based attempt");
    Check(fixture.temporaryPresent[0], "beforeCommit must run after the temporary file is written");
    Check(fixture.targetSeen[0] == initial, "beforeCommit must run before the target file is replaced");
    Check(fixture.temporarySeen[0] == expected, "the temporary file must already hold the new values");
    std::string bytes;
    Check(ReadBytes(path, bytes) && bytes == expected, "the target file must be replaced after the hook");
    Check(CountTemporaryFiles(path) == 0, "a successful save must not leave temporary files");
}

void TestConcurrentWriterBeforeCommitPreservesSettings(const std::wstring& root) {
    // 另一写者在“读完原文之后、替换之前”提交了无关设置：本轮必须放弃替换，下一轮以
    // 磁盘最新内容为准重新应用帧率值，对方已提交的字段必须保留。
    const std::string initial = "SET locale \"zhCN\"\r\nSET maxFPS \"30\"\r\nSET maxFPSBK \"20\"\r\n";
    const std::string injected =
        "SET gxResolution \"2560x1440\"\r\n"
        "SET soundOutputSystem \"1\"\r\n";
    const std::wstring client = MakeClient(root, L"before-commit-merge", initial);
    const std::wstring path = Join(client, L"WTF\\Config.wtf");
    BeforeCommitFixture fixture;
    fixture.payload = injected;
    fixture.injectAttempt = 1;
    ConfigWtfWriteHooks hooks;
    hooks.beforeCommit = InjectBeforeCommit;
    hooks.context = &fixture;
    const auto saved = SaveConfigFpsCap(client, 60, 40, hooks);
    Check(static_cast<bool>(saved), "save must converge after one beforeCommit concurrent write");
    Check(fixture.calls == 2, "the concurrent write must be detected once, then the retry commits");
    std::string bytes;
    Check(ReadBytes(path, bytes), "merged config must be readable");
    Check(Contains(bytes, "SET gxResolution \"2560x1440\"\r\n"),
          "the concurrent writer's gxResolution must be preserved");
    Check(Contains(bytes, "SET soundOutputSystem \"1\"\r\n"),
          "the concurrent writer's sound setting must be preserved");
    Check(Contains(bytes, "SET locale \"zhCN\"\r\n"), "unrelated settings from the original file must be preserved");
    Check(Contains(bytes, "SET maxFPS \"60\"\r\n"), "front fps must be applied");
    Check(Contains(bytes, "SET maxFPSBK \"40\"\r\n"), "back fps must be applied");
    Check(!Contains(bytes, "SET maxFPS \"30\""), "the stale snapshot must not be committed");
    Check(!Contains(bytes, "SET maxFPSBK \"20\""), "the stale back value must not be committed");
    Check(saved.backupPath != std::wstring(), "the merged save must report a backup path");
    Check(saved.backupAttempt == 2, "the backup must be tagged with the read it was taken from");
    if (!saved.backupPath.empty()) {
        std::string backup;
        Check(ReadBytes(saved.backupPath, backup), "the reported backup must exist");
        Check(backup == initial + injected, "the backup must be the snapshot of the committing round");
    }
    if (fixture.targetSeen.size() == 2) {
        Check(fixture.targetSeen[0] == initial, "first beforeCommit must see the original snapshot");
        Check(fixture.targetSeen[1] == initial + injected,
              "the retry round must read the writer's committed content");
        Check(fixture.temporaryPresent[0] && fixture.temporaryPresent[1],
              "beforeCommit must see a prepared temporary file in both rounds");
    } else {
        Fail("beforeCommit must observe two rounds");
    }
    Check(CountTemporaryFiles(path) == 0, "a conflicting attempt must delete its temporary file");
}

void TestConcurrentWriterBeforeCommitRewritesSetting(const std::wstring& root) {
    // 另一写者在我们读取之后改写了已有的无关设置（而不是新增行）：本轮仍须放弃替换，
    // 下一轮以改写后的内容为快照，只覆盖两个帧率值，改写的值原样保留。
    const std::string initial = "SET locale \"zhCN\"\r\nSET maxFPS \"30\"\r\nSET maxFPSBK \"20\"\r\n";
    const std::string rewritten =
        "SET locale \"enUS\"\r\n"
        "SET gxResolution \"1920x1080\"\r\n";
    const std::wstring client = MakeClient(root, L"before-commit-rewrite", initial);
    const std::wstring path = Join(client, L"WTF\\Config.wtf");
    BeforeCommitFixture fixture;
    fixture.replaceFrom = "SET locale \"zhCN\"\r\n";
    fixture.replaceTo = rewritten;
    fixture.injectAttempt = 1;
    ConfigWtfWriteHooks hooks;
    hooks.beforeCommit = InjectBeforeCommit;
    hooks.context = &fixture;
    const auto saved = SaveConfigFpsCap(client, 60, 40, hooks);
    Check(static_cast<bool>(saved), "save must converge after a rewritten unrelated setting");
    Check(fixture.calls == 2, "the rewrite must be detected once, then the retry commits");
    const std::string expected =
        rewritten + "SET maxFPS \"60\"\r\nSET maxFPSBK \"40\"\r\n";
    std::string bytes;
    Check(ReadBytes(path, bytes), "rewritten config must be readable");
    Check(bytes == expected, "the other writer's rewritten value and key order must be preserved");
    Check(saved.backupAttempt == 2, "the backup must be tagged with the committing read");
    if (!saved.backupPath.empty()) {
        std::string backup;
        Check(ReadBytes(saved.backupPath, backup), "the reported backup must exist");
        Check(backup == rewritten + "SET maxFPS \"30\"\r\nSET maxFPSBK \"20\"\r\n",
              "the backup must hold the rewritten snapshot before our fps values");
    }
    Check(CountTemporaryFiles(path) == 0, "a conflicting attempt must delete its temporary file");
}

void TestPersistentConcurrentWriterBeforeCommit(const std::wstring& root) {
    // 每一轮替换前目标都被改动：3 轮后必须失败；目标文件保留对方最后提交的内容，不能
    // 回填旧快照/旧备份；保留的备份对应最后一轮读取，可供人工恢复。
    const std::string initial = "SET locale \"zhCN\"\r\nSET maxFPS \"30\"\r\nSET maxFPSBK \"20\"\r\n";
    const std::string injected = "SET farclip \"555\"\r\n";
    const std::wstring client = MakeClient(root, L"before-commit-persistent", initial);
    const std::wstring path = Join(client, L"WTF\\Config.wtf");
    BeforeCommitFixture fixture;
    fixture.payload = injected;
    fixture.injectAttempt = 0;
    ConfigWtfWriteHooks hooks;
    hooks.beforeCommit = InjectBeforeCommit;
    hooks.context = &fixture;
    const auto saved = SaveConfigFpsCap(client, 60, 40, hooks);
    Check(!saved, "persistent beforeCommit conflicts must fail");
    Check(saved.error == ConfigWtfWriteError::VerifyFront,
          "persistent concurrency must surface the verification error");
    Check(saved.detail.find("concurrent writer detected") != std::string::npos,
          "the failure detail must name the concurrent writer");
    Check(fixture.calls == kConfigWtfWriteAttempts, "the retry loop must be bounded to 3 attempts");
    std::string bytes;
    Check(ReadBytes(path, bytes), "failed save must leave the current config readable");
    Check(bytes == initial + injected + injected + injected,
          "the target must keep the other writer's latest content");
    Check(Contains(bytes, "SET maxFPS \"30\""), "our stale snapshot must not overwrite the target");
    Check(!Contains(bytes, "SET maxFPS \"60\""), "no unverified fps value may be committed");
    Check(saved.backupPath != std::wstring(), "a recoverable backup must be kept on persistent conflict");
    Check(saved.backupAttempt == kConfigWtfWriteAttempts, "the backup must be tagged with the last read");
    if (!saved.backupPath.empty()) {
        Check(GetFileAttributesW(saved.backupPath.c_str()) != INVALID_FILE_ATTRIBUTES,
              "the reported backup must exist on disk");
        std::string backup;
        Check(ReadBytes(saved.backupPath, backup), "the recoverable backup must be readable");
        Check(backup == initial + injected + injected,
              "the backup must hold the snapshot of the last read, not the final content");
    }
    Check(CountTemporaryFiles(path) == 0, "failed conflicting attempts must not leave temporary files");
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
    TestReorderedAndInterleavedKeys(root);
    TestRangeValidation(root);
    TestLockedFile(root);
    TestBackupNamesDoNotCollide(root);
    TestGameRunningGate(root);
    TestConcurrentWriterRetry(root);
    TestPersistentAfterWriteWriterLeavesTarget(root);
    TestBeforeCommitHookOrdering(root);
    TestConcurrentWriterBeforeCommitPreservesSettings(root);
    TestConcurrentWriterBeforeCommitRewritesSetting(root);
    TestPersistentConcurrentWriterBeforeCommit(root);
    if (gFailures != 0) {
        std::fwprintf(stderr, L"config.wtf: %d failures\n", gFailures);
        return 1;
    }
    std::printf("config.wtf: fps cap read/write, encoding preservation, backup, running-process gate, "
                "before/after-commit concurrent-writer handling passed\n");
    return 0;
}
