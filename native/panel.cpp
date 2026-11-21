// dlss5-panel.exe — 游戏内中文控制面板（乙方案）
// 架构：独立置顶窗口（D3D11 + ImGui 1.92 动态字形，微软雅黑）浮于游戏之上；
//       写 host64\live-params.json → addon64 热应用 NR 参数（0.5 秒内生效）；
//       直读 Local\DLSS5_AIO_Telemetry_<pid> 共享内存展示运行状态。
// 边界：结构校验（wow.exe + host64 载体存在且非链接），不启动游戏；
//       收起态为右上角可交互信息条（含“展开面板”按钮），展开态为完整面板；游戏退出即结束。
// 构建：Build-Panel.ps1（/utf-8，MSVC x64 /MT）。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "tiers.hpp"
#include "live_params.hpp"
#include "pending_save.hpp"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;

static bool NoReparsePoints(const fs::path &path)
{
    fs::path current = path.root_path();
    for (const auto &part : path.relative_path()) {
        current /= part;
        DWORD attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}

struct Processes { DWORD game_pid = 0, host_pid = 0; bool uncertain = false; };

static Processes FindProcesses(const std::wstring &game, const std::wstring &host)
{
    Processes result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { result.uncertain = true; return result; }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    wchar_t path[MAX_PATH]{};
    bool more = Process32FirstW(snapshot, &entry);
    if (!more) result.uncertain = true;
    while (more) {
        const bool game_name = _wcsicmp(entry.szExeFile, L"wow.exe") == 0;
        const bool host_name = _wcsicmp(entry.szExeFile, L"AIO DLSS5 32-bit Wrapper.exe") == 0;
        if (game_name || host_name) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            DWORD size = MAX_PATH;
            bool got = false;
            if (process) { got = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE; CloseHandle(process); }
            if (!got) result.uncertain = true;
            else if (game_name && _wcsicmp(path, game.c_str()) == 0) result.game_pid = entry.th32ProcessID;
            else if (host_name && _wcsicmp(path, host.c_str()) == 0) result.host_pid = entry.th32ProcessID;
        }
        more = Process32NextW(snapshot, &entry);
        if (!more && GetLastError() != ERROR_NO_MORE_FILES) result.uncertain = true;
    }
    CloseHandle(snapshot);
    return result;
}

// ===== 参数模型（与 addon64 的钳制一致） =====
struct Params {
    bool nr = true;
    int model = 1;
    float intensity = 1.0f, tone = 1.0f, structure = 1.0f, skin = -1.0f;
    bool fg = true;   // zh: 帧生成独立开关——档位与微调滑杆不触碰它
    int fg_multiplier = 2;   // zh: 补帧倍数（2/3/4×）；3×/4× 需 RTX 50 系，其他卡载体自动回落 2×
    bool Equals(const Params &o) const {
        return nr == o.nr && model == o.model && fg == o.fg && fg_multiplier == o.fg_multiplier &&
            fabsf(intensity - o.intensity) < 0.005f && fabsf(tone - o.tone) < 0.005f &&
            fabsf(structure - o.structure) < 0.005f && fabsf(skin - o.skin) < 0.005f;
    }
    // 档位匹配只看观感参数：模型是独立选择器，不参与档位判定
    bool MatchesTier(const Params &o) const {
        return nr == o.nr &&
            fabsf(intensity - o.intensity) < 0.005f && fabsf(tone - o.tone) < 0.005f &&
            fabsf(structure - o.structure) < 0.005f && fabsf(skin - o.skin) < 0.005f;
    }
};
static Params g_params;
static nr_files::PendingSave g_save;
static bool g_tier_locked = true;   // zh: 出厂档=锁定配方（微调禁用），自定义=解锁可调
static Params g_custom;          // 自定义档位：最近一次应用的参数组合
static bool g_custom_valid = false;

// zh: 档位表来自 tiers.hpp（与控制台/PowerShell 共用的唯一事实源）。
static const struct { const char *name; Params p; } kSchemes[] = {
    { kNrTiers[0].name, { kNrTiers[0].nr,  kNrTiers[0].model, kNrTiers[0].intensity, kNrTiers[0].tone, kNrTiers[0].structure, kNrTiers[0].skin } },
    { kNrTiers[1].name, { kNrTiers[1].nr,  kNrTiers[1].model, kNrTiers[1].intensity, kNrTiers[1].tone, kNrTiers[1].structure, kNrTiers[1].skin } },
    { kNrTiers[2].name, { kNrTiers[2].nr,  kNrTiers[2].model, kNrTiers[2].intensity, kNrTiers[2].tone, kNrTiers[2].structure, kNrTiers[2].skin } },
    { kNrTiers[3].name, { kNrTiers[3].nr,  kNrTiers[3].model, kNrTiers[3].intensity, kNrTiers[3].tone, kNrTiers[3].structure, kNrTiers[3].skin } },
};

// zh: 模型即 DLSSNR.Style 0–2（引擎三套有效网络；Hint.Render.Preset 在本运行时
// zh: 为兼容摆设，不选网络——载体 NrStyle() 注释已验证）。
// zh: 「默认/自然/电影」为 033 从引擎契约读出的官方语义；具体视觉差异待实测。
static const char *kModelNames[3] = { "默认", "自然", "电影" };
static const char *kModelDesc[3] = {
    "默认：引擎原始风格。社区反馈重绘较保守，观感最接近原画。",
    "自然：引擎的自然向风格（视觉差异待实测）。",
    "电影：引擎的电影向风格；与「自然」实测观感差异细微。",
};

static void ClampParams(Params &p)
{
    // zh: 边界取自 tiers.hpp（033 schema bounds；与控制台/PowerShell 共用）
    p.model = std::clamp(p.model, 1, 3);
    p.fg_multiplier = std::clamp(p.fg_multiplier, 2, 4);
    p.intensity = std::clamp(p.intensity, kNrIntensityMin, kNrIntensityMax);
    p.tone = std::clamp(p.tone, kNrToneMin, kNrToneMax);
    p.structure = std::clamp(p.structure, kNrStructureMin, kNrStructureMax);
    p.skin = std::clamp(p.skin, kNrSkinMin, kNrSkinMax);
}

static std::string ParamsToJson(const Params &p)
{
    char text[256]{};
    std::snprintf(text, sizeof text,
        "{\"neuralRendering\":%s,\"model\":%d,\"intensity\":%.2f,\"localTone\":%.2f,"
        "\"localStructure\":%.2f,\"skinStructure\":%.2f,\"frameGeneration\":%s,"
        "\"fgMultiplier\":%d}",
        p.nr ? "true" : "false", p.model, p.intensity, p.tone, p.structure, p.skin,
        p.fg ? "true" : "false", p.fg_multiplier);
    return text;
}

static bool WriteParamsAtomic(const fs::path &path, const Params &p)
{
    const std::string body = ParamsToJson(p);
    const fs::path temp = path.wstring() + L".panel-tmp-" + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written, nullptr) &&
        written == body.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) { DeleteFileW(temp.c_str()); return false; }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

// 自定义档位持久化（host64\custom-tier.json；与 live-params 同格式的扁平 JSON）
static std::wstring CustomPath;
static void LoadCustomTier()
{
    HANDLE file = CreateFileW(CustomPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char text[512]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, text, sizeof text - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok) return;
    text[read] = 0;
    nr_params::Values value;
    if (!nr_params::Parse(std::string_view(text, read), value)) return;
    g_custom.model = static_cast<int>(value.model);
    g_custom.intensity = value.intensity;
    g_custom.tone = value.tone;
    g_custom.structure = value.structure;
    g_custom.skin = value.skin;
    g_custom.nr = value.nr;
    if (value.has_fg) g_custom.fg = value.fg;
    if (value.has_multiplier) g_custom.fg_multiplier = value.multiplier;
    ClampParams(g_custom);
    g_custom_valid = true;
}
static bool SaveCustomTier(const Params &p)
{
    if (!WriteParamsAtomic(CustomPath, p)) return false;
    g_custom = p;
    g_custom_valid = true;
    return true;
}

// 从 host64/ReShade.ini 读取当前值（面板启动时的初值来源）
static std::wstring IniPath, ParamsPath;
static void ReadIniParams(Params &p)
{
    if (IniPath.empty()) return;
    // zh: 用宽字符 API 直接读 wide IniPath，避免 CP_ACP 转换在中文路径下
    // 静默失败；配置值本身是 ASCII 数字/开关。
    const wchar_t *section = L"Standalone.DLSSNR";
    wchar_t value[64]{};
    auto read = [&](const wchar_t *key) -> const wchar_t * {
        value[0] = L'\0';
        GetPrivateProfileStringW(section, key, L"", value,
            static_cast<DWORD>(sizeof(value) / sizeof(value[0])), IniPath.c_str());
        return value;
    };
    const wchar_t *v = read(L"NeuralRendering");
    if (v[0]) p.nr = v[0] != L'0';
    v = read(L"FrameGeneration");
    if (v[0]) p.fg = v[0] != L'0';
    v = read(L"FgMultiplier");
    if (v[0]) p.fg_multiplier = std::clamp(_wtoi(v), 2, 4);
    v = read(L"Model");
    if (v[0]) p.model = std::clamp(_wtoi(v), 1, 3);
    v = read(L"Intensity");
    if (v[0]) p.intensity = std::clamp(static_cast<float>(_wtof(v)), kNrIntensityMin, kNrIntensityMax);
    v = read(L"LocalTone");
    if (v[0]) p.tone = std::clamp(static_cast<float>(_wtof(v)), kNrToneMin, kNrToneMax);
    v = read(L"LocalStructure");
    if (v[0]) p.structure = std::clamp(static_cast<float>(_wtof(v)), kNrStructureMin, kNrStructureMax);
    v = read(L"SkinStructure");
    if (v[0]) p.skin = std::clamp(static_cast<float>(_wtof(v)), kNrSkinMin, kNrSkinMax);
}

// ===== 遥测（共享内存直读，偏移与 performance/telemetry_reader.py 一致） =====
struct Telemetry {
    bool available = false;
    bool nr_enabled = false, pipeline_ready = false, processed_visible = false, fg = false;
    bool fg_failed = false;
    unsigned source_fps = 0, proxy_fps = 0, gpu_us = 0;
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
                const unsigned source_fps = rd32(72), proxy_fps = rd32(76);
                const unsigned gpu_us = rd32(116);
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
                    out.source_fps = source_fps;
                    out.proxy_fps = proxy_fps;
                    out.gpu_us = gpu_us;
                }
                break;
            }
        }
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return out;
}

// ===== 状态 =====
static bool g_debug_log = false;   // zh: --debug（控制台按自身开关透传）；输出到 stdout.log

// zh: stdout 统一出口（此前是 wmain 里的 lambda；调试行在别的函数也要用，提为全局）。
static void OutUtf8(const wchar_t *t)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, t, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
        std::vector<char> bytes(size - 1);
        WideCharToMultiByte(CP_UTF8, 0, t, -1, bytes.data(), size - 1, nullptr, nullptr);
        fwrite(bytes.data(), 1, bytes.size(), stdout);
        fflush(stdout);
    }
}
static void DebugLine(const wchar_t *t)
{
    if (!g_debug_log) return;
    OutUtf8(L"[debug] ");
    OutUtf8(t);
    OutUtf8(L"\n");
}
static void OutLine(const wchar_t *t)
{
    OutUtf8(t);
    OutUtf8(L"\n");
}

// zh: 退出/注销前的强制落盘（R8）。复用定时保存同一套原子写路径，但不等
// 200/1000ms 重试间隔；无论成败都关闭保存通道，退出过程中不再接受新改动。
// 幂等：已关闭的通道直接返回上次结果，不重复写盘。失败必须留下日志。
static bool FlushPendingParams(const wchar_t *reason)
{
    if (g_save.closed) return !g_save.failed;
    const bool had_pending = g_save.dirty;
    const bool ok = g_save.Flush([&] { return WriteParamsAtomic(ParamsPath, g_params) && SaveCustomTier(g_params); });
    if (!ok) {
        OutLine(L"退出前参数保存失败，最后一次改动可能未写入 live-params.json。");
        if (reason) DebugLine(reason);
    } else if (had_pending) {
        DebugLine(L"pending parameters flushed before exit");
    }
    return ok;
}

