#pragma once

#include "uninstall_plan_read.hpp"

namespace control_center {
enum class UninstallJournalKind { Prepare, Prepared, CancelPrepare, Intent, CancelPrepared, Applied, CancelIntent, IntentConflict,
                                   RollbackIntent, RollbackApplied, RollbackFailed, MembersDone, Cancelled, RolledBack, RollbackIncomplete };
struct UninstallJournalEvent {
    std::uint64_t sequence = 0;
    UninstallJournalKind kind = UninstallJournalKind::Prepare;
    size_t index = 0;
    JournalFileId staged{}, saved{};
    DWORD failure = ERROR_SUCCESS;
};
inline const char* UninstallJournalName(UninstallJournalKind kind) {
    switch (kind) {
        case UninstallJournalKind::Prepare: return "prepare"; case UninstallJournalKind::Prepared: return "prepared";
        case UninstallJournalKind::CancelPrepare: return "cancel_prepare"; case UninstallJournalKind::Intent: return "intent";
        case UninstallJournalKind::CancelPrepared: return "cancel_prepared"; case UninstallJournalKind::Applied: return "applied";
        case UninstallJournalKind::CancelIntent: return "cancel_intent"; case UninstallJournalKind::RollbackIntent: return "rollback_intent";
        case UninstallJournalKind::IntentConflict: return "intent_conflict";
        case UninstallJournalKind::RollbackApplied: return "rollback_applied"; case UninstallJournalKind::RollbackFailed: return "rollback_failed";
        case UninstallJournalKind::MembersDone: return "members_done"; case UninstallJournalKind::Cancelled: return "cancelled";
        case UninstallJournalKind::RolledBack: return "rolled_back"; case UninstallJournalKind::RollbackIncomplete: return "rollback_incomplete";
        default: return nullptr;
    }
}
inline bool UninstallJournalTerminal(UninstallJournalKind kind) {
    return kind == UninstallJournalKind::MembersDone || kind == UninstallJournalKind::Cancelled ||
           kind == UninstallJournalKind::RolledBack || kind == UninstallJournalKind::RollbackIncomplete;
}
struct UninstallJournalApplied { size_t index = 0; JournalFileId staged{}, saved{}; };
class UninstallJournalCursor {
    std::array<std::uint8_t, 32> planHash_{};
    std::vector<size_t> actions_;
    std::vector<UninstallItemState> states_;
    std::vector<bool> saves_;
    std::vector<UninstallJournalApplied> applied_;
    std::vector<UninstallJournalEvent> unresolved_;
    std::uint64_t next_ = 1;
    size_t progress_ = 0, current_ = 0;
    enum class Phase { Idle, Preparing, Prepared, Intent, RollbackIntent, Terminal } phase_ = Phase::Idle;
    bool rollback_ = false;
    UninstallJournalKind terminal_ = UninstallJournalKind::Cancelled;
    JournalFileId staged_{}, saved_{};
public:
    explicit UninstallJournalCursor(const TrustedUninstallPlan& plan) : planHash_(plan.identity.sha256) {
        for (size_t i = 0; i < plan.items.size(); ++i) {
            const auto state = plan.items[i].state;
            states_.push_back(state); saves_.push_back(plan.items[i].saveCurrent);
            if (state == UninstallItemState::DeleteInstalled || state == UninstallItemState::DeleteModified || state == UninstallItemState::RestoreOriginal)
                actions_.push_back(i);
        }
    }
    std::uint64_t Next() const { return next_; }
    bool Terminal() const { return phase_ == Phase::Terminal; }
    UninstallJournalKind TerminalKind() const { return terminal_; }
    bool Preparing() const { return phase_ == Phase::Preparing; }
    bool Prepared() const { return phase_ == Phase::Prepared; }
    bool IntentPending() const { return phase_ == Phase::Intent; }
    bool RollbackPending() const { return phase_ == Phase::RollbackIntent; }
    bool RollingBack() const { return rollback_; }
    size_t Current() const { return current_; }
    const JournalFileId& Staged() const { return staged_; }
    const JournalFileId& Saved() const { return saved_; }
    JournalFileId PendingRollbackStaged() const { return RollbackPending() && !applied_.empty() ? applied_.back().staged : JournalFileId{}; }
    JournalFileId PendingRollbackSaved() const { return RollbackPending() && !applied_.empty() ? applied_.back().saved : JournalFileId{}; }
    const std::vector<UninstallJournalApplied>& Applied() const { return applied_; }
    const std::vector<UninstallJournalEvent>& Unresolved() const { return unresolved_; }
    bool Matches(const TrustedUninstallPlan& plan) const {
        if (!plan.valid || plan.identity.sha256 != planHash_ || plan.items.size() != states_.size()) return false;
        for (size_t i = 0; i < states_.size(); ++i)
            if (plan.items[i].state != states_[i] || plan.items[i].saveCurrent != saves_[i]) return false;
        return true;
    }
    bool Accept(const UninstallJournalEvent& event) {
        if (Terminal() || event.sequence != next_ || next_ == (std::numeric_limits<std::uint64_t>::max)()) return false;
        const bool terminal = UninstallJournalTerminal(event.kind);
        if ((event.kind != UninstallJournalKind::Prepared && (event.staged.known || event.saved.known)) ||
            ((event.kind == UninstallJournalKind::RollbackFailed || event.kind == UninstallJournalKind::IntentConflict) ?
                event.failure == ERROR_SUCCESS : event.failure != ERROR_SUCCESS) ||
            (terminal && event.index != 0) || (!terminal && event.index >= states_.size())) return false;
        switch (event.kind) {
        case UninstallJournalKind::Prepare:
            if (phase_ != Phase::Idle || rollback_ || progress_ >= actions_.size() || event.index != actions_[progress_]) return false;
            current_ = event.index; phase_ = Phase::Preparing; break;
        case UninstallJournalKind::Prepared:
            if (phase_ != Phase::Preparing || event.index != current_ ||
                event.staged.known != (states_[current_] == UninstallItemState::RestoreOriginal) || event.saved.known != saves_[current_]) return false;
            staged_ = event.staged; saved_ = event.saved; phase_ = Phase::Prepared; break;
        case UninstallJournalKind::CancelPrepare:
            if (phase_ != Phase::Preparing || event.index != current_) return false;
            phase_ = Phase::Idle; break;
        case UninstallJournalKind::Intent:
            if (phase_ != Phase::Prepared || event.index != current_ || rollback_) return false;
            phase_ = Phase::Intent; break;
        case UninstallJournalKind::CancelPrepared:
            if (phase_ != Phase::Prepared || event.index != current_) return false;
            phase_ = Phase::Idle; break;
        case UninstallJournalKind::Applied:
            if (phase_ != Phase::Intent || event.index != current_) return false;
            applied_.push_back({current_, staged_, saved_}); ++progress_; phase_ = Phase::Idle; break;
        case UninstallJournalKind::CancelIntent:
            if (phase_ != Phase::Intent || event.index != current_) return false;
            phase_ = Phase::Idle; break;
        case UninstallJournalKind::IntentConflict:
            if (phase_ != Phase::Intent || event.index != current_) return false;
            unresolved_.push_back(event); rollback_ = true; phase_ = Phase::Idle; break;
        case UninstallJournalKind::RollbackIntent:
            if (phase_ != Phase::Idle || applied_.empty() || event.index != applied_.back().index) return false;
            rollback_ = true; current_ = event.index; phase_ = Phase::RollbackIntent; break;
        case UninstallJournalKind::RollbackApplied:
        case UninstallJournalKind::RollbackFailed:
            if (phase_ != Phase::RollbackIntent || event.index != current_ || applied_.empty()) return false;
            if (event.kind == UninstallJournalKind::RollbackFailed) unresolved_.push_back(event);
            applied_.pop_back(); phase_ = Phase::Idle; break;
        case UninstallJournalKind::MembersDone:
            if (phase_ != Phase::Idle || rollback_ || progress_ != actions_.size()) return false;
            phase_ = Phase::Terminal; break;
        case UninstallJournalKind::Cancelled:
            if (phase_ != Phase::Idle || rollback_ || !applied_.empty() || progress_ != 0) return false;
            phase_ = Phase::Terminal; break;
        case UninstallJournalKind::RolledBack:
        case UninstallJournalKind::RollbackIncomplete:
            if (phase_ != Phase::Idle || !rollback_ || !applied_.empty() ||
                (event.kind == UninstallJournalKind::RolledBack ? !unresolved_.empty() : unresolved_.empty())) return false;
            phase_ = Phase::Terminal; break;
        default: return false;
        }
        if (terminal) terminal_ = event.kind;
        ++next_; return true;
    }
};

struct UninstallJournalResult {
    JournalError error = JournalError::None;
    DWORD win32Error = ERROR_SUCCESS;
    bool cleanupFailed = false;
    UninstallJournalCursor cursor;
    explicit UninstallJournalResult(const TrustedUninstallPlan& plan) : cursor(plan) {}
    explicit operator bool() const { return error == JournalError::None; }
};
inline bool EncodeUninstallJournalEvent(const UninstallJournalEvent& event, const std::string& planHash, std::string& bytes) {
    const char* kind = UninstallJournalName(event.kind);
    if (!kind || !ValidSha256(planHash)) return false;
    const Json json = {{"schema", 1}, {"sequence", event.sequence}, {"kind", kind}, {"plan_sha256", planHash},
        {"index", UninstallJournalTerminal(event.kind) ? Json(nullptr) : Json(event.index)},
        {"staged_identity", event.staged.known ? JournalIdJson(event.staged) : Json(nullptr)},
        {"saved_identity", event.saved.known ? JournalIdJson(event.saved) : Json(nullptr)}, {"failure", event.failure}};
    bytes = json.dump() + "\n"; return true;
}
inline bool DecodeUninstallJournalEvent(const std::string& bytes, const std::string& planHash, UninstallJournalEvent& event) {
    const auto parsed = ParseJsonObject(bytes); std::string kind, digest;
    if (!parsed || parsed.value.size() != 8 || !parsed.value.contains("schema") || parsed.value["schema"] != 1 ||
        !parsed.value.contains("sequence") || !parsed.value["sequence"].is_number_unsigned() ||
        !StringField(parsed.value, "kind", kind) || !StringField(parsed.value, "plan_sha256", digest) || digest != planHash ||
        !parsed.value.contains("index") || !parsed.value.contains("staged_identity") || !parsed.value.contains("saved_identity") ||
        !parsed.value.contains("failure") || !parsed.value["failure"].is_number_unsigned() || parsed.value["failure"] > MAXDWORD) return false;
    bool found = false;
    for (int i = static_cast<int>(UninstallJournalKind::Prepare); i <= static_cast<int>(UninstallJournalKind::RollbackIncomplete); ++i) {
        const auto candidate = static_cast<UninstallJournalKind>(i);
        if (kind == UninstallJournalName(candidate)) { event.kind = candidate; found = true; break; }
    }
    if (!found) return false;
    event.sequence = parsed.value["sequence"].get<std::uint64_t>(); event.failure = parsed.value["failure"].get<DWORD>();
    if (UninstallJournalTerminal(event.kind)) { if (!parsed.value["index"].is_null()) return false; event.index = 0; }
    else {
        if (!parsed.value["index"].is_number_unsigned() || parsed.value["index"] > (std::numeric_limits<size_t>::max)()) return false;
        event.index = parsed.value["index"].get<size_t>();
    }
    if (!parsed.value["staged_identity"].is_null() && !DecodeJournalId(parsed.value["staged_identity"], event.staged)) return false;
    if (!parsed.value["saved_identity"].is_null() && !DecodeJournalId(parsed.value["saved_identity"], event.saved)) return false;
    return true;
}
inline UninstallJournalResult PersistUninstallJournal(const std::wstring& directory, const TrustedUninstallPlan& plan,
                                                        const UninstallJournalCursor& current, const UninstallJournalEvent& event,
                                                        const CopyIo& io = CopyIo{}) {
    UninstallJournalResult result(plan); result.cursor = current; auto next = current; std::string bytes;
    if (!current.Matches(plan) || !next.Accept(event) || !EncodeUninstallJournalEvent(event, HexSha256(plan.identity.sha256), bytes)) {
        result.error = JournalError::InvalidEvent; return result;
    }
    GuardedPath file;
    if (!file.Open(directory, JournalFileName(event.sequence))) { result.error = JournalError::UnsafeRoot; result.win32Error = file.Error(); return result; }
    const auto write = WriteNewVerified(file, bytes, io);
    if (!write) { result.error = JournalError::Write; result.win32Error = write.win32Error; result.cleanupFailed = write.cleanupFailed; return result; }
    result.cursor = std::move(next); return result;
}
inline UninstallJournalResult LoadUninstallJournal(const std::wstring& directory, const TrustedUninstallPlan& plan) {
    UninstallJournalResult result(plan);
    if (!plan.valid) { result.error = JournalError::InvalidEvent; return result; }
    std::wstring canonical; GuardedPath root;
    if (!CanonicalLocalPath(directory, canonical) || !root.Open(canonical, L"__journal_root__")) {
        result.error = JournalError::UnsafeRoot; result.win32Error = root.Error(); return result;
    }
    WIN32_FIND_DATAW found{}; HANDLE finder = FindFirstFileW((canonical + L"\\event-*").c_str(), &found);
    if (finder == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) { result.error = JournalError::Enumeration; result.win32Error = error; }
        return result;
    }
    std::vector<std::wstring> names;
    do { names.emplace_back(found.cFileName); } while (FindNextFileW(finder, &found));
    const DWORD last = GetLastError(); FindClose(finder);
    if (last != ERROR_NO_MORE_FILES) { result.error = JournalError::Enumeration; result.win32Error = last; return result; }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        GuardedPath file; std::string bytes; UninstallJournalEvent event;
        if (!file.Open(canonical, name)) { result.error = JournalError::InvalidFile; result.win32Error = file.Error(); return result; }
        if (!ReadJournalBytes(file, bytes, result.win32Error)) { result.error = JournalError::Read; return result; }
        if (!DecodeUninstallJournalEvent(bytes, HexSha256(plan.identity.sha256), event) || name != JournalFileName(event.sequence) ||
            !result.cursor.Accept(event)) { result.error = JournalError::InvalidFile; return result; }
    }
    return result;
}
}
