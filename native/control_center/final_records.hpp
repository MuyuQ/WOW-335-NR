#pragma once

#include "install_member.hpp"
#include "client_record.hpp"
#include "operation_record.hpp"

namespace control_center {
struct FinalRecordsResult {
    std::string error, clientRecord, operationRecord;
    explicit operator bool() const { return error.empty() && !clientRecord.empty() && !operationRecord.empty(); }
};

inline bool ValidUtcTimestamp(const std::string& value) {
    if (value.size() != 20 && (value.size() < 22 || value.size() > 28)) return false;
    if (value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':' || value.back() != 'Z') return false;
    if (value.size() > 20 && value[19] != '.') return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == value.size() - 1 || (value.size() > 20 && i == 19)) continue;
        if (value[i] < '0' || value[i] > '9') return false;
    }
    auto number = [&value](size_t offset, size_t length) {
        unsigned result = 0;
        for (size_t i = 0; i < length; ++i) result = result * 10 + static_cast<unsigned>(value[offset + i] - '0');
        return result;
    };
    const unsigned year = number(0, 4), month = number(5, 2), day = number(8, 2);
    const unsigned hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
    if (!year || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) return false;
    static constexpr unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned limit = days[month - 1];
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) ++limit;
    return day >= 1 && day <= limit;
}

inline bool ManifestMatchesCatalog(const Json& manifest, const CatalogResult& catalog) {
    if (!manifest.is_object() || !catalog || catalog.members.empty()) return false;
    Json lock = {{"schema", 1}, {"members", Json::array()}};
    for (const auto& item : catalog.members) {
        std::string target, source;
        if (!WideUtf8(item.target, target) || !WideUtf8(item.source, source)) return false;
        lock["members"].push_back({{"target", target}, {"source", source}, {"source_kind", item.sourceKind},
            {"sha256", item.sha256}, {"replaces_original", item.replacesOriginal}});
    }
    const auto parsed = ReadCatalog(manifest.dump(), lock.dump());
    if (!parsed || parsed.members.size() != catalog.members.size()) return false;
    for (size_t i = 0; i < catalog.members.size(); ++i) {
        const auto& a = parsed.members[i]; const auto& b = catalog.members[i];
        if (!SameTarget(a.target, b.target) || !SameTarget(a.source, b.source) || a.sourceKind != b.sourceKind ||
            a.sha256 != b.sha256 || a.replacesOriginal != b.replacesOriginal || a.required != b.required) return false;
    }
    return true;
}

// Reconstruct installed state from the client and trusted catalog when the
// original staging tree is no longer available. This is for record generation,
// not for authorizing a repair copy from an unverified source.
inline bool DecodeMissingOptional(const Json& json, const CatalogResult& catalog, std::vector<std::wstring>& missing) {
    if (!json.is_array()) return false;
    for (const auto& value : json) {
        if (!value.is_string()) return false;
        std::wstring target;
        if (!Utf8Wide(value.get<std::string>(), target)) return false;
        const CatalogMember* found = nullptr;
        for (const auto& member : catalog.members) if (SameTarget(member.target, target)) { found = &member; break; }
        if (!found || found->required) return false;
        for (const auto& old : missing) if (SameTarget(old, target)) return false;
        missing.push_back(std::move(target));
    }
    return true;
}
inline InstallPreview SnapshotInstalledForRecords(const std::wstring& clientRoot, const CatalogResult& catalog,
                                                   const std::vector<std::wstring>& missingOptional,
                                                   const std::vector<std::wstring>& preservedTargets = {}) {
    InstallPreview snapshot; snapshot.filesReady = true;
    for (const auto& member : catalog.members) {
        PreviewItem item; item.member = member;
        bool wasMissing = false;
        for (const auto& target : missingOptional) if (SameTarget(target, member.target)) { wasMissing = true; break; }
        if (wasMissing) { item.state = PreviewState::MissingOptional; snapshot.items.push_back(item); continue; }
        bool wasPreserved = false;
        for (const auto& target : preservedTargets) if (SameTarget(target, member.target)) { wasPreserved = true; break; }
        GuardedPath target;
        if (!target.Open(clientRoot, member.target)) {
            snapshot.filesReady = false; snapshot.error = "member path unavailable"; return snapshot;
        }
        const auto file = HashFile(target.Path());
        if (!file) {
            snapshot.filesReady = false; snapshot.error = "member unavailable"; return snapshot;
        }
        if (HexSha256(file.identity.sha256) != member.sha256) {
            if (!wasPreserved || !IsPreservedConfigTarget(member.target)) {
                snapshot.filesReady = false; snapshot.error = "installed member changed"; return snapshot;
            }
            // The registered personalised configuration is intentionally left
            // untouched; its bytes are not part of the release identity.
            item.state = PreviewState::PreservedConfig;
            item.existing = file.identity;
            item.source = file.identity;
            snapshot.items.push_back(item);
            continue;
        }
        item.state = PreviewState::Same; item.existing = file.identity; item.source.sha256 = file.identity.sha256;
        snapshot.items.push_back(item);
    }
    return snapshot;
}

