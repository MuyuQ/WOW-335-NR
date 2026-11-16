// WOW-335-NR 原生控制中心：诊断导出。
//
// 对应旧实现 Export-Dlss5Diagnostics（Core:1030-1052）：把 Get-Dlss5Status 的只读
// 快照写成 JSON。相对旧实现的差异：
//   - 不写客户端绝对路径（client 字段固定为脱敏字面量 “<所选客户端>”）；
//   - 显卡信息由调用方提供（DXGI 枚举），避免 WMI/CIM 的秒级延迟；
//   - 读写全程 UTF-8 无 BOM、末尾 \n、CreateNew + Flush，目标已存在则拒绝。
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "catalog.hpp"
#include "ds5_settings.hpp"
#include "file_identity.hpp"
#include "install_preview.hpp"  // HexSha256
#include "telemetry.hpp"

namespace control_center {

constexpr const wchar_t* kDiagnosticsNote = L"不包含运行时 DLL、账号、聊天、截图或原始日志。";
constexpr const wchar_t* kDiagnosticsAioVersion = L"2.2.4";
constexpr const wchar_t* kDiagnosticsReShadeVersion = L"6.8.0";
constexpr size_t kDiagnosticsMaxConfigBytes = 1u << 20;

struct DiagnosticFileEntry {
    std::wstring name;
    std::wstring status;   // 目录冲突/缺失/与候选一致/配置已变化/身份不符/架构不符
    std::wstring machine;  // 仅架构表目标且文件存在时非空
    std::wstring version;  // 仅架构表目标且文件存在时可能非空
};

struct DiagnosticsStatus {
    std::wstring gameIdentity;
    std::vector<DiagnosticFileEntry> files;
    // Config 固定有序键（空字符串 = null）。
    std::wstring neuralRendering, model, intensity, frameGeneration, nvidiaOpticalFlowMotion,
        detachedPresentation, windowedInputScaling, pipelineEnabled, classicD3d9CpuBridge, configError;
    std::vector<std::wstring> graphicsDevices;
    std::vector<std::uint32_t> gameProcesses, hostProcesses;
    size_t unidentifiedProcesses = 0;
    bool telemetryAvailable = false;
    std::wstring telemetryReason;
    TelemetrySample telemetry;
};

struct DiagnosticsWriteResult {
    bool ok = false;
    DWORD win32Error = ERROR_SUCCESS;
    std::wstring error;
    explicit operator bool() const { return ok; }
};

namespace diagnostics_detail {

inline std::wstring Widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

inline std::string Narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string bytes(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), needed, nullptr,
                        nullptr);
    return bytes;
}

inline std::string JsonEscape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const char ch : text) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char buffer[8] = {};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
                    escaped += buffer;
                } else {
                    escaped.push_back(ch);
                }
                break;
        }
    }
    return escaped;
}

inline std::string Quote(const std::wstring& text) {
    if (text.empty()) return "null";
    return "\"" + JsonEscape(Narrow(text)) + "\"";
}

inline std::wstring FormatUtcNow() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t buffer[40] = {};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth,
                  utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
    return buffer;
}

inline bool ReadWholeBytes(const std::wstring& path, std::string& bytes, DWORD& win32Error,
                           size_t maxBytes = kDiagnosticsMaxConfigBytes) {
    win32Error = ERROR_SUCCESS;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        win32Error = GetLastError();
        CloseHandle(file);
        return false;
    }
    if (static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        // 文件过大是调用方能判断的明确原因，不要留下 GetLastError 的陈旧错误码。
        win32Error = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return false;
    }
    bytes.assign(static_cast<size_t>(size.QuadPart), '\0');
    size_t total = 0;
    while (total < bytes.size()) {
        DWORD got = 0;
        if (!ReadFile(file, bytes.data() + total, static_cast<DWORD>(bytes.size() - total), &got, nullptr)) {
            win32Error = GetLastError();
            CloseHandle(file);
            return false;
        }
        if (got == 0) break;
        total += got;
    }
    bytes.resize(total);
    CloseHandle(file);
    return true;
}

inline bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

