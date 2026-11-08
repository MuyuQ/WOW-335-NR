// dlss5-panel.exe — 游戏内中文控制面板（乙方案）
// 架构：独立置顶窗口（D3D11 + ImGui 1.92 动态字形，微软雅黑）浮于游戏之上；
//       写 host64\live-params.json → addon64 热应用 NR 参数（0.5 秒内生效）；
//       直读 Local\DLSS5_AIO_Telemetry_<pid> 共享内存展示运行状态。
// 边界：结构校验（wow.exe + host64 载体存在且非链接），不启动游戏；
//       收起态为穿透信息条，展开态可交互；游戏退出即结束。
// 构建：Build-Panel.ps1（/utf-8，MSVC x64 /MT）。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
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
        return nr == o.nr && model == o.model && fg == o.fg &&
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
static bool g_params_dirty = false;
static bool g_tier_locked = true;   // zh: 出厂档=锁定配方（微调禁用），自定义=解锁可调
static Params g_custom;          // 自定义档位：最近一次应用的参数组合
static bool g_custom_valid = false;

// zh: 档位=强度阶梯。刻度对齐 033/引擎契约：强度与结构 0–1（1.0=满），
// 色调 0–2（1.0=中性）、皮肤 -1~1（0=中性）。
// 社区参考（Igor's Lab）：强度 20–35%、结构 10–25%——三档间距刻意拉开。
static const struct { const char *name; Params p; } kSchemes[] = {
    { "原画", { false, 1, 1.0f,  1.0f, 1.0f,  -1.0f } },
    { "轻量", { true,  1, 0.20f, 1.0f, 0.10f, -1.0f } },
    { "均衡", { true,  1, 0.35f, 1.0f, 0.25f, -1.0f } },
    { "浓郁", { true,  1, 0.60f, 1.3f,  0.50f, -1.0f } },
};

// zh: 模型即 DLSSNR.Style 0–2（引擎三套有效网络；Hint.Render.Preset 在本运行时
// zh: 为兼容摆设，不选网络——载体 NrStyle() 注释已验证）。
// zh: 「默认/自然/电影」为 033 从引擎契约读出的官方语义；具体视觉差异待实测。
static const char *kModelNames[3] = { "默认", "自然", "电影" };
static const char *kModelDesc[3] = {
    "默认：引擎原始风格。社区观察重绘较保守，观感最接近原画。",
    "自然：引擎的自然向风格（视觉差异待实测）。",
    "电影：引擎的电影向风格；与「自然」实测观感差异细微。",
};

