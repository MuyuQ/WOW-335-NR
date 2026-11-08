#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "window_layer.hpp"

namespace fs = std::filesystem;

static bool SamePath(const std::wstring &a, const std::wstring &b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// zh: 输出统一转 UTF-8 写入日志文件（控制中心以 -Encoding UTF8 读取）。
static void PrintUtf8(const wchar_t *text) {
    if (!text) return;
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
        std::vector<char> bytes(static_cast<size_t>(size) - 1);
        WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes.data(), size - 1, nullptr, nullptr);
        fwrite(bytes.data(), 1, bytes.size(), stdout);
        fflush(stdout);
    }
}
static void PrintLineUtf8(const wchar_t *text) { PrintUtf8(text); PrintUtf8(L"\n"); }
static void PrintErrUtf8(const wchar_t *text) {
    if (!text) return;
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
        std::vector<char> bytes(static_cast<size_t>(size) - 1);
        WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes.data(), size - 1, nullptr, nullptr);
        fwrite(bytes.data(), 1, bytes.size(), stderr);
        fflush(stderr);
    }
}

static bool NoReparsePoints(const fs::path &path) {
    fs::path current = path.root_path();
    for (const auto &part : path.relative_path()) {
        current /= part;
        DWORD attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}

static std::string Sha256(const fs::path &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
              CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    std::vector<BYTE> bytes(1024 * 1024);
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        ok = CryptHashData(hash, bytes.data(), read, 0) != FALSE;
    }
    std::array<BYTE, 32> digest{};
    DWORD length = static_cast<DWORD>(digest.size());
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &length, 0) && length == digest.size();
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    if (!ok) return {};
    static const char hex[] = "0123456789abcdef";
    std::string result;
    for (BYTE byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

static std::wstring ProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::vector<wchar_t> value(32768);
    DWORD size = static_cast<DWORD>(value.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, value.data(), &size) != FALSE;
    CloseHandle(process);
    return ok ? std::wstring(value.data(), size) : std::wstring();
}

struct Processes { std::vector<DWORD> games, hosts; bool uncertain = false; };

static Processes FindProcesses(const std::wstring &game, const std::wstring &host) {
    Processes result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { result.uncertain = true; return result; }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    BOOL more = Process32FirstW(snapshot, &entry);
    if (!more) result.uncertain = true;
    while (more) {
        const bool game_name = _wcsicmp(entry.szExeFile, L"wow.exe") == 0;
        const bool host_name = _wcsicmp(entry.szExeFile, L"AIO DLSS5 32-bit Wrapper.exe") == 0;
        if (game_name || host_name) {
            const auto path = ProcessPath(entry.th32ProcessID);
            if (path.empty()) result.uncertain = true;
            else if (game_name && SamePath(path, game)) result.games.push_back(entry.th32ProcessID);
            else if (host_name && SamePath(path, host)) result.hosts.push_back(entry.th32ProcessID);
        }
        more = Process32NextW(snapshot, &entry);
        if (!more && GetLastError() != ERROR_NO_MORE_FILES) result.uncertain = true;
    }
    CloseHandle(snapshot);
    return result;
}

struct Window { HWND hwnd; DWORD pid; RECT rect; LONG_PTR style; };
struct Enumerated { DWORD game_pid; DWORD host_pid; std::vector<Window> games, hosts; };

static BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param) {
    auto &all = *reinterpret_cast<Enumerated *>(param);
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != all.game_pid && pid != all.host_pid) return TRUE;
    wchar_t name[256]{};
    RECT rect{};
    if (!GetClassNameW(hwnd, name, 256) || !GetWindowRect(hwnd, &rect)) return TRUE;
    Window window{hwnd, pid, rect, GetWindowLongPtrW(hwnd, GWL_EXSTYLE)};
    if (pid == all.game_pid && wcscmp(name, L"GxWindowClassD3d") == 0) all.games.push_back(window);
    if (pid == all.host_pid && wcscmp(name, L"StandaloneDLSSNRNativeOutput") == 0) all.hosts.push_back(window);
    return TRUE;
}

static bool GeometryValid(const RECT &host, const RECT &game, bool preview) {
    if (!preview) return EqualRect(&host, &game) != FALSE;
    return host.left >= game.left && host.top >= game.top && host.right <= game.right &&
           host.bottom <= game.bottom && host.right > host.left && host.bottom > host.top;
}

using WindowKey = std::tuple<DWORD, UINT_PTR, DWORD, UINT_PTR, LONG_PTR, LONG, LONG, LONG, LONG>;