// zh: 定时落盘（主循环的可见/等待两条路径共用）。失败保留 pending 并按 1s 重试。
static void SavePendingIfDue(DWORD &last_flush, DWORD now)
{
    if (!g_save.dirty || g_save.closed || now - last_flush < g_save.IntervalMs()) return;
    last_flush = now;
    const bool previously_failed = g_save.failed;
    if (g_save.Try([&] { return WriteParamsAtomic(ParamsPath, g_params) && SaveCustomTier(g_params); })) {
        DebugLine(L"live-params.json written (params applied by the carrier within ~0.5s)");
    } else if (!previously_failed) {
        OutLine(L"参数保存失败，保留请求并每秒重试。");
    }
}

static HWND g_window = nullptr;
static bool g_dragging = false;         // zh: WM_ENTERSIZEMOVE/EXITSIZEMOVE 之间
static bool g_user_positioned = false;  // zh: 用户拖动过后：位置按用户偏移跟随游戏窗口
static int g_user_off_x = 0, g_user_off_y = 0;
static RECT g_game_rect_cur{};          // zh: 主循环更新的游戏窗口矩形（拖拽结束记偏移用）
static float g_panel_w = 560.0f;        // zh: 展开面板用户尺寸（手柄缩放；侧栏布局 118+内容）
static float g_panel_h = 640.0f;
static float g_dpi_scale = 1.0f;   // zh: 悬浮面板渲染与尺寸的 DPI 缩放
static bool g_expanded = false;
static bool g_running = true;
static DWORD g_last_host_pid = 0;
static Telemetry g_tele;
static float g_game_fps = 0;
static float g_src_fps = 0;                       // 真实帧率（SR 计数差分）
static unsigned long long g_last_src_frames = 0;
static long long g_last_src_qpc = 0;
static unsigned long long g_last_presented = 0;
static long long g_last_presented_qpc = 0;
static bool g_hotkey_prev = false;
static bool g_poll_hotkey = false;
// 当前 ImGui 窗口尺寸（必须在帧内捕获；帧外 GetWindowWidth 会解引用空窗口 → 0xc0000005）
static float g_ui_w = 0.0f, g_ui_h = 0.0f;

// ===== 帧率计数差分（纯逻辑；tests/panel_regression.cpp 直接覆盖） =====
struct FpsDelta {
    bool sampled = false;   // 时间窗足够，fps 有效
    bool reset = false;     // 计数回退（宿主重启/映射重建）：调用方必须重置基线
    float fps = 0.0f;
};
// zh: 无符号计数器差分前先判 current < last（宿主重启后计数从 0 重来），
// 否则相减会回绕成 ~1.8e19 再被当成帧率（R9）。
static FpsDelta ComputeFpsDelta(unsigned long long current, unsigned long long last,
    long long now_qpc, long long last_qpc, long long frequency, double min_seconds)
{
    FpsDelta result;
    if (frequency <= 0 || last_qpc <= 0 || now_qpc <= last_qpc) return result;
    if (current < last) {
        result.reset = true;
        return result;
    }
    const double seconds = static_cast<double>(now_qpc - last_qpc) / static_cast<double>(frequency);
    if (seconds < min_seconds) return result;
    result.sampled = true;
    result.fps = static_cast<float>(static_cast<double>(current - last) / seconds);
    return result;
}

// zh: 宿主进程变化时 SR/Presented 计数从 0 重新开始，差分基线连同平滑值一起清零。
static void ResetFpsBaselines()
{
    g_last_src_frames = 0;
    g_last_src_qpc = 0;
    g_last_presented = 0;
    g_last_presented_qpc = 0;
    g_src_fps = 0.0f;
    g_game_fps = 0.0f;
}

// zh: 同一页统一源帧率口径（README：以遥测 SourceFps 为准）；遥测缺失或为 0 时回退 SR 差分。
static float SourceFps()
{
    if (g_tele.available && g_tele.source_fps > 0) return static_cast<float>(g_tele.source_fps);
    return g_src_fps;
}

// ===== 图层覆盖状态（真实窗口可见性 + z 序，不止 F10 配置位） =====
enum class LayerState { NoHost, NoOutput, RawMode, Hidden, UnderGame, Covered };
static LayerState g_layer = LayerState::NoHost;
static HWND g_game_hwnd_seen = nullptr;   // 主循环按游戏进程匹配到的窗口

// ===== 视觉主题：OpenDesign 面板稿（皮革黑 + 古金 + 苔玉） =====
// 设计源：Open Design 项目 19b1a0ea-3318-4a61-a3a7-600bbec63ae4（界面重设计·游戏内面板稿 560×640）。
static const ImU32 kColCanvas = IM_COL32(20, 16, 10, 255);       // #14100a 窗口底
static const ImU32 kColNavBg = IM_COL32(22, 17, 12, 255);        // #16110c 侧栏
static const ImU32 kColSurface = IM_COL32(27, 22, 16, 255);      // #1b1610
static const ImU32 kColSurface2 = IM_COL32(35, 28, 20, 255);     // #231c14
static const ImU32 kColSurface3 = IM_COL32(44, 35, 24, 255);     // #2c2318
static const ImU32 kColLine = IM_COL32(58, 46, 31, 255);         // #3a2e1f
static const ImU32 kColLineSoft = IM_COL32(42, 33, 23, 255);     // #2a2117
static const ImU32 kColText = IM_COL32(234, 223, 198, 255);      // #eadfc6
static const ImU32 kColText2 = IM_COL32(194, 178, 149, 255);     // #c2b295
static const ImU32 kColText3 = IM_COL32(154, 138, 109, 255);     // #9a8a6d
static const ImU32 kColAccent = IM_COL32(200, 164, 92, 255);     // #c8a45c
static const ImU32 kColAccentBright = IM_COL32(227, 200, 132, 255);
static const ImU32 kColAccentInk = IM_COL32(26, 18, 6, 255);     // #1a1206
static const ImU32 kColAccentSoft = IM_COL32(200, 164, 92, 31);  // rgba(.12)
static const ImU32 kColAccentLine = IM_COL32(200, 164, 92, 82);  // rgba(.32)
static const ImU32 kColJade = IM_COL32(109, 171, 134, 255);      // #6dab86
static const ImU32 kColJadeText = IM_COL32(201, 231, 214, 255);  // #c9e7d6
static const ImU32 kColJadeSoft = IM_COL32(109, 171, 134, 31);   // rgba(.12)
static const ImU32 kColJadeLine = IM_COL32(109, 171, 134, 87);   // rgba(.34)
static const ImU32 kColWarn = IM_COL32(217, 138, 74, 255);       // #d98a4a
static const ImU32 kColDanger = IM_COL32(191, 95, 82, 255);      // #bf5f52
static const ImU32 kColKnobLight = IM_COL32(240, 246, 242, 255);

// ===== 字体（正文微软雅黑，数字用 Constantia；加载失败时逐级回退） =====
static ImFont *g_font_body = nullptr;
static ImFont *g_font_small = nullptr;
static ImFont *g_font_h1 = nullptr;
static ImFont *g_font_num = nullptr;
static ImFont *g_font_num_sm = nullptr;

static ImFont *AddFirstAvailableFont(const char *const *paths, int count, float size)
{
    for (int i = 0; i < count; ++i) {
        if (ImFont *font = ImGui::GetIO().Fonts->AddFontFromFileTTF(paths[i], size)) return font;
    }
    return nullptr;
}

