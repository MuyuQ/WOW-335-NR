// Compile the panel's actual persistence functions. The renamed application
// entry point is never called: this test opens no window or game process.
#define wmain NrPanelApplicationMain
#include "../native/panel.cpp"
#undef wmain
#include <stdexcept>
static void Check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
int wmain(int argc, wchar_t **argv) {
    try {
        Check(argc == 2, "test directory required");
        const fs::path dir(argv[1]);
        IniPath = (dir / L"ReShade.ini").wstring();
        ParamsPath = (dir / L"live-params.json").wstring();
        CustomPath = (dir / L"custom-tier.json").wstring();
        Check(WritePrivateProfileStringW(L"Standalone.DLSSNR", L"FgMultiplier", L"4", IniPath.c_str()) != 0, "INI setup failed");
        Params loaded;
        ReadIniParams(loaded);
        Check(loaded.fg_multiplier == 4, "INI lost multiplier");
        loaded.intensity = .72f;
        loaded.fg = true;
        Check(SaveCustomTier(loaded), "custom save failed");
        g_custom = Params{};
        g_custom_valid = false;
        LoadCustomTier();
        Check(g_custom_valid && g_custom.Equals(loaded), "custom reload lost fields");
        Params other = loaded;
        other.fg_multiplier = 2;
        Check(!other.Equals(loaded), "equality ignores multiplier");
        Check(WriteParamsAtomic(ParamsPath, loaded), "initial live save failed");
        HANDLE busy = CreateFileW(ParamsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(busy != INVALID_HANDLE_VALUE, "lock setup failed");
        g_save.dirty = true;
        loaded.intensity = .81f;
        Check(!g_save.Try([&] { return WriteParamsAtomic(ParamsPath, loaded); }) && g_save.dirty,
              "failed write lost pending parameters");
        CloseHandle(busy);
        Check(g_save.Try([&] { return WriteParamsAtomic(ParamsPath, loaded); }) && !g_save.dirty,
              "retry did not save after lock release");
        HANDLE file = CreateFileW(ParamsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "readback open failed");
        char text[512]{}; DWORD n = 0;
        const bool read = ReadFile(file, text, sizeof(text)-1, &n, nullptr) != 0;
        CloseHandle(file);
        nr_params::Values values;
        Check(read && nr_params::Parse(std::string_view(text, n), values) &&
              values.multiplier == 4 && fabsf(values.intensity-.81f)<.005f, "retry saved wrong parameters");
        puts("PASS panel INI/custom/live multiplier roundtrip and locked-file retry without another edit");
        return 0;
    } catch (const std::exception &e) { fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