inline std::wstring Join(const std::wstring& root, std::wstring target) {
    for (wchar_t& ch : target) {
        if (ch == L'/') ch = L'\\';
    }
    if (root.empty()) return target;
    if (root.back() == L'\\') return root + target;
    return root + L"\\" + target;
}

// 读取 PE 头里的 Machine 字段：返回 L"x86"/L"x64"，读取失败返回空串。
inline std::wstring PeMachineText(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::wstring();
    unsigned char header[4096] = {};
    DWORD got = 0;
    const BOOL read = ReadFile(file, header, sizeof(header), &got, nullptr);
    CloseHandle(file);
    if (!read || got < 64) return std::wstring();
    if (header[0] != 'M' || header[1] != 'Z') return std::wstring();
    const DWORD peOffset = static_cast<DWORD>(header[0x3C]) | (static_cast<DWORD>(header[0x3D]) << 8) |
                           (static_cast<DWORD>(header[0x3E]) << 16) | (static_cast<DWORD>(header[0x3F]) << 24);
    if (peOffset + 6 >= got) return std::wstring();
    if (header[peOffset] != 'P' || header[peOffset + 1] != 'E' || header[peOffset + 2] != 0 ||
        header[peOffset + 3] != 0) {
        return std::wstring();
    }
    const unsigned short machine = static_cast<unsigned short>(header[peOffset + 4]) |
                                   static_cast<unsigned short>(header[peOffset + 5] << 8);
    if (machine == 0x014C) return L"x86";
    if (machine == 0x8664) return L"x64";
    if (machine == 0xAA64) return L"arm64";
    return L"other";
}

// 版本号：动态加载 version.dll，避免链接 version.lib（生产编译测试只链接 bcrypt.lib）。
inline std::wstring FileVersionText(const std::wstring& path) {
    using GetSizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using GetInfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPVOID, LPCWSTR, LPVOID*, PUINT);
    static HMODULE module = LoadLibraryW(L"version.dll");
    static GetSizeFn getSize =
        module ? reinterpret_cast<GetSizeFn>(GetProcAddress(module, "GetFileVersionInfoSizeW")) : nullptr;
    static GetInfoFn getInfo =
        module ? reinterpret_cast<GetInfoFn>(GetProcAddress(module, "GetFileVersionInfoW")) : nullptr;
    static QueryFn query = module ? reinterpret_cast<QueryFn>(GetProcAddress(module, "VerQueryValueW")) : nullptr;
    if (getSize == nullptr || getInfo == nullptr || query == nullptr) return std::wstring();
    DWORD ignored = 0;
    const DWORD size = getSize(path.c_str(), &ignored);
    if (size == 0) return std::wstring();
    std::vector<unsigned char> block(size);
    if (!getInfo(path.c_str(), 0, size, block.data())) return std::wstring();
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!query(block.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) || info == nullptr) {
        return std::wstring();
    }
    if (info->dwSignature != 0xFEEF04BD) return std::wstring();
    wchar_t buffer[64] = {};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%u.%u.%u.%u", HIWORD(info->dwFileVersionMS),
                  LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return buffer;
}

inline const wchar_t* ExpectedMachine(const std::wstring& target) {
    static const wchar_t* const kX86[] = {L"d3d9.dll", L"standalone-dlssnr.addon32"};
    static const wchar_t* const kX64[] = {L"host64/AIO DLSS5 32-bit Wrapper.exe",
                                          L"host64/dxgi.dll",
                                          L"host64/standalone-dlssnr.addon64",
                                          L"host64/nvngx.dll",
                                          L"host64/nvngx_dlss.dll",
                                          L"host64/nvngx_dlssnr.dll",
                                          L"host64/nvngx_dlssg.dll"};
    for (const wchar_t* name : kX86) {
        if (target == name) return L"x86";
    }
    for (const wchar_t* name : kX64) {
        if (target == name) return L"x64";
    }
    return nullptr;
}

inline bool IsConfigTarget(const std::wstring& target) {
    return target == L"host64/ReShade.ini" || target == L"dlss5-aio-x86.cfg" || target == L"ReShade.ini";
}