static void LoadPanelFonts()
{
    const float dpi = g_dpi_scale;
    static const char *body_paths[] = {
        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/segoeui.ttf"};
    static const char *bold_paths[] = {
        "C:/Windows/Fonts/msyhbd.ttc", "C:/Windows/Fonts/msyhbd.ttf",
        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf"};
    static const char *num_paths[] = {
        "C:/Windows/Fonts/constan.ttf", "C:/Windows/Fonts/georgia.ttf",
        "C:/Windows/Fonts/times.ttf", "C:/Windows/Fonts/segoeui.ttf"};
    g_font_body = AddFirstAvailableFont(body_paths, 4, 13.5f * dpi);
    g_font_small = AddFirstAvailableFont(body_paths, 4, 11.5f * dpi);
    g_font_h1 = AddFirstAvailableFont(bold_paths, 4, 20.0f * dpi);
    g_font_num = AddFirstAvailableFont(num_paths, 4, 34.0f * dpi);
    g_font_num_sm = AddFirstAvailableFont(num_paths, 4, 26.0f * dpi);
    if (!g_font_body) g_font_body = ImGui::GetIO().Fonts->AddFontDefault();
    if (!g_font_small) g_font_small = g_font_body;
    if (!g_font_h1) g_font_h1 = g_font_body;
    if (!g_font_num) g_font_num = g_font_body;
    if (!g_font_num_sm) g_font_num_sm = g_font_body;
}

static void ApplyStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 12.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowPadding = ImVec2(0, 0);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 5.0f;
    ImVec4 *c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(20 / 255.0f, 16 / 255.0f, 10 / 255.0f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(58 / 255.0f, 46 / 255.0f, 31 / 255.0f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(234 / 255.0f, 223 / 255.0f, 198 / 255.0f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(154 / 255.0f, 138 / 255.0f, 109 / 255.0f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(35 / 255.0f, 28 / 255.0f, 20 / 255.0f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(44 / 255.0f, 35 / 255.0f, 24 / 255.0f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(58 / 255.0f, 46 / 255.0f, 31 / 255.0f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(44 / 255.0f, 35 / 255.0f, 24 / 255.0f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(58 / 255.0f, 46 / 255.0f, 31 / 255.0f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(58 / 255.0f, 46 / 255.0f, 31 / 255.0f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(200 / 255.0f, 164 / 255.0f, 92 / 255.0f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(227 / 255.0f, 200 / 255.0f, 132 / 255.0f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(200 / 255.0f, 164 / 255.0f, 92 / 255.0f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(58 / 255.0f, 46 / 255.0f, 31 / 255.0f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(154 / 255.0f, 138 / 255.0f, 109 / 255.0f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(194 / 255.0f, 178 / 255.0f, 149 / 255.0f, 1.0f);
}

static void ToggleExpanded()
{
    g_expanded = !g_expanded;
    DebugLine(g_expanded ? L"panel expanded (interactive)" : L"panel collapsed (strip)");
    // 收起态右上信息条也保留“展开面板”按钮，因此始终可交互；仅禁激活避免抢游戏焦点。
    LONG_PTR ex = GetWindowLongPtrW(g_window, GWL_EXSTYLE);
    ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_window, GWL_EXSTYLE, ex);
    SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static const char *SchemeName()
{
    if (!g_tier_locked) return "自定义";
    for (const auto &s : kSchemes)
        if (g_params.MatchesTier(s.p)) return s.name;
    return "自定义";
}

// 查找 AIO 主输出窗（排除带 NOREDIRECTIONBITMAP 的预览窗——它无视觉内容）
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
static HWND g_owned_output = nullptr;
// F10 胶囊的客户区命中矩形：RenderUi 用与绘制完全相同的布局坐标记录，
// WM_NCHITTEST 命中它时返回 HTCLIENT；标题栏其余空白处保持 HTCAPTION（可拖动）。
static bool g_f10_hit_valid = false;
static float g_f10_hit_left = 0.0f;
static float g_f10_hit_top = 0.0f;
static float g_f10_hit_right = 0.0f;
static float g_f10_hit_bottom = 0.0f;
static void SetF10HitRect(float x, float y, float width, float height)
{
    g_f10_hit_left = x;
    g_f10_hit_top = y;
    g_f10_hit_right = x + width;
    g_f10_hit_bottom = y + height;
    g_f10_hit_valid = true;
}
static void ClearF10HitRect() { g_f10_hit_valid = false; }
static bool PointInF10Chip(int x, int y)
{
    return g_f10_hit_valid && static_cast<float>(x) >= g_f10_hit_left &&
           static_cast<float>(x) <= g_f10_hit_right && static_cast<float>(y) >= g_f10_hit_top &&
           static_cast<float>(y) <= g_f10_hit_bottom;
}
// zh: 跨进程 owner 保证面板始终压在 AIO 输出窗之上。Windows 在 owner 销毁时
// 会级联销毁 owned 窗口，因此宿主重启或输出窗重建会连带结束面板；这是已知
// 取舍，由控制中心的守护定时器重新拉起（10 秒级）兜底，不要改成父子窗口或
// 轮询 SetWindowPos——那会破坏系统级 z 序保证。
static void MaintainOutputOwnership(DWORD host_pid)
{
    if (host_pid == 0) { g_owned_output = nullptr; return; }
    HWND aio_output = FindAioMainOutput(host_pid);
    if (aio_output && aio_output != g_owned_output) {
        SetWindowLongPtrW(g_window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(aio_output));
        g_owned_output = aio_output;
        DebugLine(L"output owner attached (panel follows the AIO output window)");
    }
    else if (!aio_output && g_owned_output) {
        g_owned_output = nullptr;
        DebugLine(L"output owner lost");
    }
}

// 图层是否真正覆盖游戏窗口：F10 配置位 + 输出窗真实可见性 + z 序
static LayerState EvaluateLayerCover()
{
    if (!g_tele.available) return LayerState::NoHost;
    if (!g_tele.processed_visible) return LayerState::RawMode;   // F10 处于原画模式
    const HWND output = FindAioMainOutput(g_last_host_pid);
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

// zh: 图层覆盖/原画切换由 AIO 载具暴露为窗口消息 WM_APP + 0x5A
// （kProxyShowProcessedMessage），语义是“设置目标显示状态”而不是“切换”：
// wparam 非 0 = 显示处理画面。面板按钮直接向所选客户端的 AIO 输出窗发送一次
// 请求，不注入 F10——实体 F10 在载具里逐帧轮询，一次性 down/up 可能被漏采；
// 且实体 F10 还承担“输出窗被隐藏时先恢复可见性”的职责，按钮不应冒充它。
static constexpr UINT kAioShowProcessedMessage = WM_APP + 0x5A;

static bool PostAioShowProcessed(bool show)
{
    if (!g_tele.available || g_last_host_pid == 0) {
        OutLine(L"图层切换不可用：尚未检测到 AIO 载具遥测。");
        return false;
    }
    const HWND output = FindAioMainOutput(g_last_host_pid);
    if (!output) {
        OutLine(L"图层切换失败：未找到 AIO 输出窗（载具可能尚未就绪，请稍后重试）。");
        return false;
    }
    if (!PostMessageW(output, kAioShowProcessedMessage, show ? 1 : 0, 0)) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED)
            OutLine(L"图层切换失败：请求被系统拦截（面板与载具的权限级别不同）。");
        else
            OutLine(L"图层切换失败：无法向 AIO 输出窗发送请求。");
        return false;
    }
    DebugLine(show ? L"show-processed request posted (processed view)"
                   : L"show-processed request posted (raw view)");
    return true;
}

// 按钮语义：只切换“处理画面 / 原画”，不恢复被隐藏的输出窗；按钮状态最终以遥测为准。
static void ToggleCarrierOverlay()
{
    if (!g_tele.available) {
        OutLine(L"图层切换不可用：尚未检测到 AIO 载具遥测。");
        return;
    }
    PostAioShowProcessed(!g_tele.processed_visible);
}

static void SyncWindowLayout(const RECT &game)
{
    // 位置同步（尺寸由 ImGui 决定，OS 窗口在主循环里跟随）；拖拽/缩放中让路。
    if (g_dragging) return;
    RECT cur{};
    GetWindowRect(g_window, &cur);
    const int w = cur.right - cur.left;
    int px, py;
    if (g_user_positioned) {
        px = game.right + g_user_off_x;
        py = game.top + g_user_off_y;
    } else {
        px = game.right - w - static_cast<int>(8 * g_dpi_scale);
        py = game.top + static_cast<int>(10 * g_dpi_scale);
    }
    SetWindowPos(g_window, nullptr, px, py, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
}

// 自绘 UI 的临时提示与“模型重建中”计时（毫秒 tick）
static ULONGLONG g_toast_until = 0;
static ULONGLONG g_model_busy_until = 0;

// ===== 文本与基础绘制 =====
static float MeasureWidth(ImFont *font, const char *text)
{
    if (!font) return ImGui::CalcTextSize(text).x;
    ImGui::PushFont(font);
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::PopFont();
    return width;
}
static float FontHeight(ImFont *font)
{
    if (!font) return ImGui::GetTextLineHeight();
    ImGui::PushFont(font);
    const float height = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    return height;
}
static void PutText(ImFont *font, float x, float y, ImU32 color, const char *text)
{
    if (!font) return;
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
static void PutTextF(ImFont *font, float x, float y, ImU32 color, const char *fmt, ...)
{
    char buffer[256]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    PutText(font, x, y, color, buffer);
}
static void PutTextRight(ImFont *font, float right, float y, ImU32 color, const char *text)
{
    PutText(font, right - MeasureWidth(font, text), y, color, text);
}
static const char *FloatText(float value, int decimals)
{
    static char buffers[4][32]{};
    static int slot = 0;
    slot = (slot + 1) & 3;
    if (decimals == 1) std::snprintf(buffers[slot], sizeof buffers[slot], "%.1f", value);
    else if (decimals == 0) std::snprintf(buffers[slot], sizeof buffers[slot], "%.0f", value);
    else std::snprintf(buffers[slot], sizeof buffers[slot], "%.2f", value);
    return buffers[slot];
}
static void CardRect(ImDrawList *dl, float x, float y, float w, float h, float rounding)
{
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kColSurface, rounding);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), kColLine, rounding, 0, 1.0f * g_dpi_scale);
}

// 品牌标记：古金量表弧 + 苔玉分区的迷你版本（与图标 S2 同源）
static void DrawBrandMark(ImDrawList *dl, float cx, float cy, float radius)
{
    const float kPi = 3.14159265358979323846f;
    const float thickness = radius * 0.26f;
    float point_x[25]{}, point_y[25]{};
    for (int i = 0; i <= 24; ++i) {
        const float angle = (150.0f + 240.0f * i / 24.0f) * kPi / 180.0f;
        point_x[i] = cx + std::cos(angle) * radius;
        point_y[i] = cy + std::sin(angle) * radius;
    }
    for (int i = 0; i < 24; ++i) {
        const float angle = (150.0f + 240.0f * (i + 0.5f) / 24.0f) * kPi / 180.0f;
        const ImU32 color = angle >= 300.0f * kPi / 180.0f ? kColJade : kColAccent;
        dl->AddLine(ImVec2(point_x[i], point_y[i]), ImVec2(point_x[i + 1], point_y[i + 1]), color, thickness);
    }
    const float needle = -50.0f * kPi / 180.0f;
    dl->AddLine(ImVec2(cx, cy),
        ImVec2(cx + std::cos(needle) * radius * 0.62f, cy + std::sin(needle) * radius * 0.62f),
        kColText, radius * 0.14f);
    dl->AddCircleFilled(ImVec2(cx, cy), radius * 0.13f, kColAccent);
}

// 导航图标：状态/画面增强/补帧/模型/说明
static void DrawNavIcon(ImDrawList *dl, int page, float cx, float cy, ImU32 color)
{
    const float r = 6.0f * g_dpi_scale;
    const float t = 1.4f * g_dpi_scale;
    switch (page) {
    case 0: {   // 心跳折线
        const ImVec2 points[5] = {
            ImVec2(cx - r, cy), ImVec2(cx - r * 0.35f, cy - r * 0.8f), ImVec2(cx + r * 0.05f, cy + r * 0.55f),
            ImVec2(cx + r * 0.5f, cy - r * 0.35f), ImVec2(cx + r, cy)};
        dl->AddPolyline(points, 5, color, 0, t);
        break;
    }
    case 1: {   // 层叠
        dl->AddLine(ImVec2(cx - r, cy + r * 0.55f), ImVec2(cx, cy - r * 0.15f), color, t);
        dl->AddLine(ImVec2(cx + r, cy + r * 0.55f), ImVec2(cx, cy - r * 0.15f), color, t);
        dl->AddLine(ImVec2(cx - r * 0.7f, cy + r), ImVec2(cx, cy + r * 0.35f), color, t);
        dl->AddLine(ImVec2(cx + r * 0.7f, cy + r), ImVec2(cx, cy + r * 0.35f), color, t);
        break;
    }
    case 2: {   // 双帧
        dl->AddRect(ImVec2(cx - r, cy - r * 0.7f), ImVec2(cx + r * 0.35f, cy + r * 0.7f), color, 1.5f, 0, t);
        dl->AddRect(ImVec2(cx - r * 0.35f, cy - r * 0.2f), ImVec2(cx + r, cy + r), color, 1.5f, 0, t);
        break;
    }
    case 3: {   // 立方
        dl->AddLine(ImVec2(cx, cy - r), ImVec2(cx + r, cy - r * 0.4f), color, t);
        dl->AddLine(ImVec2(cx + r, cy - r * 0.4f), ImVec2(cx + r, cy + r * 0.45f), color, t);
        dl->AddLine(ImVec2(cx + r, cy + r * 0.45f), ImVec2(cx, cy + r), color, t);
        dl->AddLine(ImVec2(cx, cy + r), ImVec2(cx - r, cy + r * 0.45f), color, t);
        dl->AddLine(ImVec2(cx - r, cy + r * 0.45f), ImVec2(cx - r, cy - r * 0.4f), color, t);
        dl->AddLine(ImVec2(cx - r, cy - r * 0.4f), ImVec2(cx, cy - r), color, t);
        dl->AddLine(ImVec2(cx, cy - r * 0.2f), ImVec2(cx + r * 0.55f, cy - r * 0.55f), color, t * 0.8f);
        break;
    }
    default: {  // 说明
        dl->AddCircle(ImVec2(cx, cy - r * 0.25f), r * 0.8f, color, 0, t);
        dl->AddCircleFilled(ImVec2(cx, cy - r * 0.35f), r * 0.16f, color);
        dl->AddLine(ImVec2(cx, cy - r * 0.05f), ImVec2(cx, cy + r * 0.3f), color, t);
        break;
    }
    }
}

static float ChipWidth(const char *text)
{
    return MeasureWidth(g_font_small, text) + 22.0f * g_dpi_scale;
}
static void DrawChip(ImDrawList *dl, float x, float y, const char *text, bool on, bool jade)
{
    const float dpi = g_dpi_scale;
    const float height = 23.0f * dpi;
    const float width = ChipWidth(text);
    const ImU32 background = on ? (jade ? kColJadeSoft : kColAccentSoft) : kColSurface2;
    const ImU32 border = on ? (jade ? kColJadeLine : kColAccentLine) : kColLineSoft;
    const ImU32 text_color = on ? (jade ? kColJadeText : kColAccent) : kColText3;
    const ImU32 dot_color = on ? (jade ? kColJade : kColAccent) : kColText3;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), background, height * 0.5f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + height), border, height * 0.5f, 0, 1.0f * dpi);
    dl->AddCircleFilled(ImVec2(x + 9.0f * dpi, y + height * 0.5f), 2.2f * dpi, dot_color);
    PutText(g_font_small, x + 16.0f * dpi, y + (height - FontHeight(g_font_small)) * 0.5f - 1.0f * dpi,
        text_color, text);
}

// 可点胶囊（F10 图层开关）：造型与 DrawChip 一致，悬停加描边提示可点
static bool ChipButton(const char *id, float x, float y, const char *text, bool on, bool jade, bool enabled)
{
    const float dpi = g_dpi_scale;
    const float height = 23.0f * dpi;
    const float width = ChipWidth(text);
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::InvisibleButton("##chip", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked() && enabled;
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    DrawChip(dl, x, y, text, on, jade);
    if (hovered && enabled) {
        dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + height),
            on ? (jade ? kColJadeLine : kColAccentLine) : kColLine, height * 0.5f, 0, 1.5f * dpi);
    }
    ImGui::PopID();
    return clicked;
}

// 分段选择条：返回被点击的序号，-1 表示未点击
static int SegmentedBar(const char *id, float x, float y, float width,
    const char *const *items, int count, int selected)
{
    const float dpi = g_dpi_scale;
    const float pad = 4.0f * dpi;
    const float gap = 4.0f * dpi;
    const float height = 30.0f * dpi;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height + pad * 2.0f), kColSurface3, 8.0f * dpi);
    const float cell_width = (width - pad * 2.0f - gap * (count - 1)) / static_cast<float>(count);
    const float text_height = FontHeight(g_font_body);
    int clicked = -1;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        const ImVec2 cell(x + pad + i * (cell_width + gap), y + pad);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(cell);
        ImGui::InvisibleButton("##seg", ImVec2(cell_width, height));
        if (ImGui::IsItemClicked()) clicked = i;
        const bool hovered = ImGui::IsItemHovered();
        if (i == selected) {
            dl->AddRectFilled(cell, ImVec2(cell.x + cell_width, cell.y + height), kColAccent, 6.0f * dpi);
        } else if (hovered) {
            dl->AddRectFilled(cell, ImVec2(cell.x + cell_width, cell.y + height), kColSurface2, 6.0f * dpi);
        }
        const ImU32 color = i == selected ? kColAccentInk : kColText2;
        PutText(g_font_body, cell.x + (cell_width - MeasureWidth(g_font_body, items[i])) * 0.5f,
            cell.y + (height - text_height) * 0.5f - 1.0f * dpi, color, items[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
    return clicked;
}

// 按钮：kind 0=primary(古金实底) 1=secondary(描边) 2=ghost
static bool Btn(const char *id, float x, float y, float width, float height, const char *label, int kind)
{
    const float dpi = g_dpi_scale;
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::InvisibleButton("##btn", ImVec2(width, height));
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float rounding = 7.0f * dpi;
    ImU32 background = kColSurface2;
    ImU32 border = kColLine;
    ImU32 text_color = kColText;
    if (kind == 0) {
        background = hovered ? kColAccentBright : kColAccent;
        border = background;
        text_color = kColAccentInk;
    } else if (kind == 1) {
        background = hovered ? kColSurface3 : kColSurface2;
        border = kColLine;
        text_color = kColText;
    } else {
        background = hovered ? kColSurface2 : IM_COL32(0, 0, 0, 0);
        border = hovered ? kColLine : IM_COL32(0, 0, 0, 0);
        text_color = kColText2;
    }
    if ((background >> 24) != 0) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), background, rounding);
    if ((border >> 24) != 0) dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + height), border, rounding, 0, 1.0f * dpi);
    PutText(g_font_body, x + (width - MeasureWidth(g_font_body, label)) * 0.5f,
        y + (height - FontHeight(g_font_body)) * 0.5f - 1.0f * dpi, text_color, label);
    ImGui::PopID();
    return clicked;
}

// 开关行（标题 + 说明 + 右侧 38×22 开关），返回是否翻转
static bool ToggleRow(const char *id, float x, float y, float width,
    const char *title, const char *desc, bool &value)
{
    const float dpi = g_dpi_scale;
    const float switch_w = 38.0f * dpi;
    const float switch_h = 22.0f * dpi;
    const float switch_x = x + width - switch_w;
    const float switch_y = y + 7.0f * dpi;
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(switch_x, switch_y));
    ImGui::InvisibleButton("##switch", ImVec2(switch_w, switch_h));
    const bool clicked = ImGui::IsItemClicked();
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (clicked) value = !value;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(switch_x, switch_y), ImVec2(switch_x + switch_w, switch_y + switch_h),
        value ? kColJade : kColSurface3, switch_h * 0.5f);
    const float knob_radius = switch_h * 0.5f - 2.0f * dpi;
    const float knob_x = value ? switch_x + switch_w - knob_radius - 2.0f * dpi : switch_x + knob_radius + 2.0f * dpi;
    dl->AddCircleFilled(ImVec2(knob_x, switch_y + switch_h * 0.5f), knob_radius,
        value ? kColKnobLight : kColText3);
    PutText(g_font_body, x, y - 1.0f * dpi, kColText, title);
    if (desc && desc[0]) PutText(g_font_small, x, y + 22.0f * dpi, kColText3, desc);
    ImGui::PopID();
    return clicked;
}

// 自定义滑杆（轨道 4px + 14px 奶白滑钮），返回数值是否改变
static bool SliderRow(const char *id, float x, float &y, float width,
    const char *label, float &value, float minimum, float maximum, bool disabled)
{
    const float dpi = g_dpi_scale;
    PutText(g_font_small, x, y, kColText2, label);
    PutTextRight(g_font_body, x + width, y - 1.0f * dpi, kColText, FloatText(value, 2));
    const float track_y = y + 22.0f * dpi;
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(x, track_y - 8.0f * dpi));
    ImGui::InvisibleButton("##slider", ImVec2(width, 16.0f * dpi));
    if (disabled) {
        PutText(g_font_small, x, y, kColText3, label);
        PutTextRight(g_font_body, x + width, y - 1.0f * dpi, kColText3, FloatText(value, 2));
    }
    const ImU32 active_color = disabled ? kColLine : kColAccent;
    const ImU32 knob_fill = disabled ? kColSurface3 : kColKnobLight;
    bool changed = false;
    if (!disabled && ImGui::IsItemActive()) {
        const float mouse = ImGui::GetIO().MousePos.x;
        float ratio = (mouse - x) / width;
        ratio = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
        const float next = minimum + ratio * (maximum - minimum);
        if (std::fabs(next - value) > 0.0005f) { value = next; changed = true; }
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float rail = track_y + 4.0f * dpi;
    dl->AddRectFilled(ImVec2(x, rail - 2.0f * dpi), ImVec2(x + width, rail + 2.0f * dpi),
        kColSurface3, 2.0f * dpi);
    float ratio = (value - minimum) / (maximum - minimum);
    ratio = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
    dl->AddRectFilled(ImVec2(x, rail - 2.0f * dpi), ImVec2(x + width * ratio, rail + 2.0f * dpi),
        active_color, 2.0f * dpi);
    const ImVec2 knob(x + width * ratio, rail);
    dl->AddCircleFilled(knob, 7.0f * dpi, knob_fill);
    dl->AddCircle(knob, 7.0f * dpi, active_color, 0, 2.0f * dpi);
    ImGui::PopID();
    y += 44.0f * dpi;
    return changed;
}

// 提示块：surface-2 圆角底 + 11.5px 文字
// 按最大宽度手工折行。ImGui 的自动换行只认空格，中文说明会整行溢出面板，
// 这里按 UTF-8 码点逐个累加并量宽，遇到换行符也尊重；同时避免收尾标点落在行首。
static bool IsClosingPunct(const char *glyph)
{
    static const char *closing[] = {"。", "，", "、", "；", "：", "！", "？", "）", ")", "」", "』", "】", "》", "%", "…"};
    for (const char *item : closing)
        if (std::strcmp(glyph, item) == 0) return true;
    return false;
}
static bool IsOpeningPunct(const char *glyph)
{
    static const char *opening[] = {"（", "(", "「", "『", "【", "《"};
    for (const char *item : opening)
        if (std::strcmp(glyph, item) == 0) return true;
    return false;
}
static float GlyphRunWidth(ImFont *font, const std::vector<std::string> &glyphs)
{
    std::string joined;
    for (const std::string &glyph : glyphs) joined += glyph;
    return MeasureWidth(font, joined.c_str());
}
static void FlushLine(std::vector<std::string> &lines, std::vector<std::string> &glyphs)
{
    std::string line;
    for (const std::string &glyph : glyphs) line += glyph;
    lines.push_back(line);
    glyphs.clear();
}
static std::vector<std::string> WrapCjk(ImFont *font, const char *text, float max_width)
{
    std::vector<std::string> lines;
    std::vector<std::string> glyphs;
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            FlushLine(lines, glyphs);
            ++p;
            continue;
        }
        int length = 1;
        const unsigned char lead = static_cast<unsigned char>(*p);
        if ((lead & 0xE0) == 0xC0) length = 2;
        else if ((lead & 0xF0) == 0xE0) length = 3;
        else if ((lead & 0xF8) == 0xF0) length = 4;
        glyphs.push_back(std::string(p, length));
        p += length;
        if (glyphs.size() > 1 && GlyphRunWidth(font, glyphs) > max_width) {
            const std::string glyph = glyphs.back();
            glyphs.pop_back();
            const bool carry_prev = !glyphs.empty() &&
                (IsClosingPunct(glyph.c_str()) || IsOpeningPunct(glyphs.back().c_str()));
            if (carry_prev && glyphs.size() >= 2) {
                std::string carry = glyphs.back();
                glyphs.pop_back();
                FlushLine(lines, glyphs);
                glyphs.push_back(carry);
            } else {
                FlushLine(lines, glyphs);
            }
            glyphs.push_back(glyph);
        }
    }
    if (!glyphs.empty() || lines.empty()) FlushLine(lines, glyphs);
    return lines;
}

static void InfoBlock(float x, float &y, float width, const char *text, bool warn)
{
    const float dpi = g_dpi_scale;
    const float pad = 10.0f * dpi;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const std::vector<std::string> lines = WrapCjk(g_font_small, text, width - pad * 2.0f);
    const float line_step = FontHeight(g_font_small) + 5.0f * dpi;
    const float height = line_step * static_cast<float>(lines.size()) + pad * 2.0f - 5.0f * dpi;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height),
        warn ? IM_COL32(217, 138, 74, 26) : kColSurface2, 8.0f * dpi);
    dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + height),
        warn ? IM_COL32(217, 138, 74, 90) : kColLineSoft, 8.0f * dpi, 0, 1.0f * dpi);
    float line_y = y + pad - 1.0f * dpi;
    for (const std::string &line : lines) {
        PutText(g_font_small, x + pad, line_y, warn ? kColWarn : kColText2, line.c_str());
        line_y += line_step;
    }
    y += height + 10.0f * dpi;
}

