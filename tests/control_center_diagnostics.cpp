#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../native/control_center/catalog.hpp"
#include "../native/control_center/diagnostics_export.hpp"
#include "../native/control_center/file_identity.hpp"
#include "../native/control_center/telemetry.hpp"

namespace {
int gFailures = 0;
void Fail(const char* label) { std::printf("FAIL %s\n", label); ++gFailures; }
void Check(bool condition, const char* label) { if (!condition) Fail(label); }
std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + (a.back() == L'\\' ? L"" : L"\\") + b;
}
bool MakeDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}
bool WriteAll(const std::wstring& path, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size();
    CloseHandle(file);
    return ok;
}
bool ReadAll(const std::wstring& path, std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return false; }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes.empty() ? nullptr : &bytes[0], static_cast<DWORD>(bytes.size()), &read,
                             nullptr) != 0;
    CloseHandle(file);
    return ok && read == bytes.size();
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
    return out;
}
bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
bool HasW(const std::wstring& text, const wchar_t* needle) { return text.find(needle) != std::wstring::npos; }

// 最小 PE 头：只包含 MZ、e_lfanew、PE 签名与 Machine，足够让 PeMachineText 识别架构。
std::string PeImage(std::uint16_t machine) {
    std::string bytes(0x100, '\0');
    bytes[0] = 'M';
    bytes[1] = 'Z';
    const std::uint32_t lfanew = 0x40;
    std::memcpy(&bytes[0x3C], &lfanew, sizeof(lfanew));
    bytes[0x40] = 'P';
    bytes[0x41] = 'E';
    std::memcpy(&bytes[0x44], &machine, sizeof(machine));
    return bytes;
}

std::string ShaOf(const std::wstring& path) {
    const control_center::HashResult hashed = control_center::HashFile(path);
    if (!hashed) return std::string();
    return control_center::HexSha256(hashed.identity.sha256);
}

const char* kReShadeIni =
    "[Standalone.DLSSNR]\r\n"
    "NeuralRendering=1\r\n"
    "Model=2\r\n"
    "Intensity=0.55\r\n"
    "FrameGeneration=0\r\n"
    "NvidiaOpticalFlowMotion=1\r\n"
    "DetachedPresentation=1\r\n"
    "WindowedInputScaling=1\r\n";

const char* kCfg =
    "[aio]\r\n"
    "enabled=1\r\n"
    "classic_d3d9_cpu_fallback=0\r\n";

control_center::CatalogResult BuildCatalog(const std::wstring& client) {
    control_center::CatalogResult catalog;
    auto add = [&](const wchar_t* target, const std::string& sha, bool replaces, bool required) {
        control_center::CatalogMember member;
        member.target = target;
        member.source = L"stage\\" + std::wstring(target);
        member.sourceKind = "stage";
        member.sha256 = sha;
        member.replacesOriginal = replaces;
        member.required = required;
        catalog.members.push_back(member);
    };
    add(L"d3d9.dll", ShaOf(Join(client, L"d3d9.dll")), true, true);
    add(L"host64/AIO DLSS5 32-bit Wrapper.exe",
        ShaOf(Join(client, L"host64\\AIO DLSS5 32-bit Wrapper.exe")), false, true);
    add(L"host64/nvngx.dll",
        "0000000000000000000000000000000000000000000000000000000000000000", false, true);
    add(L"host64/nvngx_dlss.dll",
        "1111111111111111111111111111111111111111111111111111111111111111", false, false);
    add(L"host64/ReShade.ini",
        "2222222222222222222222222222222222222222222222222222222222222222", false, true);
    return catalog;
}

