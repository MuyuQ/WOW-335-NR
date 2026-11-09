// WOW-335-NR 控制台：桌面端一站式程序。
// - 启动游戏、保活游戏内悬浮面板（dlss5-panel.exe）
// - 展示遥测（NR/FG 帧数、输出帧率、图层覆盖）
// - 源帧率上限读写（Config.wtf 的 SET MaxFPS / MaxFPSBackground）
// 调参由游戏内悬浮面板经 live-params 通道实时写入载体插件；本程序读同一参数模型做展示与档位识别。
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <algorithm>
#include <atomic>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "tiers.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;


// ===== 参数模型（与面板/载体一致） =====
struct Params {
    bool nr = true;
    int model = 1;
    float intensity = 1.0f, tone = 1.0f, structure = 1.0f, skin = -1.0f;
    bool fg = true;
    bool Equals(const Params &o) const {
        return nr == o.nr && model == o.model && fg == o.fg &&
            fabsf(intensity - o.intensity) < 0.005f && fabsf(tone - o.tone) < 0.005f &&
            fabsf(structure - o.structure) < 0.005f && fabsf(skin - o.skin) < 0.005f;
    }
    bool MatchesTier(const Params &o) const {
        return nr == o.nr &&
            fabsf(intensity - o.intensity) < 0.005f && fabsf(tone - o.tone) < 0.005f &&
            fabsf(structure - o.structure) < 0.005f && fabsf(skin - o.skin) < 0.005f;
    }
};
static Params g_params;
static bool g_tier_locked = true;   // zh: 出厂档=锁定配方（微调禁用），自定义=解锁可调
static Params g_custom;
static bool g_custom_valid = false;

// zh: 档位表来自 tiers.hpp（与面板/PowerShell 共用的唯一事实源）
static const struct { const char *name; Params p; } kSchemes[] = {
    { kNrTiers[0].name, { kNrTiers[0].nr,  kNrTiers[0].model, kNrTiers[0].intensity, kNrTiers[0].tone, kNrTiers[0].structure, kNrTiers[0].skin } },
    { kNrTiers[1].name, { kNrTiers[1].nr,  kNrTiers[1].model, kNrTiers[1].intensity, kNrTiers[1].tone, kNrTiers[1].structure, kNrTiers[1].skin } },
    { kNrTiers[2].name, { kNrTiers[2].nr,  kNrTiers[2].model, kNrTiers[2].intensity, kNrTiers[2].tone, kNrTiers[2].structure, kNrTiers[2].skin } },
    { kNrTiers[3].name, { kNrTiers[3].nr,  kNrTiers[3].model, kNrTiers[3].intensity, kNrTiers[3].tone, kNrTiers[3].structure, kNrTiers[3].skin } },
};


static std::wstring IniPath;   // zh: host64\ReShade.ini（只读；调参写入由悬浮面板经 live-params 完成）
// zh: 调试日志开关——DLSS5控制台.ini 加一行 debug=1（20261109）。GUI 程序没有
// stdout，调试行写入 exe 旁的 DLSS5控制台-debug.log（ASCII 行 + UTF-8 片段），
// 启动时截断。日志量低（秒级 tick 与状态转换），逐行开关文件即可。
static bool g_debug_log = false;
static std::wstring g_debug_log_path;

