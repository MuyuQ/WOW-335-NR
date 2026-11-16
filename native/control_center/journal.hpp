#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>
#include "client_record.hpp"
#include "write_new.hpp"

namespace control_center {
struct JournalFileId {
    DWORD volume = 0, high = 0, low = 0;
    std::uint64_t size = 0;
    bool known = false;
};
inline JournalFileId JournalId(const FileIdentity& identity) {
    return {identity.volume, identity.indexHigh, identity.indexLow, identity.size, true};
}
inline FileIdentity FileIdFromJournal(const JournalFileId& identity) {
    FileIdentity result;
    result.volume = identity.volume; result.indexHigh = identity.high; result.indexLow = identity.low; result.size = identity.size;
    return result;
}
inline bool SameJournalId(const JournalFileId& a, const JournalFileId& b) {
    return a.known == b.known && (!a.known || (a.volume == b.volume && a.high == b.high && a.low == b.low && a.size == b.size));
}
struct JournalItem {
    std::wstring target, backup, staged, quarantine;
    std::string candidateSha256, previousSha256;
    bool previouslyPresent = false;
    JournalFileId previousIdentity;
};
enum class JournalKind { Prepare, Prepared, CancelPrepare, CancelPrepared, Intent, Applied, CancelIntent, RollbackIntent, RollbackApplied, RollbackFailed,
                         Complete, Cancelled, RolledBack, RollbackIncomplete };
struct JournalEvent {
    std::uint64_t sequence = 0;
    JournalKind kind = JournalKind::Intent;
    JournalItem item;
    DWORD failureCode = ERROR_SUCCESS;
    JournalFileId stagedIdentity;
};
inline bool SameJournalItem(const JournalItem& a, const JournalItem& b) {
    return SameTarget(a.target, b.target) && a.candidateSha256 == b.candidateSha256 &&
        a.previouslyPresent == b.previouslyPresent && a.previousSha256 == b.previousSha256 &&
        SameJournalId(a.previousIdentity, b.previousIdentity) &&
        (a.backup.empty() ? b.backup.empty() : !b.backup.empty() && SameTarget(a.backup, b.backup)) &&
        SameTarget(a.staged, b.staged) &&
        (a.quarantine.empty() ? b.quarantine.empty() : !b.quarantine.empty() && SameTarget(a.quarantine, b.quarantine));
}
inline std::wstring JournalParent(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    const size_t separator = path.find_last_of(L'\\');
    return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}
inline bool SameJournalParent(const std::wstring& a, const std::wstring& b) {
    const auto left = JournalParent(a), right = JournalParent(b);
    return left.empty() ? right.empty() : !right.empty() && SameTarget(left, right);
}
inline bool ValidJournalItem(const JournalItem& item) {
    if (!ValidMemberPath(item.target) || !ValidMemberPath(item.staged) || !ValidSha256(item.candidateSha256) ||
        SameTarget(item.target, item.staged) || !SameJournalParent(item.target, item.staged)) return false;
    return item.previouslyPresent ? item.previousIdentity.known && ValidSha256(item.previousSha256) && ValidMemberPath(item.backup) &&
        item.previousSha256 != item.candidateSha256 &&
        ValidMemberPath(item.quarantine) && !SameTarget(item.target, item.quarantine) &&
        !SameTarget(item.staged, item.quarantine) && SameJournalParent(item.target, item.quarantine)
        : !item.previousIdentity.known && item.previousSha256.empty() && item.backup.empty() && item.quarantine.empty();
}
inline Json JournalIdJson(const JournalFileId& id) {
    return {{"volume", id.volume}, {"index_high", id.high}, {"index_low", id.low}, {"size", id.size}};
}
inline bool DecodeJournalId(const Json& json, JournalFileId& id) {
    if (!json.is_object() || json.size() != 4) return false;
    const auto volume = json.find("volume"), high = json.find("index_high"), low = json.find("index_low"), size = json.find("size");
    if (volume == json.end() || high == json.end() || low == json.end() || size == json.end() ||
        !volume->is_number_unsigned() || !high->is_number_unsigned() || !low->is_number_unsigned() || !size->is_number_unsigned() ||
        *volume > MAXDWORD || *high > MAXDWORD || *low > MAXDWORD) return false;
    id.volume = volume->get<DWORD>(); id.high = high->get<DWORD>(); id.low = low->get<DWORD>();
    id.size = size->get<std::uint64_t>(); id.known = true;
    return true;
}
inline const char* JournalKindName(JournalKind kind) {
    switch (kind) {
    case JournalKind::Prepare: return "prepare";
    case JournalKind::Prepared: return "prepared";
    case JournalKind::CancelPrepare: return "cancel_prepare";
    case JournalKind::CancelPrepared: return "cancel_prepared";
    case JournalKind::Intent: return "intent";
    case JournalKind::Applied: return "applied";
    case JournalKind::CancelIntent: return "cancel_intent";
    case JournalKind::RollbackIntent: return "rollback_intent";
    case JournalKind::RollbackApplied: return "rollback_applied";
    case JournalKind::RollbackFailed: return "rollback_failed";
    case JournalKind::Complete: return "complete";
    case JournalKind::Cancelled: return "cancelled";
    case JournalKind::RolledBack: return "rolled_back";
    case JournalKind::RollbackIncomplete: return "rollback_incomplete";
    }
    return nullptr;
}
inline bool WideUtf8(const std::wstring& input, std::string& output) {
    if (input.empty() || input.size() > static_cast<size_t>(INT_MAX)) return false;
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (!size) return false;
    output.resize(static_cast<size_t>(size));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), &output[0], size, nullptr, nullptr) == size;
}
inline bool EncodeJournalEvent(const JournalEvent& event, std::string& output) {
    const char* kind = JournalKindName(event.kind);
    if (!kind || !event.sequence) return false;
    if ((event.kind == JournalKind::Prepared) != event.stagedIdentity.known) return false;
    Json json = {{"schema", 1}, {"sequence", event.sequence}, {"kind", kind}};
    if (event.kind != JournalKind::Complete && event.kind != JournalKind::Cancelled && event.kind != JournalKind::RolledBack && event.kind != JournalKind::RollbackIncomplete) {
        if (!ValidJournalItem(event.item)) return false;
        std::string target, backup, staged, quarantine;
        if (!WideUtf8(event.item.target, target) || !WideUtf8(event.item.staged, staged) ||
            (event.item.previouslyPresent && (!WideUtf8(event.item.backup, backup) || !WideUtf8(event.item.quarantine, quarantine)))) return false;
        json["target"] = target;
        json["pre_state"] = event.item.previouslyPresent ? "present" : "absent";
        json["sha256"] = event.item.candidateSha256;
        json["previous_sha256"] = event.item.previouslyPresent ? Json(event.item.previousSha256) : Json(nullptr);
        json["previous_identity"] = event.item.previouslyPresent ? JournalIdJson(event.item.previousIdentity) : Json(nullptr);
        json["backup"] = event.item.previouslyPresent ? Json(backup) : Json(nullptr);
        json["staged"] = staged;
        json["quarantine"] = event.item.previouslyPresent ? Json(quarantine) : Json(nullptr);
        if (event.kind == JournalKind::Prepared) json["staged_identity"] = JournalIdJson(event.stagedIdentity);
        if (event.kind == JournalKind::RollbackFailed) json["failure_code"] = event.failureCode;
    }
    output = json.dump() + "\n";
    return true;
}
inline bool DecodeJournalEvent(const std::string& input, JournalEvent& output) {
    const auto parsed = ParseJsonObject(input);
    if (!parsed) return false;
    const Json& json = parsed.value;
    const auto schema = json.find("schema"), sequence = json.find("sequence");
    std::string kind;
    if (schema == json.end() || !schema->is_number_integer() || *schema != 1 ||
        sequence == json.end() || !sequence->is_number_unsigned() || *sequence == 0 || !StringField(json, "kind", kind)) return false;
    JournalEvent event;
    event.sequence = sequence->get<std::uint64_t>();
    if (kind == "prepare") event.kind = JournalKind::Prepare;
    else if (kind == "prepared") event.kind = JournalKind::Prepared;
    else if (kind == "cancel_prepare") event.kind = JournalKind::CancelPrepare;
    else if (kind == "cancel_prepared") event.kind = JournalKind::CancelPrepared;
    else if (kind == "intent") event.kind = JournalKind::Intent;
    else if (kind == "applied") event.kind = JournalKind::Applied;
    else if (kind == "cancel_intent") event.kind = JournalKind::CancelIntent;
    else if (kind == "rollback_intent") event.kind = JournalKind::RollbackIntent;
    else if (kind == "rollback_applied") event.kind = JournalKind::RollbackApplied;
    else if (kind == "rollback_failed") event.kind = JournalKind::RollbackFailed;
    else if (kind == "complete") event.kind = JournalKind::Complete;
    else if (kind == "cancelled") event.kind = JournalKind::Cancelled;
    else if (kind == "rolled_back") event.kind = JournalKind::RolledBack;
    else if (kind == "rollback_incomplete") event.kind = JournalKind::RollbackIncomplete;
    else return false;
    if (event.kind == JournalKind::Complete || event.kind == JournalKind::Cancelled || event.kind == JournalKind::RolledBack || event.kind == JournalKind::RollbackIncomplete) {
        if (json.size() != 3) return false;
    } else {
        std::string target, state, staged;
        if (json.size() != ((event.kind == JournalKind::RollbackFailed || event.kind == JournalKind::Prepared) ? 12u : 11u) ||
            !StringField(json, "target", target) || !Utf8Wide(target, event.item.target) ||
            !StringField(json, "staged", staged) || !Utf8Wide(staged, event.item.staged) ||
            !StringField(json, "pre_state", state) || !StringField(json, "sha256", event.item.candidateSha256)) return false;
        if (event.kind == JournalKind::RollbackFailed) {
            const auto code = json.find("failure_code");
            if (code == json.end() || !code->is_number_unsigned() || *code > MAXDWORD) return false;
            event.failureCode = code->get<DWORD>();
        }
        if (event.kind == JournalKind::Prepared) {
            const auto identity = json.find("staged_identity");
            if (identity == json.end() || !DecodeJournalId(*identity, event.stagedIdentity)) return false;
        }
        if (state == "present") {
            std::string backup, quarantine;
            event.item.previouslyPresent = true;
            if (!StringField(json, "previous_sha256", event.item.previousSha256) ||
                !StringField(json, "backup", backup) || !Utf8Wide(backup, event.item.backup) ||
                !StringField(json, "quarantine", quarantine) || !Utf8Wide(quarantine, event.item.quarantine)) return false;
            const auto identity = json.find("previous_identity");
            if (identity == json.end() || !DecodeJournalId(*identity, event.item.previousIdentity)) return false;
        } else if (state == "absent") {
            if (!json.contains("previous_sha256") || !json["previous_sha256"].is_null() ||
                !json.contains("backup") || !json["backup"].is_null() ||
                !json.contains("quarantine") || !json["quarantine"].is_null() ||
                !json.contains("previous_identity") || !json["previous_identity"].is_null()) return false;
        } else return false;
        if (!ValidJournalItem(event.item)) return false;
    }
    output = std::move(event);
    return true;
}