inline std::wstring StatusWordForFile(const std::wstring& clientRoot, const CatalogMember& member) {
    const std::wstring& target = member.target;
    const std::wstring path = Join(clientRoot, target);
    if (DirectoryExists(path)) return L"目录冲突";
    if (!FileExists(path)) return L"缺失";
    const HashResult hashed = HashFile(path);
    if (hashed && HexSha256(hashed.identity.sha256) == member.sha256) return L"与候选一致";
    if (IsConfigTarget(target)) return L"配置已变化";
    return L"身份不符";
}

inline void StripBom(std::string& bytes) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
}

inline void ReadIniConfig(const std::wstring& clientRoot, DiagnosticsStatus& status) {
    using namespace ds5_detail;
    const std::wstring iniPath = Join(clientRoot, L"host64/ReShade.ini");
    if (!FileExists(iniPath)) return;
    std::string bytes;
    DWORD error = ERROR_SUCCESS;
    if (!ReadWholeBytes(iniPath, bytes, error)) {
        status.configError = L"读取 host64\\ReShade.ini 失败。";
        return;
    }
    StripBom(bytes);
    SectionRange range;
    std::string sectionError;
    if (!FindSection(bytes, "Standalone.DLSSNR", range, sectionError)) {
        status.configError = L"配置节不存在或重复：Standalone.DLSSNR。";
        return;
    }
    const struct {
        const char* key;
        std::wstring* value;
    } keys[] = {{"NeuralRendering", &status.neuralRendering},
                {"Model", &status.model},
                {"Intensity", &status.intensity},
                {"FrameGeneration", &status.frameGeneration},
                {"NvidiaOpticalFlowMotion", &status.nvidiaOpticalFlowMotion},
                {"DetachedPresentation", &status.detachedPresentation},
                {"WindowedInputScaling", &status.windowedInputScaling}};
    for (const auto& key : keys) {
        std::string raw;
        bool present = false;
        std::string keyError;
        if (!GetIniValue(bytes, range, key.key, raw, present, keyError)) {
            status.configError = Widen(keyError);
            return;
        }
        if (present) *key.value = Widen(raw);
    }
}

// 收集 cfg 中某个键的全部出现值；返回出现次数。
inline size_t CollectCfgValues(const std::string& text, const char* key, std::vector<std::string>& values) {
    values.clear();
    size_t position = 0;
    while (position < text.size()) {
        ds5_detail::IniLine line;
        if (!ds5_detail::ReadLine(text, position, text.size(), line)) break;
        size_t valueBegin = 0, valueEnd = 0;
        if (ds5_detail::MatchKeyLine(text, line, key, valueBegin, valueEnd)) {
            values.push_back(ds5_detail::TrimBlanks(text.substr(valueBegin, valueEnd - valueBegin)));
        }
        if (line.last) break;
        position = line.nextPosition;
    }
    return values.size();
}

inline void ReadCfgConfig(const std::wstring& clientRoot, DiagnosticsStatus& status) {
    const std::wstring cfgPath = Join(clientRoot, L"dlss5-aio-x86.cfg");
    if (!FileExists(cfgPath)) return;
    std::string bytes;
    DWORD error = ERROR_SUCCESS;
    if (!ReadWholeBytes(cfgPath, bytes, error)) {
        status.configError = L"读取 dlss5-aio-x86.cfg 失败。";
        return;
    }
    StripBom(bytes);
    std::vector<std::string> values;
    if (CollectCfgValues(bytes, "enabled", values) == 1) {
        status.pipelineEnabled = ds5_detail::Widen(values[0]);
    } else if (values.size() > 1) {
        status.configError = L"AIO 管线开关重复。";
    }
    if (CollectCfgValues(bytes, "classic_d3d9_cpu_fallback", values) == 1) {
        status.classicD3d9CpuBridge = ds5_detail::Widen(values[0]);
    } else if (values.size() > 1) {
        status.configError = L"classic_d3d9_cpu_fallback 重复。";
    }
}