// 分区标题：5px 菱形 + 11.5px 文字 + 发丝线
static void SectionTitle(float x, float &y, float width, const char *title)
{
    const float dpi = g_dpi_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float cy = y + 8.0f * dpi;
    const ImVec2 diamond[4] = {
        ImVec2(x + 3.0f * dpi, cy), ImVec2(x + 6.0f * dpi, cy - 3.0f * dpi),
        ImVec2(x + 9.0f * dpi, cy), ImVec2(x + 6.0f * dpi, cy + 3.0f * dpi)};
    dl->AddConvexPolyFilled(diamond, 4, kColAccent);
    PutText(g_font_small, x + 14.0f * dpi, y, kColText2, title);
    const float line_x = x + 20.0f * dpi + MeasureWidth(g_font_small, title);
    if (line_x < x + width) dl->AddLine(ImVec2(line_x, cy), ImVec2(x + width, cy), kColLineSoft, 1.0f * dpi);
    y += 22.0f * dpi;
}

// 统计行：左侧组名 + 三列（小标签 + 值）
static void StatRow(float x, float &y, float width, const char *label,
    const char *caption1, const char *value1, const char *caption2, const char *value2,
    const char *caption3, const char *value3)
{
    const float dpi = g_dpi_scale;
    const float label_width = 52.0f * dpi;
    const float cell_width = (width - label_width) / 3.0f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + label_width - 6.0f * dpi, y + 40.0f * dpi), kColSurface3, 5.0f * dpi);
    PutText(g_font_small, x + 6.0f * dpi, y + 12.0f * dpi, kColText3, label);
    const char *captions[3] = {caption1, caption2, caption3};
    const char *values[3] = {value1, value2, value3};
    for (int i = 0; i < 3; ++i) {
        const float cell_x = x + label_width + i * cell_width;
        PutText(g_font_small, cell_x, y + 0.0f * dpi, kColText3, captions[i]);
        PutText(g_font_body, cell_x, y + 17.0f * dpi, kColText, values[i]);
    }
    y += 46.0f * dpi;
}

