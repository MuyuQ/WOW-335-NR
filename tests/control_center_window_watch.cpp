// 鼠标助手（window watch）启动单元测试。
//
// 覆盖：配置门槛、客户端校验、路径守卫、watcher-lock 三哈希、进程互斥、
// 日志目录与隐藏启动。助手可执行文件用 cmd.exe 的副本来模拟，
// “受控构建”与“_scratch 候选”两条路径都按真实的 watcher-lock 校验。
#include "../native/control_center/window_watch.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace control_center;

int gFailures = 0;

void Fail(const char* what) {
    std::fprintf(stderr, "window watch test failed: %s\n", what);
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

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring SystemCmd() {
    wchar_t systemRoot[32768] = {};
    GetEnvironmentVariableW(L"SystemRoot", systemRoot, static_cast<DWORD>(std::size(systemRoot)));
    return Join(std::wstring(systemRoot), L"System32\\cmd.exe");
}

std::string HashText(const std::wstring& path) {
    const HashResult hashed = HashFile(path);
    if (!hashed) return std::string();
    return HexSha256(hashed.identity.sha256);
}

const char* kReShadeIni =
    "[GENERAL]\n"
    "\n"
    "[Standalone.DLSSNR]\n"
    "NeuralRendering=1\n"
    "Model=1\n"
    "FrameGeneration=0\n"
    "DetachedPresentation=1\n"
    "WindowedInputScaling=1\n";

const char* kCfg =
    "[DLSS5]\n"
    "enabled=1\n"
    "classic_d3d9_cpu_fallback=1\n";

struct Fixture {
    std::wstring root;
    std::wstring product;
    std::wstring client;
    std::wstring operations;
};

void WriteLockFile(const std::wstring& product, const std::string& exeSha, const std::string& sourceSha,
                   const std::string& layerSha) {
    std::string json = "{\n \"schema\": 1,\n \"files\": {\n";
    json += std::string("  \"window_watch.exe\": \"") + exeSha + "\",\n";
    json += std::string("  \"window_watch.cpp\": \"") + sourceSha + "\",\n";
    json += std::string("  \"window_layer.hpp\": \"") + layerSha + "\"\n";
    json += " }\n}\n";
    WriteBytes(Join(product, L"watcher-lock.json"), json);
}

void WriteLockFile(const std::wstring& product) {
    WriteLockFile(product, HashText(Join(product, L"bin\\window_watch.exe")),
                  HashText(Join(product, L"native\\window_watch.cpp")),
                  HashText(Join(product, L"native\\window_layer.hpp")));
}

bool CreateFixture(Fixture& fixture) {
    fixture.product = Join(fixture.root, L"product");
    MakeDirectory(fixture.product);
    MakeDirectory(Join(fixture.product, L"bin"));
    MakeDirectory(Join(fixture.product, L"native"));
    MakeDirectory(Join(fixture.product, L"_scratch"));
    MakeDirectory(Join(fixture.product, L"_scratch\\candidate"));
    if (!CopyFileW(SystemCmd().c_str(), Join(fixture.product, L"bin\\window_watch.exe").c_str(), FALSE)) return false;
    if (!CopyFileW(SystemCmd().c_str(), Join(fixture.product, L"_scratch\\candidate\\window_watch.exe").c_str(), FALSE))
        return false;
    WriteBytes(Join(fixture.product, L"native\\window_watch.cpp"), "// window watch source\n");
    WriteBytes(Join(fixture.product, L"native\\window_layer.hpp"), "// window layer header\n");
    WriteLockFile(fixture.product);
    fixture.client = Join(fixture.root, L"client");
    MakeDirectory(fixture.client);
    MakeDirectory(Join(fixture.client, L"host64"));
    if (!CopyFileW(SystemCmd().c_str(), Join(fixture.client, L"wow.exe").c_str(), FALSE)) return false;
    WriteBytes(Join(fixture.client, L"host64\\ReShade.ini"), kReShadeIni);
    WriteBytes(Join(fixture.client, L"dlss5-aio-x86.cfg"), kCfg);
    fixture.operations = Join(fixture.root, L"operations");
    MakeDirectory(fixture.operations);
    return true;
}

void TestSuccess(const Fixture& fixture) {
    const WindowWatchStartResult started = StartWindowWatch(fixture.product, fixture.operations, fixture.client);
    Check(started.ok, "success: started");
    Check(started.pid != 0, "success: pid");
    Check(started.logDir.empty() == false, "success: log dir");
    Check(FileExists(Join(started.logDir, L"stdout.log")), "success: stdout log");
    Check(FileExists(Join(started.logDir, L"stderr.log")), "success: stderr log");
    Check(started.message == L"助手已启动；是否处理成功须查看日志并进行游戏验收。", "success: message");
    const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, started.pid);
    if (process != nullptr) {
        TerminateProcess(process, 1);
        CloseHandle(process);
    }
}

