#define wWinMain NrConsoleUnusedEntry
#include "../native/console.cpp"
#undef wWinMain
#include <stdexcept>
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "output required");
        g_client_dir = std::wstring(argv[1]) + L"\\legacy-console";
        CreateDirectoryW(g_client_dir.c_str(), nullptr);
        CreateDirectoryW((g_client_dir + L"\\WTF").c_str(), nullptr);
        g_config_wtf = g_client_dir + L"\\WTF\\Config.wtf";
        const std::string original = "SET maxFPSBk \"9\"\r\nSET maxFPS \"30\"\r\nSET locale \"zhCN\"\r\n";
        DWORD error = 0;
        Check(control_center::config_wtf_detail::WriteAllBytes(g_config_wtf, original, CREATE_NEW, error), "fixture write");
        ReadMaxFps(); Check(g_max_fps == 30 && g_max_fps_background == 9, "legacy reader uses client's existing keys");
        Check(WriteMaxFps(60, 120), "legacy write");
        ReadMaxFps(); Check(g_max_fps == 60 && g_max_fps_background == 120, "legacy roundtrip");
        std::string bytes;
        Check(control_center::config_wtf_detail::ReadWholeBytes(g_config_wtf, bytes, error), "read saved bytes");
        Check(bytes == "SET maxFPSBk \"120\"\r\nSET maxFPS \"60\"\r\nSET locale \"zhCN\"\r\n", "preserve key spelling and unrelated settings");
        Check(!WriteMaxFps(1000, 120), "reject invalid range");
        std::puts("PASS legacy console shares FPS reader/writer and preserves existing key spelling");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
