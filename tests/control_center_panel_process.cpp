#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "../native/control_center/panel_process.hpp"

namespace {

int gFailures = 0;

void Check(bool condition, const char* label) {
    if (!condition) {
        ++gFailures;
        std::printf("FAIL %s\n", label);
    }
}

void CheckWide(bool condition, const char* label, const std::wstring& actual) {
    if (!condition) {
        ++gFailures;
        std::printf("FAIL %s\n", label);
        std::printf("  actual: ");
        std::wprintf(L"%ls\n", actual.c_str());
    }
}

std::wstring Join(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (left.back() == L'\\' || left.back() == L'/') return left + right;
    return left + L"\\" + right;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        if (!EnsureDirectory(path.substr(0, slash))) return false;
    }
    return CreateDirectoryW(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteBytes(const std::wstring& path, const std::string& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
                    written == bytes.size();
    CloseHandle(file);
    return ok;
}

bool RemoveTree(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return DeleteFileW(path.c_str()) != 0;
    WIN32_FIND_DATAW find{};
    const HANDLE handle = FindFirstFileW(Join(path, L"*").c_str(), &find);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = find.cFileName;
            if (name == L"." || name == L"..") continue;
            RemoveTree(Join(path, name));
        } while (FindNextFileW(handle, &find));
        FindClose(handle);
    }
    return RemoveDirectoryW(path.c_str()) != 0;
}

bool CopySelf(const std::wstring& target) {
    EnsureDirectory(target.substr(0, target.find_last_of(L'\\')));
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const std::wstring cmd = Join(systemDirectory, L"cmd.exe");
    return CopyFileW(cmd.c_str(), target.c_str(), FALSE) != 0;
}

bool StartImage(const std::wstring& image, const std::wstring& arguments, const std::wstring& cwd, DWORD& pid) {
    std::wstring command = L"\"" + image + L"\"";
    if (!arguments.empty()) command += L" " + arguments;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(image.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &startup, &process)) {
        return false;
    }
    pid = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool TerminatePid(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) return true;
    TerminateProcess(process, 1);
    WaitForSingleObject(process, 2000);
    CloseHandle(process);
    return true;
}

bool WaitImageState(const wchar_t* image, bool expected, ULONGLONG timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeoutMs) {
        if (control_center::panel_detail::ProcessRunning(image) == expected) return true;
        Sleep(25);
    }
    return control_center::panel_detail::ProcessRunning(image) == expected;
}

void TestReadTailLines(const std::wstring& root) {
    using control_center::panel_detail::ReadTailLines;
    const std::wstring logs = Join(root, L"logs");
    EnsureDirectory(logs);

    std::vector<std::wstring> lines;
    DWORD error = ERROR_SUCCESS;
    Check(!ReadTailLines(Join(logs, L"missing.log"), 40, lines, error), "tail missing file rejected");
    Check(error == ERROR_FILE_NOT_FOUND, "tail missing file reports not found");

    const std::wstring emptyPath = Join(logs, L"empty.log");
    WriteBytes(emptyPath, std::string());
    lines.clear();
    error = ERROR_SUCCESS;
    Check(ReadTailLines(emptyPath, 40, lines, error), "tail empty file accepted");
    Check(lines.empty(), "tail empty file yields no lines");

    std::string crlf;
    for (int i = 0; i < 60; ++i) {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "line %d\r\n", i);
        crlf += buffer;
    }
    const std::wstring crlfPath = Join(logs, L"stdout.log");
    WriteBytes(crlfPath, crlf);
    lines.clear();
    Check(ReadTailLines(crlfPath, 40, lines, error), "tail crlf file accepted");
    Check(lines.size() == 40, "tail keeps only the last 40 lines");
    if (lines.size() == 40) {
        CheckWide(lines.front() == L"line 20", "tail starts at the fortieth-from-last line", lines.front());
        CheckWide(lines.back() == L"line 59", "tail ends at the last line", lines.back());
    }

    const std::wstring bomPath = Join(logs, L"bom.log");
    WriteBytes(bomPath, std::string("\xEF\xBB\xBF") + "first\r\nsecond");
    lines.clear();
    Check(ReadTailLines(bomPath, 40, lines, error), "tail bom file accepted");
    Check(lines.size() == 2, "tail bom file has two lines");
    if (lines.size() == 2) {
        CheckWide(lines.front() == L"first", "tail strips the utf-8 bom", lines.front());
        CheckWide(lines.back() == L"second", "tail keeps a last line without newline", lines.back());
    }
}