static void DebugLogF(const char *format, ...)
{
    if (!g_debug_log || g_debug_log_path.empty()) return;
    char message[1024];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[1100];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u %s\n",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
    HANDLE f = CreateFileW(g_debug_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    CloseHandle(f);
}
static void ReadIniParams(Params &p)
{
    if (IniPath.empty()) return;
    const int size = WideCharToMultiByte(CP_ACP, 0, IniPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return;
    std::string ini_ansi(static_cast<size_t>(size) - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, IniPath.c_str(), -1, ini_ansi.data(), size - 1, nullptr, nullptr);
    const char *ini = ini_ansi.c_str();
    char value[64]{};
    if (GetPrivateProfileStringA("Standalone.DLSSNR", "NeuralRendering", "", value, sizeof value, ini) && value[0])
        p.nr = value[0] != '0';
    if (GetPrivateProfileStringA("Standalone.DLSSNR", "FrameGeneration", "", value, sizeof value, ini) && value[0])
        p.fg = value[0] != '0';
    if (GetPrivateProfileStringA("Standalone.DLSSNR", "Model", "", value, sizeof value, ini) && value[0])
        p.model = std::clamp(atoi(value), 1, 3);
    if (GetPrivateProfileStringA("Standalone.DLSSNR", "Intensity", "", value, sizeof value, ini) && value[0])
        p.intensity = std::clamp(static_cast<float>(atof(value)), kNrIntensityMin, kNrIntensityMax);
    if (GetPrivateProfileStringA("Standalone.DLSSNR", "LocalTone", "", value, sizeof value, ini) && value[0])
        p.tone = std::clamp(static_cast<float>(atof(value)), kNrToneMin, kNrToneMax);
    if (GetPrivateProfileStringA("Standalone.DLSSNR", "LocalStructure", "", value, sizeof value, ini) && value[0])
        p.structure = std::clamp(static_cast<float>(atof(value)), kNrStructureMin, kNrStructureMax);
    if (GetPrivateProfileStringA("Standalone.DLSSNR", "SkinStructure", "", value, sizeof value, ini) && value[0])
        p.skin = std::clamp(static_cast<float>(atof(value)), -1.0f, 1.0f);
}

// ===== 遥测（共享内存直读） =====
struct Telemetry {
    bool available = false;
    bool nr_enabled = false, pipeline_ready = false, processed_visible = false, fg = false;
    bool fg_failed = false;
    unsigned long long nr_frames = 0, sr_frames = 0, fg_frames = 0, presented = 0;
    unsigned input_w = 0, input_h = 0, output_w = 0, output_h = 0;
    double age = 0;
};
static Telemetry ReadTelemetry(DWORD host_pid)
{
    Telemetry out;
    wchar_t name[64];
    std::swprintf(name, 64, L"Local\\DLSS5_AIO_Telemetry_%lu", host_pid);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!mapping) return out;
    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 320);
    if (view) {
        const unsigned char *data = static_cast<const unsigned char *>(view);
        auto rd32 = [&](size_t off) { unsigned v; memcpy(&v, data + off, 4); return v; };
        auto rd64 = [&](size_t off) { unsigned long long v; memcpy(&v, data + off, 8); return v; };
        if (rd32(0) == 0x4f494144u && rd32(4) == 1 && rd32(8) >= 320 && rd32(12) == host_pid) {
            // Seqlock: sample every field first, then require the sequence to be
            // unchanged across the whole snapshot. The old code verified the
            // sequence before reading flags/frames/sizes, so a torn snapshot
            // could still reach the display.
            for (int attempt = 0; attempt < 4; ++attempt) {
                const long long before = static_cast<long long>(rd64(16));
                std::atomic_thread_fence(std::memory_order_acquire);
                if (before & 1) continue;
                const long long qpc = static_cast<long long>(rd64(24));
                const long long frequency = static_cast<long long>(rd64(32));
                const unsigned flags = rd32(48);
                const unsigned input_w = rd32(56), input_h = rd32(60);
                const unsigned output_w = rd32(64), output_h = rd32(68);
                const unsigned long long nr_frames = rd64(200), sr_frames = rd64(208);
                const unsigned long long fg_frames = rd64(216), presented = rd64(224);
                std::atomic_thread_fence(std::memory_order_acquire);
                const long long after = static_cast<long long>(rd64(16));
                if (before != after || (after & 1)) continue;
                if (qpc <= 0 || frequency <= 0) break;
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                out.age = (now.QuadPart - qpc) / static_cast<double>(frequency);
                if (out.age >= -0.1 && out.age <= 3.0) {
                    out.available = true;
                    out.nr_enabled = (flags & 2) != 0;
                    out.fg = (flags & 4) != 0;
                    out.pipeline_ready = (flags & 8) != 0;
                    out.fg_failed = (flags & 16) != 0;
                    out.processed_visible = (flags & 32) != 0;
                    out.nr_frames = nr_frames;
                    out.sr_frames = sr_frames;
                    out.fg_frames = fg_frames;
                    out.presented = presented;
                    out.input_w = input_w; out.input_h = input_h;
                    out.output_w = output_w; out.output_h = output_h;
                }
                break;
            }
        }
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return out;
}

// ===== 进程定位 =====
static DWORD FindPidByPath(const wchar_t *exe_path)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD found = 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    const std::wstring want_name = exe_path;
    const size_t slash = want_name.find_last_of(L"\\/");
    const std::wstring want_base = slash == std::wstring::npos ? want_name : want_name.substr(slash + 1);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, want_base.c_str()) != 0) continue;
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!proc) continue;
            wchar_t path[MAX_PATH]{};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, path, &size) &&
                _wcsicmp(path, exe_path) == 0)
                found = pe.th32ProcessID;
            CloseHandle(proc);
        } while (!found && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// ===== 图层覆盖状态 =====
enum class LayerState { NoHost, NoOutput, RawMode, Hidden, UnderGame, Covered };
static LayerState g_layer = LayerState::NoHost;
static HWND g_game_hwnd_seen = nullptr;
static HWND g_window = nullptr;
static bool g_running = true;
static Telemetry g_tele;
static float g_game_fps = 0;
static unsigned long long g_last_presented = 0;
static long long g_last_presented_qpc = 0;
static std::wstring g_game_exe;       // wow.exe 绝对路径
static std::wstring g_cfg_path;       // exe 旁的 DLSS5控制台.ini（记住游戏目录）
static bool g_need_setup = false;   // zh: 未指定游戏目录时置位，界面顶部引导选择
static std::wstring g_client_dir;     // 客户端目录
static std::wstring g_self_dir;       // 本 exe 目录（bin）
static std::wstring g_panel_log_dir;  // 最新悬浮面板日志目录

static HWND FindAioMainOutput(DWORD host_pid)
{
    HWND w = FindWindowExW(nullptr, nullptr, L"StandaloneDLSSNRNativeOutput", nullptr);
    while (w) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == host_pid) {
            const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
            if ((ex & WS_EX_NOREDIRECTIONBITMAP) == 0) return w;
        }
        w = FindWindowExW(nullptr, w, L"StandaloneDLSSNRNativeOutput", nullptr);
    }
    return nullptr;
}