void TestStatusAndWrite() {
    const std::wstring root = L"D:\\WOW-335-CN\\_scratch\\dlss5-aio\\diagnostics-test";
    const std::wstring client = Join(root, L"client");
    MakeDirectory(root);
    MakeDirectory(client);
    MakeDirectory(Join(client, L"host64"));
    Check(WriteAll(Join(client, L"wow.exe"), PeImage(0x014C)), "write wow.exe");
    Check(WriteAll(Join(client, L"d3d9.dll"), PeImage(0x014C)), "write d3d9.dll");
    Check(WriteAll(Join(client, L"host64\\AIO DLSS5 32-bit Wrapper.exe"), PeImage(0x8664)), "write wrapper");
    Check(WriteAll(Join(client, L"host64\\nvngx.dll"), PeImage(0x014C)), "write nvngx x86");
    Check(WriteAll(Join(client, L"host64\\ReShade.ini"), kReShadeIni), "write reshade");
    Check(WriteAll(Join(client, L"dlss5-aio-x86.cfg"), kCfg), "write cfg");

    const control_center::CatalogResult catalog = BuildCatalog(client);
    std::vector<std::uint32_t> games{1234};
    std::vector<std::uint32_t> hosts{5678};
    std::vector<std::wstring> devices{L"RTX 5060 Ti", L"Microsoft Basic Render Driver"};
    const control_center::TelemetrySample telemetry;  // 缺省：不可用，reason 为空
    const control_center::DiagnosticsStatus status =
        control_center::BuildDiagnosticsStatus(client, catalog, games, hosts, 2, telemetry, devices);

    Check(HasW(status.gameIdentity, L"存在（x86，"), "game identity x86");
    Check(status.files.size() == 5, "files size");
    Check(status.files[0].status == L"与候选一致", "d3d9 candidate match");
    Check(status.files[0].machine == L"x86", "d3d9 machine");
    Check(status.files[1].status == L"与候选一致" && status.files[1].machine == L"x64", "wrapper machine");
    Check(status.files[2].status == L"架构不符", "nvngx arch mismatch");
    Check(status.files[3].status == L"缺失", "nvngx_dlss missing");
    Check(status.files[4].status == L"配置已变化", "reshade changed");
    Check(status.neuralRendering == L"1", "neural rendering");
    Check(status.model == L"2", "model");
    Check(status.intensity == L"0.55", "intensity");
    Check(status.frameGeneration == L"0", "frame generation");
    Check(status.detachedPresentation == L"1", "detached");
    Check(status.windowedInputScaling == L"1", "input scaling");
    Check(status.pipelineEnabled == L"1", "pipeline enabled");
    Check(status.classicD3d9CpuBridge == L"0", "classic bridge");
    Check(status.configError.empty(), "config error empty");

    const std::wstring output = Join(root, L"dlss5-diagnostic.json");
    DeleteFileW(output.c_str());  // 重复运行时清掉旧输出，保证 CREATE_NEW 分支可验证
    const control_center::DiagnosticsWriteResult written = control_center::WriteDiagnosticsFile(output, status);
    Check(written.ok, "write diagnostics");
    std::string bytes;
    Check(ReadAll(output, bytes), "read diagnostics");
    Check(!bytes.empty() && !(bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF), "no bom");
    Check(!bytes.empty() && bytes.back() == '\n', "trailing newline");
    Check(Has(bytes, "\"schema\": 1"), "schema");
    Check(Has(bytes, "\"client\": \"<所选客户端>\""), "client masked");
    Check(Has(bytes, "\"aio_version\": \"2.2.4\""), "aio version");
    Check(Has(bytes, "\"reshade_version\": \"6.8.0\""), "reshade version");
    Check(Has(bytes, "\"game_identity\": \"存在（x86，"), "game identity json");
    Check(Has(bytes, "\"files\": ["), "files array");
    Check(Has(bytes, "d3d9.dll") && Has(bytes, "与候选一致"), "file statuses");
    Check(Has(bytes, "架构不符") && Has(bytes, "配置已变化") && Has(bytes, "缺失"), "all status words");
    Check(Has(bytes, "\"NeuralRendering\": \"1\"") && Has(bytes, "\"ClassicD3D9CpuBridge\": \"0\""), "config json");
    Check(Has(bytes, "\"graphics_devices\": ["), "graphics");
    Check(Has(bytes, "RTX 5060 Ti"), "graphics content");
    Check(Has(bytes, "\"game_processes\": [1234]"), "game processes");
    Check(Has(bytes, "\"host_processes\": [5678]"), "host processes");
    Check(Has(bytes, "\"unidentified_processes\": 2"), "unidentified");
    Check(Has(bytes, "\"telemetry\": {\"available\": false"), "telemetry unavailable");
    Check(Has(bytes, "\"note\": \"不包含运行时 DLL"), "note present");
    const control_center::DiagnosticsWriteResult again = control_center::WriteDiagnosticsFile(output, status);
    Check(!again.ok && again.error == L"诊断文件已存在。", "exists rejected");
    const control_center::DiagnosticsWriteResult empty = control_center::WriteDiagnosticsFile(L"", status);
    Check(!empty.ok && empty.error == L"诊断文件路径为空。", "empty path rejected");
}

void TestCfgError() {
    const std::wstring root = L"D:\\WOW-335-CN\\_scratch\\dlss5-aio\\diagnostics-test";
    const std::wstring client = Join(root, L"client-dup");
    MakeDirectory(client);
    MakeDirectory(Join(client, L"host64"));
    Check(WriteAll(Join(client, L"host64\\ReShade.ini"), kReShadeIni), "dup write ini");
    Check(WriteAll(Join(client, L"dlss5-aio-x86.cfg"),
                   "[aio]\r\nenabled=1\r\nenabled=0\r\nclassic_d3d9_cpu_fallback=0\r\n"),
          "dup write cfg");
    control_center::CatalogResult catalog;
    std::vector<std::uint32_t> none;
    const control_center::DiagnosticsStatus status = control_center::BuildDiagnosticsStatus(
        client, catalog, none, none, 0, control_center::TelemetrySample{}, {});
    Check(status.configError == L"AIO 管线开关重复。", "cfg duplicate error");
    Check(status.pipelineEnabled.empty(), "pipeline empty on duplicate");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: diagnostics.exe <scratch-root>\n");
        return 2;
    }
    std::wstring root = argv[1];
    root += L"\\diagnostics";
    CreateDirectoryW(root.c_str(), nullptr);
    TestStatusAndWrite();
    TestCfgError();
    if (gFailures == 0) {
        std::printf("diagnostics: status words, config, json schema, masking and output guards passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", gFailures);
    return 1;
}