static void ClampParams(Params &p)
{
    // zh: 强度/结构按引擎真实范围 0–1（033 schema bounds；upstream 的 0–2 菜单范围偏宽）
    p.model = std::clamp(p.model, 1, 3);
    p.fg_multiplier = std::clamp(p.fg_multiplier, 2, 4);
    p.intensity = std::clamp(p.intensity, 0.0f, 1.0f);
    p.tone = std::clamp(p.tone, 0.0f, 2.0f);
    p.structure = std::clamp(p.structure, 0.0f, 1.0f);
    p.skin = std::clamp(p.skin, -1.0f, 1.0f);
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
    const fs::path temp = path.parent_path() / L"live-params.json.panel-tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written, nullptr) &&
        FlushFileBuffers(file);
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
    auto num = [&](const char *key, float &out) {
        char needle[48];
        std::snprintf(needle, sizeof needle, "\"%s\":", key);
        const char *pos = std::strstr(text, needle);
        if (!pos) return false;
        pos += std::strlen(needle);
        while (*pos == ' ') ++pos;
        char *end = nullptr;
        const double v = std::strtod(pos, &end);
        if (end == pos) return false;
        out = static_cast<float>(v);
        return true;
    };
    float model = 0, intensity = 0, tone = 0, structure = 0, skin = 0;
    if (!num("model", model) || !num("intensity", intensity) || !num("localTone", tone) ||
        !num("localStructure", structure) || !num("skinStructure", skin))
        return;
    g_custom.model = std::clamp(static_cast<int>(model), 1, 3);
    g_custom.intensity = std::clamp(intensity, 0.0f, 1.0f);
    g_custom.tone = std::clamp(tone, 0.0f, 2.0f);
    g_custom.structure = std::clamp(structure, 0.0f, 1.0f);
    g_custom.skin = std::clamp(skin, -1.0f, 1.0f);
    const char *nr = std::strstr(text, "\"neuralRendering\":true");
    g_custom.nr = nr != nullptr;
    if (std::strstr(text, "\"frameGeneration\":"))
        g_custom.fg = std::strstr(text, "\"frameGeneration\":true") != nullptr;
    g_custom_valid = true;
}
static void SaveCustomTier(const Params &p)
{
    const std::string body = ParamsToJson(p);
    HANDLE file = CreateFileW(CustomPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    g_custom = p;
    g_custom_valid = true;
}

// 从 host64/ReShade.ini 读取当前值（面板启动时的初值来源）
static std::wstring IniPath, ParamsPath;
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
        p.intensity = std::clamp(static_cast<float>(atof(value)), 0.0f, 1.0f);
    if (GetPrivateProfileStringA("Standalone.DLSSNR", "LocalTone", "", value, sizeof value, ini) && value[0])
        p.tone = std::clamp(static_cast<float>(atof(value)), 0.0f, 2.0f);
    if (GetPrivateProfileStringA("Standalone.DLSSNR", "LocalStructure", "", value, sizeof value, ini) && value[0])
        p.structure = std::clamp(static_cast<float>(atof(value)), 0.0f, 1.0f);
    if (GetPrivateProfileStringA("Standalone.DLSSNR", "SkinStructure", "", value, sizeof value, ini) && value[0])
        p.skin = std::clamp(static_cast<float>(atof(value)), -1.0f, 1.0f);
}

// ===== 遥测（共享内存直读，偏移与 performance/telemetry_reader.py 一致） =====
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
            const long long sequence = static_cast<long long>(rd64(16));
            const long long qpc = static_cast<long long>(rd64(24));
            const long long frequency = static_cast<long long>(rd64(32));
            if ((sequence & 1) == 0 && static_cast<long long>(rd64(16)) == sequence && qpc > 0 && frequency > 0) {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                out.age = (now.QuadPart - qpc) / static_cast<double>(frequency);
                if (out.age >= -0.1 && out.age <= 3.0) {
                    const unsigned flags = rd32(48);
                    out.available = true;
                    out.nr_enabled = (flags & 2) != 0;
                    out.fg = (flags & 4) != 0;
                    out.pipeline_ready = (flags & 8) != 0;
                    out.processed_visible = (flags & 32) != 0;
                    out.fg_failed = (flags & 16) != 0;
                    out.nr_frames = rd64(200);
                    out.sr_frames = rd64(208);
                    out.fg_frames = rd64(216);
                    out.presented = rd64(224);
                    out.input_w = rd32(56); out.input_h = rd32(60);
                    out.output_w = rd32(64); out.output_h = rd32(68);
                }
            }
        }
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return out;
}

// ===== 状态 =====
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

// ===== 图层覆盖状态（真实窗口可见性 + z 序，不止 F10 配置位） =====
enum class LayerState { NoHost, NoOutput, RawMode, Hidden, UnderGame, Covered };
static LayerState g_layer = LayerState::NoHost;
static HWND g_game_hwnd_seen = nullptr;   // 主循环按游戏进程匹配到的窗口

// ===== 视觉主题：与桌面控制台同款（深夜蓝黑 + 祖母绿强调） =====
static const ImVec4 kAccent(0.239f, 0.839f, 0.549f, 1.0f);     // #3DD68C
static const ImVec4 kWarn(0.941f, 0.639f, 0.369f, 1.0f);       // #F0A35E
static const ImVec4 kMuted(0.545f, 0.596f, 0.647f, 1.0f);      // #8B98A5

