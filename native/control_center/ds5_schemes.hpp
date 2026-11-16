#pragma once
// Native support for the DLSS-NR scheme presets (legacy-spec2 §2.8).
//
// The previous PowerShell console stored the built-in presets in
// <script>\schemes\*.json and the user presets in
// <project>\_scratch\dlss5-aio\user-schemes\*.json. The native console is a
// standalone product, so it resolves the two roots explicitly:
//   * builtin root: <exeDir>\schemes          (shipped next to the program)
//   * user root:    <dataDir>\user-schemes    (per-user writable directory)
// A scheme is a small JSON document:
//   {"schema":1,"name":"...","description":"...","created_utc":"...",
//    "values":{"neuralRendering":true,"model":2,"intensity":0.35,
//              "localTone":1.0,"localStructure":0.25,"skinStructure":-1.0}}
//
// The reader accepts the files written by the PowerShell console; the writer
// emits UTF-8 without BOM and a trailing '\n', matching the previous format.
#include <windows.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "client_identity.hpp"
#include "ds5_settings.hpp"
#include "guarded_path.hpp"
#include "strict_json.hpp"

namespace control_center {

constexpr size_t kDs5SchemeMaxBytes = 256u * 1024;  // generous for a preset
constexpr size_t kDs5SchemeMaxNameLength = 24;

struct Ds5Scheme {
    std::wstring name;
    std::wstring description;
    std::wstring path;
    bool builtin = false;
};

struct Ds5SchemeList {
    bool ok = false;
    DWORD win32Error = 0;
    std::wstring error;
    std::vector<Ds5Scheme> schemes;
    explicit operator bool() const { return ok; }
};

struct Ds5SchemeSaveResult {
    bool ok = false;
    DWORD win32Error = 0;
    std::wstring error;
    std::wstring path;
    explicit operator bool() const { return ok; }
};

struct Ds5SchemeApplyResult {
    bool ok = false;
    std::wstring error;
    Ds5WriteResult write;
    explicit operator bool() const { return ok; }
};

inline std::wstring Ds5BuiltinSchemesRoot(const std::wstring& exeDir) {
    if (exeDir.empty()) return L"schemes";
    if (exeDir.back() == L'\\') return exeDir + L"schemes";
    return exeDir + L"\\schemes";
}

inline std::wstring Ds5UserSchemesRoot(const std::wstring& dataDir) {
    if (dataDir.empty()) return L"user-schemes";
    if (dataDir.back() == L'\\') return dataDir + L"user-schemes";
    return dataDir + L"\\user-schemes";
}

namespace ds5_scheme_detail {

inline std::wstring Widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

inline std::string Narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string narrow(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length, nullptr,
                        nullptr);
    return narrow;
}

inline std::wstring TrimName(const std::wstring& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && (value[begin] == L' ' || value[begin] == L'\t' || value[begin] == L'\r' ||
                           value[begin] == L'\n' || value[begin] == 0x3000))
        ++begin;
    while (end > begin && (value[end - 1] == L' ' || value[end - 1] == L'\t' || value[end - 1] == L'\r' ||
                           value[end - 1] == L'\n' || value[end - 1] == 0x3000))
        --end;
    return value.substr(begin, end - begin);
}

// Mirrors [IO.Path]::GetInvalidFileNameChars() plus the reserved DOS device
// names, so a scheme name can always be used as a file name.
inline bool SchemeNameValid(const std::wstring& name, std::wstring& error) {
    if (name.empty() || name.size() > kDs5SchemeMaxNameLength) {
        error = L"方案名限 1-24 个字符，且不能含文件名非法字符。";
        return false;
    }
    static const wchar_t* const kForbidden = L"<>:\"/\\|?*";
    for (const wchar_t ch : name) {
        if (ch < 0x20 || std::wcschr(kForbidden, ch) != nullptr) {
            error = L"方案名限 1-24 个字符，且不能含文件名非法字符。";
            return false;
        }
    }
    if (name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ') {
        error = L"方案名限 1-24 个字符，且不能含文件名非法字符。";
        return false;
    }
    static const wchar_t* const kReserved[] = {L"CON", L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3",
                                               L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1",
                                               L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8",
                                               L"LPT9"};
    std::wstring upper;
    upper.reserve(name.size());
    for (const wchar_t ch : name) upper.push_back(static_cast<wchar_t>(towupper(ch)));
    for (const wchar_t* reserved : kReserved) {
        if (upper == reserved) {
            error = L"方案名限 1-24 个字符，且不能含文件名非法字符。";
            return false;
        }
    }
    return true;
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

inline std::string JsonNumber(double value) {
    std::string text = ds5_detail::FormatNumber(value);
    if (text.find('.') == std::string::npos) text += ".0";
    return text;
}

inline std::string FormatUtcNow() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char buffer[40] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay,
                  utc.wHour, utc.wMinute, utc.wSecond);
    return buffer;
}

