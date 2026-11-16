#pragma once
// Native reader/writer for the DLSS-NR parameters stored in
// <clientRoot>\host64\ReShade.ini under the [Standalone.DLSSNR] section.
//
// Behaviour mirrors the previous PowerShell implementation (legacy-spec2 §2):
//   * defaults Model=1, Intensity=1.0, LocalTone=1.0, LocalStructure=1.0,
//     SkinStructure=-1.0; NeuralRendering has no default (only reported when present)
//   * validation messages are the exact user facing strings of the PowerShell code
//   * writes happen in the historical key order and only touch the value span of the
//     matching lines, so comments, unknown keys and line endings survive
//   * the file stays UTF-8 (BOM preserved); the original ANSI/UTF-16 case is rejected
//
// Differences from the previous implementation, all intentional:
//   * a unique backup directory per write instead of a fixed one
//   * a unique temporary file name in the client directory
//   * the current file hash and the blocking processes are re-checked immediately
//     before the atomic replace, and a failed read-back restores the backup
#include <windows.h>
#include <tlhelp32.h>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "file_identity.hpp"
#include "guarded_path.hpp"
#include "journal.hpp"
#include "write_new.hpp"

namespace control_center {

constexpr size_t kReShadeIniMaxBytes = 1u << 20;  // 1 MiB is far above any real ReShade.ini
constexpr const char* kDs5SectionName = "Standalone.DLSSNR";

struct Ds5AdvancedSettings {
    bool exists = false;
    bool readable = false;
    bool bom = false;
    int model = 1;
    double intensity = 1.0;
    double localTone = 1.0;
    double localStructure = 1.0;
    double skinStructure = -1.0;
    bool neuralRendering = false;
    bool neuralRenderingPresent = false;
    bool detachedPresentation = true;
    bool detachedPresent = false;
    bool opticalFlow = false;
    bool opticalPresent = false;
    DWORD win32Error = 0;
    std::string error;
    std::wstring path;
};

enum class Ds5WriteError {
    None,
    InvalidModel,
    InvalidIntensity,
    InvalidTone,
    InvalidStructure,
    InvalidSkin,
    MissingFile,
    GameRunning,
    Read,
    Backup,
    Write,
    Verify,
};

struct Ds5Values {
    int model = 1;
    double intensity = 1.0;
    double localTone = 1.0;
    double localStructure = 1.0;
    double skinStructure = -1.0;
    bool neuralRendering = true;
};

struct Ds5WriteResult {
    Ds5WriteError error = Ds5WriteError::None;
    DWORD win32Error = 0;
    bool changed = false;
    std::wstring backupPath;
    std::wstring detail;

    explicit operator bool() const { return error == Ds5WriteError::None; }
};

inline std::wstring ReShadeIniPath(const std::wstring& clientRoot) {
    if (clientRoot.empty()) return std::wstring();
    std::wstring path = clientRoot;
    if (path.back() != L'\\' && path.back() != L'/') path.push_back(L'\\');
    path += L"host64\\ReShade.ini";
    return path;
}

namespace ds5_detail {

inline char AsciiLower(char value) {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

inline bool EqualsAsciiNoCase(const std::string& text, const char* literal) {
    const size_t literalLength = std::strlen(literal);
    if (text.size() != literalLength) return false;
    for (size_t i = 0; i < literalLength; ++i) {
        if (AsciiLower(text[i]) != AsciiLower(literal[i])) return false;
    }
    return true;
}

inline std::string TrimBlanks(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    return text.substr(begin, end - begin);
}

inline bool HasUtf8Bom(const std::string& bytes, size_t& bodyStart) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bodyStart = 3;
        return true;
    }
    bodyStart = 0;
    return false;
}

inline bool IsValidUtf8(const std::string& text) {
    if (text.empty()) return true;
    if (text.size() > static_cast<size_t>(0x7FFFFFFF)) return false;
    const int length = static_cast<int>(text.size());
    const int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    return wide > 0;
}

inline std::string FormatNumber(double value) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    std::string text(buffer);
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    if (text.empty()) return std::string("0");
    if (text == "-0") text = "0";
    return text;
}

