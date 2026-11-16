#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace control_center {

inline bool ValidPathComponent(const std::wstring& part) {
    if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
    std::wstring upper = part;
    std::transform(upper.begin(), upper.end(), upper.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    const auto dot = upper.find(L'.');
    const auto stem = upper.substr(0, dot);
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        stem == L"CONIN$" || stem == L"CONOUT$" ||
        (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
         ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' ||
          stem[3] == L'\u00b2' || stem[3] == L'\u00b3'))) return false;
    for (const wchar_t c : part) {
        if (c < 32 || c == L':' || c == L'?' || c == L'*' || c == L'"' ||
            c == L'<' || c == L'>' || c == L'|') return false;
    }
    return true;
}

// Validate an untrusted manifest member before joining it to a root. This is
// lexical validation only: callers must additionally check each filesystem
// component for reparse points and verify the opened object's identity.
inline bool ValidMemberPath(const std::wstring& path) {
    if (path.empty() || path.front() == L'/' || path.front() == L'\\' ||
        path.back() == L'/' || path.back() == L'\\') return false;
    std::vector<std::wstring> parts;
    std::wstring part;
    for (size_t i = 0; i <= path.size(); ++i) {
        const wchar_t c = i == path.size() ? L'/' : path[i];
        if (c == L'/' || c == L'\\') {
            if (!ValidPathComponent(part)) return false;
            std::wstring upper = part;
            std::transform(upper.begin(), upper.end(), upper.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
            parts.push_back(upper);
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    const auto& first = parts.front();
    return first != L"WTF" && first != L"CACHE" && first != L"LOGS" &&
           first != L"SCREENSHOTS" && first != L"DATA" && first != L"INTERFACE" && first != L"FONTS";
}
}