inline std::wstring UniqueToken() {
    static unsigned long counter = 0;
    ++counter;
    wchar_t buffer[64] = {};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%lu-%lu-%lu", GetCurrentProcessId(),
                  GetTickCount(), counter);
    return buffer;
}

inline bool DirectoryExistsLocal(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

inline bool EnsureDirectory(const std::wstring& path, DWORD& win32Error) {
    std::wstring canonical;
    if (!CanonicalLocalPath(path, canonical)) {
        win32Error = ERROR_INVALID_NAME;
        return false;
    }
    if (canonical.size() <= 3) return true;
    size_t position = 3;
    while (position <= canonical.size()) {
        const size_t next = canonical.find(L'\\', position);
        const std::wstring current =
            next == std::wstring::npos ? canonical : canonical.substr(0, next);
        if (!CreateDirectoryW(current.c_str(), nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                win32Error = error;
                return false;
            }
        }
        if (next == std::wstring::npos) break;
        position = next + 1;
    }
    return true;
}

inline bool ReadFileBytes(const std::wstring& path, std::string& bytes, DWORD& win32Error) {
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kDs5SchemeMaxBytes) {
        win32Error = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    char buffer[4096];
    while (offset < bytes.size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(
            std::min<size_t>(sizeof(buffer), bytes.size() - offset));
        if (!ReadFile(file, buffer, want, &read, nullptr) || read == 0) {
            win32Error = read == 0 ? ERROR_HANDLE_EOF : GetLastError();
            CloseHandle(file);
            return false;
        }
        std::memcpy(bytes.data() + offset, buffer, read);
        offset += read;
    }
    CloseHandle(file);
    return true;
}

inline bool WriteFileBytes(const std::wstring& path, const std::string& bytes, DWORD& win32Error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        win32Error = GetLastError();
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size() && FlushFileBuffers(file);
    if (!ok) win32Error = GetLastError();
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

inline std::wstring FileNameOnly(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

}  // namespace ds5_scheme_detail

// Enumerates the builtin and user scheme directories. Individual files that do
// not parse are skipped silently, exactly like the old console did.
inline Ds5SchemeList ListDs5Schemes(const std::wstring& builtinRoot, const std::wstring& userRoot) {
    using namespace ds5_scheme_detail;
    Ds5SchemeList result;
    const std::wstring roots[2] = {builtinRoot, userRoot};
    for (int index = 0; index < 2; ++index) {
        const bool builtin = index == 0;
        const std::wstring& root = roots[index];
        if (!DirectoryExistsLocal(root)) continue;
        const std::wstring pattern = root + L"\\*.json";
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
            const std::wstring path = root + L"\\" + data.cFileName;
            std::string bytes;
            DWORD error = 0;
            if (!ReadFileBytes(path, bytes, error)) continue;
            const JsonResult parsed = ParseJsonObject(bytes);
            if (!parsed) continue;
            const Json& document = parsed.value;
            if (!document.contains("schema") || !document["schema"].is_number_integer() ||
                document["schema"].get<int>() != 1)
                continue;
            if (!document.contains("name") || !document["name"].is_string()) continue;
            const std::string name = document["name"].get<std::string>();
            if (name.empty()) continue;
            Ds5Scheme scheme;
            scheme.name = Widen(name);
            if (scheme.name.empty()) continue;
            if (document.contains("description") && document["description"].is_string()) {
                scheme.description = Widen(document["description"].get<std::string>());
            }
            scheme.path = path;
            scheme.builtin = builtin;
            result.schemes.push_back(std::move(scheme));
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    result.ok = true;
    return result;
}

// Reads the current parameters of the client and stores them as a user scheme.
inline Ds5SchemeSaveResult SaveDs5Scheme(const std::wstring& clientRoot, const std::wstring& userRoot,
                                         const std::wstring& name, const std::wstring& description) {
    using namespace ds5_scheme_detail;
    Ds5SchemeSaveResult result;
    const std::wstring clean = TrimName(name);
    std::wstring nameError;
    if (!SchemeNameValid(clean, nameError)) {
        result.error = nameError;
        return result;
    }
    const Ds5AdvancedSettings settings = ReadDs5AdvancedSettings(clientRoot);
    if (!settings.exists) {
        result.error = L"缺少 host64\\ReShade.ini。";
        return result;
    }
    if (!settings.readable) {
        result.error = L"读取 host64\\ReShade.ini 失败。";
        return result;
    }
    const Ds5Values values = Ds5ValuesFrom(settings);

    std::string document;
    document += "{\n";
    document += "  \"schema\": 1,\n";
    document += "  \"name\": \"" + JsonEscape(Narrow(clean)) + "\",\n";
    document += "  \"description\": \"" + JsonEscape(Narrow(description)) + "\",\n";
    document += "  \"created_utc\": \"" + FormatUtcNow() + "\",\n";
    document += "  \"values\": {\n";
    document += "    \"neuralRendering\": " + std::string(values.neuralRendering ? "true" : "false") + ",\n";
    document += "    \"model\": " + std::to_string(values.model) + ",\n";
    document += "    \"intensity\": " + JsonNumber(values.intensity) + ",\n";
    document += "    \"localTone\": " + JsonNumber(values.localTone) + ",\n";
    document += "    \"localStructure\": " + JsonNumber(values.localStructure) + ",\n";
    document += "    \"skinStructure\": " + JsonNumber(values.skinStructure) + "\n";
    document += "  }\n";
    document += "}\n";

    DWORD error = 0;
    if (!EnsureDirectory(userRoot, error)) {
        result.error = L"无法创建用户方案目录。";
        result.win32Error = error;
        return result;
    }
    const std::wstring target = userRoot + L"\\" + clean + L".json";
    const DWORD existing = GetFileAttributesW(target.c_str());
    if (existing != INVALID_FILE_ATTRIBUTES) {
        result.error = L"同名方案已存在；请换一个名字。";
        return result;
    }
    const std::wstring temporary = target + L".new-" + UniqueToken();
    if (!WriteFileBytes(temporary, document, error)) {
        result.error = L"写入方案文件失败。";
        result.win32Error = error;
        return result;
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        result.win32Error = GetLastError();
        DeleteFileW(temporary.c_str());
        result.error = L"写入方案文件失败。";
        return result;
    }
    std::string verify;
    if (!ReadFileBytes(target, verify, error) || verify != document) {
        result.error = L"方案文件回读校验失败。";
        result.win32Error = error;
        return result;
    }
    result.ok = true;
    result.path = target;
    return result;
}

// Applies a scheme file. The file must live inside the builtin or the user
// scheme directory; the values are validated and written exactly like the
// advanced settings dialog.
inline Ds5SchemeApplyResult ApplyDs5Scheme(const std::wstring& clientRoot, const std::wstring& operationsRoot,
                                           const std::wstring& schemePath, const std::wstring& builtinRoot,
                                           const std::wstring& userRoot) {
    using namespace ds5_scheme_detail;
    Ds5SchemeApplyResult result;
    std::wstring canonical;
    if (!CanonicalLocalPath(schemePath, canonical)) {
        result.error = L"方案文件路径无效。";
        return result;
    }
    std::wstring canonicalBuiltin;
    std::wstring canonicalUser;
    const bool builtinKnown = CanonicalLocalPath(builtinRoot, canonicalBuiltin);
    const bool userKnown = CanonicalLocalPath(userRoot, canonicalUser);
    const bool inside = (builtinKnown && WithinRoot(canonical, canonicalBuiltin)) ||
                        (userKnown && WithinRoot(canonical, canonicalUser));
    if (!inside) {
        result.error = L"方案文件必须位于内置目录或用户方案目录。";
        return result;
    }
    const DWORD attributes = GetFileAttributesW(canonical.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result.error = L"读取方案文件失败。";
        return result;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result.error = L"方案文件必须位于内置目录或用户方案目录。";
        return result;
    }
    std::string bytes;
    DWORD error = 0;
    if (!ReadFileBytes(canonical, bytes, error)) {
        result.error = L"读取方案文件失败。";
        result.write.win32Error = error;
        return result;
    }
    const JsonResult parsed = ParseJsonObject(bytes);
    if (!parsed) {
        result.error = L"方案格式不受支持。";
        return result;
    }
    const Json& document = parsed.value;
    if (!document.contains("schema") || !document["schema"].is_number_integer() ||
        document["schema"].get<int>() != 1 || !document.contains("values") ||
        !document["values"].is_object() || document["values"].empty()) {
        result.error = L"方案格式不受支持。";
        return result;
    }
    const Json& values = document["values"];
    auto missing = [&values, &result](const char* key) {
        result.error = L"方案缺少字段 " + Widen(key) + L"。";
        return result;
    };
    if (!values.contains("model") || !values["model"].is_number_integer()) return missing("model");
    if (!values.contains("intensity") || !values["intensity"].is_number()) return missing("intensity");
    if (!values.contains("localTone") || !values["localTone"].is_number()) return missing("localTone");
    if (!values.contains("localStructure") || !values["localStructure"].is_number())
        return missing("localStructure");
    if (!values.contains("skinStructure") || !values["skinStructure"].is_number()) return missing("skinStructure");

    Ds5Values target;
    target.model = values["model"].get<int>();
    target.intensity = values["intensity"].get<double>();
    target.localTone = values["localTone"].get<double>();
    target.localStructure = values["localStructure"].get<double>();
    target.skinStructure = values["skinStructure"].get<double>();
    // The old Apply-Dlss5Scheme cast a missing NeuralRendering to false.
    target.neuralRendering =
        values.contains("neuralRendering") && values["neuralRendering"].is_boolean()
            ? values["neuralRendering"].get<bool>()
            : false;

    result.write = SaveDs5AdvancedSettings(clientRoot, operationsRoot, target, std::nullopt, std::nullopt);
    if (!result.write) {
        if (result.write.error == Ds5WriteError::None) {
            result.error = L"写入 host64\\ReShade.ini 失败。";
        }
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace control_center
