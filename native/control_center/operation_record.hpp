#pragma once

#include <string>
#include <vector>
#include "client_identity.hpp"
#include "client_record.hpp"

namespace control_center {
struct OperationRecord {
    std::string error, status;
    unsigned schema = 0;
    std::wstring client;
    std::vector<InstalledEntry> entries;
    std::vector<std::wstring> applied;
    Json original;
    explicit operator bool() const { return error.empty(); }
};

// Structural parsing only. The stored client and members are untrusted; the
// consumer must independently validate the selected client, trusted catalog,
// backups and actual on-disk identities before a recovery or repair.
inline OperationRecord ReadOperationRecord(const std::string& text) {
    OperationRecord result;
    auto fail = [&result](const char* message) {
        result.error = message; result.entries.clear(); result.applied.clear(); result.original = nullptr; return result;
    };
    auto parsed = ParseJsonObject(text);
    if (!parsed) return fail("invalid JSON");
    const auto schema = parsed.value.find("schema");
    if (schema == parsed.value.end() || !schema->is_number_integer() || (*schema != 1 && *schema != 2)) return fail("unsupported schema");
    result.schema = schema->get<unsigned>();
    if (!StringField(parsed.value, "status", result.status) ||
        (result.status != "preparing" && result.status != "in_progress" && result.status != "completed" &&
         result.status != "rolled_back" && result.status != "rollback_incomplete")) return fail("unsupported status");
    std::string clientUtf8;
    std::wstring clientInput;
    if (!StringField(parsed.value, "client", clientUtf8) || !Utf8Wide(clientUtf8, clientInput) ||
        !CanonicalLocalPath(clientInput, result.client)) return fail("invalid client path");
    const auto items = parsed.value.find(result.schema == 1 ? "members" : "entries");
    if (items == parsed.value.end() || !items->is_array() || items->empty()) return fail("missing members");
    const auto other = parsed.value.find(result.schema == 1 ? "entries" : "members");
    if (other != parsed.value.end() && !other->is_null() && (!other->is_array() || !other->empty()))
        return fail("ambiguous member lists");
    Json normalized = Json::array();
    for (const auto& item : *items) {
        if (!item.is_object()) return fail("invalid member");
        Json entry = item;
        if (result.schema == 1) {
            bool replaces = false;
            if (!BoolField(item, "replaces_original", replaces)) return fail("invalid replacement");
            entry["pre_state"] = replaces ? "present" : "absent";
            entry["previous_sha256"] = nullptr;
            entry["backup"] = replaces ? Json("d3d9.dll") : Json(nullptr);
        }
        normalized.push_back(std::move(entry));
    }
    Json wrapper = {{"schema", 1}, {"status", "installed"}, {"entries", normalized}};
    const auto clientRecord = ReadClientRecord(wrapper.dump());
    if (!clientRecord) return fail("invalid members");
    result.entries = clientRecord.entries;
    const auto applied = parsed.value.find("applied");
    if (applied != parsed.value.end() && !applied->is_null()) {
        if (!applied->is_array()) return fail("invalid applied list");
        for (const auto& value : *applied) {
            if (!value.is_string()) return fail("invalid applied member");
            std::wstring target;
            if (!Utf8Wide(value.get<std::string>(), target) || !ValidMemberPath(target)) return fail("invalid applied path");
            bool exists = false;
            for (const auto& entry : result.entries) if (SameTarget(entry.target, target)) { exists = true; break; }
            if (!exists) return fail("unknown applied member");
            for (const auto& previous : result.applied) if (SameTarget(previous, target)) return fail("duplicate applied member");
            result.applied.push_back(std::move(target));
        }
    }
    result.original = std::move(parsed.value);
    return result;
}
}
