#include "../native/control_center/ds5_settings.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace control_center;

int gFailures = 0;

void Fail(const char* what) {
    std::fprintf(stderr, "ds5 settings test failed: %s\n", what);
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

bool WriteBytes(const std::wstring& path, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size();
    CloseHandle(file);
    return ok;
}

bool ReadBytes(const std::wstring& path, std::string& bytes) {
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) bytes.append(buffer, read);
    CloseHandle(file);
    return true;
}

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

bool WideContains(const std::wstring& text, const wchar_t* needle) {
    return text.find(needle) != std::wstring::npos;
}

size_t CountOccurrences(const std::string& text, const char* needle) {
    size_t count = 0;
    size_t position = 0;
    const size_t length = std::strlen(needle);
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += length;
    }
    return count;
}

const char* kMinimalIni =
    "[GENERAL]\r\n"
    "PresetPath=.\\ReShadePreset.ini\r\n"
    "\r\n"
    "[Standalone.DLSSNR]\r\n"
    "NeuralRendering=1\r\n"
    "Model=1\r\n"
    "Intensity=1.0\r\n"
    "LocalTone=1.0\r\n"
    "LocalStructure=1.0\r\n"
    "SkinStructure=-1.0\r\n";

// Same values as kMinimalIni but already in the canonical "0.##" form, so a default save is a no-op.
const char* kNormalizedIni =
    "[GENERAL]\r\n"
    "PresetPath=.\\ReShadePreset.ini\r\n"
    "\r\n"
    "[Standalone.DLSSNR]\r\n"
    "NeuralRendering=1\r\n"
    "Model=1\r\n"
    "Intensity=1\r\n"
    "LocalTone=1\r\n"
    "LocalStructure=1\r\n"
    "SkinStructure=-1\r\n";

std::wstring MakeClient(const std::wstring& root, const wchar_t* name, const std::string& iniBytes,
                        bool withIni = true) {
    const std::wstring client = Join(root, name);
    MakeDirectory(client);
    MakeDirectory(Join(client, L"host64"));
    if (withIni) {
        if (!WriteBytes(Join(client, L"host64\\ReShade.ini"), iniBytes)) Fail("cannot write fixture ReShade.ini");
    }
    return client;
}

void TestMissingFile(const std::wstring& root, const std::wstring& ops) {
    const std::wstring client = MakeClient(root, L"missing", {}, false);
    const auto settings = ReadDs5AdvancedSettings(client);
    Check(!settings.exists && !settings.readable, "missing ReShade.ini must read as absent");
    Check(settings.model == 1 && settings.intensity == 1.0 && settings.skinStructure == -1.0,
          "missing file must expose the defaults");
    const auto saved = SaveDs5AdvancedSettings(client, ops, Ds5Values{}, true, false);
    Check(!saved && saved.error == Ds5WriteError::MissingFile, "save without ReShade.ini must fail");
}

void TestDefaultsAndTier(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"defaults", "[Standalone.DLSSNR]\r\nModel=3\r\n");
    const auto settings = ReadDs5AdvancedSettings(client);
    Check(settings.exists && settings.readable, "section with one key must be readable");
    Check(settings.model == 3, "Model must be parsed");
    Check(settings.intensity == 1.0 && settings.localTone == 1.0 && settings.localStructure == 1.0 &&
              settings.skinStructure == -1.0,
          "missing numeric keys must fall back to the historical defaults");
    Check(!settings.neuralRenderingPresent, "NeuralRendering has no default and must be reported as absent");
    Check(Ds5ModelName(settings.model) == L"电影", "model 3 maps to the movie name");
    Check(DetectDs5Tier(settings) == L"原画",
          "absent NeuralRendering counts as off, matching the legacy tier detection");

    const std::wstring neuralClient = MakeClient(root, L"neural", "[Standalone.DLSSNR]\r\nNeuralRendering=1\r\nModel=2\r\n");
    const auto neural = ReadDs5AdvancedSettings(neuralClient);
    Check(neural.readable && neural.neuralRendering, "NeuralRendering=1 must be reported");
    Check(DetectDs5Tier(neural) == L"自定义", "default numbers with NeuralRendering on cannot match a tier");

    std::string tierIni = "[Standalone.DLSSNR]\r\nNeuralRendering=1\r\nModel=2\r\nIntensity=0.35\r\n";
    tierIni += "LocalTone=1.0\r\nLocalStructure=0.25\r\nSkinStructure=-1.0\r\n";
    const std::wstring tierClient = MakeClient(root, L"tier", tierIni);
    const auto tier = ReadDs5AdvancedSettings(tierClient);
    Check(tier.readable, "tier fixture must be readable");
    Check(tier.model == 2 && tier.intensity > 0.349 && tier.intensity < 0.351, "tier values must be parsed");
    Check(DetectDs5Tier(tier) == L"均衡", "balanced tier must be detected");
    Check(tier.neuralRenderingPresent && tier.neuralRendering, "NeuralRendering=1 must be reported");
}

