// 恢复安装前文件（撤销最后一次已完成安装）单元测试。
//
// 覆盖：删除“安装时新增”的文件、还原“安装时替换”的文件（备份哈希校验）、
// 安装记录清理/写回、后续事务冲突、无事务、进程运行门、事务目录安全边界。
#include "../native/control_center/restore_operation.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace control_center;

int gFailures = 0;

void Fail(const char* what) {
    std::fprintf(stderr, "restore operation test failed: %s\n", what);
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

bool MakeTree(const std::wstring& path) {
    std::wstring current;
    size_t at = 0;
    while (at < path.size()) {
        size_t slash = path.find(L'\\', at);
        if (slash == std::wstring::npos) slash = path.size();
        current = path.substr(0, slash);
        MakeDirectory(current);
        at = slash + 1;
    }
    return true;
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

std::string Sha256OfFile(const std::wstring& path) {
    const HashResult hashed = HashFile(path);
    if (!hashed) return std::string();
    static const char* digits = "0123456789abcdef";
    std::string hex;
    for (const auto byte : hashed.identity.sha256) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0F]);
    }
    return hex;
}

std::string Utf8Of(const std::wstring& text) {
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring MakeClient(const std::wstring& root, const wchar_t* name) {
    const std::wstring client = Join(root, name);
    MakeTree(Join(client, L"host64"));
    MakeTree(Join(client, L"Data"));
    const std::wstring source = L"D:\\WOW-335-CN\\_scratch\\dlss5-aio\\native-ui-fixture\\client\\wow.exe";
    if (!CopyFileW(source.c_str(), Join(client, L"wow.exe").c_str(), FALSE)) {
        Fail("fixture: copy wow.exe");
    }
    return client;
}

std::wstring MakeOperation(const std::wstring& operations, const char* id, const std::string& json) {
    std::wstring idText;
    const size_t length = std::strlen(id);
    for (size_t i = 0; i < 32; ++i) idText.push_back(length > i % length ? static_cast<wchar_t>(id[i % length]) : L'0');
    std::wstring operation = Join(operations, idText.c_str());
    MakeTree(operation);
    WriteBytes(Join(operation, L"transaction.json"), json);
    return operation;
}

std::string EscapeJson(const std::string& text) {
    std::string out;
    for (const char value : text) {
        if (value == '\\' || value == '"') out.push_back('\\');
        out.push_back(value);
    }
    return out;
}

std::string BuildRecord(const std::wstring& client, const std::vector<std::string>& entryJson,
                        const std::vector<std::string>& applied) {
    std::string json = "{\n \"schema\": 2,\n \"status\": \"completed\",\n";
    json += " \"client\": \"" + EscapeJson(Utf8Of(client)) + "\",\n";
    json += " \"entries\": [\n  ";
    for (size_t i = 0; i < entryJson.size(); ++i) {
        json += entryJson[i];
        if (i + 1 < entryJson.size()) json += ",\n  ";
    }
    json += "\n ],\n \"applied\": [";
    for (size_t i = 0; i < applied.size(); ++i) {
        json += "\"" + applied[i] + "\"";
        if (i + 1 < applied.size()) json += ", ";
    }
    json += "],\n \"previous_client_record\": null\n}\n";
    return json;
}

std::string BuildRecordWithPrevious(const std::wstring& client, const std::vector<std::string>& entryJson,
                                    const std::vector<std::string>& applied, const std::string& previousRecord) {
    std::string json = "{\n \"schema\": 2,\n \"status\": \"completed\",\n";
    json += " \"client\": \"" + EscapeJson(Utf8Of(client)) + "\",\n";
    json += " \"entries\": [\n  ";
    for (size_t i = 0; i < entryJson.size(); ++i) {
        json += entryJson[i];
        if (i + 1 < entryJson.size()) json += ",\n  ";
    }
    json += "\n ],\n \"applied\": [";
    for (size_t i = 0; i < applied.size(); ++i) {
        json += "\"" + applied[i] + "\"";
        if (i + 1 < applied.size()) json += ", ";
    }
    json += "],\n \"previous_client_record\": " + previousRecord + "\n}\n";
    return json;
}

std::string AbsentEntry(const std::wstring& target, const std::wstring& file) {
    return "{\"target\": \"" + EscapeJson(Utf8Of(target)) + "\", \"sha256\": \"" + Sha256OfFile(file) +
           "\", \"pre_state\": \"absent\", \"replaces_original\": false, \"previous_sha256\": null, \"backup\": null}";
}

std::string PresentEntry(const std::wstring& target, const std::wstring& backupFile, const std::wstring& installedFile) {
    return "{\"target\": \"" + EscapeJson(Utf8Of(target)) + "\", \"sha256\": \"" + Sha256OfFile(installedFile) +
           "\", \"pre_state\": \"present\", \"replaces_original\": false, \"previous_sha256\": \"" +
           Sha256OfFile(backupFile) + "\", \"backup\": \"" + EscapeJson(Utf8Of(target)) + "\"}";
}

std::wstring NewId(char seed) {
    std::wstring id;
    for (int i = 0; i < 32; ++i) {
        const int value = (seed + i) % 16;
        id.push_back(static_cast<wchar_t>(value < 10 ? L'0' + value : L'a' + (value - 10)));
    }
    return id;
}

void TestNoOperations(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"client-none");
    const std::wstring operations = Join(root, L"operations-none");
    MakeDirectory(operations);
    const RestoreResult result = RestoreLastInstallOperation(client, operations, {}, CatalogResult{});
    Check(result.ok == false && result.error == L"没有可恢复的安装事务记录。", "none: no operations");
}

