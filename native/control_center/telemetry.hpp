// 运行时遥测：AI0 载体共享内存 + 进程关联（阶段二）
//
// 设计依据：旧控制中心 Dlss5.Core.psm1 的 Read-AioTelemetry（Core:860-905）与
// ControlCenter.ps1 的进程关联（CC:988-1010）。共享内存 ABI 固定为 320 字节，
// 写入端先置奇数序号、写字段、再置偶数序号；读取端必须前后序号一致才接受快照。
//
// 本文件只做读取与关联，不做 UI、不写文件、不做缓存；界面层负责采样节奏与
// 丢弃过期结果。所有中文字符串为界面/日志直接显示用。
#pragma once

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace control_center {

constexpr std::uint32_t kTelemetryMagic = 0x4F494144u;  // 小端 ASCII "DAIO"
constexpr std::uint32_t kTelemetryVersion = 1u;
constexpr std::uint32_t kTelemetrySize = 320u;
constexpr int kTelemetryReadAttempts = 20;

// 320 字节遥测结构（小端、紧凑排布）。字段偏移与旧读取实现完全一致，
// 多余区间用保留字节占位，保证 sizeof 恰为 320。
#pragma pack(push, 1)
struct TelemetryV1 {
    std::uint32_t magic;            // 0
    std::uint32_t version;          // 4
    std::uint32_t size;             // 8
    std::uint32_t pid;              // 12
    std::int64_t sequence;          // 16 偶数表示稳定
    std::int64_t timestamp;         // 24 QueryPerformanceCounter 基准
    std::int64_t frequency;         // 32 对应频率
    std::uint8_t reserved40[8];     // 40
    std::uint32_t flags;            // 48
    std::uint8_t reserved52[4];     // 52
    std::uint32_t inputWidth;       // 56
    std::uint32_t inputHeight;      // 60
    std::uint32_t outputWidth;      // 64
    std::uint32_t outputHeight;     // 68
    std::uint32_t sourceFps;        // 72
    std::uint32_t proxyFps;         // 76
    std::uint32_t activeNrModel;    // 80
    std::uint8_t reserved84[32];    // 84
    std::uint32_t gpuTotalUs;       // 116
    std::uint8_t reserved120[32];   // 120
    std::uint32_t sourceFrameAverageUs;  // 152
    std::uint8_t reserved156[44];   // 156
    std::uint64_t nrFrames;         // 200
    std::uint64_t srFrames;         // 208
    std::uint64_t fgFrames;         // 216
    std::uint64_t framesPresented;  // 224
    std::uint8_t reserved232[88];   // 232
};
#pragma pack(pop)

static_assert(sizeof(TelemetryV1) == kTelemetrySize, "遥测结构必须恰为 320 字节");

// flags 位定义（与旧界面一致）。
constexpr std::uint32_t kTelemetryNrEnabled = 1u << 1;              // 2
constexpr std::uint32_t kTelemetryFrameGeneration = 1u << 2;        // 4
constexpr std::uint32_t kTelemetryPipelineReady = 1u << 3;          // 8
constexpr std::uint32_t kTelemetryProcessedOutputSelected = 1u << 5;  // 32
constexpr std::uint32_t kTelemetryProxyHidden = 1u << 8;            // 256

struct TelemetrySample {
    bool available = false;
    std::string reason;            // 不可用原因（中文，界面直接显示）
    double ageSeconds = 0.0;       // 快照距离读取时刻的秒数
    double timestampSeconds = 0.0; // 快照 timestamp/frequency（QPC 秒）
    std::uint32_t flags = 0;
    std::uint32_t inputWidth = 0;
    std::uint32_t inputHeight = 0;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t sourceFps = 0;
    std::uint32_t proxyFps = 0;
    std::uint32_t activeNrModel = 0;
    std::uint32_t gpuTotalUs = 0;
    std::uint32_t sourceFrameAverageUs = 0;
    std::uint64_t nrFrames = 0;
    std::uint64_t srFrames = 0;
    std::uint64_t fgFrames = 0;
    std::uint64_t framesPresented = 0;

    bool NrEnabled() const { return (flags & kTelemetryNrEnabled) != 0; }
    bool FrameGeneration() const { return (flags & kTelemetryFrameGeneration) != 0; }
    bool PipelineReady() const { return (flags & kTelemetryPipelineReady) != 0; }
    bool ProcessedOutputSelected() const {
        return (flags & kTelemetryProcessedOutputSelected) != 0;
    }
    bool ProxyHidden() const { return (flags & kTelemetryProxyHidden) != 0; }
};

namespace telemetry_detail {

inline std::wstring TelemetryMappingName(std::uint32_t processId) {
    return L"Local\\DLSS5_AIO_Telemetry_" + std::to_wstring(processId);
}

// 读取前后序号一致才接受：与旧实现的 20 次重试语义一致。
inline bool ReadStableSnapshot(HANDLE mapping, TelemetryV1& out) {
    for (int attempt = 0; attempt < kTelemetryReadAttempts; ++attempt) {
        const void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, kTelemetrySize);
        if (view == nullptr) return false;
        TelemetryV1 first;
        std::memcpy(&first, view, sizeof(first));
        if ((first.sequence & 1) != 0) {  // 写入中：短暂让出后重试
            ::UnmapViewOfFile(view);
            ::SwitchToThread();
            continue;
        }
        TelemetryV1 second;
        std::memcpy(&second, view, sizeof(second));
        ::UnmapViewOfFile(view);
        if (first.sequence != second.sequence || (second.sequence & 1) != 0) continue;
        out = second;
        return true;
    }
    return false;
}

}  // namespace telemetry_detail