void TestConfigGate(const Fixture& fixture) {
    const std::wstring client = Join(fixture.root, L"client-gate");
    MakeDirectory(client);
    MakeDirectory(Join(client, L"host64"));
    CopyFileW(Join(fixture.client, L"wow.exe").c_str(), Join(client, L"wow.exe").c_str(), FALSE);
    WriteBytes(Join(client, L"dlss5-aio-x86.cfg"), kCfg);
    WriteBytes(Join(client, L"host64\\ReShade.ini"),
               "[Standalone.DLSSNR]\nFrameGeneration=1\nDetachedPresentation=1\nWindowedInputScaling=1\n");
    const WindowWatchStartResult started = StartWindowWatch(fixture.product, fixture.operations, client);
    Check(started.ok == false, "gate: rejected");
    Check(started.error == L"当前配置不符合独立输出候选：请检查 AIO 管线、CPU 桥、输入缩放及 FG 状态。",
          "gate: message");

    const std::wstring missingIni = Join(fixture.root, L"client-no-ini");
    MakeDirectory(missingIni);
    CopyFileW(Join(fixture.client, L"wow.exe").c_str(), Join(missingIni, L"wow.exe").c_str(), FALSE);
    WriteBytes(Join(missingIni, L"dlss5-aio-x86.cfg"), kCfg);
    const WindowWatchStartResult noIni = StartWindowWatch(fixture.product, fixture.operations, missingIni);
    Check(noIni.ok == false && noIni.error == L"当前配置不符合独立输出候选：请检查 AIO 管线、CPU 桥、输入缩放及 FG 状态。",
          "gate: missing ini");
}

void TestClientGuard(const Fixture& fixture) {
    const std::wstring empty = Join(fixture.root, L"client-empty");
    MakeDirectory(empty);
    const WindowWatchStartResult missing = StartWindowWatch(fixture.product, fixture.operations, empty);
    Check(missing.ok == false && missing.error == L"请先选择含 wow.exe 的客户端目录。", "client: missing wow.exe");
}

void TestPathGuard(const Fixture& fixture) {
    const std::wstring outside = Join(fixture.root, L"elsewhere");
    MakeDirectory(outside);
    CopyFileW(Join(fixture.product, L"bin\\window_watch.exe").c_str(), Join(outside, L"window_watch.exe").c_str(),
              FALSE);
    const WindowWatchStartResult started =
        StartWindowWatch(fixture.product, fixture.operations, fixture.client, Join(outside, L"window_watch.exe"));
    Check(started.ok == false && started.error == L"窗口助手须为受控构建或 _scratch 中的候选。", "path: outside");
}

void TestLockMismatch(const Fixture& fixture) {
    WriteLockFile(fixture.product, std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c'));
    const WindowWatchStartResult started = StartWindowWatch(fixture.product, fixture.operations, fixture.client);
    Check(started.ok == false && started.error == L"窗口助手或源码身份不符。", "lock: mismatch");
    WriteLockFile(fixture.product);

    const std::wstring lockPath = Join(fixture.product, L"watcher-lock.json");
    const std::wstring backup = Join(fixture.product, L"watcher-lock.json.bak");
    MoveFileW(lockPath.c_str(), backup.c_str());
    const WindowWatchStartResult noLock = StartWindowWatch(fixture.product, fixture.operations, fixture.client);
    Check(noLock.ok == false && noLock.error == L"缺少 watcher-lock.json。", "lock: missing");
    MoveFileW(backup.c_str(), lockPath.c_str());
}

void TestScratchCandidate(const Fixture& fixture) {
    const std::wstring candidate = Join(fixture.product, L"_scratch\\candidate\\window_watch.exe");
    WriteLockFile(fixture.product, HashText(candidate), HashText(Join(fixture.product, L"native\\window_watch.cpp")),
                  HashText(Join(fixture.product, L"native\\window_layer.hpp")));
    const WindowWatchStartResult started =
        StartWindowWatch(fixture.product, fixture.operations, fixture.client, candidate);
    Check(started.ok, "scratch: started");
    const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, started.pid);
    if (process != nullptr) {
        TerminateProcess(process, 1);
        CloseHandle(process);
    }
    WriteLockFile(fixture.product);
}