void TestDeleteNewFile(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"client-delete");
    const std::wstring operations = Join(root, L"operations-delete");
    MakeDirectory(operations);
    WriteBytes(Join(client, L"d3d9.dll"), "new d3d9 payload");
    MakeTree(Join(client, L"host64\\dlss5-backup"));
    WriteBytes(Join(client, L"host64\\dlss5-backup\\old.bin"), "old");
    const std::string record =
        BuildRecord(client, {AbsentEntry(L"d3d9.dll", Join(client, L"d3d9.dll"))}, {"d3d9.dll"});
    const std::wstring operation = MakeOperation(operations, "a", record);
    const RestoreResult result = RestoreLastInstallOperation(client, operations, {}, CatalogResult{});
    Check(result.ok, "delete: ok");
    Check(result.deleted == 1, "delete: deleted count");
    Check(result.skipped == 0 && result.restored == 0, "delete: counters");
    Check(GetFileAttributesW(Join(client, L"d3d9.dll").c_str()) == INVALID_FILE_ATTRIBUTES, "delete: file removed");
    Check(result.recordRemoved, "delete: record removed");
    Check(result.backupDirectoryRemoved, "delete: backup dir removed");
    Check(result.status == L"restored", "delete: status");
    Check(GetFileAttributesW(Join(operation, L"restore.json").c_str()) != INVALID_FILE_ATTRIBUTES,
          "delete: restore.json written");
    // restore.json 的 members 必须是对象数组；不能把 std::wstring 交给 JSON
    // 序列化器（会被当成宽字符整数数组）。
    std::string restoreBytes;
    Check(ReadBytes(Join(operation, L"restore.json"), restoreBytes), "delete: restore.json readable");
    Check(restoreBytes.find("\"target\": \"d3d9.dll\"") != std::string::npos, "delete: member object target");
    Check(restoreBytes.find("\"action\": \"") != std::string::npos, "delete: member object action");
    Check(restoreBytes.find("\"members\": [\n    [") == std::string::npos, "delete: members not numeric arrays");
    Check(result.report.find(L"d3d9.dll") != std::wstring::npos, "delete: report");
}

void TestRestoreReplacedFile(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"client-present");
    const std::wstring operations = Join(root, L"operations-present");
    MakeDirectory(operations);
    MakeTree(Join(client, L"host64"));
    const std::wstring target = L"host64\\ReShade.ini";
    WriteBytes(Join(client, target.c_str()), "[Standalone.DLSSNR]\nNew=1\n");
    const std::wstring backupSource = Join(operations, L"backup-source.ini");
    WriteBytes(backupSource, "[Standalone.DLSSNR]\nOld=1\n");
    const std::string record =
        BuildRecord(client, {PresentEntry(target, backupSource, Join(client, target.c_str()))}, {"host64/ReShade.ini"});
    const std::wstring operation = MakeOperation(operations, "b", record);
    MakeTree(Join(operation, L"backup\\host64"));
    CopyFileW(backupSource.c_str(), Join(operation, L"backup\\host64\\ReShade.ini").c_str(), FALSE);
    const RestoreResult result = RestoreLastInstallOperation(client, operations, {}, CatalogResult{});
    Check(result.ok, "present: ok");
    Check(result.restored == 1, "present: restored count");
    std::string content;
    ReadBytes(Join(client, target.c_str()), content);
    Check(content.find("Old=1") != std::string::npos && content.find("New=1") == std::string::npos,
          "present: content restored");
}