void TestReadTailLargeFile(const std::wstring& root) {
    using control_center::panel_detail::ReadTailLines;
    const std::wstring logs = Join(root, L"logs");
    std::string content;
    std::wstring last;
    for (int i = 0; i < 4000; ++i) {
        char narrow[32] = {};
        std::snprintf(narrow, sizeof(narrow), "row-%04d-", i);
        std::string line(narrow);
        line.append(90, 'x');
        content += line;
        content += "\r\n";
        wchar_t wide[160] = {};
        std::swprintf(wide, 160, L"row-%04d-", i);
        std::wstring wideLine(wide);
        wideLine.append(90, L'x');
        last = wideLine;
    }
    Check(content.size() > control_center::kLogTailMaxBytes, "large fixture exceeds the tail window");
    const std::wstring path = Join(logs, L"large.log");
    Check(WriteBytes(path, content), "large log written");
    std::vector<std::wstring> lines;
    DWORD error = ERROR_SUCCESS;
    Check(ReadTailLines(path, 40, lines, error), "tail large file accepted");
    Check(lines.size() == 40, "tail large file keeps 40 lines");
    if (!lines.empty()) {
        Check(lines.front().size() == 99, "tail drops the partial first line");
        Check(lines.front().rfind(L"row-", 0) == 0, "tail first line is complete");
        CheckWide(lines.back() == last, "tail large file ends at the last line", lines.back());
    }
}

void TestReadLogTail(const std::wstring& root) {
    using control_center::ReadLogTail;
    auto empty = ReadLogTail(std::wstring());
    Check(!empty.ok, "log tail rejects an empty directory");
    CheckWide(empty.error == L"日志目录为空。", "log tail empty directory message", empty.error);

    const std::wstring logDir = Join(root, L"panel-logs\\run-1");
    EnsureDirectory(logDir);
    std::string stdoutText;
    for (int i = 0; i < 50; ++i) {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "stdout-%d\n", i);
        stdoutText += buffer;
    }
    WriteBytes(Join(logDir, L"stdout.log"), stdoutText);
    WriteBytes(Join(logDir, L"stderr.log"), std::string("warn-1\r\nwarn-2\r\n"));
    const auto tail = ReadLogTail(logDir);
    Check(tail.ok, "log tail accepts a log directory");
    Check(tail.lines.size() == 42, "log tail concatenates stdout and stderr windows");
    if (tail.lines.size() == 42) {
        CheckWide(tail.lines.front() == L"stdout-10", "log tail starts at the last 40 stdout lines", tail.lines.front());
        CheckWide(tail.lines.back() == L"warn-2", "log tail ends with the last stderr line", tail.lines.back());
    }
}