void TestPortableWithoutSources(const Fixture& fixture) {
    // 便携包不带 native 源码：两份源码都缺失时应只核对程序哈希。
    const std::wstring portable = Join(fixture.root, L"portable-product");
    MakeDirectory(portable);
    MakeDirectory(Join(portable, L"bin"));
    CopyFileW(Join(fixture.product, L"bin\\window_watch.exe").c_str(),
              Join(portable, L"bin\\window_watch.exe").c_str(), FALSE);
    WriteLockFile(portable, HashText(Join(portable, L"bin\\window_watch.exe")), std::string(64, 'd'),
                  std::string(64, 'e'));
    const std::wstring operations = Join(fixture.root, L"operations-portable");
    MakeDirectory(operations);
    const WindowWatchStartResult started = StartWindowWatch(portable, operations, fixture.client);
    Check(started.ok, "portable: started without sources");
    Check(FileExists(Join(started.logDir, L"stdout.log")), "portable: stdout log");
    Check(started.message == L"助手已启动；是否处理成功须查看日志并进行游戏验收。", "portable: message");
    if (started.pid != 0) {
        const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, started.pid);
        if (process != nullptr) {
            TerminateProcess(process, 1);
            CloseHandle(process);
        }
    }

    // 只带一份源码时仍按开发目录严格核对，缺另一份视为身份不符。
    const std::wstring partial = Join(fixture.root, L"partial-product");
    MakeDirectory(partial);
    MakeDirectory(Join(partial, L"bin"));
    MakeDirectory(Join(partial, L"native"));
    CopyFileW(Join(fixture.product, L"bin\\window_watch.exe").c_str(),
              Join(partial, L"bin\\window_watch.exe").c_str(), FALSE);
    WriteBytes(Join(partial, L"native\\window_watch.cpp"), "// window watch source\n");
    WriteLockFile(partial, HashText(Join(partial, L"bin\\window_watch.exe")),
                  HashText(Join(partial, L"native\\window_watch.cpp")), std::string(64, 'f'));
    const std::wstring partialOperations = Join(fixture.root, L"operations-partial");
    MakeDirectory(partialOperations);
    const WindowWatchStartResult rejected = StartWindowWatch(partial, partialOperations, fixture.client);
    Check(rejected.ok == false && rejected.error == L"窗口助手或源码身份不符。", "portable: partial sources rejected");
}

void TestMutualExclusion(const Fixture& fixture) {
    const std::wstring watcher = Join(fixture.product, L"bin\\window_watch.exe");
    std::wstring command = L"\"" + watcher + L"\" /c ping -n 20 127.0.0.1 > nul";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(watcher.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        fixture.product.c_str(), &startup, &process)) {
        Fail("mutex: start fixture process");
        return;
    }
    Sleep(200);
    const WindowWatchStartResult started = StartWindowWatch(fixture.product, fixture.operations, fixture.client);
    Check(started.ok == false && started.error == L"窗口助手候选已在运行。", "mutex: rejected");
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Sleep(200);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: window-watch.exe <output-root>\n");
        return 2;
    }
    Fixture fixture;
    fixture.root = Join(argv[1], L"window-watch");
    MakeDirectory(fixture.root);
    if (!CreateFixture(fixture)) {
        std::fprintf(stderr, "window watch: fixture setup failed\n");
        return 1;
    }
    TestSuccess(fixture);
    TestConfigGate(fixture);
    TestClientGuard(fixture);
    TestPathGuard(fixture);
    TestLockMismatch(fixture);
    TestScratchCandidate(fixture);
    TestPortableWithoutSources(fixture);
    TestMutualExclusion(fixture);
    if (gFailures != 0) {
        std::fprintf(stderr, "window watch: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("window watch: config gate, path/lock guards, mutex and hidden start passed\n");
    return 0;
}