// 有 previous_client_record 时：写回上一份安装记录，且必须保留 host64\dlss5-backup
// （那份记录仍引用备份；删掉会导致后续卸载/修复失去原件）。
void TestRestorePreviousRecord(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"client-previous");
    const std::wstring operations = Join(root, L"operations-previous");
    MakeDirectory(operations);
    MakeTree(Join(client, L"host64"));
    const std::wstring target = L"host64\\ReShade.ini";
    WriteBytes(Join(client, target.c_str()), "[Standalone.DLSSNR]\nNew=1\n");
    const std::wstring installedRecord = Join(client, L"host64\\dlss5-install-record.json");
    WriteBytes(installedRecord, BuildRecord(client, {AbsentEntry(L"d3d9.dll", Join(client, L"host64\\ReShade.ini"))},
                                           {"d3d9.dll"}));
    const std::wstring backupDir = Join(client, L"host64\\dlss5-backup");
    MakeTree(backupDir);
    WriteBytes(Join(backupDir, L"keep.bin"), "keep me");
    const std::wstring backupSource = Join(operations, L"backup-source-previous.ini");
    WriteBytes(backupSource, "[Standalone.DLSSNR]\nOld=1\n");
    const std::string previousRecord = "{\"schema\": 1, \"status\": \"installed\", \"entries\": [{\"target\": \"d3d9.dll\", "
                                       "\"sha256\": \"" +
                                       Sha256OfFile(backupSource) +
                                       "\", \"pre_state\": \"absent\", \"replaces_original\": false}]}";
    const std::string record =
        BuildRecordWithPrevious(client, {PresentEntry(target, backupSource, Join(client, target.c_str()))},
                                {"host64/ReShade.ini"}, previousRecord);
    const std::wstring operation = MakeOperation(operations, "d", record);
    MakeTree(Join(operation, L"backup\\host64"));
    CopyFileW(backupSource.c_str(), Join(operation, L"backup\\host64\\ReShade.ini").c_str(), FALSE);
    const RestoreResult result = RestoreLastInstallOperation(client, operations, {}, CatalogResult{});
    Check(result.ok, "previous: ok");
    Check(result.restored == 1, "previous: restored count");
    Check(result.recordRestored && !result.recordRemoved, "previous: record restored");
    Check(!result.backupDirectoryRemoved, "previous: backup dir kept");
    Check(GetFileAttributesW(Join(backupDir, L"keep.bin").c_str()) != INVALID_FILE_ATTRIBUTES,
          "previous: backup content kept");
    std::string recordBytes;
    ReadBytes(installedRecord, recordBytes);
    Check(recordBytes.find("\"d3d9.dll\"") != std::string::npos, "previous: record written back");
    std::string content;
    ReadBytes(Join(client, target.c_str()), content);
    Check(content.find("Old=1") != std::string::npos && content.find("New=1") == std::string::npos,
          "previous: content restored");
}

void TestLaterTransactionConflict(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"client-conflict");
    const std::wstring operations = Join(root, L"operations-conflict");
    MakeDirectory(operations);
    WriteBytes(Join(client, L"d3d9.dll"), "payload");
    const std::string record = BuildRecord(client, {AbsentEntry(L"d3d9.dll", Join(client, L"d3d9.dll"))}, {"d3d9.dll"});
    const std::wstring chosen = MakeOperation(operations, "c", record);
    const std::wstring newer = operations + L"\\" + NewId('7');
    MakeTree(newer);
    SYSTEMTIME now{};
    GetSystemTime(&now);
    FILETIME fileTime{};
    SystemTimeToFileTime(&now, &fileTime);
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    value.QuadPart += 60ull * 1000ull * 1000ull * 10ull;
    fileTime.dwLowDateTime = value.LowPart;
    fileTime.dwHighDateTime = value.HighPart;
    HANDLE handle = CreateFileW(newer.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        SetFileTime(handle, nullptr, nullptr, &fileTime);
        CloseHandle(handle);
    }
    const RestoreResult result = RestoreLastInstallOperation(client, operations, {}, CatalogResult{});
    Check(result.ok == false && result.error == L"客户端有后续安装事务，请先恢复最近一次安装。", "conflict: rejected");
    (void)chosen;
}

void TestUnsafeOperationsRoot(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"client-unsafe");
    const std::wstring operations = Join(client, L"operations");
    MakeDirectory(operations);
    const RestoreResult result = RestoreLastInstallOperation(client, operations, {}, CatalogResult{});
    Check(result.ok == false && result.error == L"安装事务目录不安全。", "unsafe: rejected");
}

void TestGameRunningGate(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"client-running");
    const std::wstring operations = Join(root, L"operations-running");
    MakeDirectory(operations);
    wchar_t systemRoot[32768] = {};
    GetEnvironmentVariableW(L"SystemRoot", systemRoot, static_cast<DWORD>(std::size(systemRoot)));
    const std::wstring cmd = Join(std::wstring(systemRoot), L"System32\\cmd.exe");
    const std::wstring fake = Join(root, L"wow.exe");
    if (!CopyFileW(cmd.c_str(), fake.c_str(), FALSE)) {
        Fail("running: copy fake game");
        return;
    }
    std::wstring command = L"\"" + fake + L"\" /c ping -n 20 127.0.0.1 > nul";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(fake.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        root.c_str(), &startup, &process)) {
        Fail("running: start fake game");
        return;
    }
    Sleep(200);
    const RestoreResult result = RestoreLastInstallOperation(client, operations, {}, CatalogResult{});
    Check(result.ok == false && result.error == L"请先退出游戏、启动器和 AIO 载体。", "running: gate");
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Sleep(200);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: restore-operation.exe <output-root>\n");
        return 2;
    }
    const std::wstring root = Join(argv[1], L"restore-operation");
    MakeDirectory(root);
    TestNoOperations(root);
    TestDeleteNewFile(root);
    TestRestoreReplacedFile(root);
    TestRestorePreviousRecord(root);
    TestLaterTransactionConflict(root);
    TestUnsafeOperationsRoot(root);
    TestGameRunningGate(root);
    if (gFailures != 0) {
        std::fprintf(stderr, "restore operation: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("restore operation: delete/restore members, record cleanup, conflicts and gates passed\n");
    return 0;
}