void TestPanelGuards(const std::wstring& root) {
    const std::wstring emptyProduct = Join(root, L"empty-product");
    const std::wstring client = Join(root, L"client");
    const std::wstring dataDir = Join(root, L"data");
    EnsureDirectory(emptyProduct);
    EnsureDirectory(client);
    CopySelf(Join(client, L"wow.exe"));

    auto missing = control_center::panel_detail::StartPanel(emptyProduct, dataDir, client);
    Check(!missing.ok, "start panel rejects a missing program");
    CheckWide(missing.error == L"面板程序缺失：bin/dlss5-panel.exe", "start panel missing program message",
              missing.error);

    const std::wstring product = Join(root, L"product");
    Check(CopySelf(Join(product, L"bin\\dlss5-panel.exe")), "panel fixture copied");
    const std::wstring emptyClient = Join(root, L"empty-client");
    EnsureDirectory(emptyClient);
    const bool external = control_center::PanelProcessRunning();
    if (external) {
        // 本机可能已有其它 dlss5-panel 实例（程序按映像名做全局互斥），此时只能验证互斥文案。
        const auto busy = control_center::panel_detail::StartPanel(product, dataDir, client);
        Check(!busy.ok, "start panel rejects an external instance");
        CheckWide(busy.error == L"游戏内面板已在运行。", "start panel external instance message", busy.error);
        std::printf("note: an existing dlss5-panel process was detected; launch smoke test skipped\n");
    } else {
        auto noGame = control_center::panel_detail::StartPanel(product, dataDir, emptyClient);
        Check(!noGame.ok, "start panel rejects a client without wow.exe");
        CheckWide(noGame.error == L"请先选择含 wow.exe 的客户端目录。", "start panel missing client message",
                  noGame.error);

        auto noData = control_center::panel_detail::StartPanel(product, std::wstring(), client);
        Check(!noData.ok, "start panel rejects an empty data directory");
        CheckWide(noData.error == L"无法确定日志目录。", "start panel empty data directory message", noData.error);
    }

    auto game = control_center::StartClientGame(emptyClient);
    Check(!game.ok, "start game rejects a client without wow.exe");
    CheckWide(game.error == L"请先在「切换客户端」中选择含 wow.exe 的目录。", "start game missing client message",
              game.error);
    auto gameEmpty = control_center::StartClientGame(std::wstring());
    Check(!gameEmpty.ok, "start game rejects an empty client root");
}

void TestPanelStart(const std::wstring& root) {
    const std::wstring product = Join(root, L"product");
    const std::wstring client = Join(root, L"client");
    const std::wstring dataDir = Join(root, L"data");
    const std::wstring panelExe = Join(product, L"bin\\dlss5-panel.exe");
    Check(FileExists(panelExe), "panel fixture present");

    if (control_center::PanelProcessRunning()) {
        std::printf("note: panel start smoke test skipped because an instance is already running\n");
        return;
    }
    DWORD runningPid = 0;
    Check(StartImage(panelExe, L"/c ping -n 30 127.0.0.1 > nul", product, runningPid), "manual panel process started");
    Check(WaitImageState(L"dlss5-panel.exe", true, 3000), "panel process detected as running");
    auto busy = control_center::panel_detail::StartPanel(product, dataDir, client);
    Check(!busy.ok, "start panel rejects a second instance");
    CheckWide(busy.error == L"游戏内面板已在运行。", "start panel already running message", busy.error);
    TerminatePid(runningPid);
    Check(WaitImageState(L"dlss5-panel.exe", false, 3000), "panel process termination observed");

    const auto started = control_center::panel_detail::StartPanel(product, dataDir, client);
    Check(started.ok, "start panel launches the program");
    Check(started.pid != 0, "start panel reports the process id");
    Check(!started.logDir.empty(), "start panel reports the log directory");
    const std::wstring stdoutPath = Join(started.logDir, L"stdout.log");
    const std::wstring stderrPath = Join(started.logDir, L"stderr.log");
    Check(FileExists(stdoutPath), "start panel creates stdout.log");
    Check(FileExists(stderrPath), "start panel creates stderr.log");
    TerminatePid(started.pid);

    const auto game = control_center::StartClientGame(client);
    Check(game.ok, "start game launches wow.exe");
    Check(game.pid != 0, "start game reports the process id");
    TerminatePid(game.pid);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: panel-process.exe <scratch-root>\n");
        return 2;
    }
    const std::wstring root = Join(std::wstring(argv[1]), L"panel-process");
    RemoveTree(root);
    if (!EnsureDirectory(root)) {
        std::printf("FAIL create scratch root\n");
        return 1;
    }
    TestReadTailLines(root);
    TestReadTailLargeFile(root);
    TestReadLogTail(root);
    TestPanelGuards(root);
    TestPanelStart(root);
    if (gFailures == 0) {
        std::printf("panel process: log tails, panel guards, hidden start and client game launch passed\n");
        return 0;
    }
    std::printf("panel process: %d failure(s)\n", gFailures);
    return 1;
}