static LayerState EvaluateLayerCover(DWORD host_pid)
{
    if (!g_tele.available) return LayerState::NoHost;
    if (!g_tele.processed_visible) return LayerState::RawMode;
    const HWND output = FindAioMainOutput(host_pid);
    if (!output) return LayerState::NoOutput;
    if (!IsWindowVisible(output)) return LayerState::Hidden;
    if (g_game_hwnd_seen && IsWindowVisible(g_game_hwnd_seen)) {
        HWND w = output;
        for (int steps = 0; (w = GetWindow(w, GW_HWNDPREV)) != nullptr && steps < 256; ++steps)
            if (w == g_game_hwnd_seen) return LayerState::UnderGame;
    }
    return LayerState::Covered;
}

static const char *LayerShortName(LayerState s)
{
    switch (s) {
    case LayerState::Covered:   return "覆盖中";
    case LayerState::RawMode:   return "原画(F10)";
    case LayerState::Hidden:    return "已隐藏";
    case LayerState::UnderGame: return "被遮挡";
    case LayerState::NoOutput:  return "无输出";
    default:                    return "未确认";
    }
}

// ===== 悬浮面板保活（游戏运行且面板未启时拉起，日志重定向） =====
static void EnsureOverlayPanel()
{
    if (FindWindowW(L"Dlss5PanelWindow", nullptr) != nullptr) return;
    const std::wstring panel = g_self_dir + L"dlss5-panel.exe";
    if (!PathFileExistsW(panel.c_str())) return;
    const std::wstring root = g_client_dir + L"host64\\panel-logs";
    std::wstring guid(36, L'0');
    for (int i = 0; i < 36; ++i) guid[i] = L"0123456789abcdef"[rand() % 16];
    const std::wstring log_dir = root + L"\\" + guid;
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(log_dir.c_str(), nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    const std::wstring out_log = log_dir + L"\\stdout.log";
    const std::wstring err_log = log_dir + L"\\stderr.log";
    HANDLE out_file = CreateFileW(out_log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err_file = CreateFileW(err_log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring cmdline = L"\"" + panel + L"\" --game \"" + g_game_exe + L"\"";
    if (g_debug_log) cmdline += L" --debug";   // zh: 控制台调试开启时，面板也输出调试行
    DebugLogF("panel spawn: %ls", cmdline.c_str());
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_file;
    si.hStdError = err_file;
    PROCESS_INFORMATION pi{};
    const bool ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, g_client_dir.c_str(), &si, &pi);
    DebugLogF("panel spawn %s (pid=%lu)", ok ? "ok" : "FAILED", ok ? pi.dwProcessId : 0);
    if (out_file) CloseHandle(out_file);
    if (err_file) CloseHandle(err_file);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        g_panel_log_dir = log_dir;
    }
}

// ===== 源帧率上限（Config.wtf） =====
static int g_max_fps = -1;        // -1 = 未设置
static int g_max_fps_background = -1;
static std::wstring g_config_wtf;

static void ReadMaxFps()
{
    g_max_fps = g_max_fps_background = -1;
    HANDLE file = CreateFileW(g_config_wtf.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string text;
    char buf[4096]{};
    DWORD read = 0;
    while (ReadFile(file, buf, sizeof buf, &read, nullptr) && read) text.append(buf, read);
    CloseHandle(file);
    auto find = [&](const char *key) {
        const std::string needle = std::string("SET ") + key + " \"";
        size_t pos = text.find(needle);
        if (pos == std::string::npos) return -1;
        pos += needle.size();
        const size_t end = text.find('"', pos);
        if (end == std::string::npos) return -1;
        return atoi(text.substr(pos, end - pos).c_str());
    };
    g_max_fps = find("MaxFPS");
    g_max_fps_background = find("MaxFPSBackground");
}

static void WriteMaxFps(int fps, int bg)
{
    HANDLE file = CreateFileW(g_config_wtf.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string text;
    if (file != INVALID_HANDLE_VALUE) {
        char buf[4096]{};
        DWORD read = 0;
        while (ReadFile(file, buf, sizeof buf, &read, nullptr) && read) text.append(buf, read);
        CloseHandle(file);
    }
    auto set_line = [&](const char *key, int value) {
        const std::string needle = std::string("SET ") + key + " \"";
        const size_t pos = text.find(needle);
        char line[64];
        std::snprintf(line, sizeof line, "SET %s \"%d\"", key, value);
        if (pos == std::string::npos) {
            if (!text.empty() && text.back() != '\n') text += "\n";
            text += line;
            text += "\n";
        } else {
            const size_t vstart = pos + needle.size();
            const size_t vend = text.find('"', vstart);
            if (vend != std::string::npos)
                text = text.substr(0, vstart) + std::to_string(value) + text.substr(vend);
        }
    };
    set_line("MaxFPS", fps);
    set_line("MaxFPSBackground", bg);
    file = CreateFileW(g_config_wtf.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DebugLogF("maxfps save FAILED (path=%ls locked or missing)", g_config_wtf.c_str());
        return;
    }
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    g_max_fps = fps;
    g_max_fps_background = bg;
    DebugLogF("maxfps saved: foreground=%d background=%d (%ls)", fps, bg, g_config_wtf.c_str());
}

// ===== ImGui / D3D11 =====
static ID3D11Device *g_device = nullptr;
static ID3D11DeviceContext *g_context = nullptr;
static IDXGISwapChain *g_swapchain = nullptr;
static ID3D11RenderTargetView *g_target = nullptr;

static bool CreateD3D(HWND window)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.RefreshRate = {60, 1};
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    UINT flags = D3D11_CREATE_DEVICE_FLAG();
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &desc, &g_swapchain, &g_device, &level, &g_context);
    if (FAILED(hr)) return false;
    ID3D11Texture2D *backbuffer = nullptr;
    g_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backbuffer));
    if (!backbuffer) return false;
    g_device->CreateRenderTargetView(backbuffer, nullptr, &g_target);
    backbuffer->Release();
    return g_target != nullptr;
}