inline bool VerifyPersistentSources(const std::wstring& operation, const InstallPreview& snapshot, DWORD& error) {
    if (!snapshot.filesReady) return false;
    for (const auto& item : snapshot.items) {
        if (item.state == PreviewState::MissingOptional) continue;
        if (item.state != PreviewState::Same && item.state != PreviewState::PreservedConfig) return false;
        GuardedPath source;
        if (!source.Open(operation, L"stage/" + item.member.target)) { error = source.Error(); return false; }
        const auto hash = HashFile(source.Path());
        if (!hash || HexSha256(hash.identity.sha256) != item.member.sha256) { error = hash.win32Error; return false; }
    }
    return true;
}

// Build legacy-compatible records from validated inputs. This function does not
// write or commit either file; the caller must revalidate the filesystem and
// journal before publishing them under a recoverable finalization protocol.
inline FinalRecordsResult BuildFinalRecords(const MemberInstallContext& context, const CatalogResult& catalog,
                                             const Json& manifest, const InstallPreview& finalSnapshot,
                                             const JournalCursor& cursor, const ClientRecord* previous,
                                             const std::string& installedUtc) {
    FinalRecordsResult result;
    auto fail = [&result](const char* text) { result.error = text; return result; };
    if (!catalog || catalog.members.empty() || !ValidOperationId(context.operationId) ||
        cursor.Pending() || cursor.Prepared() || cursor.RollingBack() || cursor.Terminal() ||
        cursor.Applied().empty() || !TrustedJournalCursor(cursor, catalog, context.operationId)) return fail("invalid or incomplete journal");
    if (!ValidUtcTimestamp(installedUtc)) return fail("invalid timestamp");
    std::wstring client, operation;
    if (!CanonicalLocalPath(context.clientRoot, client) || !CanonicalLocalPath(context.journalRoot, operation) ||
        WithinRoot(client, operation) || WithinRoot(operation, client)) return fail("invalid paths");
    std::string clientUtf8, operationUtf8;
    if (!WideUtf8(client, clientUtf8) || !WideUtf8(operation, operationUtf8)) return fail("invalid path encoding");
    if (!ManifestMatchesCatalog(manifest, catalog)) return fail("manifest and catalog mismatch");
    std::string version;
    if (!StringField(manifest, "updated", version)) return fail("manifest version missing");
    const auto residues = manifest.find("residue");
    if (residues != manifest.end() && !residues->is_array()) return fail("invalid manifest residue");
    if (!finalSnapshot.filesReady || finalSnapshot.items.size() != catalog.members.size()) return fail("incomplete file snapshot");
    std::vector<bool> observed(catalog.members.size(), false);
    for (const auto& item : finalSnapshot.items) {
        size_t match = catalog.members.size();
        for (size_t i = 0; i < catalog.members.size(); ++i) if (SameTarget(catalog.members[i].target, item.member.target)) { match = i; break; }
        if (match == catalog.members.size() || observed[match]) return fail("snapshot member mismatch");
        const auto& trusted = catalog.members[match];
        if (!SameTarget(trusted.source, item.member.source) || trusted.sourceKind != item.member.sourceKind ||
            trusted.sha256 != item.member.sha256 || trusted.required != item.member.required ||
            trusted.replacesOriginal != item.member.replacesOriginal ||
            (item.state != PreviewState::Same && item.state != PreviewState::PreservedConfig &&
             !(item.state == PreviewState::MissingOptional && !trusted.required)))
            return fail("snapshot not ready");
        if (item.state == PreviewState::Same &&
            (HexSha256(item.source.sha256) != trusted.sha256 || item.source.sha256 != item.existing.sha256))
            return fail("snapshot hashes do not match");
        if (item.state == PreviewState::PreservedConfig) {
            // A personalised configuration may only survive because the prior
            // record proves this exact target was installed from the current
            // release template; never preserve an unregistered third-party file.
            if (!IsPreservedConfigTarget(trusted.target) || !trusted.required) return fail("preserved member not supported");
            bool registered = false;
            if (previous) for (const auto& old : previous->entries)
                if (SameTarget(old.target, trusted.target)) { registered = old.sha256 == trusted.sha256; break; }
            if (!registered) return fail("preserved member is not registered");
        }
        observed[match] = true;
    }
    Json merged = Json::array();
    Json previousJson = nullptr;
    if (previous) {
        if (!*previous || !previous->original.is_object()) return fail("invalid prior record");
        previousJson = previous->original;
        merged = previous->original["entries"];
    }
    Json changed = Json::array(), applied = Json::array();
    for (const auto& item : cursor.Applied()) {
        const CatalogMember* match = nullptr;
        for (const auto& candidate : catalog.members) if (SameTarget(candidate.target, item.target)) { match = &candidate; break; }
        if (!match) return fail("journal member absent from catalog");
        std::string target, backup;
        if (!WideUtf8(item.target, target) || (item.previouslyPresent && !WideUtf8(item.backup, backup))) return fail("invalid member encoding");
        Json entry = {{"target", target}, {"sha256", item.candidateSha256}, {"replaces_original", match->replacesOriginal},
                      {"pre_state", item.previouslyPresent ? "present" : "absent"},
                      {"previous_sha256", item.previouslyPresent ? Json(item.previousSha256) : Json(nullptr)},
                      {"backup", item.previouslyPresent ? Json(backup) : Json(nullptr)}};
        changed.push_back(entry);
        applied.push_back(target);
        bool found = false;
        if (previous) for (const auto& old : previous->entries) if (SameTarget(old.target, item.target)) { found = true; break; }
        if (!found) merged.push_back(std::move(entry));
    }
    Json clientRecord = {{"schema", 1}, {"status", "installed"}, {"installed_utc", installedUtc},
        {"client", clientUtf8}, {"manifest_version", version}, {"tool", "WOW-335-NR"},
        {"operation", operationUtf8}, {"entries", merged}, {"residue", residues == manifest.end() ? Json::array() : *residues}};
    const auto checked = ReadClientRecord(clientRecord.dump());
    if (!checked) return fail("generated client record invalid");
    Json operationRecord = {{"schema", 2}, {"status", "completed"}, {"client", clientUtf8},
        {"stage", ""}, {"manifest_version", version}, {"tool", "WOW-335-NR"}, {"entries", changed},
        {"repair_entries", merged}, {"previous_client_record", previousJson}, {"applied", applied}, {"errors", Json::array()}};
    std::wstring stage;
    std::string stageUtf8;
    if (!CanonicalLocalPath(context.stageRoot, stage) || !WideUtf8(stage, stageUtf8)) return fail("invalid stage path");
    operationRecord["stage"] = stageUtf8;
    const auto operationChecked = ReadOperationRecord(operationRecord.dump());
    if (!operationChecked) return fail("generated operation record invalid");
    result.clientRecord = clientRecord.dump(2) + "\n";
    result.operationRecord = operationRecord.dump(2) + "\n";
    return result;
}
}