class JournalCursor {
    std::uint64_t next_ = 1;
    bool rollback_ = false, terminal_ = false, pending_ = false, prepared_ = false;
    JournalKind terminalKind_ = JournalKind::Complete;
    JournalEvent pendingEvent_{};
    JournalItem preparedItem_{};
    JournalFileId preparedIdentity_{};
    std::vector<JournalItem> applied_;
    std::vector<JournalItem> seen_;
    std::vector<JournalFileId> appliedIdentities_;
    std::vector<JournalEvent> unresolved_;
    std::vector<JournalEvent> discarded_;
public:
    std::uint64_t Next() const { return next_; }
    bool Pending() const { return pending_; }
    bool Prepared() const { return prepared_; }
    const JournalItem& PreparedItem() const { return preparedItem_; }
    bool Terminal() const { return terminal_; }
    JournalKind TerminalKind() const { return terminalKind_; }
    bool RollingBack() const { return rollback_; }
    const JournalEvent& PendingEvent() const { return pendingEvent_; }
    JournalFileId PendingCandidateIdentity() const {
        if (!pending_) return {};
        if (pendingEvent_.kind == JournalKind::Intent) return preparedIdentity_;
        if (pendingEvent_.kind == JournalKind::RollbackIntent && !appliedIdentities_.empty()) return appliedIdentities_.back();
        return {};
    }
    const std::vector<JournalItem>& Applied() const { return applied_; }
    const std::vector<JournalItem>& Seen() const { return seen_; }
    const std::vector<JournalFileId>& AppliedIdentities() const { return appliedIdentities_; }
    const std::vector<JournalEvent>& Unresolved() const { return unresolved_; }
    const std::vector<JournalEvent>& Discarded() const { return discarded_; }
    bool Accept(const JournalEvent& event) {
        if (terminal_ || event.sequence != next_ || next_ == (std::numeric_limits<std::uint64_t>::max)()) return false;
        if ((event.kind == JournalKind::Prepared) != event.stagedIdentity.known) return false;
        if (event.kind != JournalKind::Complete && event.kind != JournalKind::Cancelled && event.kind != JournalKind::RolledBack &&
            event.kind != JournalKind::RollbackIncomplete && !ValidJournalItem(event.item)) return false;
        switch (event.kind) {
        case JournalKind::Prepare:
            if (pending_ || prepared_ || rollback_) return false;
            for (const auto& prior : applied_) if (SameTarget(prior.target, event.item.target)) return false;
            pending_ = true; pendingEvent_ = event; break;
        case JournalKind::Prepared:
        case JournalKind::CancelPrepare:
            if (!pending_ || pendingEvent_.kind != JournalKind::Prepare || !SameJournalItem(pendingEvent_.item, event.item)) return false;
            if (event.kind == JournalKind::Prepared) { prepared_ = true; preparedItem_ = event.item; preparedIdentity_ = event.stagedIdentity; }
            else discarded_.push_back(event);
            pending_ = false; break;
        case JournalKind::Intent:
            if (pending_ || !prepared_ || rollback_ || !SameJournalItem(preparedItem_, event.item)) return false;
            pending_ = true; pendingEvent_ = event; break;
        case JournalKind::CancelPrepared:
            if (pending_ || !prepared_ || rollback_ || !SameJournalItem(preparedItem_, event.item)) return false;
            discarded_.push_back(event); prepared_ = false; break;
        case JournalKind::Applied:
        case JournalKind::CancelIntent:
            if (!pending_ || pendingEvent_.kind != JournalKind::Intent || !SameJournalItem(pendingEvent_.item, event.item)) return false;
            if (event.kind == JournalKind::Applied) { applied_.push_back(event.item); appliedIdentities_.push_back(preparedIdentity_); }
            else discarded_.push_back(event);
            pending_ = false; prepared_ = false; break;
        case JournalKind::RollbackIntent:
            if (pending_ || prepared_ || applied_.empty() || !SameJournalItem(applied_.back(), event.item)) return false;
            rollback_ = true; pending_ = true; pendingEvent_ = event; break;
        case JournalKind::RollbackApplied:
        case JournalKind::RollbackFailed:
            if (!pending_ || pendingEvent_.kind != JournalKind::RollbackIntent || !SameJournalItem(pendingEvent_.item, event.item)) return false;
            if (event.kind == JournalKind::RollbackFailed) unresolved_.push_back(event);
            applied_.pop_back(); appliedIdentities_.pop_back(); pending_ = false; break;
        case JournalKind::Complete:
            if (pending_ || prepared_ || rollback_) return false;
            terminal_ = true; break;
        case JournalKind::Cancelled:
            if (pending_ || prepared_ || rollback_ || !applied_.empty()) return false;
            terminal_ = true; break;
        case JournalKind::RolledBack:
            if (pending_ || !rollback_ || !applied_.empty() || !unresolved_.empty()) return false;
            terminal_ = true; break;
        case JournalKind::RollbackIncomplete:
            if (pending_ || !rollback_ || !applied_.empty() || unresolved_.empty()) return false;
            terminal_ = true; break;
        default: return false;
        }
        if (terminal_) terminalKind_ = event.kind;
        else seen_.push_back(event.item);
        ++next_;
        return true;
    }
};