// ===== 视觉主题：深夜蓝黑 + 祖母绿强调 =====
static const ImVec4 kAccent(0.239f, 0.839f, 0.549f, 1.0f);     // #3DD68C
static const ImVec4 kAccentBright(0.310f, 0.910f, 0.620f, 1.0f);
static const ImVec4 kAccentDim(0.110f, 0.290f, 0.200f, 1.0f);
static const ImVec4 kWarn(0.941f, 0.639f, 0.369f, 1.0f);       // #F0A35E
static const ImVec4 kMuted(0.545f, 0.596f, 0.647f, 1.0f);      // #8B98A5
static const ImVec4 kInk(0.020f, 0.049f, 0.039f, 1.0f);        // 强调按钮上的深字
static ImFont *g_font_big = nullptr;
static float g_dpi_scale = 1.0f;   // zh: DPI 缩放（窗口过程与界面共用）


// 安全路径拼接：目录带不带结尾分隔符都可以
static std::wstring JoinPath(const std::wstring &dir, const std::wstring &rel)
{
    if (dir.empty()) return rel;
    const wchar_t last = dir.back();
    const wchar_t sep = static_cast<wchar_t>(0x5C);   // 反斜杠
    if (last == sep) return dir + rel;
    return dir + sep + rel;
}

// zh: 由客户端目录派生的配置路径统一在此计算。选目录后必须重算——旧版只在启动时
// 算一次，首次引导选完目录后参数读取与帧率上限一直读写错误位置（20261109 修复）。
static void RefreshClientPaths()
{
    const std::wstring host64 = JoinPath(g_client_dir, L"host64");
    IniPath = JoinPath(host64, L"ReShade.ini");
    g_config_wtf = JoinPath(JoinPath(g_client_dir, L"WTF"), L"Config.wtf");
    DebugLogF("client paths refreshed: ini=%ls wtf=%ls", IniPath.c_str(), g_config_wtf.c_str());
}

// zh: 载入参数并重判档位锁定；启动时与选完目录后共用同一入口。
static void LoadParamsAndTier()
{
    ReadIniParams(g_params);
    g_tier_locked = false;
    for (const auto &s : kSchemes)
        if (g_params.MatchesTier(s.p)) { g_tier_locked = true; break; }
    ReadMaxFps();
}

// 游戏目录应用/持久化/选择（wWinMain 与界面引导卡共用）
static void ApplyGameDir(const std::wstring &dir)
{
    wchar_t p[MAX_PATH]{};
    GetFullPathNameW((dir + L"\\wow.exe").c_str(), MAX_PATH, p, nullptr);
    g_game_exe = p;
    const size_t slash = g_game_exe.find_last_of(L"\\/");
    g_client_dir = g_game_exe.substr(0, slash + 1);
}
// zh: 记忆文件按 UTF-8 存客户端目录。旧版按 wchar 低字节逐个截断，中文路径写坏；
// 回读又把行尾换行带进路径，导致记忆永远失效、每次启动都重新选择（20261109 修复）。
static void SaveGameDirCfg()
{
    HANDLE f = CreateFileW(g_cfg_path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const int size = WideCharToMultiByte(CP_UTF8, 0, g_client_dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
        std::string line = "game=";
        line.resize(5 + static_cast<size_t>(size) - 1);
        WideCharToMultiByte(CP_UTF8, 0, g_client_dir.c_str(), -1, line.data() + 5, size - 1, nullptr, nullptr);
        line += "\n";
        DWORD written = 0;
        WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
    CloseHandle(f);
}
static bool PickGameDirAndApply()
{
    IFileDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog)))) {
        MessageBoxW(g_window, L"无法打开目录选择器（系统组件初始化失败）。", L"WOW-335-NR 控制台", MB_ICONERROR);
        return false;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS);
    bool ok = SUCCEEDED(dialog->Show(g_window));
    if (ok) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                ApplyGameDir(path);
                SaveGameDirCfg();
                RefreshClientPaths();
                LoadParamsAndTier();
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
        ok = PathFileExistsW(g_game_exe.c_str());   // 所选目录必须含 wow.exe
        if (!ok) MessageBoxW(g_window,
            L"所选目录下没有 wow.exe——请选择游戏客户端的根目录（含 wow.exe 与 host64）。",
            L"WOW-335-NR 控制台", MB_ICONWARNING);
    } else {
        dialog->Release();
    }
    return ok;
}

