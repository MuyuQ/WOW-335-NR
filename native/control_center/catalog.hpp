#pragma once

#include <windows.h>
#include <algorithm>
#include <climits>
#include <string>
#include <vector>
#include "member_path.hpp"
#include "strict_json.hpp"

namespace control_center {
struct CatalogMember {
    std::wstring target, source;
    std::string sourceKind, sha256;
    bool replacesOriginal = false, required = true;
};
struct CatalogResult {
    std::string error;
    std::vector<CatalogMember> members;
    explicit operator bool() const { return error.empty(); }
};
inline bool Utf8Wide(const std::string& input, std::wstring& output) {
    if (input.empty() || input.size() > static_cast<size_t>(INT_MAX)) return false;
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (!size) return false;
    output.resize(static_cast<size_t>(size));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), &output[0], size) == size;
}
inline bool SameTarget(std::wstring a, std::wstring b) {
    std::replace(a.begin(), a.end(), L'/', L'\\');
    std::replace(b.begin(), b.end(), L'/', L'\\');
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
inline bool StringField(const Json& obj, const char* key, std::string& result) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return false;
    result = it->get<std::string>();
    return !result.empty();
}
inline bool BoolField(const Json& obj, const char* key, bool& result, bool optional = false) {
    const auto it = obj.find(key);
    if (it == obj.end()) return optional;
    if (!it->is_boolean()) return false;
    result = it->get<bool>();
    return true;
}

// Validate both authoritative lists before they can be used to select paths.
// This does not check the contents of source or destination files.
inline CatalogResult ReadCatalog(const std::string& manifestText, const std::string& lockText) {
    CatalogResult result;
    const auto manifest = ParseJsonObject(manifestText);
    const auto lock = ParseJsonObject(lockText);
    auto fail = [&result](const char* message) { result.members.clear(); result.error = message; return result; };
    if (!manifest || !lock) return fail("invalid JSON");
    const auto schemaOk = [](const Json& j) {
        const auto it = j.find("schema");
        return it != j.end() && it->is_number_integer() && *it == 1;
    };
    if (!schemaOk(manifest.value) || !schemaOk(lock.value)) return fail("unsupported schema");
    const auto entries = manifest.value.find("entries");
    const auto members = lock.value.find("members");
    if (entries == manifest.value.end() || !entries->is_array() || entries->empty() ||
        members == lock.value.end() || !members->is_array() || members->empty()) return fail("missing entries");
    unsigned replacements = 0;
    for (const auto& member : *members) {
        if (!member.is_object()) return fail("invalid member");
        CatalogMember item;
        std::string target, source;
        if (!StringField(member, "target", target) || !StringField(member, "source", source) ||
            !StringField(member, "source_kind", item.sourceKind) || !StringField(member, "sha256", item.sha256) ||
            !BoolField(member, "replaces_original", item.replacesOriginal) ||
            !Utf8Wide(target, item.target) || !Utf8Wide(source, item.source) ||
            !ValidMemberPath(item.target) || !ValidMemberPath(item.source)) return fail("invalid member fields");
        if (item.sourceKind != "stage" && item.sourceKind != "template" && item.sourceKind != "runtime") return fail("invalid source kind");
        if (item.sha256.size() != 64 || !std::all_of(item.sha256.begin(), item.sha256.end(),
            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return fail("invalid hash");
        for (const auto& previous : result.members) if (SameTarget(previous.target, item.target)) return fail("duplicate member target");
        if (item.replacesOriginal && (++replacements > 1 || !SameTarget(item.target, L"d3d9.dll"))) return fail("invalid replacement");
        result.members.push_back(std::move(item));
    }
    std::vector<bool> seen(result.members.size(), false);
    for (const auto& entry : *entries) {
        if (!entry.is_object()) return fail("invalid entry");
        std::string target, kind, folder, purpose;
        std::wstring targetWide;
        bool replaces = false, required = true;
        if (!StringField(entry, "target", target) || !StringField(entry, "source_kind", kind) ||
            !StringField(entry, "folder", folder) || !StringField(entry, "purpose", purpose) ||
            !BoolField(entry, "replaces_original", replaces) || !BoolField(entry, "required", required, true) ||
            !Utf8Wide(target, targetWide) || !ValidMemberPath(targetWide)) return fail("invalid entry fields");
        size_t match = result.members.size();
        for (size_t i = 0; i < result.members.size(); ++i) if (SameTarget(result.members[i].target, targetWide)) { match = i; break; }
        if (match == result.members.size() || seen[match] || result.members[match].sourceKind != kind ||
            result.members[match].replacesOriginal != replaces) return fail("manifest and lock mismatch");
        seen[match] = true;
        result.members[match].required = required;
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end()) return fail("manifest missing member");
    return result;
}
}