enum class JournalError { None, InvalidEvent, UnsafeRoot, Write, Enumeration, InvalidFile, Read };
struct JournalResult {
    JournalError error = JournalError::None;
    DWORD win32Error = ERROR_SUCCESS;
    bool cleanupFailed = false;
    JournalCursor cursor;
    explicit operator bool() const { return error == JournalError::None; }
};
inline std::wstring JournalFileName(std::uint64_t sequence) {
    wchar_t buffer[64]{};
    if (swprintf_s(buffer, L"event-%020llu.json", static_cast<unsigned long long>(sequence)) < 0) return {};
    return buffer;
}

// The caller serializes access to an operation directory. A successful call
// means the checkpoint file was flushed and verified; no overwrite is allowed.
inline JournalResult PersistJournalEvent(const std::wstring& directory, const JournalCursor& current,
                                         const JournalEvent& event, const CopyIo& io = CopyIo{}) {
    JournalResult result;
    result.cursor = current;
    JournalCursor next = current;
    std::string data;
    if (!next.Accept(event) || !EncodeJournalEvent(event, data)) { result.error = JournalError::InvalidEvent; return result; }
    GuardedPath file;
    if (!file.Open(directory, JournalFileName(event.sequence))) {
        result.error = JournalError::UnsafeRoot; result.win32Error = file.Error(); return result;
    }
    const auto write = WriteNewVerified(file, data, io);
    if (!write) {
        result.error = JournalError::Write; result.win32Error = write.win32Error; result.cleanupFailed = write.cleanupFailed; return result;
    }
    result.cursor = std::move(next);
    return result;
}