static void ApplyStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 8.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.WindowPadding = ImVec2(16, 14);
    style.FramePadding = ImVec2(10, 7);
    style.ItemSpacing = ImVec2(12, 9);
    style.ItemInnerSpacing = ImVec2(7, 4);
    style.ScrollbarSize = 12.0f;
    ImVec4 *c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.043f, 0.059f, 0.078f, 1.0f);   // #0B0F14
    c[ImGuiCol_ChildBg] = ImVec4(0.071f, 0.090f, 0.122f, 1.0f);    // #12171F
    c[ImGuiCol_Border] = ImVec4(0.110f, 0.157f, 0.200f, 1.0f);     // #1C2833
    c[ImGuiCol_TitleBg] = ImVec4(0.043f, 0.059f, 0.078f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.902f, 0.929f, 0.953f, 1.0f);       // #E6EDF3
    c[ImGuiCol_TextDisabled] = ImVec4(0.545f, 0.596f, 0.647f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.106f, 0.137f, 0.176f, 1.0f);     // #1B232D
    c[ImGuiCol_ButtonHovered] = ImVec4(0.145f, 0.192f, 0.244f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.180f, 0.240f, 0.300f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.094f, 0.122f, 0.155f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.118f, 0.153f, 0.192f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.137f, 0.180f, 0.227f, 1.0f);
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentBright;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_Header] = ImVec4(0.106f, 0.137f, 0.176f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.145f, 0.192f, 0.244f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.110f, 0.157f, 0.200f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.071f, 0.090f, 0.122f, 0.98f);
}

// zh: 区块问号标记——默认收起，悬停展开说明
static void SectionHelp(const char *tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
}

// 卡片容器：标题（强调色）+ (?) 帮助 + 自动高度
static void BeginCard(const char *title, const char *help)
{
    ImGui::BeginChild(title, ImVec2(0, 0),
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (help) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
    }
    ImGui::Spacing();
}
static void EndCard() { ImGui::EndChild(); }

// 开关滑块：圆角胶囊 + 圆形滑钮
static bool ToggleSwitch(const char *label, bool v)
{
    ImGui::PushID(label);
    const float h = ImGui::GetFrameHeight() * 0.70f;
    const float w = h * 1.85f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool changed = false;
    ImGui::InvisibleButton("##toggle", ImVec2(w, h));
    if (ImGui::IsItemClicked()) { v = !v; changed = true; }
    const ImU32 bg_on = IM_COL32(61, 214, 140, 255);
    const ImU32 bg_off = IM_COL32(52, 63, 76, 255);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), v ? bg_on : bg_off, h * 0.5f);
    const float r = h * 0.5f - 2.0f;
    const float cx = v ? pos.x + w - r - 2.0f : pos.x + r + 2.0f;
    dl->AddCircleFilled(ImVec2(cx, pos.y + h * 0.5f), r, IM_COL32(233, 240, 245, 255));
    ImGui::SameLine(0, 10);
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

// 分段选择按钮：选中态强调色填充 + 深字
static bool SegmentedOption(const char *label, bool active, float width)
{
    bool clicked = false;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentBright);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    }
    clicked = ImGui::Button(label, ImVec2(width, 0));
    if (active) ImGui::PopStyleColor(4);
    return clicked;
}

static const char *SchemeName()
{
    if (!g_tier_locked) return "自定义";
    for (const auto &s : kSchemes)
        if (g_params.MatchesTier(s.p)) return s.name;
    return "自定义";
}

