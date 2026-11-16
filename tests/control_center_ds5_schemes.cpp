#include "../native/control_center/ds5_schemes.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace control_center;

int gFailures = 0;

void Fail(const char* what) {
    std::fprintf(stderr, "ds5 schemes test failed: %s\n", what);
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

bool ContainsW(const std::wstring& text, const wchar_t* needle) {
    return text.find(needle) != std::wstring::npos;
}

const char* kIni =
    "[GENERAL]\r\n"
    "PresetPath=.\\ReShadePreset.ini\r\n"
    "\r\n"
    "[Standalone.DLSSNR]\r\n"
    "NeuralRendering=1\r\n"
    "Model=2\r\n"
    "Intensity=0.35\r\n"
    "LocalTone=1\r\n"
    "LocalStructure=0.25\r\n"
    "SkinStructure=-1\r\n";

const char* kScheme =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"name\": \"内置均衡\",\n"
    "  \"description\": \"shipped preset\",\n"
    "  \"created_utc\": \"2026-01-01T00:00:00Z\",\n"
    "  \"values\": {\n"
    "    \"neuralRendering\": true,\n"
    "    \"model\": 2,\n"
    "    \"intensity\": 0.35,\n"
    "    \"localTone\": 1.0,\n"
    "    \"localStructure\": 0.25,\n"
    "    \"skinStructure\": -1.0\n"
    "  }\n"
    "}\n";

std::wstring MakeClient(const std::wstring& root, const wchar_t* name, const char* ini = kIni, bool withIni = true) {
    const std::wstring client = Join(root, name);
    MakeDirectory(client);
    MakeDirectory(Join(client, L"host64"));
    if (withIni && !WriteBytes(Join(client, L"host64\\ReShade.ini"), ini)) Fail("cannot write fixture ReShade.ini");
    return client;
}

std::wstring MakeSchemesRoot(const std::wstring& root, const wchar_t* name) {
    const std::wstring path = Join(root, name);
    MakeDirectory(path);
    return path;
}

void TestList(const std::wstring& root) {
    const auto empty = ListDs5Schemes(Join(root, L"no-builtin"), Join(root, L"no-user"));
    Check(empty.ok && empty.schemes.empty(), "missing roots must list nothing");

    const std::wstring builtin = MakeSchemesRoot(root, L"builtin");
    const std::wstring user = MakeSchemesRoot(root, L"user");
    Check(WriteBytes(Join(builtin, L"a.json"), kScheme), "cannot write builtin scheme");
    Check(WriteBytes(Join(builtin, L"broken.json"), "{not json"), "cannot write broken scheme");
    Check(WriteBytes(Join(builtin, L"schema2.json"),
                     "{\"schema\":2,\"name\":\"x\",\"values\":{}}\n"),
          "cannot write schema 2 scheme");
    Check(WriteBytes(Join(builtin, L"noname.json"), "{\"schema\":1,\"name\":\"\",\"values\":{}}\n"),
          "cannot write nameless scheme");
    Check(WriteBytes(Join(user, L"b.json"),
                     "{\"schema\":1,\"name\":\"用户方案\",\"description\":\"mine\",\"values\":{\"model\":1}}\n"),
          "cannot write user scheme");

    const auto list = ListDs5Schemes(builtin, user);
    Check(list.ok, "listing must succeed");
    Check(list.schemes.size() == 2, "invalid scheme files must be skipped");
    if (list.schemes.size() == 2) {
        Check(list.schemes[0].builtin && list.schemes[1].builtin == false, "builtin schemes must come first");
        Check(list.schemes[0].name == L"内置均衡", "builtin name must be parsed");
        Check(list.schemes[0].description == L"shipped preset", "builtin description must be parsed");
        Check(ContainsW(list.schemes[0].path, L"builtin\\a.json"), "builtin path must point at the file");
        Check(list.schemes[1].name == L"用户方案", "user name must be parsed");
        Check(ContainsW(list.schemes[1].path, L"user\\b.json"), "user path must point at the file");
    }
}

void TestSave(const std::wstring& root) {
    const std::wstring client = MakeClient(root, L"client-save");
    const std::wstring user = Join(root, L"save-user");  // intentionally does not exist yet
    const auto saved = SaveDs5Scheme(client, user, L"  我的方案  ", L"由夹具保存");
    Check(static_cast<bool>(saved), "scheme save must succeed");
    if (!saved) return;
    Check(ContainsW(saved.path, L"我的方案.json"), "scheme file must use the cleaned name");
    std::string bytes;
    Check(ReadBytes(saved.path, bytes), "saved scheme must be readable");
    Check(!(bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF),
          "scheme must be UTF-8 without BOM");
    Check(!bytes.empty() && bytes.back() == '\n', "scheme must end with a newline");
    Check(Contains(bytes, "\"schema\": 1"), "scheme must declare schema 1");
    Check(Contains(bytes, "\"我的方案\""), "scheme must store the name");
    Check(Contains(bytes, "\"model\": 2"), "scheme must store the model");
    Check(Contains(bytes, "\"intensity\": 0.35"), "scheme must store the intensity");

    const auto duplicate = SaveDs5Scheme(client, user, L"我的方案", L"");
    Check(!duplicate && duplicate.error == L"同名方案已存在；请换一个名字。", "duplicate names must be rejected");

    const auto invalid = SaveDs5Scheme(client, user, L"a?b", L"");
    Check(!invalid && invalid.error == L"方案名限 1-24 个字符，且不能含文件名非法字符。",
          "invalid characters must be rejected");
    const auto tooLong = SaveDs5Scheme(client, user, L"0123456789012345678901234", L"");
    Check(!tooLong && !tooLong.error.empty(), "names longer than 24 characters must be rejected");
    const auto emptyName = SaveDs5Scheme(client, user, L"   ", L"");
    Check(!emptyName && !emptyName.error.empty(), "blank names must be rejected");

    const std::wstring noIni = MakeClient(root, L"client-noini", nullptr, false);
    const auto missing = SaveDs5Scheme(noIni, user, L"x", L"");
    Check(!missing && missing.error == L"缺少 host64\\ReShade.ini。", "saving without ReShade.ini must fail");

    const auto list = ListDs5Schemes(Join(root, L"unused-builtin"), user);
    Check(list.ok && list.schemes.size() == 1 && list.schemes[0].name == L"我的方案",
          "saved scheme must appear in the list");
}

void TestApplyRoundTrip(const std::wstring& root, const std::wstring& ops) {
    const std::wstring client = MakeClient(root, L"client-apply");
    const std::wstring user = MakeSchemesRoot(root, L"apply-user");
    const std::wstring builtin = MakeSchemesRoot(root, L"apply-builtin");
    const auto saved = SaveDs5Scheme(client, user, L"roundtrip", L"");
    Check(static_cast<bool>(saved), "roundtrip scheme must be saved");
    if (!saved) return;

    Ds5Values changed;
    changed.model = 1;
    changed.intensity = 1.0;
    changed.localTone = 2.0;
    changed.localStructure = 1.0;
    changed.skinStructure = 0.5;
    changed.neuralRendering = false;
    const auto write = SaveDs5AdvancedSettings(client, ops, changed, std::nullopt, std::nullopt);
    Check(static_cast<bool>(write) && write.changed, "the fixture parameters must be changeable");

    const auto applied = ApplyDs5Scheme(client, ops, saved.path, builtin, user);
    Check(static_cast<bool>(applied), "scheme apply must succeed");
    const auto reread = ReadDs5AdvancedSettings(client);
    Check(reread.readable, "ReShade.ini must stay readable after apply");
    Check(reread.model == 2 && std::fabs(reread.intensity - 0.35) < 1e-6 &&
              std::fabs(reread.localStructure - 0.25) < 1e-6 && reread.neuralRendering,
          "apply must restore the saved values");
    Check(applied.write.changed, "apply must report a change");
}

void TestApplyGuards(const std::wstring& root, const std::wstring& ops) {
    const std::wstring client = MakeClient(root, L"client-guards");
    const std::wstring user = MakeSchemesRoot(root, L"guard-user");
    const std::wstring builtin = MakeSchemesRoot(root, L"guard-builtin");
    const std::wstring outside = Join(root, L"outside.json");
    Check(WriteBytes(outside, kScheme), "cannot write outside scheme");

    const auto outsideApply = ApplyDs5Scheme(client, ops, outside, builtin, user);
    Check(!outsideApply && outsideApply.error == L"方案文件必须位于内置目录或用户方案目录。",
          "schemes outside the allowed roots must be rejected");

    const std::wstring malformed = Join(user, L"malformed.json");
    Check(WriteBytes(malformed, "{\"schema\":2,\"name\":\"x\",\"values\":{}}\n"), "cannot write malformed");
    const auto malformedApply = ApplyDs5Scheme(client, ops, malformed, builtin, user);
    Check(!malformedApply && malformedApply.error == L"方案格式不受支持。", "schema 2 must be rejected");

    const std::wstring emptyValues = Join(user, L"empty.json");
    Check(WriteBytes(emptyValues, "{\"schema\":1,\"name\":\"x\",\"values\":{}}\n"), "cannot write empty values");
    const auto emptyApply = ApplyDs5Scheme(client, ops, emptyValues, builtin, user);
    Check(!emptyApply && emptyApply.error == L"方案格式不受支持。", "empty values must be rejected");

    const std::wstring noModel = Join(user, L"nomodel.json");
    Check(WriteBytes(noModel,
                     "{\"schema\":1,\"values\":{\"intensity\":1,\"localTone\":1,\"localStructure\":1,"
                     "\"skinStructure\":-1}}\n"),
          "cannot write no-model scheme");
    const auto noModelApply = ApplyDs5Scheme(client, ops, noModel, builtin, user);
    Check(!noModelApply && noModelApply.error == L"方案缺少字段 model。",
          "missing fields must be reported explicitly");

    const std::wstring invalidModel = Join(user, L"invalidmodel.json");
    Check(WriteBytes(invalidModel,
                     "{\"schema\":1,\"values\":{\"model\":9,\"intensity\":1,\"localTone\":1,"
                     "\"localStructure\":1,\"skinStructure\":-1}}\n"),
          "cannot write invalid-model scheme");
    const auto invalidModelApply = ApplyDs5Scheme(client, ops, invalidModel, builtin, user);
    Check(!invalidModelApply && invalidModelApply.write.error == Ds5WriteError::InvalidModel,
          "invalid values must be rejected by the settings writer");

    const std::wstring builtinScheme = Join(builtin, L"builtin.json");
    Check(WriteBytes(builtinScheme, kScheme), "cannot write builtin scheme");
    const auto builtinApply = ApplyDs5Scheme(client, ops, builtinScheme, builtin, user);
    Check(static_cast<bool>(builtinApply), "builtin schemes must be applicable");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: control_center_ds5_schemes <output-root>\n");
        return 1;
    }
    const std::wstring root = Join(argv[1], L"ds5-schemes");
    const std::wstring ops = Join(root, L"ops");
    MakeDirectory(root);
    TestList(root);
    TestSave(root);
    TestApplyRoundTrip(root, ops);
    TestApplyGuards(root, ops);
    if (gFailures != 0) {
        std::fwprintf(stderr, L"ds5 schemes: %d failures\n", gFailures);
        return 1;
    }
    std::printf("ds5 schemes: listing, save, duplicate/name guards, apply and round-trip passed\n");
    return 0;
}