inline bool ParseNumber(const std::string& raw, double& value) {
    const std::string text = TrimBlanks(raw);
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    if (*begin == '+') {
        ++begin;
        if (begin == end) return false;
    }
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

inline bool ParseInteger(const std::string& raw, int& value) {
    const std::string text = TrimBlanks(raw);
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    if (*begin == '+') {
        ++begin;
        if (begin == end) return false;
    }
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

inline std::string UniqueToken() {
    static LONG counter = 0;
    const LONG sequence = InterlockedIncrement(&counter);
    char buffer[96] = {};
    std::snprintf(buffer, sizeof(buffer), "%lu-%llu-%ld", static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(GetTickCount64()), static_cast<long>(sequence));
    return std::string(buffer);
}

inline std::wstring Widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

inline bool SameFileIdentity(const FileIdentity& a, const FileIdentity& b) {
    return a.sha256 == b.sha256 && a.volume == b.volume && a.indexHigh == b.indexHigh &&
           a.indexLow == b.indexLow && a.size == b.size;
}

// One physical line: lineStart is the offset of the first byte, contentEnd excludes
// the trailing CR (if any); nextPosition is the offset after the LF, or text.size()
// when the line is the last one and has no newline.
struct IniLine {
    size_t lineStart = 0;
    size_t contentEnd = 0;
    size_t nextPosition = 0;
    bool last = false;
};

// Parses the line starting at "position" within [position, limit).
inline bool ReadLine(const std::string& text, size_t position, size_t limit, IniLine& line) {
    if (position >= limit) return false;
    line = IniLine{};
    line.lineStart = position;
    size_t index = position;
    while (index < limit && text[index] != '\n') ++index;
    if (index >= limit) {
        line.contentEnd = limit;
        line.nextPosition = limit;
        line.last = true;
        return true;
    }
    size_t contentEnd = index;
    if (contentEnd > position && text[contentEnd - 1] == '\r') --contentEnd;
    line.contentEnd = contentEnd;
    line.nextPosition = index + 1;
    line.last = line.nextPosition >= limit;
    return true;
}

// Recognizes "[ \t]*[ name ][ \t]*" and returns the trimmed section name.
inline bool ParseSectionHeader(const std::string& text, const IniLine& line, std::string& name) {
    size_t index = line.lineStart;
    while (index < line.contentEnd && (text[index] == ' ' || text[index] == '\t')) ++index;
    if (index >= line.contentEnd || text[index] != '[') return false;
    ++index;
    size_t nameBegin = index;
    while (index < line.contentEnd && text[index] != ']') ++index;
    if (index >= line.contentEnd) return false;
    size_t nameEnd = index;
    ++index;
    while (index < line.contentEnd && (text[index] == ' ' || text[index] == '\t')) ++index;
    if (index != line.contentEnd) return false;
    while (nameBegin < nameEnd && (text[nameBegin] == ' ' || text[nameBegin] == '\t')) ++nameBegin;
    while (nameEnd > nameBegin && (text[nameEnd - 1] == ' ' || text[nameEnd - 1] == '\t')) --nameEnd;
    name.assign(text, nameBegin, nameEnd - nameBegin);
    return true;
}

struct SectionRange {
    bool found = false;
    size_t bodyBegin = 0;
    size_t bodyEnd = 0;
};

inline bool FindSection(const std::string& text, const char* section, SectionRange& range, std::string& error) {
    range = SectionRange{};
    std::vector<size_t> headerStarts;
    std::vector<size_t> bodyStarts;
    std::vector<std::string> names;
    size_t position = 0;
    while (position < text.size()) {
        IniLine line;
        if (!ReadLine(text, position, text.size(), line)) break;
        std::string name;
        if (ParseSectionHeader(text, line, name)) {
            headerStarts.push_back(line.lineStart);
            bodyStarts.push_back(line.last ? text.size() : line.nextPosition);
            names.push_back(std::move(name));
        }
        if (line.last) break;
        position = line.nextPosition;
    }
    size_t match = names.size();
    size_t matchCount = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        if (EqualsAsciiNoCase(names[i], section)) {
            match = i;
            ++matchCount;
        }
    }
    const std::wstring sectionWide = Widen(section);
    if (matchCount == 0) {
        error = "section not found";
        (void)sectionWide;
        return false;
    }
    if (matchCount > 1) {
        error = "section duplicated";
        return false;
    }
    range.found = true;
    range.bodyBegin = bodyStarts[match];
    range.bodyEnd = (match + 1 < headerStarts.size()) ? headerStarts[match + 1] : text.size();
    if (range.bodyEnd < range.bodyBegin) range.bodyEnd = text.size();
    return true;
}

// Matches "[ \t]*key[ \t]*=[ \t]*<value>" (case-insensitive key) inside one line and
// returns the byte span of the value (trailing blanks included, CR excluded).
inline bool MatchKeyLine(const std::string& text, const IniLine& line, const char* key, size_t& valueBegin,
                         size_t& valueEnd) {
    size_t index = line.lineStart;
    while (index < line.contentEnd && (text[index] == ' ' || text[index] == '\t')) ++index;
    const size_t keyLength = std::strlen(key);
    if (index + keyLength > line.contentEnd) return false;
    for (size_t i = 0; i < keyLength; ++i) {
        if (AsciiLower(text[index + i]) != AsciiLower(key[i])) return false;
    }
    index += keyLength;
    while (index < line.contentEnd && (text[index] == ' ' || text[index] == '\t')) ++index;
    if (index >= line.contentEnd || text[index] != '=') return false;
    ++index;
    while (index < line.contentEnd && (text[index] == ' ' || text[index] == '\t')) ++index;
    valueBegin = index;
    valueEnd = line.contentEnd;
    return true;
}

struct ValueSpan {
    size_t begin = 0;
    size_t end = 0;
};

// Collects every matching key line inside the section body.
inline bool CollectKeySpans(const std::string& text, const SectionRange& range, const char* key,
                            std::vector<ValueSpan>& spans, std::string& error) {
    spans.clear();
    size_t position = range.bodyBegin;
    while (position < range.bodyEnd) {
        IniLine line;
        if (!ReadLine(text, position, range.bodyEnd, line)) break;
        size_t valueBegin = 0;
        size_t valueEnd = 0;
        if (MatchKeyLine(text, line, key, valueBegin, valueEnd)) spans.push_back({valueBegin, valueEnd});
        if (line.last) break;
        position = line.nextPosition;
    }
    if (spans.size() > 1) {
        error = "key duplicated";
        return false;
    }
    return true;
}

inline bool GetIniValue(const std::string& text, const SectionRange& range, const char* key, std::string& value,
                        bool& present, std::string& error) {
    present = false;
    value.clear();
    std::vector<ValueSpan> spans;
    if (!CollectKeySpans(text, range, key, spans, error)) return false;
    if (spans.empty()) return true;
    present = true;
    value = TrimBlanks(text.substr(spans[0].begin, spans[0].end - spans[0].begin));
    return true;
}

// Replaces the value of an existing key, or appends "Key=Value" at the end of the
// section body using the file's own dominant line ending.
inline bool SetIniValue(std::string& text, const char* section, const char* key, const std::string& value,
                        std::string& error) {
    SectionRange range;
    if (!FindSection(text, section, range, error)) return false;
    std::vector<ValueSpan> spans;
    if (!CollectKeySpans(text, range, key, spans, error)) return false;
    if (spans.size() == 1) {
        text.replace(spans[0].begin, spans[0].end - spans[0].begin, value);
        return true;
    }
    const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string addition;
    if (range.bodyEnd > 0 && text[range.bodyEnd - 1] != '\n') addition += newline;
    addition += key;
    addition += '=';
    addition += value;
    addition += newline;
    text.insert(range.bodyEnd, addition);
    return true;
}

inline bool FileExistsNoReparse(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool CreateDirectoryTree(const std::wstring& path, DWORD& win32Error) {
    win32Error = ERROR_SUCCESS;
    std::wstring full;
    if (path.empty() || !CanonicalLocalPath(path, full)) {
        win32Error = ERROR_INVALID_NAME;
        return false;
    }
    std::wstring current = full.substr(0, 3);
    size_t at = 3;
    for (;;) {
        const size_t end = full.find(L'\\', at);
        const std::wstring part = full.substr(at, end == std::wstring::npos ? end : end - at);
        if (!part.empty()) {
            if (current.back() != L'\\') current += L'\\';
            current += part;
            if (!CreateDirectoryW(current.c_str(), nullptr)) {
                const DWORD createError = GetLastError();
                if (createError != ERROR_ALREADY_EXISTS) {
                    win32Error = createError;
                    return false;
                }
            }
        }
        if (end == std::wstring::npos) break;
        at = end + 1;
    }
    return true;
}

inline bool GameProcessRunning() {
    const wchar_t* const names[] = {L"wow.exe", L"wowclassic.exe", L"patchmenu.exe",
                                    L"AIO DLSS5 32-bit Wrapper.exe"};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            for (const wchar_t* name : names) {
                if (_wcsicmp(entry.szExeFile, name) == 0) {
                    running = true;
                    break;
                }
            }
            if (running) break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

}  // namespace ds5_detail

inline const wchar_t* Ds5WriteErrorText(Ds5WriteError error) {
    switch (error) {
        case Ds5WriteError::None: return L"";
        case Ds5WriteError::InvalidModel: return L"模型必须为 1、2 或 3。";
        case Ds5WriteError::InvalidIntensity: return L"强度 必须在 0 到 1 之间。";
        case Ds5WriteError::InvalidTone: return L"色调结构 必须在 0 到 2 之间。";
        case Ds5WriteError::InvalidStructure: return L"结构强度 必须在 0 到 1 之间。";
        case Ds5WriteError::InvalidSkin: return L"皮肤结构必须在 -1 到 1 之间。";
        case Ds5WriteError::MissingFile: return L"缺少 host64\\ReShade.ini。";
        case Ds5WriteError::GameRunning: return L"请先退出游戏、启动器和 AIO 载体。";
        case Ds5WriteError::Read: return L"读取 host64\\ReShade.ini 失败。";
        case Ds5WriteError::Backup: return L"创建配置备份失败。";
        case Ds5WriteError::Write: return L"写入 host64\\ReShade.ini 失败。";
        case Ds5WriteError::Verify: return L"配置回读不一致；备份已保留。";
    }
    return L"";
}

// Reads [Standalone.DLSSNR] from <clientRoot>\host64\ReShade.ini. A missing file is
// reported through exists=false without an error string.
inline Ds5AdvancedSettings ReadDs5AdvancedSettings(const std::wstring& clientRoot) {
    using namespace ds5_detail;
    Ds5AdvancedSettings settings;
    settings.path = ReShadeIniPath(clientRoot);
    if (clientRoot.empty() || settings.path.empty()) {
        settings.error = "client path empty";
        return settings;
    }
    GuardedPath ini;
    if (!ini.Open(clientRoot, L"host64\\ReShade.ini")) {
        settings.win32Error = ini.Error();
        if (settings.win32Error != ERROR_FILE_NOT_FOUND && settings.win32Error != ERROR_PATH_NOT_FOUND) {
            settings.error = "ReShade.ini unreadable";
        }
        return settings;
    }
    std::string bytes;
    DWORD readError = ERROR_SUCCESS;
    if (!ReadJournalBytes(ini, bytes, readError)) {
        settings.win32Error = readError;
        settings.error = "ReShade.ini unreadable";
        return settings;
    }
    settings.exists = true;
    size_t bodyStart = 0;
    settings.bom = HasUtf8Bom(bytes, bodyStart);
    const std::string body = bytes.substr(bodyStart);
    if (!IsValidUtf8(body)) {
        settings.error = "ReShade.ini is not valid UTF-8";
        return settings;
    }
    SectionRange range;
    std::string sectionError;
    if (!FindSection(body, kDs5SectionName, range, sectionError)) {
        settings.error = sectionError;
        return settings;
    }
    std::string error;
    std::string raw;
    bool present = false;
    if (!GetIniValue(body, range, "Model", raw, present, error)) {
        settings.error = error;
        return settings;
    }
    if (present && !raw.empty()) {
        int value = 0;
        if (ParseInteger(raw, value) && value >= 1 && value <= 3) settings.model = value;
    }
    if (!GetIniValue(body, range, "Intensity", raw, present, error)) {
        settings.error = error;
        return settings;
    }
    if (present && !raw.empty()) {
        double value = 0.0;
        if (ParseNumber(raw, value)) settings.intensity = value;
    }
    if (!GetIniValue(body, range, "LocalTone", raw, present, error)) {
        settings.error = error;
        return settings;
    }
    if (present && !raw.empty()) {
        double value = 0.0;
        if (ParseNumber(raw, value)) settings.localTone = value;
    }
    if (!GetIniValue(body, range, "LocalStructure", raw, present, error)) {
        settings.error = error;
        return settings;
    }
    if (present && !raw.empty()) {
        double value = 0.0;
        if (ParseNumber(raw, value)) settings.localStructure = value;
    }
    if (!GetIniValue(body, range, "SkinStructure", raw, present, error)) {
        settings.error = error;
        return settings;
    }
    if (present && !raw.empty()) {
        double value = 0.0;
        if (ParseNumber(raw, value)) settings.skinStructure = value;
    }
    if (!GetIniValue(body, range, "NeuralRendering", raw, present, error)) {
        settings.error = error;
        return settings;
    }
    settings.neuralRenderingPresent = present;
    if (present) settings.neuralRendering = TrimBlanks(raw) == "1";
    if (!GetIniValue(body, range, "DetachedPresentation", raw, present, error)) {
        settings.error = error;
        return settings;
    }
    settings.detachedPresent = present;
    if (present) settings.detachedPresentation = TrimBlanks(raw) != "0";
    if (!GetIniValue(body, range, "NvidiaOpticalFlowMotion", raw, present, error)) {
        settings.error = error;
        return settings;
    }
    settings.opticalPresent = present;
    if (present) settings.opticalFlow = TrimBlanks(raw) != "0";
    settings.readable = true;
    return settings;
}

struct Ds5TierEntry {
    const wchar_t* name;
    bool neuralRendering;
    double intensity;
    double localTone;
    double localStructure;
};

inline const std::array<Ds5TierEntry, 4>& Ds5Tiers() {
    static const std::array<Ds5TierEntry, 4> tiers = {{
        {L"原画", false, 1.0, 1.0, 1.0},
        {L"轻量", true, 0.20, 1.0, 0.10},
        {L"均衡", true, 0.35, 1.0, 0.25},
        {L"浓郁", true, 0.60, 1.30, 0.50},
    }};
    return tiers;
}

inline std::wstring Ds5ModelName(int model) {
    switch (model) {
        case 2: return L"自然";
        case 3: return L"电影";
        default: return L"默认";
    }
}

// Mirrors Get-TierInfo: the tier table is scanned in order and a tier matches when
// the NeuralRendering flag and all three numeric values are within 0.02.
inline std::wstring DetectDs5Tier(const Ds5AdvancedSettings& settings) {
    if (!settings.readable) return L"自定义";
    for (const auto& tier : Ds5Tiers()) {
        if (settings.neuralRendering != tier.neuralRendering) continue;
        if (std::fabs(settings.intensity - tier.intensity) > 0.02) continue;
        if (std::fabs(settings.localTone - tier.localTone) > 0.02) continue;
        if (std::fabs(settings.localStructure - tier.localStructure) > 0.02) continue;
        return tier.name;
    }
    return L"自定义";
}

inline Ds5Values Ds5ValuesFrom(const Ds5AdvancedSettings& settings) {
    Ds5Values values;
    values.model = settings.model;
    values.intensity = settings.intensity;
    values.localTone = settings.localTone;
    values.localStructure = settings.localStructure;
    values.skinStructure = settings.skinStructure;
    values.neuralRendering = settings.neuralRenderingPresent ? settings.neuralRendering : true;
    return values;
}

// Writes the DLSS-NR parameters in the historical order:
//   NeuralRendering, Model, Intensity, LocalTone, LocalStructure, SkinStructure,
//   optional DetachedPresentation (plus WindowedInputScaling / WindowedVirtualization
//   when true), optional NvidiaOpticalFlowMotion.
// On success backupPath is the verified copy of the previous file.
inline Ds5WriteResult SaveDs5AdvancedSettings(const std::wstring& clientRoot, const std::wstring& operationsRoot,
                                              const Ds5Values& values, std::optional<bool> detachedPresentation,
                                              std::optional<bool> opticalFlow) {
    using namespace ds5_detail;
    Ds5WriteResult result;
    const auto fail = [&result](Ds5WriteError error, DWORD win32Error) {
        result.error = error;
        result.win32Error = win32Error;
        return result;
    };
    if (values.model < 1 || values.model > 3) return fail(Ds5WriteError::InvalidModel, 0);
    if (!std::isfinite(values.intensity) || values.intensity < 0.0 || values.intensity > 1.0) {
        return fail(Ds5WriteError::InvalidIntensity, 0);
    }
    if (!std::isfinite(values.localTone) || values.localTone < 0.0 || values.localTone > 2.0) {
        return fail(Ds5WriteError::InvalidTone, 0);
    }
    if (!std::isfinite(values.localStructure) || values.localStructure < 0.0 || values.localStructure > 1.0) {
        return fail(Ds5WriteError::InvalidStructure, 0);
    }
    if (!std::isfinite(values.skinStructure) || values.skinStructure < -1.0 || values.skinStructure > 1.0) {
        return fail(Ds5WriteError::InvalidSkin, 0);
    }
    if (clientRoot.empty() || operationsRoot.empty()) return fail(Ds5WriteError::MissingFile, 0);
    if (GameProcessRunning()) return fail(Ds5WriteError::GameRunning, 0);
    const std::wstring iniPath = ReShadeIniPath(clientRoot);
    if (!FileExistsNoReparse(iniPath)) return fail(Ds5WriteError::MissingFile, 0);

    std::string original;
    FileIdentity originalIdentity{};
    std::wstring fullIniPath;
    {
        GuardedPath ini;
        if (!ini.Open(clientRoot, L"host64\\ReShade.ini")) return fail(Ds5WriteError::Read, ini.Error());
        fullIniPath = ini.Path();
        const HashResult hash = HashFile(fullIniPath);
        if (!hash) return fail(Ds5WriteError::Read, hash.win32Error);
        originalIdentity = hash.identity;
        DWORD readError = ERROR_SUCCESS;
        if (!ReadJournalBytes(ini, original, readError)) return fail(Ds5WriteError::Read, readError);
    }

    size_t bodyStart = 0;
    const bool bom = HasUtf8Bom(original, bodyStart);
    const std::string body = original.substr(bodyStart);
    if (!IsValidUtf8(body)) {
        result.detail = L"host64\\ReShade.ini 不是有效的 UTF-8 文本。";
        return fail(Ds5WriteError::Read, ERROR_NO_UNICODE_TRANSLATION);
    }

    std::string updated = body;
    std::string editError;
    const auto setKey = [&updated, &editError](const char* key, const std::string& value) {
        return SetIniValue(updated, kDs5SectionName, key, value, editError);
    };
    if (!setKey("NeuralRendering", values.neuralRendering ? "1" : "0")) return fail(Ds5WriteError::Read, 0);
    if (!setKey("Model", std::to_string(values.model))) return fail(Ds5WriteError::Read, 0);
    if (!setKey("Intensity", FormatNumber(values.intensity))) return fail(Ds5WriteError::Read, 0);
    if (!setKey("LocalTone", FormatNumber(values.localTone))) return fail(Ds5WriteError::Read, 0);
    if (!setKey("LocalStructure", FormatNumber(values.localStructure))) return fail(Ds5WriteError::Read, 0);
    if (!setKey("SkinStructure", FormatNumber(values.skinStructure))) return fail(Ds5WriteError::Read, 0);
    if (detachedPresentation.has_value()) {
        if (!setKey("DetachedPresentation", *detachedPresentation ? "1" : "0")) {
            return fail(Ds5WriteError::Read, 0);
        }
        if (*detachedPresentation) {
            if (!setKey("WindowedInputScaling", "1") || !setKey("WindowedVirtualization", "1")) {
                return fail(Ds5WriteError::Read, 0);
            }
        }
    }
    if (opticalFlow.has_value()) {
        if (!setKey("NvidiaOpticalFlowMotion", *opticalFlow ? "1" : "0")) return fail(Ds5WriteError::Read, 0);
    }

    std::string finalBytes;
    finalBytes.reserve(updated.size() + 3);
    if (bom) finalBytes.append("\xEF\xBB\xBF", 3);
    finalBytes += updated;
    if (finalBytes == original) {
        result.changed = false;
        return result;
    }

    // Verified backup under the operations root before touching the client file.
    const std::string token = UniqueToken();
    DWORD directoryError = ERROR_SUCCESS;
    if (!CreateDirectoryTree(operationsRoot, directoryError)) {
        result.detail = L"无法创建事务目录。";
        return fail(Ds5WriteError::Backup, directoryError);
    }
    const std::wstring backupMember = L"config-" + Widen(token) + L"\\before.ini";
    std::wstring backupPath;
    {
        GuardedPath backup;
        if (!backup.Open(operationsRoot, backupMember, true)) return fail(Ds5WriteError::Backup, backup.Error());
        const WriteNewResult written = WriteNewVerified(backup, original);
        if (!written) return fail(Ds5WriteError::Backup, written.win32Error);
        backupPath = backup.Path();
    }
    result.backupPath = backupPath;

    // Unique temporary file next to the target, then an atomic replace.
    const std::wstring temporaryMember = L"host64\\ReShade.ini.dlss5-settings-" + Widen(token);
    std::wstring temporaryPath;
    {
        GuardedPath temporary;
        if (!temporary.Open(clientRoot, temporaryMember)) return fail(Ds5WriteError::Write, temporary.Error());
        const WriteNewResult written = WriteNewVerified(temporary, finalBytes);
        if (!written) {
            DeleteFileW(temporary.Path().c_str());
            return fail(Ds5WriteError::Write, written.win32Error);
        }
        temporaryPath = temporary.Path();
    }

    if (GameProcessRunning()) {
        DeleteFileW(temporaryPath.c_str());
        return fail(Ds5WriteError::GameRunning, 0);
    }
    const HashResult current = HashFile(fullIniPath);
    if (!current) {
        DeleteFileW(temporaryPath.c_str());
        return fail(Ds5WriteError::Read, current.win32Error);
    }
    if (!SameFileIdentity(current.identity, originalIdentity)) {
        DeleteFileW(temporaryPath.c_str());
        result.detail = L"配置在保存期间发生变化，已保留现状。";
        return fail(Ds5WriteError::Write, ERROR_INVALID_DATA);
    }
    if (!MoveFileExW(temporaryPath.c_str(), fullIniPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        CopyFileW(backupPath.c_str(), fullIniPath.c_str(), FALSE);
        return fail(Ds5WriteError::Write, moveError);
    }

    std::string readBack;
    bool verified = false;
    {
        GuardedPath verify;
        DWORD verifyError = ERROR_SUCCESS;
        if (verify.Open(clientRoot, L"host64\\ReShade.ini") && ReadJournalBytes(verify, readBack, verifyError)) {
            verified = readBack == finalBytes;
        }
    }
    if (!verified) {
        CopyFileW(backupPath.c_str(), fullIniPath.c_str(), FALSE);
        return fail(Ds5WriteError::Verify, 0);
    }
    result.changed = true;
    return result;
}

}  // namespace control_center