// 页面头： eyebrow（带 live 点）+ 20px 标题 + 说明
static void PageHead(float x, float &y, float width, const char *eyebrow, const char *title, const char *sub)
{
    const float dpi = g_dpi_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(x + 3.0f * dpi, y + 6.0f * dpi), 2.4f * dpi,
        g_tele.available ? kColJade : kColText3);
    PutText(g_font_small, x + 10.0f * dpi, y, kColAccent, eyebrow);
    y += 20.0f * dpi;
    PutText(g_font_h1, x, y, kColText, title);
    y += 30.0f * dpi;
    PutText(g_font_small, x, y, kColText3, sub);
    y += 24.0f * dpi;
    (void)width;
}

// ===== 页面：状态 =====
static void RenderStatusPage(float x, float &y, float width)
{
    const float dpi = g_dpi_scale;
    PageHead(x, y, width, "实时状态", "状态", "载体遥测每 500 ms 刷新一次");

    const float hero_height = 158.0f * dpi;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + hero_height), kColSurface2, 10.0f * dpi);
    dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + hero_height), kColLine, 10.0f * dpi, 0, 1.0f * dpi);

    PutText(g_font_small, x + 14.0f * dpi, y + 12.0f * dpi, kColText3, "输出帧率");
    const char *fps_text = g_tele.available && g_game_fps > 0.0f ? FloatText(g_game_fps, 1) : "—";
    PutText(g_font_num, x + 14.0f * dpi, y + 24.0f * dpi, kColText, fps_text);
    const float value_width = MeasureWidth(g_font_num, fps_text);
    PutText(g_font_body, x + 14.0f * dpi + value_width + 6.0f * dpi,
        y + 24.0f * dpi + FontHeight(g_font_num) - FontHeight(g_font_body) - 6.0f * dpi,
        kColAccent, "FPS");

    // 图层胶囊（右上）
    const char *layer_text = "图层未确认";
    bool layer_on = false;
    switch (g_layer) {
    case LayerState::Covered: layer_text = "图层覆盖中"; layer_on = true; break;
    case LayerState::RawMode: layer_text = "原画模式"; break;
    case LayerState::Hidden: layer_text = "图层已隐藏"; break;
    case LayerState::UnderGame: layer_text = "图层被遮挡"; break;
    case LayerState::NoOutput: layer_text = "无输出窗"; break;
    default: break;
    }
    DrawChip(dl, x + width - 14.0f * dpi - ChipWidth(layer_text), y + 12.0f * dpi, layer_text, layer_on, true);

    if (!g_tele.available) {
        PutText(g_font_small, x + 14.0f * dpi, y + 84.0f * dpi, kColText3, "启动游戏后显示实时数据");
    } else if (SourceFps() > 0.0f && g_game_fps > 0.0f) {
        // zh: 与下方「运行数据·源帧率」同一口径（遥测 SourceFps 优先）；差分回退时标注
        const float src = SourceFps();
        const bool src_from_delta = g_tele.source_fps == 0;
        const float gain = src > 0.01f ? g_game_fps / src : 0.0f;
        PutTextF(g_font_small, x + 14.0f * dpi, y + 84.0f * dpi, kColText3,
            "源 %.1f%s → 输出 %.1f · 增益 %.2f×", src, src_from_delta ? "（差分）" : "", g_game_fps, gain);
    } else {
        PutText(g_font_small, x + 14.0f * dpi, y + 84.0f * dpi, kColText3, "正在采样帧率…");
    }

    const float fact_top = y + 110.0f * dpi;
    const float fact_width = (width - 28.0f * dpi) / 4.0f;
    const char *fact_labels[4] = {"档位", "模型", "强度", "管线"};
    const char *fact_values[4] = {
        SchemeName(), kModelNames[g_params.model - 1], FloatText(g_params.intensity, 2),
        g_tele.available ? (g_tele.pipeline_ready ? "就绪" : "未就绪") : "未确认"};
    for (int i = 0; i < 4; ++i) {
        const float fact_x = x + 14.0f * dpi + i * fact_width;
        PutText(g_font_small, fact_x, fact_top, kColText3, fact_labels[i]);
        PutText(g_font_body, fact_x, fact_top + 16.0f * dpi, kColText, fact_values[i]);
        if (i > 0) {
            dl->AddLine(ImVec2(fact_x - 6.0f * dpi, fact_top - 2.0f * dpi),
                ImVec2(fact_x - 6.0f * dpi, fact_top + 34.0f * dpi), kColLineSoft, 1.0f * dpi);
        }
    }
    y += hero_height + 18.0f * dpi;

    SectionTitle(x, y, width, "运行数据");
    const char *dash = "—";
    char res_in[32]{}, res_out[32]{}, res_scale[32]{};
    char latency[32]{}, source_fps[32]{}, output_fps[32]{};
    char nr_frames[32]{}, sr_frames[32]{}, fg_frames[32]{};
    if (g_tele.available) {
        std::snprintf(res_in, sizeof res_in, "%ux%u", g_tele.input_w, g_tele.input_h);
        std::snprintf(res_out, sizeof res_out, "%ux%u", g_tele.output_w, g_tele.output_h);
        std::snprintf(res_scale, sizeof res_scale, "%.2f×",
            g_tele.input_w > 0 ? static_cast<double>(g_tele.output_w) / g_tele.input_w : 1.0);
        if (g_tele.gpu_us > 0) std::snprintf(latency, sizeof latency, "%.1f ms", g_tele.gpu_us / 1000.0);
        std::snprintf(source_fps, sizeof source_fps, "%.1f", static_cast<double>(SourceFps()));
        std::snprintf(output_fps, sizeof output_fps, "%.1f", static_cast<double>(g_game_fps));
        std::snprintf(nr_frames, sizeof nr_frames, "%llu", g_tele.nr_frames);
        std::snprintf(sr_frames, sizeof sr_frames, "%llu", g_tele.sr_frames);
        std::snprintf(fg_frames, sizeof fg_frames, "%llu", g_tele.fg_frames);
    }
    StatRow(x, y, width, "分辨率", "输入", g_tele.available ? res_in : dash,
        "输出", g_tele.available ? res_out : dash, "缩放", g_tele.available ? res_scale : dash);
    StatRow(x, y, width, "性能", "延迟", g_tele.available && g_tele.gpu_us > 0 ? latency : dash,
        "源帧率", g_tele.available ? source_fps : dash, "输出帧率", g_tele.available ? output_fps : dash);
    StatRow(x, y, width, "计数", "增强帧", g_tele.available ? nr_frames : dash,
        "超分帧", g_tele.available ? sr_frames : dash, "生成帧", g_tele.available ? fg_frames : dash);
    y += 6.0f * dpi;
    InfoBlock(x, y, width, "调整即时生效（约 0.5 秒内由载体应用）；关闭面板不会回滚设置。", false);
}

// ===== 页面：画面增强 =====
static void RenderNrPage(float x, float &y, float width)
{
    const float dpi = g_dpi_scale;
    PageHead(x, y, width, "画质", "画面增强", "Neural Rendering · 调整即时生效");

    const float card_height = 58.0f * dpi;
    CardRect(ImGui::GetWindowDrawList(), x, y, width, card_height, 8.0f * dpi);
    if (ToggleRow("nr-toggle", x + 14.0f * dpi, y + 10.0f * dpi, width - 28.0f * dpi,
        "画面增强", "关闭时等同原画，保留引擎原始输出", g_params.nr)) {
        g_save.Request();
    }
    y += card_height + 16.0f * dpi;

    if (g_save.failed) {
        char warn_text[96]{};
        std::snprintf(warn_text, sizeof warn_text, "参数未保存，正在重试");
        PutText(g_font_small, x, y, kColWarn, warn_text);
        y += 18.0f * dpi;
    }

    SectionTitle(x, y, width, "档位");
    static const char *tier_items[5] = {"原画", "轻量", "均衡", "浓郁", "自定义"};
    int selected_tier = -1;
    for (int i = 0; i < 4; ++i) {
        if (g_tier_locked && g_params.MatchesTier(kSchemes[i].p)) selected_tier = i;
    }
    if (!g_tier_locked) selected_tier = 4;
    const int tier_clicked = SegmentedBar("tier", x, y, width, tier_items, 5, selected_tier);
    y += 30.0f * dpi + 8.0f * dpi + 8.0f * dpi;
    if (tier_clicked >= 0 && tier_clicked < 4) {
        const int keep_model = g_params.model;
        const bool keep_fg = g_params.fg;
        const int keep_multiplier = g_params.fg_multiplier;
        g_params = kSchemes[tier_clicked].p;
        g_params.model = keep_model;
        g_params.fg = keep_fg;
        g_params.fg_multiplier = keep_multiplier;
        ClampParams(g_params);
        g_tier_locked = true;
        g_save.Request();
    } else if (tier_clicked == 4 && g_tier_locked) {
        g_tier_locked = false;
        if (g_custom_valid && !g_params.Equals(g_custom)) {
            g_params = g_custom;
            g_save.Request();
        }
    }
    PutText(g_font_small, x, y, kColText3, "档位是完整配方（强度/色调/结构/皮肤）；原画等于关闭 NR。");
    y += 24.0f * dpi;

    SectionTitle(x, y, width, "微调");
    if (g_tier_locked) {
        PutText(g_font_small, x, y, kColText3, "出厂档位锁定。点「自定义」解锁微调。");
        y += 22.0f * dpi;
    }
    ImGui::BeginDisabled(g_tier_locked);
    bool dirty = false;
    dirty = SliderRow("intensity", x, y, width, "整体重绘幅度", g_params.intensity,
        kNrIntensityMin, kNrIntensityMax, g_tier_locked) || dirty;
    PutText(g_font_small, x, y - 18.0f * dpi, kColText3, "0 关闭/最弱，1 满强度；过高会放大锐化与伪影。");
    y += 4.0f * dpi;
    dirty = SliderRow("tone", x, y, width, "色调结构", g_params.tone, kNrToneMin, kNrToneMax, g_tier_locked) || dirty;
    PutText(g_font_small, x, y - 18.0f * dpi, kColText3, "1.00 为中性（0–2）；越低越平，越高保留越多。");
    y += 4.0f * dpi;
    dirty = SliderRow("structure", x, y, width, "结构强度", g_params.structure,
        kNrStructureMin, kNrStructureMax, g_tier_locked) || dirty;
    PutText(g_font_small, x, y - 18.0f * dpi, kColText3, "0 最平滑，1 细节最多；过高会放大噪点。");
    y += 4.0f * dpi;
    dirty = SliderRow("skin", x, y, width, "皮肤结构", g_params.skin, kNrSkinMin, kNrSkinMax, g_tier_locked) || dirty;
    PutText(g_font_small, x, y - 18.0f * dpi, kColText3, "-1 更平滑，0 中性，+1 保留更多纹理。");
    y += 6.0f * dpi;
    ImGui::EndDisabled();
    if (dirty) g_save.Request();
    ClampParams(g_params);

    if (Btn("reread", x, y, 150.0f * dpi, 32.0f * dpi, "重新读取当前设置", 1)) {
        Params p;
        ReadIniParams(p);
        ClampParams(p);
        g_params = p;
        g_toast_until = GetTickCount64() + 2400;
        DebugLine(L"carrier ReShade.ini re-read into the panel (display only)");
    }
    if (g_toast_until > GetTickCount64())
        PutText(g_font_small, x + 160.0f * dpi, y + 9.0f * dpi, kColJadeText, "已重新读取载体当前设置");
    y += 32.0f * dpi + 8.0f * dpi;
}