void TestWritePreservesBytes(const std::wstring& root, const std::wstring& ops) {
    std::string original;
    original += "\xEF\xBB\xBF";  // BOM must be preserved
    original += "[GENERAL]\r\nPresetPath=.\\ReShadePreset.ini\r\n\r\n";
    original += "[Standalone.DLSSNR]\r\n";
    original += "; user comment that must survive\r\n";
    original += "NeuralRendering=0\r\n";
    original += "Model=1\r\n";
    original += "Intensity=1.0\r\n";
    original += "LocalTone=1.0\r\n";
    original += "LocalStructure=1.0\r\n";
    original += "SkinStructure=-1.0\r\n";
    original += "FrameGeneration=1\r\n";
    original += "\r\n[OVERLAY]\r\nShowFPS=0\r\n";
    const std::wstring client = MakeClient(root, L"preserve", original);

    Ds5Values values;
    values.model = 2;
    values.intensity = 0.6;
    values.localTone = 1.5;
    values.localStructure = 0.35;
    values.skinStructure = -0.5;
    values.neuralRendering = true;
    const auto saved = SaveDs5AdvancedSettings(client, ops, values, std::optional<bool>(true),
                                               std::optional<bool>(false));
    Check(static_cast<bool>(saved), "preserving write must succeed");
    Check(saved.changed, "changed values must be reported as written");
    if (!saved) return;

    std::string updated;
    Check(ReadBytes(Join(client, L"host64\\ReShade.ini"), updated), "updated ReShade.ini must be readable");
    Check(updated.size() > 3 && static_cast<unsigned char>(updated[0]) == 0xEF &&
              static_cast<unsigned char>(updated[1]) == 0xBB && static_cast<unsigned char>(updated[2]) == 0xBF,
          "BOM must be preserved");
    Check(Contains(updated, "; user comment that must survive\r\n"), "comments must survive");
    Check(Contains(updated, "FrameGeneration=1\r\n"), "unknown keys must survive");
    Check(Contains(updated, "[OVERLAY]\r\nShowFPS=0\r\n"), "other sections must survive");
    Check(Contains(updated, "NeuralRendering=1\r\n"), "NeuralRendering must be rewritten");
    Check(Contains(updated, "Model=2\r\n"), "Model must be rewritten");
    Check(Contains(updated, "Intensity=0.6\r\n"), "Intensity must use the 0.## format");
    Check(Contains(updated, "LocalTone=1.5\r\n"), "LocalTone must use the 0.## format");
    Check(Contains(updated, "LocalStructure=0.35\r\n"), "LocalStructure must use the 0.## format");
    Check(Contains(updated, "SkinStructure=-0.5\r\n"), "SkinStructure must use the 0.## format");
    Check(Contains(updated, "DetachedPresentation=1\r\n"), "DetachedPresentation must be written");
    Check(Contains(updated, "WindowedInputScaling=1\r\n"), "detached output must enable windowed input scaling");
    Check(Contains(updated, "WindowedVirtualization=1\r\n"),
          "detached output must enable windowed virtualization");
    Check(Contains(updated, "NvidiaOpticalFlowMotion=0\r\n"), "OpticalFlow=false must be written");

    const auto reread = ReadDs5AdvancedSettings(client);
    Check(reread.readable && reread.model == 2 && reread.neuralRendering, "written values must read back");
    Check(std::fabs(reread.intensity - 0.6) < 1e-6 && std::fabs(reread.localTone - 1.5) < 1e-6,
          "written numbers must read back exactly");

    std::string backup;
    Check(ReadBytes(saved.backupPath, backup), "backup must exist");
    Check(backup == original, "backup must contain the original bytes");
    Check(WideContains(saved.backupPath, L"config-"), "backup must live in a unique config directory");

    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(Join(client, L"host64\\*.dlss5-settings-*").c_str(), &data);
    Check(find == INVALID_HANDLE_VALUE, "no temporary file may be left behind");
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
}

