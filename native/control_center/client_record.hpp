#pragma once

#include <string>
#include <vector>
#include "catalog.hpp"

namespace control_center {
struct InstalledEntry {
    std::wstring target, backup;
    std::string sha256, previousSha256;
    bool previouslyPresent = false, replacesOriginal = false;
};
struct ResidueEntry {
    std::wstring path;
    std::string kind, policy;
};
struct ClientRecord {
    std::string error;
    Json original;
    std::vector<InstalledEntry> entries;
    std::vector<ResidueEntry> residue;
    explicit operator bool() const { return error.empty(); }
};
inline bool ValidSha256(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Parses an existing client record, but never authorizes mutation from its
// contents alone. The consumer must compare targets and residue against trusted
// release metadata and verify every referenced file before acting.
inline ClientRecord ReadClientRecord(const std::string& text) {
    ClientRecord result;
    auto fail = [&result](const char* message) {
        result.error = message; result.entries.clear(); result.residue.clear(); result.original = nullptr; return result;
    };
    auto parsed = ParseJsonObject(text);
    if (!parsed) return fail("invalid JSON");
    const auto schema = parsed.value.find("schema");
    const auto status = parsed.value.find("status");
    const auto entries = parsed.value.find("entries");
    if (schema == parsed.value.end() || !schema->is_number_integer() || *schema != 1 ||
        status == parsed.value.end() || !status->is_string() || *status != "installed" ||
        entries == parsed.value.end() || !entries->is_array() || entries->empty()) return fail("unsupported client record");
    for (const auto& entry : *entries) {
        if (!entry.is_object()) return fail("invalid entry");
        InstalledEntry item;
        std::string target, state;
        if (!StringField(entry, "target", target) || !Utf8Wide(target, item.target) || !ValidMemberPath(item.target) ||
            !StringField(entry, "sha256", item.sha256) || !ValidSha256(item.sha256) ||
            !StringField(entry, "pre_state", state) || !BoolField(entry, "replaces_original", item.replacesOriginal))
            return fail("invalid entry fields");
        if (state == "present") item.previouslyPresent = true;
        else if (state != "absent") return fail("invalid pre-state");
        for (const auto& previous : result.entries) if (SameTarget(previous.target, item.target)) return fail("duplicate target");
        const auto backup = entry.find("backup");
        const auto previous = entry.find("previous_sha256");
        if (item.previouslyPresent) {
            if (backup == entry.end() || !backup->is_string() || !Utf8Wide(backup->get<std::string>(), item.backup) ||
                !ValidMemberPath(item.backup)) return fail("invalid backup");
            if (previous != entry.end() && !previous->is_null()) {
                if (!previous->is_string()) return fail("invalid previous hash");
                item.previousSha256 = previous->get<std::string>();
                if (!ValidSha256(item.previousSha256)) return fail("invalid previous hash");
            }
        } else if ((backup != entry.end() && !backup->is_null()) ||
                   (previous != entry.end() && !previous->is_null())) return fail("unexpected backup");
        result.entries.push_back(std::move(item));
    }
    const auto residue = parsed.value.find("residue");
    if (residue != parsed.value.end() && !residue->is_null()) {
        if (!residue->is_array()) return fail("invalid residue");
        for (const auto& entry : *residue) {
            if (!entry.is_object()) return fail("invalid residue entry");
            ResidueEntry item;
            std::string path;
            if (!StringField(entry, "path", path) || !StringField(entry, "kind", item.kind) ||
                !StringField(entry, "policy", item.policy) || !Utf8Wide(path, item.path)) return fail("invalid residue fields");
            if ((item.kind != "file" && item.kind != "tree") || (item.policy != "delete" && item.policy != "report"))
                return fail("unsupported residue policy");
            if (item.policy == "delete" && !ValidMemberPath(item.path)) return fail("unsafe residue path");
            result.residue.push_back(std::move(item));
        }
    }
    result.original = std::move(parsed.value);
    return result;
}
}