// ===== 页面：补帧 =====
static void RenderFgPage(float x, float &y, float width)
{
    const float dpi = g_dpi_scale;
    PageHead(x, y, width, "画质", "补帧", "DLSS-G · 2× / 3× / 4× 插帧");

    const float card_height = 58.0f * dpi;
    CardRect(ImGui::GetWindowDrawList(), x, y, width, card_height, 8.0f * dpi);
    if (ToggleRow("fg-toggle", x + 14.0f * dpi, y + 10.0f * dpi, width - 28.0f * dpi,
        "补帧", "关闭时只输出超分帧", g_params.fg)) {
        g_save.Request();
    }
    y += card_height + 16.0f * dpi;

    // 状态胶囊
    const char *state_text = "未确认";
    bool state_on = false;
    bool state_warn = false;
    if (g_tele.available) {
        if (!g_tele.fg) { state_text = "补帧 关"; }
        else if (g_tele.fg_failed) { state_text = "补帧失败"; state_warn = true; }
        else if (g_tele.fg_frames > 0) {
            char buffer[64]{};
            std::snprintf(buffer, sizeof buffer, "已插帧 %llu", g_tele.fg_frames);
            PutTextF(g_font_small, x, y + 4.0f * dpi, kColJadeText, "已插帧 %llu", g_tele.fg_frames);
            state_text = "补帧 开"; state_on = true;
        }
        else { state_text = "补帧 等待"; state_warn = true; }
    }
    const float chip_x = x + width - ChipWidth(state_text);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (g_tele.available && g_tele.fg && g_tele.fg_frames > 0) {
        // 上方已经输出文字，这里补一个胶囊在右侧
        DrawChip(dl, chip_x, y, state_text, state_on, true);
        y += 30.0f * dpi;
    } else {
        DrawChip(dl, x, y, state_text, false, false);
        y += 26.0f * dpi;
        if (state_warn) PutText(g_font_small, x + ChipWidth(state_text) + 8.0f * dpi, y - 22.0f * dpi,
            kColWarn, "开启但求值失败时显示真实帧");
    }
    if (g_tele.available)
        PutTextF(g_font_small, x, y, kColText3, "累计：增强帧 %llu → 生成帧 %llu",
            g_tele.nr_frames, g_tele.fg_frames), y += 20.0f * dpi;

    SectionTitle(x, y, width, "补帧倍数");
    static const char *multiplier_items[3] = {"2×", "3×", "4×"};
    const int multiplier_selected = std::clamp(g_params.fg_multiplier, 2, 4) - 2;
    const int multiplier_clicked = SegmentedBar("fg-multiplier", x, y, width,
        multiplier_items, 3, multiplier_selected);
    y += 30.0f * dpi + 8.0f * dpi + 8.0f * dpi;
    if (multiplier_clicked >= 0) {
        g_params.fg_multiplier = multiplier_clicked + 2;
        g_save.Request();
    }
    if (g_params.fg_multiplier >= 4)
        InfoBlock(x, y, width, "4× 需要 RTX 50 系显卡；其他卡载体自动回落 2×。4× 显著增加显存占用，出现提示时请降低档位。", true);
    else
        InfoBlock(x, y, width, "3×/4× 需要 RTX 50 系显卡（其他卡自动回落 2×）。倍数越高呈现帧率越高，动态画面软化与延迟也会增加。", false);
    InfoBlock(x, y, width, "插帧会成倍增加显存与内存占用。源帧率上限请在桌面控制台设置（默认 30，防止内存爆涨退出）。", false);
}

// ===== 页面：模型 =====
static void RenderModelPage(float x, float &y, float width)
{
    const float dpi = g_dpi_scale;
    PageHead(x, y, width, "画质", "模型", "三套引擎外观方案，切换后约 1 秒后台重建");

    static const char *model_short[3] = {"默认", "自然", "电影"};
    for (int m = 1; m <= 3; ++m) {
        const float row_height = 66.0f * dpi;
        const bool selected = g_params.model == m;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + row_height),
            selected ? kColAccentSoft : kColSurface, 10.0f * dpi);
        dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + row_height),
            selected ? kColAccentLine : kColLineSoft, 10.0f * dpi, 0, 1.0f * dpi);
        const float radio_x = x + 16.0f * dpi, radio_y = y + row_height * 0.5f;
        dl->AddCircle(ImVec2(radio_x, radio_y), 7.0f * dpi, selected ? kColAccent : kColText3, 0, 1.6f * dpi);
        if (selected) dl->AddCircleFilled(ImVec2(radio_x, radio_y), 3.4f * dpi, kColAccent);
        PutText(g_font_body, x + 34.0f * dpi, y + 12.0f * dpi, kColText, kModelNames[m - 1]);
        if (selected) PutText(g_font_small, x + 34.0f * dpi + MeasureWidth(g_font_body, kModelNames[m - 1]) + 8.0f * dpi,
            y + 15.0f * dpi, kColAccent, "使用中");
        if (g_model_busy_until > GetTickCount64() && selected)
            PutText(g_font_small, x + width - 14.0f * dpi - MeasureWidth(g_font_small, "重建中"),
                y + 15.0f * dpi, kColAccent, "重建中");
        ImGui::PushID(m);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::InvisibleButton("##model", ImVec2(width, row_height));
        if (ImGui::IsItemClicked() && g_params.model != m) {
            g_params.model = m;
            g_save.Request();
            g_model_busy_until = GetTickCount64() + 1200;
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        // 描述：wrap 到右侧可用宽度
        ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, kColText3);
        ImGui::SetCursorScreenPos(ImVec2(x + 34.0f * dpi, y + 34.0f * dpi));
        ImGui::PushTextWrapPos(x + width - 14.0f * dpi);
        ImGui::TextUnformatted(kModelDesc[m - 1]);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        y += row_height + 10.0f * dpi;
    }
    y += 6.0f * dpi;
    InfoBlock(x, y, width, "模型是引擎外观风格；切换后立即写入，后台约 1 秒重建（期间帧率短暂波动）。", false);
}

// ===== 页面：说明 =====
static void RenderHelpPage(float x, float &y, float width)
{
    const float dpi = g_dpi_scale;
    PageHead(x, y, width, "其他", "说明", "热键与调参约定");
    ImDrawList *dl = ImGui::GetWindowDrawList();

    auto bullet = [&](const char *text) {
        dl->AddCircleFilled(ImVec2(x + 4.0f * dpi, y + 8.0f * dpi), 1.8f * dpi, kColText3);
        PutText(g_font_small, x + 14.0f * dpi, y, kColText2, text);
        y += 22.0f * dpi;
    };
    SectionTitle(x, y, width, "操作");
    bullet("F10：切换图层覆盖（原画对比；AIO 热键；输出窗隐藏时先恢复可见性）");
    bullet("点标题栏「处理画面」或收起态右侧图层名：向载具发送一次显示状态请求（只切换处理画面/原画）");
    bullet("F11：展开 / 收起本面板");
    bullet("拖动顶部标题行移动面板");
    y += 8.0f * dpi;
    SectionTitle(x, y, width, "调参");
    bullet("调整即时生效，经 live-params 写入；关闭面板不回滚");
    bullet("档位是出厂配方（微调锁定）；点「自定义」解锁滑杆");
    bullet("模型切换立即生效，约 1 秒后台重建");
    y += 8.0f * dpi;
    SectionTitle(x, y, width, "其他");
    bullet("源帧率上限在桌面控制台设置（防插帧爆内存）");
    y += 10.0f * dpi;
    InfoBlock(x, y, width, "关于：WOW-335-NR 游戏内面板 · 作者 Mcode · 工具版本 1.2.0", false);
}

// ===== 展开态 / 收起态 =====
static int g_nav_page = 0;   // 0 状态 / 1 画面增强 / 2 补帧 / 3 模型 / 4 说明
static void RenderStrip(float dpi)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(238.0f * dpi, 176.0f * dpi), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 11.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * dpi);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(27 / 255.0f, 22 / 255.0f, 16 / 255.0f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, kColLine);
    ImGui::Begin("##strip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    const ImVec2 origin = ImGui::GetWindowPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float x = origin.x + 12.0f * dpi;
    float y = origin.y + 12.0f * dpi;

    DrawBrandMark(dl, x + 8.0f * dpi, y + 9.0f * dpi, 8.0f * dpi);
    PutText(g_font_body, x + 22.0f * dpi, y, kColText, "WOW-335-NR");
    const char *scheme = SchemeName();
    const float pill_width = MeasureWidth(g_font_small, scheme) + 16.0f * dpi;
    dl->AddRectFilled(ImVec2(origin.x + 238.0f * dpi - 12.0f * dpi - pill_width, y),
        ImVec2(origin.x + 238.0f * dpi - 12.0f * dpi, y + 20.0f * dpi), kColSurface3, 10.0f * dpi);
    PutText(g_font_small, origin.x + 238.0f * dpi - 12.0f * dpi - pill_width + 8.0f * dpi,
        y + 3.0f * dpi, kColAccent, scheme);
    y += 26.0f * dpi;

    const bool nr_on = g_tele.available ? g_tele.nr_enabled : g_params.nr;
    const bool fg_on = g_tele.available ? g_tele.fg : g_params.fg;
    DrawChip(dl, x, y, nr_on ? "画面增强 开" : "画面增强 关", nr_on, true);
    DrawChip(dl, x + ChipWidth(nr_on ? "画面增强 开" : "画面增强 关") + 8.0f * dpi, y,
        fg_on ? "补帧 开" : "补帧 关", fg_on, true);
    y += 30.0f * dpi;

    const char *fps_text = g_tele.available && g_game_fps > 0.0f ? FloatText(g_game_fps, 1) : "—";
    PutText(g_font_num_sm, x, y, kColText, fps_text);
    PutText(g_font_small, x + MeasureWidth(g_font_num_sm, fps_text) + 5.0f * dpi,
        y + FontHeight(g_font_num_sm) - FontHeight(g_font_small) - 3.0f * dpi, kColAccent, "FPS");
    {
        const char *layer_short = LayerShortName(g_layer);
        const float lw = MeasureWidth(g_font_small, layer_short);
        const float lx = origin.x + 238.0f * dpi - 12.0f * dpi - lw;
        ImGui::SetCursorScreenPos(ImVec2(lx - 3.0f * dpi, y + 3.0f * dpi));
        ImGui::InvisibleButton("##strip-f10", ImVec2(lw + 6.0f * dpi, 20.0f * dpi));
        const bool hovered = ImGui::IsItemHovered() && g_tele.available;
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hovered && ImGui::IsItemClicked()) ToggleCarrierOverlay();
        PutText(g_font_small, lx, y + 8.0f * dpi, hovered ? kColAccent : kColText3, layer_short);
    }
    y += 30.0f * dpi;
    if (g_tele.available) {
        char sub[96]{};
        std::snprintf(sub, sizeof sub, "%ux%u → %ux%u", g_tele.input_w, g_tele.input_h,
            g_tele.output_w, g_tele.output_h);
        PutText(g_font_small, x, y, kColText3, sub);
    } else {
        PutText(g_font_small, x, y, kColText3, "等待载体遥测…");
    }
    y += 20.0f * dpi;
    if (Btn("strip-expand", x, y, 238.0f * dpi - 24.0f * dpi, 32.0f * dpi, "展开面板", 1)) ToggleExpanded();
    g_ui_w = ImGui::GetWindowWidth();
    g_ui_h = ImGui::GetWindowHeight();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

