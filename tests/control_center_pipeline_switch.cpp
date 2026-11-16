// AIO 管线开关（dlss5-aio-x86.cfg 的 enabled）单元测试。
//
// 覆盖：读取（缺失/重复/非 0-1）、受控写入（只改值、保留其余字节、备份、
// 回读、无变化短路）、进程运行门、临时文件清理。
#include "../native/control_center/pipeline_switch.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace control_center;

int gFailures = 0;

void Fail(const char* what) {
    std::fprintf(stderr, "pipeline switch test failed: %s\n", what);
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

bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

std::wstring MakeClient(const std::wstring& root, const wchar_t* name, const std::string& cfg, bool withCfg) {
    const std::wstring client = Join(root, name);
    MakeDirectory(client);
    if (withCfg) WriteBytes(Join(client, L"dlss5-aio-x86.cfg"), cfg);
    return client;
}

bool HasBackupDir(const std::wstring& operationsRoot) {
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = Join(operationsRoot, L"config-*");
    const HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find);
    return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool HasTempFile(const std::wstring& client) {
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = Join(client, L"dlss5-aio-x86.cfg.dlss5-settings-*");
    const HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find);
    return true;
}

const char* kSampleCfg =
    "; AIO 固化配置\n"
    "[DLSS5]\n"
    "enabled=1\n"
    "classic_d3d9_cpu_fallback = 1\n"
    "[OTHER]\n"
    "note=keep\n";

void TestMissingFile(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"missing", std::string(), false);
    const std::wstring ops = Join(root, L"missing-ops");

    const PipelineSwitchState state = ReadPipelineSwitch(client);
    Check(state.exists == false, "missing: exists");
    Check(state.readable == false, "missing: readable");

    const PipelineSwitchResult saved = SetPipelineSwitch(client, ops, 0);
    Check(saved.error == PipelineSwitchError::MissingFile, "missing: save error");
}

void TestReadAndWrite(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"write", kSampleCfg, true);
    const std::wstring ops = Join(root, L"write-ops");

    const PipelineSwitchState state = ReadPipelineSwitch(client);
    Check(state.exists == true, "write: exists");
    Check(state.readable == true, "write: readable");
    Check(state.enabled == 1, "write: enabled");

    const PipelineSwitchResult saved = SetPipelineSwitch(client, ops, 0);
    Check(static_cast<bool>(saved) == true, "write: save ok");
    Check(saved.error == PipelineSwitchError::None, "write: save error enum");
    Check(saved.changed == true, "write: changed");
    Check(saved.backupPath.empty() == false, "write: backup path");

    std::string bytes;
    Check(ReadBytes(Join(client, L"dlss5-aio-x86.cfg"), bytes), "write: read cfg");
    Check(Contains(bytes, "enabled=0"), "write: value replaced");
    Check(Contains(bytes, "classic_d3d9_cpu_fallback = 1"), "write: other key preserved");
    Check(Contains(bytes, "[OTHER]") && Contains(bytes, "note=keep"), "write: other sections preserved");
    Check(HasBackupDir(ops), "write: backup directory");
    Check(HasTempFile(client) == false, "write: no temp file");

    std::string backup;
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW(Join(ops, L"config-*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        const std::wstring dir = Join(ops, data.cFileName);
        ReadBytes(Join(dir, L"before.cfg"), backup);
        FindClose(find);
    }
    Check(Contains(backup, "enabled=1"), "write: backup keeps original");

    const PipelineSwitchState reread = ReadPipelineSwitch(client);
    Check(reread.readable == true && reread.enabled == 0, "write: reread value");

    const PipelineSwitchResult again = SetPipelineSwitch(client, ops, 0);
    Check(static_cast<bool>(again) == true, "write: second save ok");
    Check(again.changed == false, "write: unchanged short-circuit");
}

void TestDuplicateAndInvalid(const std::wstring& root) {
    const std::wstring duplicate = MakeClient(root, L"duplicate", "[DLSS5]\nenabled=1\nenabled=0\n", true);
    const PipelineSwitchState dupState = ReadPipelineSwitch(duplicate);
    Check(dupState.exists == true, "duplicate: exists");
    Check(dupState.readable == true && dupState.enabled == -1, "duplicate: unknown value");
    const PipelineSwitchResult dupSave = SetPipelineSwitch(duplicate, Join(root, L"dup-ops"), 1);
    Check(dupSave.error == PipelineSwitchError::Ambiguous, "duplicate: save error");

    const std::wstring invalid = MakeClient(root, L"invalid", "[DLSS5]\nenabled=2\n", true);
    const PipelineSwitchState invalidState = ReadPipelineSwitch(invalid);
    Check(invalidState.readable == true && invalidState.enabled == -1, "invalid: unknown value");
    const PipelineSwitchResult invalidSave = SetPipelineSwitch(invalid, Join(root, L"invalid-ops"), 0);
    Check(invalidSave.error == PipelineSwitchError::Ambiguous, "invalid: save error");

    const std::wstring absent = MakeClient(root, L"absent", "[DLSS5]\n", true);
    const PipelineSwitchState absentState = ReadPipelineSwitch(absent);
    Check(absentState.exists == true && absentState.enabled == -1, "absent: unknown value");
}

void TestBomFirstLine(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"bom", std::string("\xEF\xBB\xBF") + "enabled=1\n", true);
    const PipelineSwitchState state = ReadPipelineSwitch(client);
    Check(state.exists == true, "bom: exists");
    Check(state.readable == true && state.enabled == 1, "bom: enabled read");
    const PipelineSwitchResult saved = SetPipelineSwitch(client, Join(root, L"bom-ops"), 0);
    Check(static_cast<bool>(saved) == true && saved.changed, "bom: save");
    std::string bytes;
    Check(ReadBytes(Join(client, L"dlss5-aio-x86.cfg"), bytes), "bom: read");
    Check(bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && Contains(bytes, "enabled=0"),
          "bom: prefix preserved");
}
void TestGameRunningGate(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"running", kSampleCfg, true);
    const std::wstring ops = Join(root, L"running-ops");
    wchar_t systemRoot[32768] = {};
    GetEnvironmentVariableW(L"SystemRoot", systemRoot, static_cast<DWORD>(std::size(systemRoot)));
    const std::wstring cmd = Join(std::wstring(systemRoot), L"System32\\cmd.exe");
    if (!CopyFileW(cmd.c_str(), Join(client, L"wow.exe").c_str(), FALSE)) {
        Fail("running: copy cmd.exe");
        return;
    }
    std::wstring command = L"cmd.exe /c ping -n 20 127.0.0.1 > nul";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(Join(client, L"wow.exe").c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, client.c_str(), &startup, &process)) {
        Fail("running: start fake game");
        return;
    }
    Sleep(200);
    const PipelineSwitchResult saved = SetPipelineSwitch(client, ops, 0);
    Check(saved.error == PipelineSwitchError::GameRunning, "running: gate error");
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Sleep(200);
    const PipelineSwitchResult after = SetPipelineSwitch(client, ops, 0);
    Check(static_cast<bool>(after) == true, "running: save after exit");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pipeline-switch.exe <output-root>\n");
        return 2;
    }
    const std::wstring root = Join(argv[1], L"pipeline-switch");
    MakeDirectory(root);
    TestMissingFile(root);
    TestReadAndWrite(root);
    TestDuplicateAndInvalid(root);
    TestBomFirstLine(root);
    TestGameRunningGate(root);
    if (gFailures != 0) {
        std::fprintf(stderr, "pipeline switch: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("pipeline switch: read/write, backup, duplicate, BOM and running-process gate passed\n");
    return 0;
}
