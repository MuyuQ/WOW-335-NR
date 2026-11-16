// 遥测共享内存读取与进程关联的单元测试。
//
// 覆盖：共享内存不存在 / pid 无效 / 正常快照字段与 flags / 身份（magic、version、
// size、pid）校验 / 时钟无效 / 过期与超前快照 / 写入中（奇数序号）拒绝 /
// 进程关联的正向匹配、路径不符计入 unidentified、进程退出后清空。
#include "../native/control_center/telemetry.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace control_center;

int gFailures = 0;

void Fail(const char* what) {
    std::fprintf(stderr, "telemetry test failed: %s\n", what);
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

std::wstring SystemExecutable(const wchar_t* name) {
    wchar_t root[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(root, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return name;
    return Join(std::wstring(root, length), name);
}

bool CopyExecutable(const std::wstring& source, const std::wstring& target) {
    return CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE;
}

// 页文件支持的 320 字节共享内存块，模拟载体写入端。
struct SharedBlock {
    HANDLE mapping = nullptr;
    TelemetryV1* view = nullptr;
    std::wstring name;

    bool Create(std::uint32_t pid) {
        name = telemetry_detail::TelemetryMappingName(pid);
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kTelemetrySize, name.c_str());
        if (mapping == nullptr) return false;
        view = static_cast<TelemetryV1*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, kTelemetrySize));
        return view != nullptr;
    }

    void FillSample(std::uint32_t pid) {
        std::memset(view, 0, sizeof(*view));
        LARGE_INTEGER frequency{};
        LARGE_INTEGER counter{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        view->magic = kTelemetryMagic;
        view->version = kTelemetryVersion;
        view->size = kTelemetrySize;
        view->pid = pid;
        view->sequence = 2;
        view->frequency = frequency.QuadPart;
        view->timestamp = counter.QuadPart - frequency.QuadPart / 100;  // 10ms 前
        view->flags = kTelemetryNrEnabled | kTelemetryFrameGeneration | kTelemetryPipelineReady |
                      kTelemetryProcessedOutputSelected;
        view->inputWidth = 1920;
        view->inputHeight = 1080;
        view->outputWidth = 2560;
        view->outputHeight = 1440;
        view->sourceFps = 60;
        view->proxyFps = 120;
        view->activeNrModel = 2;
        view->gpuTotalUs = 8000;
        view->sourceFrameAverageUs = 16000;
        view->nrFrames = 600;
        view->srFrames = 2400;
        view->fgFrames = 1200;
        view->framesPresented = 1200;
    }

    ~SharedBlock() {
        if (view != nullptr) UnmapViewOfFile(view);
        if (mapping != nullptr) CloseHandle(mapping);
    }
};

std::uint32_t SelfPid() {
    return static_cast<std::uint32_t>(GetCurrentProcessId());
}

struct ChildProcess {
    PROCESS_INFORMATION info{};
    bool started = false;

    bool Start(const std::wstring& exe, const wchar_t* arguments) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        std::wstring command = L"\"" + exe + L"\" " + arguments;
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(L'\0');
        started = CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup, &info) != FALSE;
        return started;
    }

    void Stop() {
        if (!started) return;
        if (info.hProcess != nullptr) {
            TerminateProcess(info.hProcess, 0);
            WaitForSingleObject(info.hProcess, 5000);
            CloseHandle(info.hProcess);
            info.hProcess = nullptr;
        }
        if (info.hThread != nullptr) {
            CloseHandle(info.hThread);
            info.hThread = nullptr;
        }
        started = false;
    }

    ~ChildProcess() { Stop(); }
};

bool ContainsPid(const std::vector<std::uint32_t>& values, std::uint32_t pid) {
    for (const std::uint32_t value : values) {
        if (value == pid) return true;
    }
    return false;
}

void TestMissingAndInvalidPid() {
    const TelemetrySample missing = ReadAioTelemetry(999999u);
    Check(!missing.available, "missing mapping must be unavailable");
    Check(missing.reason == "载体未上报遥测（共享内存不存在）。", "missing mapping reason");

    const TelemetrySample invalid = ReadAioTelemetry(0u);
    Check(!invalid.available, "zero pid must be unavailable");
    Check(invalid.reason == "载体进程无效。", "zero pid reason");
}

void TestValidSnapshot() {
    SharedBlock block;
    Check(block.Create(SelfPid()), "create shared block");
    if (block.view == nullptr) return;
    block.FillSample(SelfPid());

    const TelemetrySample sample = ReadAioTelemetry(SelfPid());
    Check(sample.available, "valid snapshot must be available");
    Check(sample.reason.empty(), "valid snapshot has no reason");
    Check(sample.inputWidth == 1920 && sample.inputHeight == 1080, "input dimensions");
    Check(sample.outputWidth == 2560 && sample.outputHeight == 1440, "output dimensions");
    Check(sample.sourceFps == 60 && sample.proxyFps == 120, "fps fields");
    Check(sample.activeNrModel == 2, "active model");
    Check(sample.gpuTotalUs == 8000, "gpu total");
    Check(sample.sourceFrameAverageUs == 16000, "frame average");
    Check(sample.nrFrames == 600 && sample.srFrames == 2400, "nr/sr counters");
    Check(sample.fgFrames == 1200 && sample.framesPresented == 1200, "fg/presented counters");
    Check(sample.NrEnabled(), "nr flag");
    Check(sample.FrameGeneration(), "frame generation flag");
    Check(sample.PipelineReady(), "pipeline flag");
    Check(sample.ProcessedOutputSelected(), "processed output flag");
    Check(!sample.ProxyHidden(), "proxy hidden flag off");
    Check(sample.ageSeconds >= -0.1 && sample.ageSeconds <= 3.0, "age within window");
    Check(sample.timestampSeconds > 0.0, "timestamp seconds");
}