inline std::wstring GameIdentity(const std::wstring& clientRoot) {
    const std::wstring exe = Join(clientRoot, L"wow.exe");
    if (!FileExists(exe)) return L"缺失";
    const std::wstring machine = PeMachineText(exe);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    unsigned long long size = 0;
    if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &attributes)) {
        size = (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    }
    if (machine == L"x86") {
        wchar_t buffer[128] = {};
        std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"存在（x86，%llu 字节；不比对哈希）", size);
        return buffer;
    }
    const std::wstring text = machine.empty() ? L"未知" : machine;
    return L"存在但架构为 " + text + L"（应为 x86）";
}

inline std::string BuildJson(const DiagnosticsStatus& status) {
    std::string json;
    json += "{\n";
    json += "  \"schema\": 1,\n";
    json += "  \"generated_utc\": " + Quote(FormatUtcNow()) + ",\n";
    json += "  \"client\": \"<所选客户端>\",\n";
    json += "  \"aio_version\": " + Quote(kDiagnosticsAioVersion) + ",\n";
    json += "  \"reshade_version\": " + Quote(kDiagnosticsReShadeVersion) + ",\n";
    json += "  \"game_identity\": " + Quote(status.gameIdentity) + ",\n";
    json += "  \"files\": [";
    for (size_t i = 0; i < status.files.size(); ++i) {
        const DiagnosticFileEntry& entry = status.files[i];
        json += i == 0 ? "\n" : ",\n";
        json += "    {\"name\": " + Quote(entry.name) + ", \"status\": " + Quote(entry.status) +
                ", \"machine\": " + Quote(entry.machine) + ", \"version\": " + Quote(entry.version) + "}";
    }
    json += status.files.empty() ? "],\n" : "\n  ],\n";
    const struct {
        const char* key;
        const std::wstring* value;
    } configKeys[] = {{"NeuralRendering", &status.neuralRendering},
                      {"Model", &status.model},
                      {"Intensity", &status.intensity},
                      {"FrameGeneration", &status.frameGeneration},
                      {"NvidiaOpticalFlowMotion", &status.nvidiaOpticalFlowMotion},
                      {"DetachedPresentation", &status.detachedPresentation},
                      {"WindowedInputScaling", &status.windowedInputScaling},
                      {"PipelineEnabled", &status.pipelineEnabled},
                      {"ClassicD3D9CpuBridge", &status.classicD3d9CpuBridge},
                      {"Error", &status.configError}};
    json += "  \"config\": {\n";
    for (size_t i = 0; i < sizeof(configKeys) / sizeof(configKeys[0]); ++i) {
        json += "    \"" + std::string(configKeys[i].key) + "\": " + Quote(*configKeys[i].value);
        json += i + 1 == sizeof(configKeys) / sizeof(configKeys[0]) ? "\n" : ",\n";
    }
    json += "  },\n";
    json += "  \"graphics_devices\": [";
    for (size_t i = 0; i < status.graphicsDevices.size(); ++i) {
        json += (i == 0 ? "\n    " : ",\n    ") + Quote(status.graphicsDevices[i]);
    }
    json += status.graphicsDevices.empty() ? "],\n" : "\n  ],\n";
    json += "  \"game_processes\": [";
    for (size_t i = 0; i < status.gameProcesses.size(); ++i) {
        json += (i == 0 ? "" : ", ") + std::to_string(status.gameProcesses[i]);
    }
    json += "],\n";
    json += "  \"host_processes\": [";
    for (size_t i = 0; i < status.hostProcesses.size(); ++i) {
        json += (i == 0 ? "" : ", ") + std::to_string(status.hostProcesses[i]);
    }
    json += "],\n";
    json += "  \"unidentified_processes\": " + std::to_string(status.unidentifiedProcesses) + ",\n";
    json += "  \"telemetry\": {\"available\": ";
    json += status.telemetryAvailable ? "true" : "false";
    json += ", \"reason\": " + Quote(status.telemetryReason);
    if (status.telemetryAvailable) {
        const TelemetrySample& sample = status.telemetry;
        json += ", \"age_seconds\": " + std::to_string(sample.ageSeconds);
        json += ", \"flags\": " + std::to_string(sample.flags);
        json += ", \"input_width\": " + std::to_string(sample.inputWidth);
        json += ", \"input_height\": " + std::to_string(sample.inputHeight);
        json += ", \"output_width\": " + std::to_string(sample.outputWidth);
        json += ", \"output_height\": " + std::to_string(sample.outputHeight);
        json += ", \"source_fps\": " + std::to_string(sample.sourceFps);
        json += ", \"proxy_fps\": " + std::to_string(sample.proxyFps);
        json += ", \"active_nr_model\": " + std::to_string(sample.activeNrModel);
        json += ", \"gpu_total_us\": " + std::to_string(sample.gpuTotalUs);
        json += ", \"source_frame_average_us\": " + std::to_string(sample.sourceFrameAverageUs);
        json += ", \"nr_frames\": " + std::to_string(sample.nrFrames);
        json += ", \"sr_frames\": " + std::to_string(sample.srFrames);
        json += ", \"fg_frames\": " + std::to_string(sample.fgFrames);
        json += ", \"frames_presented\": " + std::to_string(sample.framesPresented);
    }
    json += "},\n";
    json += "  \"note\": " + Quote(kDiagnosticsNote) + "\n";
    json += "}\n";
    return json;
}

}  // namespace diagnostics_detail

