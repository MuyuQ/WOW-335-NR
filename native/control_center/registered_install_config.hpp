#pragma once

#include <string>
#include <vector>
#include "capture_client_record.hpp"
#include "catalog.hpp"
#include "client_record.hpp"
#include "guarded_path.hpp"
#include "install_preview.hpp"

namespace control_center {
struct RegisteredInstallConfig {
    // trust is only granted for a well-formed record that belongs to this
    // client directory. An absent record is a normal first install, not an
    // error, but it never grants preservation.
    bool trusted = false;
    std::string error;
    ClientRecord record;
    RegisteredConfigPolicy policy;
};

// The record stores the client path it was published for. Compare canonical
// paths so a moved or renamed copy of a client cannot inherit another
// client's preservation decisions.
inline bool ClientRecordOwnedBy(const ClientRecord& record, const std::wstring& clientRoot) {
    if (!record) return false;
    const auto it = record.original.find("client");
    if (it == record.original.end() || !it->is_string()) return false;
    std::wstring recorded;
    if (!Utf8Wide(it->get<std::string>(), recorded)) return false;
    std::wstring recordedCanonical, clientCanonical;
    if (!CanonicalLocalPath(recorded, recordedCanonical) || !CanonicalLocalPath(clientRoot, clientCanonical)) return false;
    return CompareStringOrdinal(recordedCanonical.c_str(), -1, clientCanonical.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Collect the registered configuration targets from a trusted record. An entry
// only qualifies when the active record registered exactly the current
// release template hash for that target.
inline std::vector<std::wstring> PreservedTargetsFromRecord(const ClientRecord& record, const CatalogResult& catalog) {
    std::vector<std::wstring> targets;
    if (!record) return targets;
    for (const auto& member : catalog.members) {
        if (!IsPreservedConfigTarget(member.target)) continue;
        for (const auto& entry : record.entries) {
            if (SameTarget(entry.target, member.target) && entry.sha256 == member.sha256) {
                targets.push_back(member.target);
                break;
            }
        }
    }
    return targets;
}

// Load the active client record for an install preview. The returned policy is
// only valid for the catalog that was passed in: a later catalog change makes
// the registered source hash stale and preservation stops being offered.
inline RegisteredInstallConfig LoadRegisteredInstallConfig(const std::wstring& clientRoot, const CatalogResult& catalog) {
    RegisteredInstallConfig result;
    if (!catalog || catalog.members.empty() || clientRoot.empty()) {
        result.error = "catalog or client unavailable";
        return result;
    }
    const auto captured = CaptureClientRecord(clientRoot);
    if (captured.state == CapturedRecordState::Absent) return result;
    if (captured.state != CapturedRecordState::Present) {
        result.error = "client record unavailable";
        return result;
    }
    if (!ClientRecordOwnedBy(captured.record, clientRoot)) {
        result.error = "client record belongs to another client";
        return result;
    }
    result.record = captured.record;
    result.trusted = true;
    result.policy.trusted = true;
    for (const auto& member : catalog.members) {
        if (!IsPreservedConfigTarget(member.target)) continue;
        for (const auto& entry : captured.record.entries) {
            if (SameTarget(entry.target, member.target) && entry.sha256 == member.sha256) {
                result.policy.entries.push_back({member.target, member.sha256});
                break;
            }
        }
    }
    return result;
}

inline bool SameRegisteredConfig(const RegisteredConfigPolicy& a, const RegisteredConfigPolicy& b) {
    if (a.trusted != b.trusted || a.entries.size() != b.entries.size()) return false;
    for (size_t i = 0; i < a.entries.size(); ++i)
        if (!SameTarget(a.entries[i].target, b.entries[i].target) || a.entries[i].sha256 != b.entries[i].sha256) return false;
    return true;
}
}  // namespace control_center