void TestIdentityRejections() {
    SharedBlock block;
    Check(block.Create(SelfPid()), "create shared block for identity");
    if (block.view == nullptr) return;
    block.FillSample(SelfPid());

    block.view->magic = 0x12345678u;
    Check(ReadAioTelemetry(SelfPid()).reason == "遥测身份或版本不符。", "wrong magic");
    block.FillSample(SelfPid());

    block.view->version = kTelemetryVersion + 1;
    Check(ReadAioTelemetry(SelfPid()).reason == "遥测身份或版本不符。", "wrong version");
    block.FillSample(SelfPid());

    block.view->size = 64;
    Check(ReadAioTelemetry(SelfPid()).reason == "遥测身份或版本不符。", "wrong size");
    block.FillSample(SelfPid());

    block.view->pid = SelfPid() + 1;
    Check(ReadAioTelemetry(SelfPid()).reason == "遥测身份或版本不符。", "wrong pid");
    block.FillSample(SelfPid());

    block.view->timestamp = 0;
    Check(ReadAioTelemetry(SelfPid()).reason == "遥测时钟无效。", "zero timestamp");
    block.FillSample(SelfPid());

    block.view->frequency = 0;
    Check(ReadAioTelemetry(SelfPid()).reason == "遥测时钟无效。", "zero frequency");
    block.FillSample(SelfPid());

    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);

    block.view->timestamp = counter.QuadPart - 5 * frequency.QuadPart;
    Check(ReadAioTelemetry(SelfPid()).reason == "遥测未在最近三秒更新。", "stale timestamp");
    block.FillSample(SelfPid());

    block.view->timestamp = counter.QuadPart + 5 * frequency.QuadPart;
    Check(ReadAioTelemetry(SelfPid()).reason == "遥测未在最近三秒更新。", "future timestamp");
    block.FillSample(SelfPid());

    // 序号恒为奇数：模拟写入端一直处于写入中，20 次重试后必须放弃。
    block.view->sequence = 1;
    Check(ReadAioTelemetry(SelfPid()).reason == "无法取得稳定的遥测快照。", "odd sequence rejected");
}

void TestProcessAssociation(const std::wstring& root) {
    const std::wstring client = Join(root, L"client");
    const std::wstring hostDir = Join(client, L"host64");
    const std::wstring decoy = Join(root, L"decoy");
    const std::wstring decoyHostDir = Join(decoy, L"host64");
    Check(MakeDirectory(root), "make root");
    Check(MakeDirectory(client), "make client dir");
    Check(MakeDirectory(hostDir), "make host dir");
    Check(MakeDirectory(decoy), "make decoy");
    Check(MakeDirectory(decoyHostDir), "make decoy host dir");

    const std::wstring systemCmd = SystemExecutable(L"cmd.exe");
    Check(CopyExecutable(systemCmd, Join(client, L"wow.exe")), "copy fake wow.exe");
    Check(CopyExecutable(systemCmd, Join(hostDir, L"AIO DLSS5 32-bit Wrapper.exe")), "copy fake wrapper.exe");
    Check(CopyExecutable(systemCmd, Join(decoy, L"wow.exe")), "copy decoy wow.exe");

    // 无进程运行时：关联结果应为空且没有无法识别的匹配进程。
    ProcessAssociation idle = AssociateClientProcesses(client);
    Check(idle.error.empty(), "idle association has no error");
    Check(idle.games.empty() && idle.hosts.empty(), "idle association empty");
    Check(idle.unidentified == 0, "idle association has no unidentified");

    ChildProcess game;
    ChildProcess host;
    Check(game.Start(Join(client, L"wow.exe"), L"/c ping -n 60 127.0.0.1 > nul"), "start fake game");
    Check(host.Start(Join(hostDir, L"AIO DLSS5 32-bit Wrapper.exe"), L"/c ping -n 60 127.0.0.1 > nul"),
          "start fake host");
    if (game.started && host.started) {
        const std::uint32_t gamePid = game.info.dwProcessId;
        const std::uint32_t hostPid = host.info.dwProcessId;
        ProcessAssociation running = AssociateClientProcesses(client);
        Check(running.error.empty(), "running association has no error");
        Check(ContainsPid(running.games, gamePid), "running association finds game");
        Check(ContainsPid(running.hosts, hostPid), "running association finds host");
        Check(running.unidentified == 0, "running association identifies all matches");

        // 另一个客户端根下：同名进程存在但路径不符，必须计入 unidentified。
        ProcessAssociation other = AssociateClientProcesses(decoy);
        Check(other.games.empty(), "decoy association has no game");
        Check(other.hosts.empty(), "decoy association has no host");
        Check(other.unidentified >= 2, "decoy association counts unmatched processes");
    } else {
        Fail("start fake processes");
    }

    game.Stop();
    host.Stop();

    ProcessAssociation after = AssociateClientProcesses(client);
    Check(after.games.empty() && after.hosts.empty(), "association empty after exit");
    Check(after.unidentified == 0, "association identifies nothing after exit");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring root;
    if (argc >= 2) {
        root = argv[1];
    } else {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetCurrentDirectoryW(MAX_PATH, buffer);
        root.assign(buffer, length);
    }
    if (!MakeDirectory(root)) {
        std::fprintf(stderr, "telemetry test failed: cannot create root\n");
        return 1;
    }

    TestMissingAndInvalidPid();
    TestValidSnapshot();
    TestIdentityRejections();
    TestProcessAssociation(Join(root, L"telemetry"));

    if (gFailures != 0) {
        std::fprintf(stderr, "telemetry: %d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("telemetry: shared memory snapshot, flags, identity/stale rejection and process association passed\n");
    return 0;
}