// 读取指定载体进程的遥测快照。失败时 available=false 并给出中文原因，
// 不抛异常（与旧 Read-AioTelemetry 的返回约定一致）。
inline TelemetrySample ReadAioTelemetry(std::uint32_t processId) {
    TelemetrySample sample;
    if (processId == 0) {
        sample.reason = "载体进程无效。";
        return sample;
    }
    const std::wstring name = telemetry_detail::TelemetryMappingName(processId);
    HANDLE mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (mapping == nullptr) {
        sample.reason = "载体未上报遥测（共享内存不存在）。";
        return sample;
    }
    TelemetryV1 raw{};
    const bool stable = telemetry_detail::ReadStableSnapshot(mapping, raw);
    ::CloseHandle(mapping);
    if (!stable) {
        sample.reason = "无法取得稳定的遥测快照。";
        return sample;
    }
    if (raw.magic != kTelemetryMagic || raw.version != kTelemetryVersion ||
        raw.size < kTelemetrySize || raw.pid != processId) {
        sample.reason = "遥测身份或版本不符。";
        return sample;
    }
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (!::QueryPerformanceFrequency(&frequency) || !::QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0) {
        sample.reason = "无法读取本机时钟。";
        return sample;
    }
    if (raw.timestamp <= 0 || raw.frequency <= 0) {
        sample.reason = "遥测时钟无效。";
        return sample;
    }
    const double timestampSeconds =
        static_cast<double>(raw.timestamp) / static_cast<double>(raw.frequency);
    const double nowSeconds =
        static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
    const double age = nowSeconds - timestampSeconds;
    if (age < -0.1 || age > 3.0) {
        sample.reason = "遥测未在最近三秒更新。";
        return sample;
    }
    sample.available = true;
    sample.ageSeconds = age;
    sample.timestampSeconds = timestampSeconds;
    sample.flags = raw.flags;
    sample.inputWidth = raw.inputWidth;
    sample.inputHeight = raw.inputHeight;
    sample.outputWidth = raw.outputWidth;
    sample.outputHeight = raw.outputHeight;
    sample.sourceFps = raw.sourceFps;
    sample.proxyFps = raw.proxyFps;
    sample.activeNrModel = raw.activeNrModel;
    sample.gpuTotalUs = raw.gpuTotalUs;
    sample.sourceFrameAverageUs = raw.sourceFrameAverageUs;
    sample.nrFrames = raw.nrFrames;
    sample.srFrames = raw.srFrames;
    sample.fgFrames = raw.fgFrames;
    sample.framesPresented = raw.framesPresented;
    return sample;
}

// 进程关联：找出属于当前客户端的游戏进程与 AIO 载体进程。
// 与旧实现一致：路径不可读或与客户端不符的匹配进程计入 unidentified，
// 不静默忽略；只有 host 唯一且 unidentified==0 时才允许读取遥测。
struct ProcessAssociation {
    std::vector<std::uint32_t> games;
    std::vector<std::uint32_t> hosts;
    int unidentified = 0;
    std::string error;  // 快照枚举失败时的中文原因
};

namespace telemetry_detail {

inline bool SamePath(const std::wstring& left, const std::wstring& right) {
    return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

inline std::wstring QueryImagePath(HANDLE process) {
    wchar_t buffer[1024];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!::QueryFullProcessImageNameW(process, 0, buffer, &size)) return std::wstring();
    return std::wstring(buffer, size);
}

}  // namespace telemetry_detail

inline ProcessAssociation AssociateClientProcesses(const std::wstring& clientRoot) {
    ProcessAssociation result;
    const std::wstring gamePath = clientRoot + L"\\wow.exe";
    const std::wstring hostPath = clientRoot + L"\\host64\\AIO DLSS5 32-bit Wrapper.exe";
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        result.error = "无法枚举进程。";
        return result;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool ok = ::Process32FirstW(snapshot, &entry) != FALSE;
    while (ok) {
        const bool isGame =
            ::_wcsicmp(entry.szExeFile, L"wow.exe") == 0;
        const bool isHost =
            ::_wcsicmp(entry.szExeFile, L"AIO DLSS5 32-bit Wrapper.exe") == 0;
        if (isGame || isHost) {
            HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr) {
                ++result.unidentified;
            } else {
                const std::wstring path = telemetry_detail::QueryImagePath(process);
                ::CloseHandle(process);
                if (path.empty()) {
                    ++result.unidentified;
                } else if (isGame) {
                    if (telemetry_detail::SamePath(path, gamePath)) {
                        result.games.push_back(entry.th32ProcessID);
                    } else {
                        ++result.unidentified;
                    }
                } else {
                    if (telemetry_detail::SamePath(path, hostPath)) {
                        result.hosts.push_back(entry.th32ProcessID);
                    } else {
                        ++result.unidentified;
                    }
                }
            }
        }
        entry.dwSize = sizeof(entry);
        ok = ::Process32NextW(snapshot, &entry) != FALSE;
    }
    ::CloseHandle(snapshot);
    return result;
}

}  // namespace control_center