static void RenderUi(bool game_running, DWORD host_pid)
{
    RECT rc{};
    GetClientRect(g_window, &rc);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(rc.right - rc.left),
                                    static_cast<float>(rc.bottom - rc.top)));
    ImGui::Begin("WOW-335-NR 控制台", nullptr, ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

    // 沉浸式标题行：拖拽区（HTCAPTION）+ 自绘窗口按钮（NCHITTEST 按此区域豁免）
    const float tbh = 34.0f * g_dpi_scale;
    const float btnw = 34.0f * g_dpi_scale;
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x,
        (tbh - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextColored(kAccent, "WOW-335-NR 控制台");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - 3 * btnw);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.145f, 0.192f, 0.244f, 1.0f));
    if (ImGui::Button("-", ImVec2(btnw - 4, tbh - 8)))
        ShowWindow(g_window, SW_MINIMIZE);
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.145f, 0.192f, 0.244f, 1.0f));
    if (ImGui::Button("□", ImVec2(btnw - 4, tbh - 8))) {
        static RECT pre_max{};
        static bool maxed = false;
        RECT cur{}, wa{};
        GetWindowRect(g_window, &cur);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        if (!maxed) {
            pre_max = cur;
            SetWindowPos(g_window, nullptr, wa.left, wa.top,
                wa.right - wa.left, wa.bottom - wa.top, SWP_NOZORDER | SWP_NOACTIVATE);
            maxed = true;
        } else {
            SetWindowPos(g_window, nullptr, pre_max.left, pre_max.top,
                pre_max.right - pre_max.left, pre_max.bottom - pre_max.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            maxed = false;
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.898f, 0.325f, 0.294f, 1.0f));
    if (ImGui::Button("×", ImVec2(btnw - 4, tbh - 8)))
        g_running = false;
    ImGui::PopStyleColor(3);

    // 游戏状态 + 启动按钮（内容首行；未指定目录时此行留空，由下方引导卡引导）
    ImGui::SetCursorPosY(tbh + ImGui::GetStyle().ItemSpacing.y);
    if (g_need_setup) {
        // 留空：引导卡在下方
    } else if (game_running) {
        ImGui::TextColored(kAccent, "● 游戏运行中");
        const float launch_w = 132.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - launch_w - ImGui::GetStyle().WindowPadding.x);
        ImGui::BeginDisabled(true);
        ImGui::Button("游戏运行中", ImVec2(launch_w, 0));
        ImGui::EndDisabled();
    } else {
        ImGui::TextColored(kMuted, "● 游戏未运行");
        const float launch_w = 132.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - launch_w - ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentBright);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Text, kInk);
        if (ImGui::Button("启动游戏", ImVec2(launch_w, 0)))
            ShellExecuteW(nullptr, L"open", g_game_exe.c_str(), nullptr, g_client_dir.c_str(), SW_SHOWNORMAL);
        ImGui::PopStyleColor(4);
    }

    // 引导卡：未指定游戏目录时的首屏设置入口
    if (g_need_setup) {
        BeginCard("选择游戏客户端", nullptr);
        ImGui::TextColored(kWarn, "尚未指定游戏客户端路径。");
        ImGui::TextWrapped("选择包含 wow.exe 与 host64 的客户端根目录；\n选择结果会被记住，之后启动不再询问。");
        if (ImGui::Button("选择游戏目录", ImVec2(-1, 0))) {
            if (PickGameDirAndApply()) {
                g_need_setup = false;   // zh: 参数/帧率/档位已在 PickGameDirAndApply 内重载
            }
        }
        EndCard();
    }

    // 实时状态卡
    BeginCard("实时状态", "载体实时遥测：NR/帧生成是否在工作、已处理帧数、输入与输出分辨率、\n最终呈现帧率（由输出窗呈现计数计算）。");
    if (g_tele.available) {
        ImGui::PushFont(g_font_big);
        ImGui::Text("%.1f", g_game_fps);
        ImGui::PopFont();
        ImGui::SameLine(0, 12);
        ImGui::BeginGroup();
        ImGui::TextDisabled("输出 FPS");
        if (g_tele.nr_enabled) ImGui::TextColored(kAccent, "● NR 运行中");
        else ImGui::TextColored(kMuted, "● NR 已停用");
        if (!g_tele.fg) ImGui::TextColored(kMuted, "● 帧生成 关");
        else if (g_tele.fg_failed) ImGui::TextColored(kWarn, "● 帧生成 求值失败");
        else if (g_tele.fg_frames > 0) ImGui::TextColored(kAccent, "● 帧生成 已插帧 %llu", g_tele.fg_frames);
        else ImGui::TextColored(kWarn, "● 帧生成 待首次插帧");
        if (g_layer == LayerState::Covered) ImGui::TextColored(kAccent, "● 图层覆盖中");
        else ImGui::TextColored(kWarn, "● 图层 %s", LayerShortName(g_layer));
        ImGui::EndGroup();
        ImGui::SameLine(0, 20);
        ImGui::BeginGroup();
        ImGui::TextDisabled("分辨率");
        ImGui::Text("%ux%u", g_tele.input_w, g_tele.input_h);
        ImGui::TextDisabled("→ 输出");
        ImGui::Text("%ux%u", g_tele.output_w, g_tele.output_h);
        ImGui::EndGroup();
        ImGui::SameLine(0, 20);
        ImGui::BeginGroup();
        ImGui::TextDisabled("已处理帧数");
        ImGui::Text("NR %llu", g_tele.nr_frames);
        ImGui::Text("SR %llu", g_tele.sr_frames);
        ImGui::TextDisabled("管线");
        ImGui::TextUnformatted(g_tele.pipeline_ready ? "就绪" : "未就绪");
        ImGui::EndGroup();
    } else {
        ImGui::TextColored(kWarn, "载体遥测：未确认（载体未运行或刚启动）");
    }
    ImGui::TextDisabled("调参请使用游戏内悬浮面板（游戏中按 F11 展开）。");
    EndCard();

    // 源帧率卡
    BeginCard("源帧率上限", "给游戏设帧率上限，防 32 位地址空间被采集队列撑爆（FG 开启后不限速会内存耗尽退出）：\n前台 30=源 30 帧→帧生成插到 60；后台 10=切出游戏时降载；0=不限速（不推荐）。\n建议游戏未运行时保存；游戏内输入 /console MaxFPS 30 即时生效且不会被退出回写覆盖。");
    static int fps_edit = 0, bg_edit = 0;
    static bool fps_loaded = false;
    if (!fps_loaded) { fps_edit = g_max_fps; bg_edit = g_max_fps_background; fps_loaded = true; }
    ImGui::SetNextItemWidth(150);
    ImGui::InputInt("前台（0=不限）", &fps_edit);
    ImGui::SetNextItemWidth(150);
    ImGui::InputInt("后台（0=不限）", &bg_edit);
    if (ImGui::Button("保存到 Config.wtf")) {
        WriteMaxFps((std::max)(fps_edit, 0), (std::max)(bg_edit, 0));
        fps_loaded = false;
    }
    ImGui::TextDisabled("建议游戏未运行时保存——游戏退出回写会覆盖运行中的外部修改；\n也可在游戏内输入 /console MaxFPS 30 即时生效并由游戏自己保存。");
    EndCard();

    // 日志卡
    BeginCard("日志", nullptr);
    if (ImGui::Button("打开悬浮面板日志目录", ImVec2(-1, 0))) {
        std::wstring dir = g_panel_log_dir.empty()
            ? g_client_dir + L"host64\\panel-logs"
            : g_panel_log_dir;
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::Button("打开载体日志", ImVec2(-1, 0))) {
        const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
        std::wstring dir = local ? std::wstring(local) + L"\\RHI\\Logs" : std::wstring();
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    EndCard();

    ImGui::End();
}