void TestAppendMissingKeys(const std::wstring& root, const std::wstring& ops) {
    const std::wstring client = MakeClient(root, L"append", "[Standalone.DLSSNR]\r\nModel=1\r\n");
    Ds5Values values;
    const auto saved = SaveDs5AdvancedSettings(client, ops, values, std::nullopt, std::nullopt);
    Check(static_cast<bool>(saved) && saved.changed, "missing keys must be appended");
    std::string updated;
    ReadBytes(Join(client, L"host64\\ReShade.ini"), updated);
    Check(Contains(updated, "NeuralRendering=1\r\n"), "NeuralRendering must be appended");
    Check(Contains(updated, "Intensity=1\r\n"), "default intensity must be appended as 1");
    Check(Contains(updated, "SkinStructure=-1\r\n"), "default skin structure must be appended as -1");
    Check(CountOccurrences(updated, "Model=") == 1, "existing key must be replaced, not duplicated");

    const auto reread = ReadDs5AdvancedSettings(client);
    Check(reread.readable && reread.intensity == 1.0 && reread.skinStructure == -1.0 &&
              reread.neuralRendering,
          "appended keys must read back");
}

void TestUnchangedShortCircuit(const std::wstring& root, const std::wstring& ops) {
    const std::wstring client = MakeClient(root, L"unchanged", kNormalizedIni);
    std::string before;
    Check(ReadBytes(Join(client, L"host64\\ReShade.ini"), before), "fixture must be readable");
    Ds5Values values;
    const auto saved = SaveDs5AdvancedSettings(client, ops, values, std::nullopt, std::nullopt);
    Check(static_cast<bool>(saved), "no-op save must succeed");
    Check(!saved.changed, "no-op save must report unchanged");
    Check(saved.backupPath.empty(), "no-op save must not create a backup");
    std::string after;
    ReadBytes(Join(client, L"host64\\ReShade.ini"), after);
    Check(after == before, "no-op save must not touch the file");
}

void TestSectionProblems(const std::wstring& root, const std::wstring& ops) {
    const std::wstring missing = MakeClient(root, L"nosection", "[GENERAL]\r\nPresetPath=.\\x.ini\r\n");
    const auto missingRead = ReadDs5AdvancedSettings(missing);
    Check(missingRead.exists && !missingRead.readable, "missing section must be unreadable");
    const auto missingSave = SaveDs5AdvancedSettings(missing, ops, Ds5Values{}, std::nullopt, std::nullopt);
    Check(!missingSave && missingSave.error == Ds5WriteError::Read, "save without the section must fail");

    std::string duplicatedSection = kMinimalIni;
    duplicatedSection += "\r\n[Standalone.DLSSNR]\r\nModel=2\r\n";
    const std::wstring duplicateSectionClient = MakeClient(root, L"dupsection", duplicatedSection);
    const auto duplicatedRead = ReadDs5AdvancedSettings(duplicateSectionClient);
    Check(duplicatedRead.exists && !duplicatedRead.readable, "duplicated section must be unreadable");

    std::string duplicatedKey = kMinimalIni;
    duplicatedKey += "Model=3\r\n";
    const std::wstring duplicateKeyClient = MakeClient(root, L"dupkey", duplicatedKey);
    const auto duplicatedKeyRead = ReadDs5AdvancedSettings(duplicateKeyClient);
    Check(duplicatedKeyRead.exists && !duplicatedKeyRead.readable, "duplicated key must be unreadable");
    const auto duplicatedKeySave =
        SaveDs5AdvancedSettings(duplicateKeyClient, ops, Ds5Values{}, std::nullopt, std::nullopt);
    Check(!duplicatedKeySave && duplicatedKeySave.error == Ds5WriteError::Read,
          "save with a duplicated key must fail");
}