// Read a regular checkpoint through an unshared write/delete handle. The size
// limit applies only to journal records, not to game or component files.
inline bool ReadJournalBytes(const GuardedPath& path, std::string& bytes, DWORD& error) {
    HANDLE file = CreateFileW(path.Path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(file, &info) || !GetFileSizeEx(file, &size)) {
        error = GetLastError(); CloseHandle(file); return false;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        GetFileType(file) != FILE_TYPE_DISK || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        error = ERROR_INVALID_DATA; CloseHandle(file); return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        const DWORD requested = static_cast<DWORD>((std::min)(bytes.size() - offset, static_cast<size_t>(65536)));
        if (!ReadFile(file, &bytes[offset], requested, &count, nullptr)) {
            error = GetLastError(); CloseHandle(file); return false;
        }
        if (!count) { error = ERROR_HANDLE_EOF; CloseHandle(file); return false; }
        offset += count;
    }
    CloseHandle(file);
    return true;
}

// Replay stops at malformed, missing, reordered or inconsistent checkpoints.
// A pending intent is intentionally returned to the caller for reconciliation
// with actual files; replay never assumes that the mutation did or did not run.
inline JournalResult LoadJournal(const std::wstring& directory) {
    JournalResult result;
    GuardedPath root;
    if (!root.Open(directory, L"__journal_root__")) {
        result.error = JournalError::UnsafeRoot; result.win32Error = root.Error(); return result;
    }
    std::wstring canonical;
    if (!CanonicalLocalPath(directory, canonical)) { result.error = JournalError::UnsafeRoot; return result; }
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((canonical + L"\\event-*").c_str(), &found);
    std::vector<std::wstring> names;
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) { result.error = JournalError::Enumeration; result.win32Error = error; }
        return result;
    }
    do { names.emplace_back(found.cFileName); } while (FindNextFileW(find, &found));
    const DWORD last = GetLastError();
    FindClose(find);
    if (last != ERROR_NO_MORE_FILES) { result.error = JournalError::Enumeration; result.win32Error = last; return result; }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        GuardedPath file;
        if (!file.Open(canonical, name)) { result.error = JournalError::InvalidFile; result.win32Error = file.Error(); return result; }
        std::string bytes;
        if (!ReadJournalBytes(file, bytes, result.win32Error)) { result.error = JournalError::Read; return result; }
        JournalEvent event;
        if (!DecodeJournalEvent(bytes, event) || name != JournalFileName(event.sequence) || !result.cursor.Accept(event)) {
            result.error = JournalError::InvalidFile; return result;
        }
    }
    return result;
}
}