static void RenderUi()
{
    const float dpi = g_dpi_scale;
    if (!g_expanded) {
        ClearF10HitRect();
        RenderStrip(dpi);
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(g_panel_w * dpi, g_panel_h * dpi), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(480 * dpi, 460 * dpi), ImVec2(900 * dpi, 1800 * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * dpi);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kColCanvas);
    ImGui::PushStyleColor(ImGuiCol_Border, kColLine);
    ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 wpos = ImGui::GetWindowPos();
    const float wtotal = ImGui::GetWindowWidth();
    const float htotal = ImGui::GetWindowHeight();
    const float cap_h = 48.0f * dpi;
    const float nav_w = 136.0f * dpi;
    const float bot_h = 44.0f * dpi;
    const float body_top = wpos.y + cap_h;
    const float body_height = htotal - cap_h - bot_h;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // ---- 标题栏 ----
    dl->AddRectFilled(wpos, ImVec2(wpos.x + wtotal, body_top), kColCanvas);
    dl->AddLine(ImVec2(wpos.x, body_top - 1.0f * dpi), ImVec2(wpos.x + wtotal, body_top - 1.0f * dpi),
        kColLineSoft, 1.0f * dpi);
    DrawBrandMark(dl, wpos.x + 27.0f * dpi, wpos.y + cap_h * 0.5f, 9.0f * dpi);
    PutText(g_font_body, wpos.x + 42.0f * dpi, wpos.y + cap_h * 0.5f - FontHeight(g_font_body) * 0.5f,
        kColText, "WOW-335-NR");
    // 状态胶囊靠右（收起按钮左侧）
    const bool nr_on = g_tele.available ? g_tele.nr_enabled : g_params.nr;
    const bool fg_on = g_tele.available ? g_tele.fg : g_params.fg;
    char nr_text[24]{}, fg_text[24]{};
    std::snprintf(nr_text, sizeof nr_text, "画面增强 %s", nr_on ? "开" : "关");
    std::snprintf(fg_text, sizeof fg_text, "补帧 %s", fg_on ? "开" : "关");
    // 处理画面/原画：按钮只向载具发送“设置显示状态”请求，不等同于实体 F10
    //（F10 还会在输出窗隐藏时先恢复可见性）；最终状态以遥测 processed_visible 为准。
    const bool overlay_on = g_tele.available && g_tele.processed_visible;
    const char *f10_text = overlay_on ? "处理画面 开" : "处理画面 关";
    const float collapse_w = 36.0f * dpi;
    const float chip_right = wpos.x + wtotal - collapse_w - 8.0f * dpi;
    const float fg_width = ChipWidth(fg_text);
    const float nr_width = ChipWidth(nr_text);
    const float f10_width = ChipWidth(f10_text);
    const float chip_y = wpos.y + (cap_h - 23.0f * dpi) * 0.5f;
    const float fg_x = chip_right - fg_width;
    const float nr_x = fg_x - 8.0f * dpi - nr_width;
    const float f10_x = nr_x - 8.0f * dpi - f10_width;
    // 命中测试与绘制同源：胶囊矩形记录在客户区坐标，供 WM_NCHITTEST 返回 HTCLIENT。
    if (g_tele.available)
        SetF10HitRect(f10_x, chip_y, f10_width, 23.0f * dpi);
    else
        ClearF10HitRect();
    if (ChipButton("f10-overlay", f10_x, chip_y, f10_text, overlay_on, true, g_tele.available))
        ToggleCarrierOverlay();
    DrawChip(dl, nr_x, chip_y, nr_text, nr_on, true);
    DrawChip(dl, fg_x, chip_y, fg_text, fg_on, true);
    if (wtotal >= 560.0f * dpi) {
        const float meta_x = f10_x - 12.0f * dpi - MeasureWidth(g_font_small, "Mcode · v1.2.0");
        PutText(g_font_small, meta_x, wpos.y + (cap_h - FontHeight(g_font_small)) * 0.5f,
            kColText3, "Mcode · v1.2.0");
    }
    // 收起按钮（右上）
    {
        const float button_x = wpos.x + wtotal - collapse_w - 2.0f * dpi;
        const float button_y = wpos.y + (cap_h - 26.0f * dpi) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(button_x, button_y));
        ImGui::InvisibleButton("##collapse", ImVec2(26.0f * dpi, 26.0f * dpi));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hovered)
            dl->AddRectFilled(ImVec2(button_x, button_y),
                ImVec2(button_x + 26.0f * dpi, button_y + 26.0f * dpi), kColSurface2, 6.0f * dpi);
        const float icon_cx = button_x + 13.0f * dpi, icon_cy = button_y + 13.0f * dpi;
        dl->AddLine(ImVec2(icon_cx - 4.0f * dpi, icon_cy + 1.5f * dpi),
            ImVec2(icon_cx, icon_cy - 3.0f * dpi), kColText2, 1.5f * dpi);
        dl->AddLine(ImVec2(icon_cx, icon_cy - 3.0f * dpi),
            ImVec2(icon_cx + 4.0f * dpi, icon_cy + 1.5f * dpi), kColText2, 1.5f * dpi);
        if (ImGui::IsItemClicked()) ToggleExpanded();
    }

    // ---- 左侧导航 ----
    dl->AddRectFilled(ImVec2(wpos.x, body_top), ImVec2(wpos.x + nav_w, body_top + body_height), kColNavBg);
    dl->AddLine(ImVec2(wpos.x + nav_w, body_top), ImVec2(wpos.x + nav_w, body_top + body_height),
        kColLineSoft, 1.0f * dpi);
    static const char *nav_labels[5] = {"状态", "画面增强", "补帧", "模型", "说明"};
    static const char *nav_eyebrows[2] = {"画质", "其他"};
    float nav_y = body_top + 16.0f * dpi;
    PutText(g_font_small, wpos.x + 18.0f * dpi, nav_y, kColText3, nav_eyebrows[0]);
    nav_y += 22.0f * dpi;
    for (int i = 0; i < 4; ++i) {
        const float item_x = wpos.x + 8.0f * dpi;
        const float item_h = 34.0f * dpi;
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(item_x, nav_y));
        ImGui::InvisibleButton("##nav", ImVec2(nav_w - 16.0f * dpi, item_h));
        if (ImGui::IsItemClicked()) g_nav_page = i;
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (g_nav_page == i) {
            dl->AddRectFilled(ImVec2(item_x, nav_y), ImVec2(item_x + nav_w - 16.0f * dpi, nav_y + item_h),
                kColAccentSoft, 6.0f * dpi);
            dl->AddRectFilled(ImVec2(item_x, nav_y), ImVec2(item_x + 3.0f * dpi, nav_y + item_h),
                kColAccent, 2.0f * dpi);
        }
        DrawNavIcon(dl, i, item_x + 18.0f * dpi, nav_y + item_h * 0.5f,
            g_nav_page == i ? kColAccent : kColText3);
        PutText(g_font_body, item_x + 34.0f * dpi, nav_y + (item_h - FontHeight(g_font_body)) * 0.5f,
            g_nav_page == i ? kColText : kColText2, nav_labels[i]);
        nav_y += item_h + 2.0f * dpi;
        ImGui::PopID();
    }
    nav_y += 12.0f * dpi;
    PutText(g_font_small, wpos.x + 18.0f * dpi, nav_y, kColText3, nav_eyebrows[1]);
    nav_y += 22.0f * dpi;
    {
        const float item_x = wpos.x + 8.0f * dpi;
        const float item_h = 34.0f * dpi;
        ImGui::PushID(4);
        ImGui::SetCursorScreenPos(ImVec2(item_x, nav_y));
        ImGui::InvisibleButton("##nav", ImVec2(nav_w - 16.0f * dpi, item_h));
        if (ImGui::IsItemClicked()) g_nav_page = 4;
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (g_nav_page == 4) {
            dl->AddRectFilled(ImVec2(item_x, nav_y), ImVec2(item_x + nav_w - 16.0f * dpi, nav_y + item_h),
                kColAccentSoft, 6.0f * dpi);
            dl->AddRectFilled(ImVec2(item_x, nav_y), ImVec2(item_x + 3.0f * dpi, nav_y + item_h),
                kColAccent, 2.0f * dpi);
        }
        DrawNavIcon(dl, 4, item_x + 18.0f * dpi, nav_y + item_h * 0.5f,
            g_nav_page == 4 ? kColAccent : kColText3);
        PutText(g_font_body, item_x + 34.0f * dpi, nav_y + (item_h - FontHeight(g_font_body)) * 0.5f,
            g_nav_page == 4 ? kColText : kColText2, nav_labels[4]);
        ImGui::PopID();
    }
    // 侧栏底部：遥测连接点
    const bool tele_ok = g_tele.available;
    dl->AddCircleFilled(ImVec2(wpos.x + 20.0f * dpi, body_top + body_height - 24.0f * dpi),
        2.4f * dpi, tele_ok ? kColJade : kColText3);
    PutText(g_font_small, wpos.x + 29.0f * dpi, body_top + body_height - 32.0f * dpi,
        tele_ok ? kColJadeText : kColText3, tele_ok ? "遥测已连接" : "遥测未连接");

    // ---- 内容页（可滚动） ----
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + nav_w, body_top));
    ImGui::BeginChild("##pages", ImVec2(wtotal - nav_w, body_height), ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    {
        const ImVec2 child_pos = ImGui::GetWindowPos();
        const float pad_x = 20.0f * dpi;
        const float content_width = ImGui::GetWindowWidth() - pad_x * 2.0f;
        float y = child_pos.y + 16.0f * dpi;
        const float x = child_pos.x + pad_x;
        switch (g_nav_page) {
        case 0: RenderStatusPage(x, y, content_width); break;
        case 1: RenderNrPage(x, y, content_width); break;
        case 2: RenderFgPage(x, y, content_width); break;
        case 3: RenderModelPage(x, y, content_width); break;
        default: RenderHelpPage(x, y, content_width); break;
        }
        ImGui::SetCursorScreenPos(ImVec2(child_pos.x + 4.0f * dpi, y));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }
    ImGui::EndChild();

    // ---- 底栏 ----
    const float footer_y = wpos.y + htotal - bot_h;
    dl->AddLine(ImVec2(wpos.x, footer_y), ImVec2(wpos.x + wtotal, footer_y), kColLineSoft, 1.0f * dpi);
    dl->AddCircleFilled(ImVec2(wpos.x + 22.0f * dpi, footer_y + bot_h * 0.5f), 2.2f * dpi,
        g_save.failed ? kColWarn : kColJade);
    PutText(g_font_small, wpos.x + 30.0f * dpi, footer_y + (bot_h - FontHeight(g_font_small)) * 0.5f,
        g_save.failed ? kColWarn : kColText3,
        g_save.failed ? "参数未保存，正在重试" : "调整即时生效 · 自动保存");
    {
        const float exit_w = 92.0f * dpi, exit_h = 30.0f * dpi;
        const float exit_x = wpos.x + wtotal - exit_w - 14.0f * dpi;
        const float exit_y = footer_y + (bot_h - exit_h) * 0.5f;
        if (Btn("panel-exit", exit_x, exit_y, exit_w, exit_h, "退出面板", 1)) {
            FlushPendingParams(L"exit button");
            g_running = false;
        }
    }

    g_ui_w = ImGui::GetWindowWidth();
    g_ui_h = ImGui::GetWindowHeight();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// ===== Win32 / D3D11 =====
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

static LRESULT CALLBACK PanelWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_NCHITTEST:
        if (g_expanded) {
            POINT pt{ static_cast<SHORT>(LOWORD(lparam)), static_cast<SHORT>(HIWORD(lparam)) };
            ScreenToClient(window, &pt);
            RECT rc{};
            GetClientRect(window, &rc);
            const float cap_h = 48.0f * g_dpi_scale;          // 自绘标题行（与 RenderUi cap_h 一致）
            const float btn_zone = 48.0f * g_dpi_scale;       // 右上 ✕ 按钮豁免区（HTCLIENT 才可点击）
            // 与绘制同一布局记录的胶囊矩形：命中时返回 HTCLIENT，否则点击会被
            // HTCAPTION 当成拖动而吞掉（F10 胶囊位于拖动区）。
            if (PointInF10Chip(static_cast<int>(pt.x), static_cast<int>(pt.y))) return HTCLIENT;
            if (pt.y < cap_h) {
                if (pt.x >= rc.right - static_cast<LONG>(btn_zone)) return HTCLIENT;
                return HTCAPTION;   // 标题行空白处拖拽（NOACTIVATE 下系统仍可移动）
            }
        }
        break;
    case WM_SIZE:
        // 收起/展开切换会改变窗口尺寸：交换链必须同步重建，否则 UI 被拉伸且鼠标错位。
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
    case WM_ENTERSIZEMOVE: g_dragging = true; break;
    case WM_EXITSIZEMOVE:
        g_dragging = false;
        // 拖拽/缩放结束：记下用户相对游戏窗口右上角的偏移（此后跟随游戏窗口）
        if (g_game_rect_cur.right > g_game_rect_cur.left) {
            RECT wr{};
            if (GetWindowRect(window, &wr)) {
                g_user_off_x = wr.left - g_game_rect_cur.right;
                g_user_off_y = wr.top - g_game_rect_cur.top;
                g_user_positioned = true;
            }
        }
        break;
    case WM_QUERYENDSESSION:
        // zh: 注销/关机前强制落盘；系统不保证随后还会走正常退出路径。
        FlushPendingParams(L"WM_QUERYENDSESSION");
        return TRUE;
    case WM_ENDSESSION:
        // zh: 关机被取消时恢复保存通道；确认结束时让主循环正常收尾。
        if (!wparam) {
            g_save.Reopen();
            DebugLine(L"end-session cancelled; pending-save channel reopened");
        } else {
            g_running = false;
        }
        return 0;
    case WM_DESTROY:
        FlushPendingParams(L"WM_DESTROY");
        g_running = false;
        return 0;
    case WM_HOTKEY:
        // zh: RegisterHotKey 成功路径此前无人处理，F11 会静默失效
        //（只有热键被占用走轮询兜底的机器上才碰巧可用）。
        if (wparam == 1) ToggleExpanded();
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t **argv)
{
    // 单实例：重复拉起直接退出（控制台守护与手动启动并存时的启动竞态防护）
    CreateMutexW(nullptr, TRUE, L"DLSS5PanelMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    setvbuf(stdout, nullptr, _IONBF, 0);
    auto out = [](const wchar_t *t) { OutUtf8(t); };
    auto out_line = [&](const wchar_t *t) { out(t); out(L"\n"); };
    // zh: 参数 --game <wow.exe 路径> [--debug]（20261109：控制台按自身开关透传调试行）
    const wchar_t *game_arg = nullptr;
    bool debug_arg = false;
    for (int i = 1; argv && i < argc; ++i) {
        if (wcscmp(argv[i], L"--game") == 0 && i + 1 < argc) game_arg = argv[++i];
        else if (wcscmp(argv[i], L"--debug") == 0) debug_arg = true;
    }
    g_debug_log = debug_arg;
    if (!game_arg) {
        out_line(L"用法：dlss5-panel.exe --game <wow.exe 绝对路径> [--debug]");
        return 2;
    }
    fs::path game;
    try {
        game = fs::path(game_arg);
        if (!game.is_absolute() || _wcsicmp(game.filename().c_str(), L"wow.exe") != 0) return 3;
        game = game.lexically_normal();
    } catch (const fs::filesystem_error &) { return 3; }
    if (g_debug_log) out_line(L"[debug] panel started with --debug; diagnostics follow in stdout.log");
    const fs::path host = game.parent_path() / L"host64" / L"AIO DLSS5 32-bit Wrapper.exe";
    // zh: 结构校验（文件存在 + 无链接）——SHA 绑定只适用于单机私人部署，
    // 分发时目标客户端的哈希必然不同，写配置文件本身对任意 DLSS5-AIO 客户端无害。
    if (!fs::exists(game) || !fs::exists(host) ||
        !NoReparsePoints(game) || !NoReparsePoints(host)) {
        out_line(L"拒绝执行：未找到 DLSS5-AIO 游戏或载体（wow.exe / host64）。");
        return 4;
    }
    IniPath = (game.parent_path() / L"host64" / L"ReShade.ini").wstring();
    ParamsPath = (game.parent_path() / L"host64" / L"live-params.json").wstring();
    CustomPath = (game.parent_path() / L"host64" / L"custom-tier.json").wstring();
    ReadIniParams(g_params);
    LoadCustomTier();
    g_tier_locked = false;   // zh: 不匹配任何出厂档=自定义，解锁微调（旧版只会保持锁定）
    for (const auto &s : kSchemes)
        if (g_params.MatchesTier(s.p)) { g_tier_locked = true; break; }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"Dlss5PanelWindow";
    RegisterClassExW(&wc);

    // DPI 感知：悬浮面板按物理像素渲染（锐利），并随系统缩放调整尺寸
    using SetDpiCtxFn = BOOL (WINAPI *)(DPI_AWARENESS_CONTEXT);
    const auto set_dpi_ctx = reinterpret_cast<SetDpiCtxFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_dpi_ctx) set_dpi_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    using GetDpiForSystemFn = UINT (WINAPI *)();
    const auto get_dpi = reinterpret_cast<GetDpiForSystemFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    g_dpi_scale = (get_dpi ? static_cast<float>(get_dpi()) : 96.0f) / 96.0f;

    g_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"WOW-335-NR 面板", WS_POPUP, 0, 0,
        static_cast<int>(238 * g_dpi_scale), static_cast<int>(176 * g_dpi_scale),
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_window) return 5;

    if (!CreateD3D(g_window)) { out_line(L"D3D11 初始化失败。"); return 6; }
    ImGui::CreateContext();
    ApplyStyle();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);
    LoadPanelFonts();
    g_poll_hotkey = RegisterHotKey(g_window, 1, 0, VK_F11) == 0;
    if (g_poll_hotkey) out_line(L"提示：F11 热键被占用，改用按键轮询检测。");
    out_line(L"WOW-335-NR 面板已启动：收起态为信息条，F11 展开完整面板。游戏退出后面板转入等待。");

    RECT game_rect{};
    DWORD last_tick = static_cast<DWORD>(GetTickCount64()), last_flush = 0;
    bool saw_game = false;
    ShowWindow(g_window, SW_HIDE);
    Processes procs;
    while (g_running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                FlushPendingParams(L"WM_QUIT");
                g_running = false;
            }
        }
        if (!g_running) break;

        // 游戏与载体的身份与几何同步（1 秒一次）
        const DWORD now = static_cast<DWORD>(GetTickCount64());
        if (now - last_tick >= 1000) {
            last_tick = now;
            procs = FindProcesses(game.wstring(), host.wstring());
            if (procs.uncertain || procs.game_pid == 0) {
                if (saw_game) out_line(L"所选游戏已退出，面板转入等待（下次启动游戏自动显示）。");
                saw_game = false;
                ShowWindow(g_window, SW_HIDE);
                continue;
            }
            if (!saw_game) out_line(L"已检测到游戏，面板显示。");
            saw_game = true;
            ShowWindow(g_window, SW_SHOWNA);
            if (g_debug_log) {
                wchar_t buf[128]{};
                std::swprintf(buf, 128, L"procs: game=%lu host=%lu uncertain=%d",
                    static_cast<unsigned long>(procs.game_pid),
                    static_cast<unsigned long>(procs.host_pid), procs.uncertain ? 1 : 0);
                DebugLine(buf);
            }
        }

        // zh: 面板不可见（未检测到游戏）时不渲染、不 Present；阻塞等待窗口消息，
        // 最多 1 秒后重新探测进程（保留原有 1 秒检查节奏）。隐藏时 CPU 接近 0。
        if (!IsWindowVisible(g_window)) {
            SavePendingIfDue(last_flush, now);
            const DWORD hidden_now = static_cast<DWORD>(GetTickCount64());
            const DWORD elapsed = hidden_now - last_tick;
            const DWORD wait_ms = elapsed < 1000 ? 1000 - elapsed : 0;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, wait_ms, QS_ALLINPUT);
            continue;
        }
        // 游戏窗口矩形：按窗口类查找并校验属于已匹配的游戏进程
        HWND game_window = FindWindowExW(nullptr, nullptr, L"GxWindowClassD3d", nullptr);
        DWORD window_pid = 0;
        bool game_window_ok = false;
        while (game_window) {
            GetWindowThreadProcessId(game_window, &window_pid);
            if (window_pid == procs.game_pid && procs.game_pid != 0) { game_window_ok = true; break; }
            game_window = FindWindowExW(nullptr, game_window, L"GxWindowClassD3d", nullptr);
        }
        g_game_hwnd_seen = game_window_ok ? game_window : nullptr;
        if (game_window_ok && GetWindowRect(game_window, &game_rect)) {
            g_game_rect_cur = game_rect;
            SyncWindowLayout(game_rect);
        }

        // 所有权关系维护：面板作为 AIO 主输出窗的 owned window，由系统保证
        // 始终位于其上方（AIO 每 500ms 的 HWND_TOPMOST 重申无法越过所有权约束），
        // 且随输出窗显隐联动。输出窗重建后自动重新建立关系。

        // 遥测（0.5 秒一次）
        static DWORD last_tele = 0;
        if (now - last_tele >= 500) {
            last_tele = now;
            if (procs.host_pid != g_last_host_pid) {
                // zh: 宿主重启后遥测计数从 0 重新开始；沿用旧基线会让无符号差值回绕（R9）。
                g_last_host_pid = procs.host_pid;
                ResetFpsBaselines();
                DebugLine(L"host pid changed; FPS counter baselines reset");
            }
            g_tele = g_last_host_pid ? ReadTelemetry(g_last_host_pid) : Telemetry{};
            g_layer = EvaluateLayerCover();
            {
                static DWORD last_dbg_tele = 0;
                if (g_debug_log && !g_tele.available && now - last_dbg_tele >= 10000) {
                    last_dbg_tele = now;
                    wchar_t buf[160]{};
                    std::swprintf(buf, 160, L"telemetry unavailable (host pid %lu; mapping not open or stale)",
                        static_cast<unsigned long>(g_last_host_pid));
                    DebugLine(buf);
                }
            }
            LARGE_INTEGER now_qpc{}, freq{};
            QueryPerformanceCounter(&now_qpc);
            QueryPerformanceFrequency(&freq);
            if (g_tele.presented > 0 && g_last_presented_qpc > 0) {
                const FpsDelta delta = ComputeFpsDelta(g_tele.presented, g_last_presented,
                    now_qpc.QuadPart, g_last_presented_qpc, freq.QuadPart, 0.5);
                if (delta.reset) {
                    g_game_fps = 0.0f;   // 计数回退：丢弃回绕值，下一窗口重新采样
                } else if (delta.sampled) {
                    g_game_fps = delta.fps;
                }
                g_last_presented = g_tele.presented;
                g_last_presented_qpc = now_qpc.QuadPart;
            } else if (g_tele.presented > 0) {
                g_last_presented = g_tele.presented;
                g_last_presented_qpc = now_qpc.QuadPart;
            }
            // zh: 真实帧率（SR 逐真实帧计数差分）——仅作遥测 SourceFps 缺失时的回退
            if (g_tele.sr_frames > 0 && g_last_src_qpc > 0) {
                const FpsDelta delta = ComputeFpsDelta(g_tele.sr_frames, g_last_src_frames,
                    now_qpc.QuadPart, g_last_src_qpc, freq.QuadPart, 0.25);
                if (delta.reset) {
                    g_src_fps = 0.0f;
                } else if (delta.sampled) {
                    g_src_fps = delta.fps;
                }
                g_last_src_frames = g_tele.sr_frames;
                g_last_src_qpc = now_qpc.QuadPart;
            } else if (g_tele.sr_frames > 0) {
                g_last_src_frames = g_tele.sr_frames;
                g_last_src_qpc = now_qpc.QuadPart;
            }
        }

        // 所有权维护（面板始终位于 AIO 输出窗上方，系统级保证）
        MaintainOutputOwnership(procs.host_pid);

        // 参数落盘（用户改动时；可见/等待两条路径共用同一节流逻辑）
        SavePendingIfDue(last_flush, now);

        // 热键轮询兜底（RegisterHotKey 失败时用 Alt+F10 边沿检测切换）
        if (g_poll_hotkey) {
            const bool down = (GetAsyncKeyState(VK_F11) & 0x8000);
            if (down && !g_hotkey_prev) ToggleExpanded();
            g_hotkey_prev = down;
        }

        // 渲染
        // zh: ImGui 期望尺寸（上一帧帧内捕获）≠ OS 窗口 → OS 实时跟随（交换链经 WM_SIZE 重建）。
        // zh: 严禁在帧外直接调 ImGui::GetWindowWidth——CurrentWindow 为空会解引用崩溃。
        if (g_ui_w > 0.0f && g_ui_h > 0.0f) {
            RECT cur{};
            GetWindowRect(g_window, &cur);
            const int cw = cur.right - cur.left, ch = cur.bottom - cur.top;
            if (static_cast<int>(g_ui_w) != cw || static_cast<int>(g_ui_h) != ch) {
                SetWindowPos(g_window, nullptr, 0, 0, static_cast<int>(g_ui_w), static_cast<int>(g_ui_h),
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderUi();
        ImGui::Render();
        const float clear[4] = {20.0f / 255.0f, 16.0f / 255.0f, 10.0f / 255.0f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapchain->Present(1, 0);
        Sleep(10);
    }

    // 兜底：任何未显式 flush 的退出路径都要落盘（幂等：已 flush 的通道不重复写）
    FlushPendingParams(L"teardown");
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_target) g_target->Release();
    if (g_swapchain) g_swapchain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    out_line(L"面板已结束。");
    return 0;
}