void TestValidation(const std::wstring& root, const std::wstring& ops) {
    const std::wstring client = MakeClient(root, L"validate", kMinimalIni);
    std::string original;
    Check(ReadBytes(Join(client, L"host64\\ReShade.ini"), original), "validation fixture must be readable");

    Ds5Values values;
    values.model = 0;
    Check(SaveDs5AdvancedSettings(client, ops, values, std::nullopt, std::nullopt).error ==
              Ds5WriteError::InvalidModel,
          "model 0 must be rejected");
    values = Ds5Values{};
    values.model = 4;
    Check(SaveDs5AdvancedSettings(client, ops, values, std::nullopt, std::nullopt).error ==
              Ds5WriteError::InvalidModel,
          "model 4 must be rejected");
    values = Ds5Values{};
    values.intensity = -0.01;
    Check(SaveDs5AdvancedSettings(client, ops, values, std::nullopt, std::nullopt).error ==
              Ds5WriteError::InvalidIntensity,
          "negative intensity must be rejected");
    values.intensity = 1.01;
    Check(SaveDs5AdvancedSettings(client, ops, values, std::nullopt, std::nullopt).error ==
              Ds5WriteError::InvalidIntensity,
          "intensity above 1 must be rejected");
    values = Ds5Values{};
    values.localTone = 2.01;
    Check(SaveDs5AdvancedSettings(client, ops, values, std::nullopt, std::nullopt).error ==
              Ds5WriteError::InvalidTone,
          "local tone above 2 must be rejected");
    values = Ds5Values{};
    values.localStructure = 1.01;
    Check(SaveDs5AdvancedSettings(client, ops, values, std::nullopt, std::nullopt).error ==
              Ds5WriteError::InvalidStructure,
          "local structure above 1 must be rejected");
    values = Ds5Values{};
    values.skinStructure = -1.01;
    Check(SaveDs5AdvancedSettings(client, ops, values, std::nullopt, std::nullopt).error ==
              Ds5WriteError::InvalidSkin,
          "skin structure below -1 must be rejected");

    std::string after;
    ReadBytes(Join(client, L"host64\\ReShade.ini"), after);
    Check(after == original, "rejected writes must not touch the file");
}

void TestGameRunningGate(const std::wstring& root, const std::wstring& ops) {
    const std::wstring client = MakeClient(root, L"game", kNormalizedIni);
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        Fail("cannot locate the system directory for the running-process check");
        return;
    }
    const std::wstring source = Join(systemDirectory, L"cmd.exe");
    const std::wstring fakeGame = Join(client, L"wow.exe");
    if (!CopyFileW(source.c_str(), fakeGame.c_str(), FALSE)) {
        Fail("cannot copy cmd.exe into the fixture");
        return;
    }
    std::wstring command = L"\"" + fakeGame + L"\" /c ping -n 20 127.0.0.1 >nul";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        Fail("cannot start the fake game process");
        return;
    }
    Sleep(400);
    const auto blocked = SaveDs5AdvancedSettings(client, ops, Ds5Values{}, std::nullopt, std::nullopt);
    Check(!blocked && blocked.error == Ds5WriteError::GameRunning,
          "save must be refused while a wow.exe process runs");
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Sleep(200);
    const auto saved = SaveDs5AdvancedSettings(client, ops, Ds5Values{}, std::nullopt, std::nullopt);
    Check(static_cast<bool>(saved) && !saved.changed, "save must succeed after the fake game exits");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: control_center_ds5_settings <output-root>\n");
        return 1;
    }
    const std::wstring root = Join(argv[1], L"ds5-settings");
    const std::wstring ops = Join(root, L"ops");
    MakeDirectory(root);
    TestMissingFile(root, ops);
    TestDefaultsAndTier(root);
    TestWritePreservesBytes(root, ops);
    TestAppendMissingKeys(root, ops);
    TestUnchangedShortCircuit(root, ops);
    TestSectionProblems(root, ops);
    TestValidation(root, ops);
    TestGameRunningGate(root, ops);
    if (gFailures != 0) {
        std::fwprintf(stderr, L"ds5 settings: %d failures\n", gFailures);
        return 1;
    }
    std::printf("ds5 settings: ReShade.ini read/write, tier detection, backup and gates passed\n");
    return 0;
}