// 构建只读状态快照（文件状态 + 配置 + 游戏身份 + 调用方提供的进程/GPU/遥测数据）。
inline DiagnosticsStatus BuildDiagnosticsStatus(const std::wstring& clientRoot, const CatalogResult& catalog,
                                                const std::vector<std::uint32_t>& gameProcesses,
                                                const std::vector<std::uint32_t>& hostProcesses,
                                                size_t unidentifiedProcesses, const TelemetrySample& telemetry,
                                                const std::vector<std::wstring>& graphicsDevices) {
    using namespace diagnostics_detail;
    DiagnosticsStatus status;
    status.gameIdentity = GameIdentity(clientRoot);
    for (const CatalogMember& member : catalog.members) {
        DiagnosticFileEntry entry;
        entry.name = member.target;
        entry.status = StatusWordForFile(clientRoot, member);
        const wchar_t* expected = ExpectedMachine(entry.name);
        if (expected != nullptr && entry.status != L"缺失" && entry.status != L"目录冲突") {
            entry.machine = PeMachineText(Join(clientRoot, entry.name));
            if (!entry.machine.empty() && entry.machine != expected) entry.status = L"架构不符";
            entry.version = FileVersionText(Join(clientRoot, entry.name));
        }
        status.files.push_back(std::move(entry));
    }
    ReadIniConfig(clientRoot, status);
    ReadCfgConfig(clientRoot, status);
    status.graphicsDevices = graphicsDevices;
    status.gameProcesses = gameProcesses;
    status.hostProcesses = hostProcesses;
    status.unidentifiedProcesses = unidentifiedProcesses;
    status.telemetryAvailable = telemetry.available;
    status.telemetryReason = Widen(telemetry.reason);
    status.telemetry = telemetry;
    return status;
}

inline DiagnosticsWriteResult WriteDiagnosticsFile(const std::wstring& path, const DiagnosticsStatus& status) {
    using namespace diagnostics_detail;
    DiagnosticsWriteResult result;
    if (path.empty()) {
        result.error = L"诊断文件路径为空。";
        return result;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        result.error = L"诊断文件已存在。";
        return result;
    }
    const std::string json = BuildJson(status);
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.win32Error = GetLastError();
        result.error = L"无法创建诊断文件。";
        return result;
    }
    size_t total = 0;
    bool wrote = true;
    while (total < json.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(json.size() - total);
        if (!WriteFile(file, json.data() + total, chunk, &written, nullptr) || written == 0) {
            wrote = false;
            break;
        }
        total += written;
    }
    const BOOL flushed = wrote && FlushFileBuffers(file);
    if (!wrote || !flushed) result.win32Error = GetLastError();
    CloseHandle(file);
    if (!wrote || !flushed) {
        DeleteFileW(path.c_str());
        result.error = L"写入诊断文件失败。";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace control_center