int wmain(int argc, wchar_t **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3 || std::wstring(argv[1]) != L"--game") {
        PrintErrUtf8(L"用法：window_watch.exe --game <wow.exe 绝对路径>\n");
        return 2;
    }
    fs::path game;
    try {
        game = fs::path(argv[2]);
        if (!game.is_absolute() || _wcsicmp(game.filename().c_str(), L"wow.exe") != 0) return 3;
        game = game.lexically_normal();
    } catch (const fs::filesystem_error &) { return 3; }
    const fs::path host = game.parent_path() / L"host64" / L"AIO DLSS5 32-bit Wrapper.exe";
    if (!NoReparsePoints(game) || !NoReparsePoints(host) ||
        Sha256(game) != "4c459de64d38b5795becc6c01a03ba00d34686598fae707c442446722617868a" ||
        Sha256(host) != "e05a0aef9595849e2910d408cdfadf60c79a26addccba6dfe79c96b424bdef8b") {
        PrintErrUtf8(L"拒绝执行：可执行文件身份或路径校验未通过。\n");
        return 4;
    }
    const auto deadline = GetTickCount64() + 10ULL * 60ULL * 1000ULL;
    bool saw_game = false;
    std::map<WindowKey, unsigned> stable;
    std::set<std::pair<DWORD, UINT_PTR>> attempted;
    PrintLineUtf8(L"等待所选游戏启动；本工具不会自行启动游戏。");
    for (;;) {
        const auto processes = FindProcesses(game.wstring(), host.wstring());
        if (processes.uncertain || processes.games.size() > 1 || processes.hosts.size() > 1) {
            PrintErrUtf8(L"拒绝执行：进程身份不可用或有歧义。\n");
            return 5;
        }
        if (processes.games.empty()) {
            if (saw_game) { PrintLineUtf8(L"所选游戏已退出，助手结束。"); return 0; }
            if (GetTickCount64() >= deadline) { PrintLineUtf8(L"等待超时：游戏未在限定时间内启动，助手结束。"); return 0; }
            Sleep(500);
            continue;
        }
        if (!saw_game) { saw_game = true; PrintUtf8(L"已检测到游戏进程 pid="); { char pid_text[16]{}; std::snprintf(pid_text, sizeof pid_text, "%lu", static_cast<unsigned long>(processes.games[0])); fwrite(pid_text, 1, std::strlen(pid_text), stdout); fflush(stdout); } PrintUtf8(L"\n"); }
        if (processes.hosts.empty()) { stable.clear(); Sleep(500); continue; }
        Enumerated found{processes.games[0], processes.hosts[0], {}, {}};
        if (!EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&found))) {
            PrintErrUtf8(L"拒绝执行：窗口枚举失败。\n");
            return 6;
        }
        std::set<std::pair<DWORD, UINT_PTR>> existing;
        for (const auto &window : found.hosts) existing.emplace(window.pid, reinterpret_cast<UINT_PTR>(window.hwnd));
        for (auto it = attempted.begin(); it != attempted.end();) {
            if (!existing.count(*it)) it = attempted.erase(it); else ++it;
        }
        std::map<WindowKey, unsigned> next;
        if (found.games.size() == 1 && GetForegroundWindow() == found.games[0].hwnd) {
            const auto &game_window = found.games[0];
            for (const auto &window : found.hosts) {
                const LONG_PTR main_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
                const LONG_PTR preview_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
                const bool preview = window.style == preview_style;
                if ((window.style != main_style && !preview) || !GeometryValid(window.rect, game_window.rect, preview)) continue;
                WindowKey key{window.pid, reinterpret_cast<UINT_PTR>(window.hwnd), game_window.pid,
                              reinterpret_cast<UINT_PTR>(game_window.hwnd), window.style,
                              window.rect.left, window.rect.top, window.rect.right, window.rect.bottom};
                next[key] = stable[key] + 1;
                const auto identity = std::make_pair(window.pid, reinterpret_cast<UINT_PTR>(window.hwnd));
                if (next[key] < 2 || attempted.count(identity)) continue;
                DWORD game_owner = 0, host_owner = 0;
                GetWindowThreadProcessId(game_window.hwnd, &game_owner);
                GetWindowThreadProcessId(window.hwnd, &host_owner);
                RECT game_now{}, host_now{};
                wchar_t game_class[256]{}, host_class[256]{};
                if (GetForegroundWindow() != game_window.hwnd || game_owner != game_window.pid || host_owner != window.pid ||
                    !IsWindowVisible(game_window.hwnd) || !IsWindowVisible(window.hwnd) ||
                    !GetClassNameW(game_window.hwnd, game_class, 256) || wcscmp(game_class, L"GxWindowClassD3d") != 0 ||
                    !GetClassNameW(window.hwnd, host_class, 256) || wcscmp(host_class, L"StandaloneDLSSNRNativeOutput") != 0 ||
                    !SamePath(ProcessPath(game_owner), game.wstring()) || !SamePath(ProcessPath(host_owner), host.wstring()) ||
                    Sha256(game) != "4c459de64d38b5795becc6c01a03ba00d34686598fae707c442446722617868a" ||
                    Sha256(host) != "e05a0aef9595849e2910d408cdfadf60c79a26addccba6dfe79c96b424bdef8b" ||
                    !GetWindowRect(game_window.hwnd, &game_now) || !GetWindowRect(window.hwnd, &host_now) ||
                    !GeometryValid(host_now, game_now, preview)) continue;
                game_owner = host_owner = 0;
                GetWindowThreadProcessId(game_window.hwnd, &game_owner);
                GetWindowThreadProcessId(window.hwnd, &host_owner);
                if (GetForegroundWindow() != game_window.hwnd || game_owner != game_window.pid || host_owner != window.pid ||
                    !GetClassNameW(game_window.hwnd, game_class, 256) || wcscmp(game_class, L"GxWindowClassD3d") != 0 ||
                    !GetClassNameW(window.hwnd, host_class, 256) || wcscmp(host_class, L"StandaloneDLSSNRNativeOutput") != 0) continue;
                attempted.insert(identity);
                const auto result = SetClickThroughLayer(window.hwnd, preview);
                PrintUtf8(preview ? L"窗口=预览 应用=" : L"窗口=主输出 应用=");
                { char state_text[32]{}; std::snprintf(state_text, sizeof state_text, "%u error=%lu", result.ok ? 1u : 0u, result.error); fwrite(state_text, 1, std::strlen(state_text), stdout); fflush(stdout); }
                PrintUtf8(L"\n");
                std::fflush(stdout);
            }
        }
        stable = std::move(next);
        Sleep(500);
    }
}