// ===== Win32 / D3D11 =====
static LRESULT CALLBACK ConsoleWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_NCCALCSIZE:
        // 沉浸式：客户区=整窗（原生标题栏内缩移除；缩放边框由 NCHITTEST 提供）
        if (wparam) return 0;
        break;
    case WM_NCHITTEST:
    {
        POINT pt{ static_cast<SHORT>(LOWORD(lparam)), static_cast<SHORT>(HIWORD(lparam)) };
        ScreenToClient(window, &pt);
        RECT rc{};
        GetClientRect(window, &rc);
        const LONG edge = static_cast<LONG>(8 * g_dpi_scale);
        const bool at_l = pt.x < edge, at_r = pt.x >= rc.right - edge;
        const bool at_t = pt.y < edge, at_b = pt.y >= rc.bottom - edge;
        if (at_l && at_t) return HTTOPLEFT;
        if (at_r && at_t) return HTTOPRIGHT;
        if (at_l && at_b) return HTBOTTOMLEFT;
        if (at_r && at_b) return HTBOTTOMRIGHT;
        if (at_l) return HTLEFT;
        if (at_r) return HTRIGHT;
        if (at_t) return HTTOP;
        if (at_b) return HTBOTTOM;
        const float tbh = 34.0f * g_dpi_scale;
        const float zone = 3.0f * 34.0f * g_dpi_scale;
        if (pt.y < tbh) {
            if (pt.x >= rc.right - static_cast<LONG>(zone)) return HTCLIENT;   // ─ □ × 按钮区
            return HTCAPTION;   // 标题行其余区域：拖拽/双击最大化/贴边分屏
        }
        return HTCLIENT;
    }
    case WM_SIZE:
        if (g_swapchain && wparam != SIZE_MINIMIZED) {
            if (g_target) { g_target->Release(); g_target = nullptr; }
            g_swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D *backbuffer = nullptr;
            if (SUCCEEDED(g_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void **>(&backbuffer)))) {
                g_device->CreateRenderTargetView(backbuffer, nullptr, &g_target);
                backbuffer->Release();
            }
        }
        return 0;
    case WM_DESTROY: g_running = false; return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    // 单实例
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CreateMutexW(nullptr, TRUE, L"DLSS5ConsoleMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // 路径：self = exe 目录（tools\dlss5\bin）；--game 可覆盖默认客户端
    wchar_t self_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    g_self_dir = self_path;
    const size_t self_slash = g_self_dir.find_last_of(L"\\/");
    g_self_dir.resize(self_slash + 1);
    // 游戏路径：--game 参数 > exe 旁 ini 记忆 > 都没有则界面顶部引导选择（不弹窗）
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring arg_game;
    for (int i = 1; argv && i + 1 < argc; ++i)
        if (wcscmp(argv[i], L"--game") == 0) {
            wchar_t p[MAX_PATH]{};
            GetFullPathNameW(argv[i + 1], MAX_PATH, p, nullptr);
            arg_game = p;
        }
    if (argv) LocalFree(argv);

    g_cfg_path = g_self_dir + L"DLSS5控制台.ini";
    {
        HANDLE f = CreateFileW(g_cfg_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[MAX_PATH * 4]{};
            DWORD read = 0;
            if (ReadFile(f, buf, sizeof buf - 1, &read, nullptr) && read > 5) {
                buf[read] = 0;
                if (std::strncmp(buf, "game=", 5) == 0) {
                    // zh: 去掉行尾换行，再按 UTF-8 解码（与 SaveGameDirCfg 对应；纯 ASCII 旧文件不受影响）。
                    char *end = buf + read;
                    while (end > buf && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) --end;
                    *end = 0;
                    const int wide = MultiByteToWideChar(CP_UTF8, 0, buf + 5, -1, nullptr, 0);
                    if (wide > 1) {
                        std::wstring dir(static_cast<size_t>(wide), L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, buf + 5, -1, dir.data(), wide);
                        dir.resize(static_cast<size_t>(wide) - 1);
                        g_client_dir = std::move(dir);
                    }
                }
                // zh: 调试开关：ini 里手工加一行 debug=1（行首或换行后），重启生效。
                const char *dbg = std::strstr(buf, "debug=");
                if (dbg && (dbg == buf || dbg[-1] == '\n' || dbg[-1] == '\r') && dbg[6] == '1')
                    g_debug_log = true;
            }
            CloseHandle(f);
        }
    }
    g_debug_log_path = g_self_dir + L"DLSS5控制台-debug.log";
    if (g_debug_log) {
        HANDLE f = CreateFileW(g_debug_log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        DebugLogF("console started with debug=1 (self=%ls)", g_self_dir.c_str());
    }

    if (!arg_game.empty()) {
        ApplyGameDir(arg_game);
        SaveGameDirCfg();   // --game 视为「设定并记住」
    } else if (!g_client_dir.empty() &&
        PathFileExistsW((g_client_dir + L"\\wow.exe").c_str())) {
        ApplyGameDir(g_client_dir);   // ini 记忆命中
    } else {
        g_need_setup = true;   // 无有效游戏路径：进入界面后顶部引导选择
    }
    RefreshClientPaths();
    LoadParamsAndTier();

    // DPI 感知：未声明时 DWM 会拉伸窗口位图，字体发虚
    using SetDpiCtxFn = BOOL (WINAPI *)(DPI_AWARENESS_CONTEXT);
    const auto set_dpi_ctx = reinterpret_cast<SetDpiCtxFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_dpi_ctx) set_dpi_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    using GetDpiForSystemFn = UINT (WINAPI *)();
    const auto get_dpi = reinterpret_cast<GetDpiForSystemFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    g_dpi_scale = (get_dpi ? static_cast<float>(get_dpi()) : 96.0f) / 96.0f;
    const float dpi_scale = g_dpi_scale;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ConsoleWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"Dlss5ConsoleWindow";
    RegisterClassExW(&wc);
    g_window = CreateWindowExW(0, wc.lpszClassName, L"WOW-335-NR 控制台",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(600 * dpi_scale), static_cast<int>(900 * dpi_scale),
        nullptr, nullptr, instance, nullptr);
    if (!g_window) return 5;
    if (!CreateD3D(g_window)) {
        MessageBoxW(nullptr, L"D3D11 初始化失败。", L"WOW-335-NR 控制台", MB_ICONERROR);
        return 6;
    }
    ImGui::CreateContext();
    ApplyStyle();
    if (dpi_scale != 1.0f) ImGui::GetStyle().ScaleAllSizes(dpi_scale);
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);
    ImFont *zh_font = nullptr;
    for (const char *font_path : {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/msyhl.ttc",
                                  "C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/segoeui.ttf"}) {
        if ((zh_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(font_path, 16.0f * dpi_scale))) break;
    }
    for (const char *font_path : {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/msyhl.ttc"}) {
        if ((g_font_big = ImGui::GetIO().Fonts->AddFontFromFileTTF(font_path, 27.0f * dpi_scale))) break;
    }
    ShowWindow(g_window, SW_SHOW);

    DWORD last_tick = 0, last_tele = 0, last_ensure = 0;
    DWORD game_pid = 0, host_pid = 0;
    bool game_running = false;
    while (g_running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        const DWORD now = static_cast<DWORD>(GetTickCount64());
        if (now - last_tick >= 1000) {
            last_tick = now;
            game_pid = FindPidByPath(g_game_exe.c_str());
            const std::wstring host_exe = g_client_dir + L"host64\\AIO DLSS5 32-bit Wrapper.exe";
            host_pid = FindPidByPath(host_exe.c_str());
            const bool now_running = game_pid != 0;
            game_running = now_running;   // 面板拉起统一由 5 秒守护负责（避免同秒双重拉起）
            static bool prev_running = false;
            if (now_running != prev_running) {
                DebugLogF("game %s (pid=%lu), host pid=%lu", now_running ? "started" : "exited",
                    static_cast<unsigned long>(game_pid), static_cast<unsigned long>(host_pid));
                prev_running = now_running;
            }
            // 游戏窗口（按类名匹配到游戏进程）
            HWND game_window = FindWindowExW(nullptr, nullptr, L"GxWindowClassD3d", nullptr);
            g_game_hwnd_seen = nullptr;
            while (game_window) {
                DWORD pid = 0;
                GetWindowThreadProcessId(game_window, &pid);
                if (pid == game_pid && game_pid) { g_game_hwnd_seen = game_window; break; }
                game_window = FindWindowExW(nullptr, game_window, L"GxWindowClassD3d", nullptr);
            }
        }
        if (game_running && now - last_ensure >= 5000) {
            last_ensure = now;
            EnsureOverlayPanel();   // 守护：面板意外退出后自动补位
        }
        if (now - last_tele >= 500) {
            last_tele = now;
            g_tele = host_pid ? ReadTelemetry(host_pid) : Telemetry{};
            g_layer = EvaluateLayerCover(host_pid);
            {
                static LayerState prev_layer = LayerState::NoHost;
                if (g_layer != prev_layer) {
                    DebugLogF("layer state -> %s", LayerShortName(g_layer));
                    prev_layer = g_layer;
                }
                static DWORD last_dbg_tele = 0;
                if (g_debug_log && !g_tele.available && host_pid && now - last_dbg_tele >= 10000) {
                    last_dbg_tele = now;
                    DebugLogF("telemetry unavailable (host=%lu; mapping missing or stale)",
                        static_cast<unsigned long>(host_pid));
                }
            }
            if (g_tele.presented > 0 && g_last_presented_qpc) {
                LARGE_INTEGER now_qpc{}, freq{};
                QueryPerformanceCounter(&now_qpc);
                QueryPerformanceFrequency(&freq);
                const double seconds = (now_qpc.QuadPart - g_last_presented_qpc) / static_cast<double>(freq.QuadPart);
                if (seconds > 0.5) g_game_fps = (g_tele.presented - g_last_presented) / static_cast<float>(seconds);
                g_last_presented = g_tele.presented;
                g_last_presented_qpc = now_qpc.QuadPart;
            } else if (g_tele.presented > 0) {
                g_last_presented = g_tele.presented;
                LARGE_INTEGER now_qpc{};
                QueryPerformanceCounter(&now_qpc);
                g_last_presented_qpc = now_qpc.QuadPart;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (zh_font) ImGui::PushFont(zh_font);
        RenderUi(game_running, host_pid);
        if (zh_font) ImGui::PopFont();
        ImGui::Render();
        const float clear[4] = {0.03f, 0.05f, 0.07f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapchain->Present(1, 0);
        Sleep(10);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
