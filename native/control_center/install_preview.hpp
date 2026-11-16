#pragma once

#include <windows.h>
#include <array>
#include <string>
#include <vector>
#include "catalog.hpp"
#include "file_identity.hpp"
#include "guarded_path.hpp"

namespace control_center {
enum class PreviewState { Absent, Present, Same, MissingOptional, Invalid };
struct PreviewItem {
    CatalogMember member;
    PreviewState state = PreviewState::Invalid;
    std::string issue;
    FileIdentity source{}, existing{};
};
struct InstallPreview {
    std::vector<PreviewItem> items;
    // File checks alone do not establish that the client or processes are safe.
    bool filesReady = false;
    std::string error;
};
inline std::string HexSha256(const std::array<std::uint8_t, 32>& hash) {
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (const auto b : hash) { text.push_back(hex[b >> 4]); text.push_back(hex[b & 15]); }
    return text;
}
inline bool MissingPathError(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Read-only snapshot. Every source and target must be reopened and revalidated
// by the future transaction immediately before mutation; no preview identity
// grants permission to overwrite a file later.
inline InstallPreview PreviewInstall(const std::wstring& clientRoot, const std::wstring& stageRoot,
                                     const std::wstring& templateRoot, const CatalogResult& catalog) {
    InstallPreview result;
    if (!catalog || catalog.members.empty()) return result;
    GuardedPath clientRootGuard, stageRootGuard;
    if (!clientRootGuard.Open(clientRoot, L"__root_check__") ||
        !stageRootGuard.Open(stageRoot, L"__root_check__")) {
        result.error = "client or stage root unavailable or unsafe"; return result;
    }
    result.filesReady = true;
    for (const auto& member : catalog.members) {
        PreviewItem item;
        item.member = member;
        auto fail = [&](const char* issue) { item.issue = issue; item.state = PreviewState::Invalid; result.filesReady = false; };
        GuardedPath source;
        const auto& sourceRoot = member.sourceKind == "template" ? templateRoot : stageRoot;
        if (!source.Open(sourceRoot, member.source)) {
            if (MissingPathError(source.Error()) && !member.required) item.state = PreviewState::MissingOptional;
            else fail("source parent unavailable or unsafe");
            result.items.push_back(std::move(item)); continue;
        }
        const auto sourceHash = HashFile(source.Path());
        if (!sourceHash) {
            if (sourceHash.error == HashError::Open && MissingPathError(sourceHash.win32Error) && !member.required)
                item.state = PreviewState::MissingOptional;
            else fail("source missing or unreadable");
            result.items.push_back(std::move(item)); continue;
        }
        item.source = sourceHash.identity;
        if (HexSha256(item.source.sha256) != member.sha256) {
            fail("source hash mismatch"); result.items.push_back(std::move(item)); continue;
        }
        GuardedPath target;
        if (!target.Open(clientRoot, member.target)) {
            if (MissingPathError(target.Error())) item.state = PreviewState::Absent;
            else fail("target parent unavailable or unsafe");
            result.items.push_back(std::move(item)); continue;
        }
        const auto current = HashFile(target.Path());
        if (!current) {
            if (current.error == HashError::Open && MissingPathError(current.win32Error)) item.state = PreviewState::Absent;
            else fail("target unavailable or unsafe");
            result.items.push_back(std::move(item)); continue;
        }
        item.existing = current.identity;
        if (current.identity.sha256 == item.source.sha256) item.state = PreviewState::Same;
        else if (member.replacesOriginal) item.state = PreviewState::Present;
        else fail("target conflicts with existing file");
        result.items.push_back(std::move(item));
    }
    return result;
}
}