static void ApplyStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.ScrollbarSize = 12.0f;
    style.WindowPadding = ImVec2(14, 11);
    style.FramePadding = ImVec2(9, 6);
    style.ItemSpacing = ImVec2(10, 8);
    ImVec4 *c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.071f, 0.090f, 0.122f, 0.97f);  // #12171F
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
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.310f, 0.910f, 0.620f, 1.0f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_Header] = ImVec4(0.106f, 0.137f, 0.176f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.110f, 0.157f, 0.200f, 1.0f);
}

static void ToggleExpanded()
{
    g_expanded = !g_expanded;
    // 立即预设目标尺寸，避免切换瞬间内容被旧尺寸裁切一帧
    LONG_PTR ex = GetWindowLongPtrW(g_window, GWL_EXSTYLE);
    if (g_expanded) ex &= ~WS_EX_TRANSPARENT;
    else ex |= WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_window, GWL_EXSTYLE, ex);
    SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// 开关滑块：圆角胶囊 + 圆形滑钮（与桌面控制台同款）
// zh: v 必须是引用——翻转运出的新值写回调用方的存储，否则开关视觉闪一帧即弹回
static bool ToggleSwitch(const char *label, bool &v)
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
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.310f, 0.910f, 0.620f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.020f, 0.049f, 0.039f, 1.0f));
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
static void MaintainOutputOwnership(DWORD host_pid)
{
    if (host_pid == 0) { g_owned_output = nullptr; return; }
    HWND aio_output = FindAioMainOutput(host_pid);
    if (aio_output && aio_output != g_owned_output) {
        SetWindowLongPtrW(g_window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(aio_output));
        g_owned_output = aio_output;
    }
    else if (!aio_output && g_owned_output) {
        g_owned_output = nullptr;
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
        // 沿 z 序向上走：先遇到游戏窗口 = 输出窗被盖住
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

static void SyncWindowLayout(const RECT &game)
{
    // 位置同步（尺寸由 ImGui 手柄决定，OS 窗口在主循环里跟随）；拖拽/缩放中让路。
    // 默认贴游戏右上角；用户拖动过后按记下的偏移跟随（游戏窗口移动/缩放时仍跟随）。
    if (g_dragging) return;
    RECT cur{};
    GetWindowRect(g_window, &cur);
    const int w = cur.right - cur.left;
    int px, py;
    if (g_user_positioned) {
        px = game.right + g_user_off_x;
        py = game.top + g_user_off_y;
    } else if (g_expanded) {
        px = game.right - w - static_cast<int>(8 * g_dpi_scale);
        py = game.top + static_cast<int>(10 * g_dpi_scale);
    } else {
        px = game.right - w - static_cast<int>(8 * g_dpi_scale);
        py = game.top + static_cast<int>(10 * g_dpi_scale);
    }
    SetWindowPos(g_window, nullptr, px, py, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
}

// ===== 展开态布局：参考 033 设置面板（自绘标题行 + 左侧导航 + 内容页 + 底栏）=====
static int g_nav_page = 0;   // 0 状态 / 1 画面增强 / 2 补帧 / 3 模型 / 4 说明

static void PageHeader(const char *title)
{
    ImGui::TextColored(kAccent, "%s", title);
    ImGui::Spacing();
}

// 侧栏导航项：选中 = 强调色文字 + 浅色底 + 左侧 3px 竖条
static bool NavItem(const char *label, int idx)
{
    const bool sel = (idx == g_nav_page);
    if (sel) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.118f, 0.173f, 0.153f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.137f, 0.196f, 0.173f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.118f, 0.173f, 0.153f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    }
    const bool clicked = ImGui::Selectable(label, sel);
    if (sel) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        dl->AddRectFilled(mn, ImVec2(mn.x + 3.0f * g_dpi_scale, mx.y),
            IM_COL32(61, 214, 140, 255));
        ImGui::PopStyleColor(4);
    }
    return clicked;
}

static void PageStatus()
{
    PageHeader("状态");
    if (g_game_fps > 0) ImGui::Text("真实帧率：%.1f → 输出帧率：%.1f FPS",
        g_src_fps > 0 ? g_src_fps : 0.0f, g_game_fps);
    if (g_tele.available) {
        ImGui::Text("模型：%s   强度：%.0f%%   管线：%s", kModelNames[g_params.model - 1],
            g_params.intensity * 100.0f, g_tele.pipeline_ready ? "就绪" : "未就绪");
        if (!g_tele.fg)
            ImGui::TextDisabled("补帧：关（配置未开启）");
        else if (g_tele.fg_failed)
            ImGui::TextColored(kWarn, "补帧：开启但求值失败（显示真实帧）");
        else if (g_tele.fg_frames > 0)
            ImGui::TextColored(ImVec4(0.62f, 1.0f, 0.78f, 1.0f), "补帧：开（已插帧 %llu）", g_tele.fg_frames);
        else
            ImGui::TextColored(kWarn, "补帧：开（尚未插出帧）");
        switch (g_layer) {
        case LayerState::Covered:
            ImGui::TextColored(ImVec4(0.62f, 1.0f, 0.78f, 1.0f), "显示图层：覆盖中（你看到的是 NR 处理画面）");
            break;
        case LayerState::RawMode:
            ImGui::TextColored(kWarn, "显示图层：F10 处于原画模式！按 F10 切回，否则参数变化不可见");
            break;
        case LayerState::Hidden:
            ImGui::TextColored(kWarn, "显示图层：输出窗已隐藏——回到游戏窗口即恢复");
            break;
        case LayerState::UnderGame:
            ImGui::TextColored(kWarn, "显示图层：输出窗被游戏窗口盖住——回到游戏窗口即恢复");
            break;
        case LayerState::NoOutput:
            ImGui::TextColored(kWarn, "显示图层：未找到输出窗（载体未运行或正在启动）");
            break;
        default:
            ImGui::TextColored(kWarn, "显示图层：载体遥测未确认");
            break;
        }
        ImGui::Text("NR 帧数：%llu   SR 帧数：%llu", g_tele.nr_frames, g_tele.sr_frames);
        ImGui::Text("分辨率：%ux%u → %ux%u", g_tele.input_w, g_tele.input_h, g_tele.output_w, g_tele.output_h);
    } else {
        ImGui::TextColored(kWarn, "载体遥测：未确认（载体未运行或刚启动）");
    }
    ImGui::TextDisabled("档位：%s", SchemeName());
}

static void PageNr()
{
    PageHeader("画面增强（NR）");
    if (ToggleSwitch("画面增强（NR）", g_params.nr)) g_params_dirty = true;

    ImGui::Spacing();
    ImGui::TextColored(kAccent, "档位");
    ImGui::TextDisabled("档位是完整配方，NR 开关跟随档位（原画=关闭）。");
    for (int i = 0; i < 4; ++i) {
        if (i % 3) ImGui::SameLine();
        if (SegmentedOption(kSchemes[i].name, g_tier_locked && g_params.MatchesTier(kSchemes[i].p),
            124 * g_dpi_scale)) {
            const int keep_model = g_params.model;
            const bool keep_fg = g_params.fg;
            g_params = kSchemes[i].p;
            g_params.model = keep_model;
            g_params.fg = keep_fg;
            ClampParams(g_params);   // zh: 配方聚合初始化不含倍数字段，钳到默认 2×
            g_tier_locked = true;
            g_params_dirty = true;
        }
    }
    ImGui::SameLine();
    if (SegmentedOption("自定义", !g_tier_locked, 124 * g_dpi_scale)) {
        g_tier_locked = false;
        if (g_custom_valid && !g_params.Equals(g_custom)) {
            g_params = g_custom;
            g_params_dirty = true;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("进入自定义模式：解锁微调；已保存的组合会被召回");

    ImGui::Spacing();
    ImGui::TextColored(kAccent, "微调");
    if (g_tier_locked) ImGui::TextDisabled("出厂档位锁定。点「自定义」解锁微调。");
    if (g_tier_locked) ImGui::BeginDisabled(true);
    // 强度/结构用百分比显示（0–1 刻度，1.0=100%）
    {
        float pct = g_params.intensity * 100.0f;
        if (ImGui::SliderFloat("强度", &pct, 0.0f, 100.0f, "%.0f%%")) {
            g_params.intensity = pct / 100.0f;
            g_params_dirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("整体重绘幅度（社区参考 20–35%）");
        pct = g_params.structure * 100.0f;
        if (ImGui::SliderFloat("结构强度", &pct, 0.0f, 100.0f, "%.0f%%")) {
            g_params.structure = pct / 100.0f;
            g_params_dirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("边缘与细节的保持程度（社区参考 10–25%）");
    }
    if (ImGui::SliderFloat("色调结构", &g_params.tone, 0.0f, 2.0f, "%.2f")) g_params_dirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("色彩与明暗关系的保持程度（1.0 为中性）");
    if (ImGui::SliderFloat("皮肤结构", &g_params.skin, -1.0f, 1.0f, "%.2f")) g_params_dirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("人物皮肤的处理倾向：负值偏平滑，正值保留纹理");
    ClampParams(g_params);
    if (ImGui::Button("读取游戏配置")) { Params p; ReadIniParams(p); ClampParams(p); g_params = p; }
    if (g_tier_locked) ImGui::EndDisabled();
}

static void PageFg()
{
    PageHeader("补帧（DLSS-G）");
    if (ToggleSwitch("补帧（DLSS-G）", g_params.fg)) g_params_dirty = true;
    if (!g_tele.fg)
        ImGui::TextDisabled("补帧：关（配置未开启）");
    else if (g_tele.fg_failed)
        ImGui::TextColored(kWarn, "补帧：开启但求值失败（显示真实帧）");
    else if (g_tele.fg_frames > 0)
        ImGui::TextColored(ImVec4(0.62f, 1.0f, 0.78f, 1.0f), "补帧：开（已插帧 %llu）", g_tele.fg_frames);
    else
        ImGui::TextColored(kWarn, "补帧：开（尚未插出帧）");
    if (g_tele.available && (g_tele.fg_frames > 0 || g_tele.nr_frames > 0))
        ImGui::Text("累计：真实帧 %llu → 插帧 %llu", g_tele.nr_frames, g_tele.fg_frames);
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "补帧倍数");
    for (int m = 2; m <= 4; ++m) {
        if (m > 2) ImGui::SameLine();
        char label[8];
        std::snprintf(label, sizeof label, "%d×", m);
        // zh: 固定宽 + SameLine；-1 宽会把后续按钮挤到面板外（首钮吃满整行）
        if (SegmentedOption(label, g_params.fg_multiplier == m, 100 * g_dpi_scale)) {
            g_params.fg_multiplier = m;
            g_params_dirty = true;
        }
    }
    ImGui::TextWrapped("3×/4× 需要 RTX 50 系显卡（其他卡自动回落 2×）。倍数越高呈现帧率越高，"
        "动态画面软化与延迟也会随之增加。");
    ImGui::Spacing();
    ImGui::TextWrapped("插帧会成倍增加显存与内存占用。源帧率上限请在桌面控制台设置（默认 30，防止内存爆涨退出）。");
}

static void PageModel()
{
    PageHeader("模型");
    for (int m = 1; m <= 3; ++m) {
        if (SegmentedOption(kModelNames[m - 1], g_params.model == m, -1.0f)) {
            g_params.model = m;
            g_params_dirty = true;
        }
    }
    ImGui::TextWrapped("%s", kModelDesc[g_params.model - 1]);
    ImGui::TextDisabled("三选一即引擎外观风格（默认/自然/电影）。切换立即生效，后台重建约 1 秒。");
}

static void PageHelp()
{
    PageHeader("说明");
    ImGui::TextDisabled("操作");
    ImGui::BulletText("F10：原画对比（AIO 自带热键）");
    ImGui::BulletText("F11：展开 / 收起本面板");
    ImGui::BulletText("拖动顶部标题行移动面板；拖右下角缩放");
    ImGui::TextDisabled("调参");
    ImGui::BulletText("调整即时生效，经 live-params 通道写入；关闭面板不回滚");
    ImGui::BulletText("档位是出厂配方（微调锁定）；点「自定义」解锁滑杆");
    ImGui::BulletText("模型切换立即生效，约 1 秒后台重建");
    ImGui::TextDisabled("其他");
    ImGui::BulletText("源帧率上限在桌面控制台设置（防插帧爆内存）");
}

static void RenderUi()
{
    const float dpi = g_dpi_scale;

    // 收起态：右上信息条（穿透，仅展示）
    if (!g_expanded) {
        ImGui::SetNextWindowPos(ImVec2(5 * dpi, 3 * dpi), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(204 * dpi, 116 * dpi));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * dpi);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.071f, 0.090f, 0.122f, 0.92f));
        ImGui::Begin("##strip", nullptr, ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextColored(kAccent, "WOW-335-NR");
        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted(SchemeName());
        if (g_tele.available) {
            ImGui::TextColored(g_tele.nr_enabled ? kAccent : kMuted, "● NR %s", g_tele.nr_enabled ? "开" : "关");
            ImGui::SameLine(0, 8);
            if (!g_tele.fg) ImGui::TextColored(kMuted, "● FG 关");
            else if (g_tele.fg_failed) ImGui::TextColored(kWarn, "● FG ✗");
            else if (g_tele.fg_frames > 0) ImGui::TextColored(kAccent, "● FG 开");
            else ImGui::TextColored(kWarn, "● FG 待");
        } else
            ImGui::TextUnformatted("NR 状态：未确认");
        if (g_layer == LayerState::RawMode || g_layer == LayerState::Hidden ||
            g_layer == LayerState::UnderGame)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.45f, 1.0f), "图层：%s", LayerShortName(g_layer));
        else
            ImGui::Text("图层：%s", LayerShortName(g_layer));
        if (ImGui::Button("展开面板 >>", ImVec2(-1, 0))) ToggleExpanded();
        // 尺寸必须在帧内捕获（End 之后 CurrentWindow 为空，再查尺寸会崩）
        g_ui_w = ImGui::GetWindowWidth();
        g_ui_h = ImGui::GetWindowHeight();
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }

    // 展开态：三段式（自绘标题行 / 侧栏+内容页 / 底栏）
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(g_panel_w * dpi, g_panel_h * dpi), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(400 * dpi, 360 * dpi),
        ImVec2(900 * dpi, 2400 * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("WOW-335-NR 面板", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    const ImVec2 wpos = ImGui::GetWindowPos();
    const float wtotal = ImGui::GetWindowWidth();
    const float cap_h = 40.0f * dpi;     // 标题行（与 WM_NCHITTEST 拖拽区高度一致）
    const float bot_h = 36.0f * dpi;     // 底栏
    const float nav_w = 118.0f * dpi;    // 侧栏
    const float body_h = ImGui::GetWindowHeight() - cap_h - bot_h;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 col_sep = IM_COL32(40, 50, 62, 255);
    const ImU32 col_title_bg = IM_COL32(11, 15, 20, 255);

    // ---- 标题行：居中标题 + 右侧状态点 + 关闭按钮（红色悬停）----
    dl->AddRectFilled(wpos, ImVec2(wpos.x + wtotal, wpos.y + cap_h), col_title_bg);
    {
        const float th = ImGui::GetTextLineHeight();
        const char *title = "WOW-335-NR 画面设置";
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + (wtotal - ImGui::CalcTextSize(title).x) * 0.5f,
            wpos.y + (cap_h - th) * 0.5f));
        ImGui::TextUnformatted(title);

        if (g_tele.available) {
            char nr_s[16], fg_s[16];
            std::snprintf(nr_s, sizeof nr_s, "● NR %s", g_tele.nr_enabled ? "开" : "关");
            std::snprintf(fg_s, sizeof fg_s, "● FG %s",
                !g_tele.fg ? "关" : g_tele.fg_failed ? "✗" : g_tele.fg_frames > 0 ? "开" : "待");
            const float gap = 12.0f * dpi;
            const float total = ImGui::CalcTextSize(nr_s).x + gap + ImGui::CalcTextSize(fg_s).x;
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + wtotal - 48 * dpi - total - gap,
                wpos.y + (cap_h - th) * 0.5f));
            ImGui::TextColored(g_tele.nr_enabled ? kAccent : kMuted, "%s", nr_s);
            ImGui::SameLine(0, gap);
            ImGui::TextColored(!g_tele.fg ? kMuted : g_tele.fg_failed ? kWarn
                : g_tele.fg_frames > 0 ? kAccent : kWarn, "%s", fg_s);
        }

        const float bx = 30.0f * dpi;
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + wtotal - bx - 10 * dpi,
            wpos.y + (cap_h - 24 * dpi) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, 0);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.23f, 0.23f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.88f, 0.30f, 0.30f, 1.0f));
        if (ImGui::Button("×", ImVec2(bx, 24 * dpi))) ToggleExpanded();
        ImGui::PopStyleColor(3);
    }

    // ---- 侧栏 ----
    ImGui::SetCursorScreenPos(ImVec2(wpos.x, wpos.y + cap_h));
    // zh: 左内边距 20——此前 10 与窗口边贴得太近（用户反馈），也比内容页 16 更局促。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * dpi, 12 * dpi));
    ImGui::BeginChild("##nav", ImVec2(nav_w, body_h));
    {
        ImGui::TextDisabled("画质");
        if (NavItem("状态", 0)) g_nav_page = 0;
        if (NavItem("画面增强", 1)) g_nav_page = 1;
        if (NavItem("补帧", 2)) g_nav_page = 2;
        if (NavItem("模型", 3)) g_nav_page = 3;
        ImGui::Spacing();
        ImGui::TextDisabled("其他");
        if (NavItem("说明", 4)) g_nav_page = 4;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    dl->AddLine(ImVec2(wpos.x + nav_w, wpos.y + cap_h),
        ImVec2(wpos.x + nav_w, wpos.y + cap_h + body_h), col_sep);

    // ---- 内容页 ----
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + nav_w + 1, wpos.y + cap_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * dpi, 12 * dpi));
    ImGui::BeginChild("##page", ImVec2(wtotal - nav_w - 1, body_h));
    switch (g_nav_page) {
    case 0: PageStatus(); break;
    case 1: PageNr(); break;
    case 2: PageFg(); break;
    case 3: PageModel(); break;
    default: PageHelp(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // ---- 底栏：状态文字 + 退出 ----
    const float by = wpos.y + cap_h + body_h;
    dl->AddLine(ImVec2(wpos.x, by), ImVec2(wpos.x + wtotal, by), col_sep);
    {
        const float th = ImGui::GetTextLineHeight();
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + 14 * dpi, by + (bot_h - th) * 0.5f));
        ImGui::TextDisabled("调整即时生效 · 自动保存");
        const float bw = 92 * dpi, bh = 25 * dpi;
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + wtotal - bw - 10 * dpi, by + (bot_h - bh) * 0.5f));
        if (ImGui::Button("退出面板", ImVec2(bw, bh))) g_running = false;
    }

    g_ui_w = ImGui::GetWindowWidth();
    g_ui_h = ImGui::GetWindowHeight();
    ImGui::End();
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
            const float cap_h = 40.0f * g_dpi_scale;          // 自绘标题行（与 RenderUi cap_h 一致）
            const float btn_zone = 48.0f * g_dpi_scale;       // 右上 ✕ 按钮豁免区（HTCLIENT 才可点击）
            if (pt.y < cap_h) {
                if (pt.x >= rc.right - static_cast<LONG>(btn_zone)) return HTCLIENT;
                return HTCAPTION;   // 标题行拖拽（NOACTIVATE 下系统仍可移动）
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
    case WM_DESTROY: g_running = false; return 0;
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
    auto out = [](const wchar_t *t) {
        int size = WideCharToMultiByte(CP_UTF8, 0, t, -1, nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            std::vector<char> bytes(size - 1);
            WideCharToMultiByte(CP_UTF8, 0, t, -1, bytes.data(), size - 1, nullptr, nullptr);
            fwrite(bytes.data(), 1, bytes.size(), stdout);
            fflush(stdout);
        }
    };
    auto out_line = [&](const wchar_t *t) { out(t); out(L"\n"); };
    if (argc != 3 || std::wstring(argv[1]) != L"--game") {
        out_line(L"用法：dlss5-panel.exe --game <wow.exe 绝对路径>");
        return 2;
    }
    fs::path game;
    try {
        game = fs::path(argv[2]);
        if (!game.is_absolute() || _wcsicmp(game.filename().c_str(), L"wow.exe") != 0) return 3;
        game = game.lexically_normal();
    } catch (const fs::filesystem_error &) { return 3; }
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
        static_cast<int>(216 * g_dpi_scale), static_cast<int>(74 * g_dpi_scale),
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_window) return 5;

    if (!CreateD3D(g_window)) { out_line(L"D3D11 初始化失败。"); return 6; }
    ImGui::CreateContext();
    ApplyStyle();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);
    ImFont *zh_font = nullptr;
    for (const char *font_path : {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/msyhl.ttc",
                                     "C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/segoeui.ttf"}) {
        if ((zh_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(font_path, 17.0f * g_dpi_scale))) break;
    }
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
            if (message.message == WM_QUIT) g_running = false;
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
                Sleep(500);
                continue;
            }
            if (!saw_game) out_line(L"已检测到游戏，面板显示。");
            saw_game = true;
            ShowWindow(g_window, SW_SHOWNA);
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
            g_last_host_pid = procs.host_pid;
            g_tele = g_last_host_pid ? ReadTelemetry(g_last_host_pid) : Telemetry{};
            g_layer = EvaluateLayerCover();
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
            // zh: 真实帧率（SR 逐真实帧计数差分）——与输出帧率对照可定位丢帧层级
            if (g_tele.sr_frames > 0 && g_last_src_qpc) {
                LARGE_INTEGER now_qpc{}, freq{};
                QueryPerformanceCounter(&now_qpc);
                QueryPerformanceFrequency(&freq);
                const double seconds = (now_qpc.QuadPart - g_last_src_qpc) / static_cast<double>(freq.QuadPart);
                if (seconds > 0.25) g_src_fps = (g_tele.sr_frames - g_last_src_frames) / static_cast<float>(seconds);
                g_last_src_frames = g_tele.sr_frames;
                g_last_src_qpc = now_qpc.QuadPart;
            } else if (g_tele.sr_frames > 0) {
                g_last_src_frames = g_tele.sr_frames;
                LARGE_INTEGER now_qpc{};
                QueryPerformanceCounter(&now_qpc);
                g_last_src_qpc = now_qpc.QuadPart;
            }
        }

        // 所有权维护（面板始终位于 AIO 输出窗上方，系统级保证）
        MaintainOutputOwnership(procs.host_pid);

        // 参数落盘（用户改动时）
        if (g_params_dirty && now - last_flush >= 200) {
            last_flush = now;
            g_params_dirty = false;
            if (WriteParamsAtomic(ParamsPath, g_params)) {
                SaveCustomTier(g_params);   // 自定义档位跟随最近一次调整
            } else {
                out_line(L"参数写入失败：live-params.json 不可写。");
            }
        }

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
        if (zh_font) ImGui::PushFont(zh_font);
        RenderUi();
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
    if (g_target) g_target->Release();
    if (g_swapchain) g_swapchain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    out_line(L"面板已结束。");
    return 0;
}
