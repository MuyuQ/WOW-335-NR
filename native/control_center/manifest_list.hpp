// WOW-335-NR 原生控制中心：移入清单导出（Markdown）。
//
// 对应旧实现 Export-Dlss5ManifestList（Core:195-257）：按产品内置安装清单与组件锁
// 生成“需要移入客户端的文件”核对表。没有选择客户端时只输出目标/目录/来源/必需/
// 用途；选择客户端后追加“状态”列（目录冲突 / 待移入 / 已就位 / 已存在（内容不同））。
//
// 策略修正（旧缺陷 D33）：文件已存在时统一报错“输出文件已存在。”，不再让保存
// 对话框先询问覆盖，避免“确认覆盖后仍失败”。
#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "diagnostics_export.hpp"  // diagnostics_detail::{Widen, Narrow, Join, FileExists, DirectoryExists}
#include "file_identity.hpp"
#include "install_preview.hpp"  // HexSha256
#include "strict_json.hpp"

namespace control_center {

struct ManifestListRow {
    std::wstring target;
    std::wstring folder;
    std::wstring source;  // 中文来源说明
    std::wstring purpose;
    std::wstring status;  // 仅选择客户端时非空
    bool required = true;
};

struct ManifestResidueRow {
    std::wstring path;
    std::wstring note;
    bool removeOnUninstall = true;
};

struct ManifestListBuild {
    bool ok = false;
    std::wstring error;
    std::wstring markdown;
    std::vector<ManifestListRow> rows;
    std::vector<ManifestResidueRow> residue;
};

struct ManifestListWriteResult {
    bool ok = false;
    DWORD win32Error = ERROR_SUCCESS;
    std::wstring error;
    explicit operator bool() const { return ok; }
};

namespace manifest_detail {

inline std::wstring StringField(const Json& object, const char* key) {
    if (!object.contains(key) || !object[key].is_string()) return std::wstring();
    return diagnostics_detail::Widen(object[key].get<std::string>());
}

inline std::wstring SourceText(const std::string& sourceKind) {
    if (sourceKind == "stage") return L"组件暂存";
    if (sourceKind == "template") return L"模板";
    if (sourceKind == "runtime") return L"运行时";
    return L"未知来源";
}

// 状态词按旧实现：同名目录 / 待移入 / 已就位 / 已存在（内容不同）。
inline std::wstring StatusForRow(const std::wstring& clientRoot, const CatalogMember& member) {
    using namespace diagnostics_detail;
    const std::wstring path = Join(clientRoot, member.target);
    if (DirectoryExists(path)) return L"目录冲突";
    if (!FileExists(path)) return L"待移入";
    const HashResult hashed = HashFile(path);
    if (hashed && HexSha256(hashed.identity.sha256) == member.sha256) return L"已就位";
    return L"已存在（内容不同）";
}

inline std::wstring CellText(const std::wstring& text) {
    if (text.empty()) return L"—";
    std::wstring escaped;
    escaped.reserve(text.size() + 4);
    for (const wchar_t ch : text) {
        if (ch == L'|') escaped += L"\\|";
        else if (ch == L'\r' || ch == L'\n') escaped += L' ';
        else escaped.push_back(ch);
    }
    return escaped;
}

inline std::wstring BuildMarkdown(const ManifestListBuild& build, bool hasClient) {
    std::wstring markdown;
    markdown += L"# WOW-335-NR 客户端安装清单\r\n";
    markdown += L"\r\n";
    markdown += L"本清单由控制中心根据内置安装清单（install-manifest.json）与组件锁定（payload-lock.json）生成，"
                L"用于核对需要移入客户端根目录的文件。\r\n";
    markdown += L"\r\n";
    markdown += hasClient
                    ? L"“状态”列按当前所选客户端的实际文件计算：待移入表示文件不存在，已就位表示 SHA-256 与组件锁一致。\r\n"
                    : L"未选择客户端，因此不显示状态列。\r\n";
    markdown += L"\r\n";
    markdown += L"| 目标 | 目录 | 来源 | 必需 | 用途 |";
    if (hasClient) markdown += L" 状态 |";
    markdown += L"\r\n";
    markdown += L"| --- | --- | --- | --- | --- |";
    if (hasClient) markdown += L" --- |";
    markdown += L"\r\n";
    for (const ManifestListRow& row : build.rows) {
        markdown += L"| `" + CellText(row.target) + L"` | " + CellText(row.folder) + L" | " +
                    CellText(row.source) + L" | " + (row.required ? L"必需" : L"可选") + L" | " +
                    CellText(row.purpose) + L" |";
        if (hasClient) markdown += L" " + CellText(row.status) + L" |";
        markdown += L"\r\n";
    }
    markdown += L"\r\n";
    markdown += L"## 卸载时会删除的运行产物\r\n";
    markdown += L"\r\n";
    if (build.residue.empty()) {
        markdown += L"（清单未声明运行产物。）\r\n";
    } else {
        for (const ManifestResidueRow& row : build.residue) {
            markdown += L"- `" + CellText(row.path) + L"`（" +
                        (row.removeOnUninstall ? L"删除" : L"只报告，不删除") + L"）：" + CellText(row.note) +
                        L"\r\n";
        }
    }
    markdown += L"\r\n";
    markdown += L"安装记录保存在 `host64\\dlss5-install-record.json`；安装前被替换的原件备份在 "
                L"`host64\\dlss5-backup`，卸载或恢复时按记录还原。\r\n";
    return markdown;
}

}  // namespace manifest_detail

// 解析安装清单与组件锁，生成 Markdown 正文（clientRoot 为空时不含状态列）。
inline ManifestListBuild BuildManifestListMarkdown(const std::wstring& clientRoot, const std::string& manifestText,
                                                   const CatalogResult& catalog) {
    using namespace manifest_detail;
    ManifestListBuild build;
    if (!catalog) {
        build.error = L"安装清单无效。";
        return build;
    }
    const JsonResult parsed = ParseJsonObject(manifestText);
    if (!parsed) {
        build.error = L"安装清单不是合法 JSON。";
        return build;
    }
    const Json& document = parsed.value;
    if (!document.contains("entries") || !document["entries"].is_array()) {
        build.error = L"安装清单缺少 entries 数组。";
        return build;
    }
    std::map<std::wstring, const CatalogMember*> byTarget;
    for (const CatalogMember& member : catalog.members) byTarget[member.target] = &member;
    const bool hasClient = !clientRoot.empty();
    for (const Json& entry : document["entries"]) {
        if (!entry.is_object()) continue;
        if (!entry.contains("target") || !entry["target"].is_string()) continue;
        ManifestListRow row;
        const std::wstring target = diagnostics_detail::Widen(entry["target"].get<std::string>());
        row.target = target;
        row.folder = StringField(entry, "folder");
        row.purpose = StringField(entry, "purpose");
        if (entry.contains("required") && entry["required"].is_boolean()) {
            row.required = entry["required"].get<bool>();
        }
        const auto found = byTarget.find(target);
        if (found != byTarget.end()) {
            row.source = SourceText(found->second->sourceKind);
            if (hasClient) row.status = StatusForRow(clientRoot, *found->second);
        }
        build.rows.push_back(std::move(row));
    }
    if (document.contains("residue") && document["residue"].is_array()) {
        for (const Json& entry : document["residue"]) {
            if (!entry.is_object()) continue;
            if (!entry.contains("path") || !entry["path"].is_string()) continue;
            ManifestResidueRow row;
            row.path = diagnostics_detail::Widen(entry["path"].get<std::string>());
            row.note = StringField(entry, "note");
            if (entry.contains("policy") && entry["policy"].is_string()) {
                row.removeOnUninstall = entry["policy"].get<std::string>() == "delete";
            }
            build.residue.push_back(std::move(row));
        }
    }
    build.markdown = BuildMarkdown(build, hasClient);
    build.ok = true;
    return build;
}

inline ManifestListWriteResult WriteManifestListFile(const std::wstring& path, const std::wstring& markdown) {
    using namespace diagnostics_detail;
    ManifestListWriteResult result;
    if (path.empty()) {
        result.error = L"输出文件路径为空。";
        return result;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        result.error = L"输出文件已存在。";
        return result;
    }
    const std::string bytes = Narrow(markdown);
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.win32Error = GetLastError();
        result.error = L"无法创建输出文件。";
        return result;
    }
    size_t total = 0;
    bool wrote = true;
    while (total < bytes.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(bytes.size() - total);
        if (!WriteFile(file, bytes.data() + total, chunk, &written, nullptr) || written == 0) {
            wrote = false;
            break;
        }
        total += written;
    }
    const BOOL flushed = wrote && FlushFileBuffers(file);
    if (!wrote || !flushed) result.win32Error = GetLastError();
    CloseHandle(file);
    if (!wrote || !flushed) {
        DeleteFileW(path.c_str());
        result.error = L"写入输出文件失败。";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace control_center
