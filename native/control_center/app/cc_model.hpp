// WOW-335-NR 原生控制中心：只读数据模型。
// 本文件只做文件/系统读取与纯计算，不创建窗口、不触碰 UI；重活由 cc_jobs.hpp 在
// 后台线程调用。所有结果都带 generation，切换客户端后旧结果必须被调用方丢弃。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../catalog.hpp"
#include "../capture_client_record.hpp"
#include "../client_identity.hpp"
#include "../client_record.hpp"
#include "../config_wtf.hpp"
#include "../guarded_path.hpp"
#include "../registered_install_config.hpp"

namespace cc {

using control_center::CapturedRecordState;
using control_center::ClientError;
using control_center::ClientRecord;
using control_center::ProcessState;
using control_center::SameTarget;

inline std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), &output[0], size, nullptr, nullptr);
    return output;
}

inline std::wstring Utf8ToWide(const std::string& input) {
    std::wstring output;
    control_center::Utf8Wide(input, output);
    return output;
}

// ---- 路径与设置 ----
struct AppPaths {
    std::wstring exePath, exeDir, dataDir, settingsPath, logPath;
    bool Init() {
        std::vector<wchar_t> buffer(32768);
        const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!count || count >= buffer.size()) return false;
        exePath.assign(buffer.data(), count);
        const size_t slash = exePath.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return false;
        exeDir = exePath.substr(0, slash);
        // 产品根回退：便携包把资源放在 exe 同目录；日常部署位把控制台放在 bin\ 子目录、
        // 资源放在上一级目录。两种布局都支持：exe 同目录没有清单时改用父目录（存在清单时）。
        const std::wstring manifest = exeDir + L"\\install-manifest.json";
        if (GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const size_t parentSlash = exeDir.find_last_of(L"\\/");
            if (parentSlash != std::wstring::npos && parentSlash > 2) {
                const std::wstring parent = exeDir.substr(0, parentSlash);
                if (GetFileAttributesW((parent + L"\\install-manifest.json").c_str()) != INVALID_FILE_ATTRIBUTES) {
                    exeDir = parent;
                }
            }
        }
        buffer.assign(32768, L'\0');
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
        std::wstring local = (n && n < buffer.size()) ? std::wstring(buffer.data(), n) : exeDir;
        dataDir = local + L"\\WOW-335-NR";
        settingsPath = dataDir + L"\\control-center.json";
        logPath = dataDir + L"\\logs\\control-center.log";
        return true;
    }
};

struct RecentClient {
    std::wstring path, name;
};

struct Settings {
    std::wstring client, stage;
    std::vector<RecentClient> recent;
    // 与旧界面一致：当前客户端排第一，最多保留 5 个仍存在的目录。
    void AddRecent(const std::wstring& path, const std::wstring& name) {
        std::vector<RecentClient> next;
        if (!path.empty()) next.push_back(RecentClient{path, name});
        for (const auto& item : recent) {
            if (item.path.empty()) continue;
            if (!path.empty() && CompareStringOrdinal(item.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL) continue;
            if (GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            next.push_back(item);
            if (next.size() >= 5) break;
        }
        recent = std::move(next);
    }
};

inline bool ReadWholeFile(const std::wstring& path, std::string& output, DWORD& error,
                          std::uint64_t maxBytes = 64ull << 20) {
    output.clear();
    error = ERROR_SUCCESS;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > maxBytes) {
        error = GetLastError();
        CloseHandle(file);
        return false;
    }
    output.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < output.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(output.size() - offset, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(file, &output[offset], chunk, &read, nullptr)) {
            error = GetLastError();
            CloseHandle(file);
            output.clear();
            return false;
        }
        if (read == 0) break;
        offset += read;
    }
    CloseHandle(file);
    output.resize(offset);
    return true;
}

inline Settings LoadSettings(const AppPaths& paths) {
    Settings settings;
    std::string bytes;
    DWORD error = ERROR_SUCCESS;
    if (!ReadWholeFile(paths.settingsPath, bytes, error, 1u << 20)) return settings;
    const auto parsed = control_center::ParseJsonObject(bytes);
    if (!parsed) return settings;
    const auto& root = parsed.value;
    const auto client = root.find("client");
    if (client != root.end() && client->is_string()) settings.client = Utf8ToWide(client->get<std::string>());
    const auto stage = root.find("stage");
    if (stage != root.end() && stage->is_string()) settings.stage = Utf8ToWide(stage->get<std::string>());
    const auto recent = root.find("recent");
    if (recent != root.end() && recent->is_array()) {
        for (const auto& item : *recent) {
            if (!item.is_object()) continue;
            const auto path = item.find("path");
            if (path == item.end() || !path->is_string() || path->get<std::string>().empty()) continue;
            RecentClient entry;
            entry.path = Utf8ToWide(path->get<std::string>());
            const auto name = item.find("name");
            if (name != item.end() && name->is_string()) entry.name = Utf8ToWide(name->get<std::string>());
            settings.recent.push_back(std::move(entry));
        }
    }
    return settings;
}

inline bool SaveSettings(const AppPaths& paths, const Settings& settings) {
    control_center::Json root;
    root["client"] = WideToUtf8(settings.client);
    root["stage"] = WideToUtf8(settings.stage);
    root["recent"] = control_center::Json::array();
    for (const auto& item : settings.recent) {
        control_center::Json entry;
        entry["path"] = WideToUtf8(item.path);
        entry["name"] = WideToUtf8(item.name);
        root["recent"].push_back(std::move(entry));
    }
    const std::string text = root.dump(2);
    if (!CreateDirectoryW(paths.dataDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const std::wstring temp = paths.settingsPath + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), paths.settingsPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

// ---- 客户端识别（与旧界面 Test-ClientRoot 相同的快速判定）----
inline std::wstring DisplayNameForClientPath(const std::wstring& path) {
    if (path.empty()) return L"未选择客户端";
    std::wstring trimmed = path;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    const size_t slash = trimmed.find_last_of(L"\\/");
    const std::wstring leaf = slash == std::wstring::npos ? trimmed : trimmed.substr(slash + 1);
    if (!leaf.empty() && leaf.front() == L'_') {
        const std::wstring parentPath = slash == std::wstring::npos ? std::wstring() : trimmed.substr(0, slash);
        const size_t slash2 = parentPath.find_last_of(L"\\/");
        const std::wstring parent = slash2 == std::wstring::npos ? parentPath : parentPath.substr(slash2 + 1);
        if (!parent.empty()) return parent;
    }
    return leaf;
}

inline bool FileExistsNoReparse(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool DirectoryExistsNoReparse(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

inline bool TestClientRoot(const std::wstring& path) {
    if (path.empty() || !DirectoryExistsNoReparse(path)) return false;
    const std::wstring base = path + L"\\";
    if (!FileExistsNoReparse(base + L"wow.exe")) return false;
    const std::wstring data = base + L"Data\\";
    return FileExistsNoReparse(data + L"common.MPQ") || FileExistsNoReparse(data + L"common-2.MPQ");
}

inline std::vector<std::wstring> ProductProtectedRoots(const AppPaths& paths) {
    std::vector<std::wstring> roots;
    roots.push_back(paths.exeDir);
    roots.push_back(paths.dataDir);
    auto pushEnv = [&roots](const wchar_t* name) {
        std::vector<wchar_t> buffer(32768);
        const DWORD n = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n && n < buffer.size()) roots.push_back(std::wstring(buffer.data(), n));
    };
    std::vector<wchar_t> buffer(32768);
    const DWORD n = GetWindowsDirectoryW(buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n && n < buffer.size()) roots.push_back(std::wstring(buffer.data(), n));
    pushEnv(L"ProgramFiles");
    pushEnv(L"ProgramFiles(x86)");
    pushEnv(L"ProgramData");
    pushEnv(L"USERPROFILE");
    wchar_t systemDir[MAX_PATH]{};
    if (GetSystemDirectoryW(systemDir, MAX_PATH) >= 3) roots.push_back(std::wstring(systemDir, 3));
    return roots;
}

// ---- 安装清单（产品目录内 install-manifest.json + payload-lock.json）----
struct CatalogBundle {
    bool ok = false;
    std::string error;
    control_center::CatalogResult catalog;
    std::wstring manifestPath, lockPath;
};

inline CatalogBundle LoadCatalogBundle(const std::wstring& dir) {
    CatalogBundle bundle;
    bundle.manifestPath = dir + L"\\install-manifest.json";
    bundle.lockPath = dir + L"\\payload-lock.json";
    std::string manifestText, lockText;
    DWORD error = ERROR_SUCCESS;
    if (!ReadWholeFile(bundle.manifestPath, manifestText, error, 8u << 20)) {
        bundle.error = "安装清单读取失败（错误 " + std::to_string(error) + "）";
        return bundle;
    }
    if (!ReadWholeFile(bundle.lockPath, lockText, error, 8u << 20)) {
        bundle.error = "载荷锁定文件读取失败（错误 " + std::to_string(error) + "）";
        return bundle;
    }
    bundle.catalog = control_center::ReadCatalog(manifestText, lockText);
    if (!bundle.catalog) {
        bundle.error = "安装清单无效：" + bundle.catalog.error;
        return bundle;
    }
    bundle.ok = true;
    return bundle;
}

// ---- 只读快照 ----
enum class MemberState { Unknown, Installed, InstalledVerified, Modified, Missing, NotInstalled, OptionalMissing, Unsafe };

inline const wchar_t* MemberStateText(MemberState state) {
    switch (state) {
        case MemberState::Unknown: return L"未知";
        case MemberState::Installed: return L"已安装";
        case MemberState::InstalledVerified: return L"已安装·已校验";
        case MemberState::Modified: return L"已修改";
        case MemberState::Missing: return L"文件缺失";
        case MemberState::NotInstalled: return L"未安装";
        case MemberState::OptionalMissing: return L"可选·未安装";
        case MemberState::Unsafe: return L"路径异常";
    }
    return L"未知";
}

struct MemberStatus {
    std::wstring target;
    bool required = true;
    std::string sourceKind;
    MemberState state = MemberState::Unknown;
};

struct ConfigFpsCap {
    bool exists = false;
    bool readable = false;
    int front = -1, back = -1;
    std::wstring path;
    std::string error;
};

struct GpuInfo {
    bool ok = false;
    std::wstring name;
    std::string error;
};

inline GpuInfo QueryPrimaryGpu() {
    GpuInfo info;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        info.error = "DXGI 工厂创建失败";
        return info;
    }
    GpuInfo software;
    bool hasSoftware = false;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
        DXGI_ADAPTER_DESC1 desc{};
        const bool ok = SUCCEEDED(adapter->GetDesc1(&desc));
        adapter->Release();
        if (!ok) continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            if (!hasSoftware) {
                software.name = desc.Description;
                software.ok = true;
                hasSoftware = true;
            }
            continue;
        }
        info.name = desc.Description;
        info.ok = true;
        break;
    }
    factory->Release();
    if (!info.ok && hasSoftware) return software;
    if (!info.ok) info.error = "未找到显卡";
    return info;
}

// ---- WTF/Config.wtf 只读采样 ----
// 解析与写入都走核心 config_wtf.hpp（按字节处理，保留原始编码与行尾），
// 这里只把结果映射为界面模型。
inline ConfigFpsCap ReadConfigWtf(const std::wstring& clientRoot) {
    ConfigFpsCap cap;
    cap.path = control_center::ConfigWtfPath(clientRoot);
    const auto native = control_center::ReadConfigFpsCap(clientRoot);
    cap.exists = native.exists;
    cap.readable = native.readable;
    cap.front = native.front;
    cap.back = native.back;
    if (!cap.exists && native.win32Error != ERROR_SUCCESS && native.win32Error != ERROR_FILE_NOT_FOUND &&
        native.win32Error != ERROR_PATH_NOT_FOUND) {
        cap.error = "Config.wtf 不可读";
    }
    return cap;
}

struct ClientSnapshot {
    std::uint64_t generation = 0;
    std::wstring clientRoot;
    std::wstring displayName;
    bool clientValid = false;
    ClientError clientError = ClientError::None;
    DWORD clientWin32 = ERROR_SUCCESS;

    bool catalogOk = false;
    std::string catalogError;
    size_t catalogMembers = 0;
    size_t requiredMembers = 0;

    CapturedRecordState recordState = CapturedRecordState::Unavailable;
    // 磁盘上的记录存在，但属于另一个客户端目录：不可用于“已安装”判定、卸载或修复。
    bool recordForeign = false;
    std::string recordError;
    DWORD recordWin32 = ERROR_SUCCESS;
    size_t recordEntries = 0;
    size_t recordResidue = 0;
    bool recordMatchesCatalog = false;

    std::vector<MemberStatus> members;
    size_t installed = 0, verified = 0, missing = 0, modified = 0, notInstalled = 0, unsafe = 0;
    bool hashesChecked = false;

    ProcessState processState = ProcessState::Unknown;
    std::wstring blockingProcess;
    DWORD processWin32 = ERROR_SUCCESS;

    ConfigFpsCap fpsCap;
    GpuInfo gpu;

    std::string error;  // 非空表示本次读取整体失败
    bool valid() const { return error.empty(); }
};

inline void MemberCounts(ClientSnapshot& snapshot) {
    snapshot.installed = snapshot.verified = snapshot.missing = snapshot.modified = snapshot.notInstalled =
        snapshot.unsafe = 0;
    for (const auto& member : snapshot.members) {
        switch (member.state) {
            case MemberState::InstalledVerified: ++snapshot.verified; ++snapshot.installed; break;
            case MemberState::Installed: ++snapshot.installed; break;
            case MemberState::Missing: ++snapshot.missing; break;
            case MemberState::Modified: ++snapshot.modified; break;
            case MemberState::NotInstalled: if (member.required) ++snapshot.notInstalled; break;
            case MemberState::OptionalMissing: break;
            case MemberState::Unsafe: ++snapshot.unsafe; ++snapshot.missing; break;
            default: break;
        }
    }
}

// 快速路径：只判断存在性与安装记录，不计算哈希（不阻塞，首次读取即可用）。
// 安装时因“目标已存在且内容一致”被跳过、因而没有写入安装记录的成员，按文件存在性标记为已安装。
inline ClientSnapshot SnapshotClient(const std::wstring& clientRoot, const CatalogBundle& catalog,
                                     const std::vector<std::wstring>& protectedRoots, std::uint64_t generation) {
    ClientSnapshot snapshot;
    snapshot.generation = generation;
    snapshot.clientRoot = clientRoot;
    snapshot.displayName = DisplayNameForClientPath(clientRoot);
    snapshot.catalogOk = catalog.ok;
    snapshot.catalogError = catalog.error;
    if (!catalog.ok) {
        snapshot.error = catalog.error;
        return snapshot;
    }
    snapshot.catalogMembers = catalog.catalog.members.size();
    for (const auto& member : catalog.catalog.members) {
        if (member.required) ++snapshot.requiredMembers;
    }
    snapshot.gpu = QueryPrimaryGpu();

    auto fillUnknown = [&snapshot, &catalog]() {
        for (const auto& member : catalog.catalog.members) {
            snapshot.members.push_back(
                MemberStatus{member.target, member.required, member.sourceKind, MemberState::Unknown});
        }
    };
    if (clientRoot.empty()) {
        snapshot.error = "未选择客户端";
        fillUnknown();
        return snapshot;
    }
    const auto client = control_center::ValidateClient(clientRoot, protectedRoots);
    snapshot.clientError = client.error;
    snapshot.clientWin32 = client.win32Error;
    snapshot.clientValid = static_cast<bool>(client);
    if (!client) {
        snapshot.error = "客户端目录不可用";
        fillUnknown();
        return snapshot;
    }
    snapshot.clientRoot = client.root;
    snapshot.displayName = DisplayNameForClientPath(client.root);

    const auto captured = control_center::CaptureClientRecord(client.root);
    snapshot.recordState = captured.state;
    snapshot.recordWin32 = captured.win32Error;
    snapshot.recordError = captured.record.error;
    if (captured.state == CapturedRecordState::Present &&
        !control_center::ClientRecordOwnedBy(captured.record, client.root)) {
        snapshot.recordForeign = true;
        snapshot.recordState = CapturedRecordState::Absent;
        snapshot.recordError = "client record belongs to another client";
    }
    if (captured.state == CapturedRecordState::Present && !snapshot.recordForeign) {
        snapshot.recordEntries = captured.record.entries.size();
        snapshot.recordResidue = captured.record.residue.size();
        std::vector<bool> covered(catalog.catalog.members.size(), false);
        for (const auto& entry : captured.record.entries) {
            for (size_t i = 0; i < catalog.catalog.members.size(); ++i) {
                if (SameTarget(catalog.catalog.members[i].target, entry.target)) {
                    covered[i] = true;
                    break;
                }
            }
        }
        snapshot.recordMatchesCatalog = std::find(covered.begin(), covered.end(), false) == covered.end();
    }
    const ClientRecord* record =
        (captured.state == CapturedRecordState::Present && !snapshot.recordForeign) ? &captured.record : nullptr;

    const auto blocking = control_center::CheckBlockingProcesses();
    snapshot.processState = blocking.state;
    snapshot.blockingProcess = blocking.name;
    snapshot.processWin32 = blocking.win32Error;
    snapshot.fpsCap = ReadConfigWtf(client.root);

    for (const auto& member : catalog.catalog.members) {
        MemberStatus status;
        status.target = member.target;
        status.required = member.required;
        status.sourceKind = member.sourceKind;
        status.state = member.required ? MemberState::NotInstalled : MemberState::OptionalMissing;
        if (!record) {
            snapshot.members.push_back(std::move(status));
            continue;
        }
        const control_center::InstalledEntry* installed = nullptr;
        for (const auto& entry : record->entries) {
            if (SameTarget(entry.target, member.target)) {
                installed = &entry;
                break;
            }
        }
        if (!installed) {
            // 安装时被跳过的成员（目标已存在且内容一致）不会出现在安装记录里；
            // 这里按文件存在性判定，避免已经就位的组件仍显示“未安装”。缺失的成员保持未安装语义。
            control_center::GuardedPath guard;
            if (guard.Open(client.root, member.target)) {
                const DWORD attributes = GetFileAttributesW(guard.Path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) {
                    status.state = member.required ? MemberState::NotInstalled : MemberState::OptionalMissing;
                } else if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
                    status.state = MemberState::Unsafe;
                } else {
                    status.state = MemberState::Installed;
                }
            } else {
                const DWORD openError = guard.Error();
                if (openError != ERROR_FILE_NOT_FOUND && openError != ERROR_PATH_NOT_FOUND) {
                    status.state = MemberState::Unsafe;
                }
            }
            snapshot.members.push_back(std::move(status));
            continue;
        }
        control_center::GuardedPath guard;
        if (!guard.Open(client.root, member.target)) {
            const DWORD openError = guard.Error();
            status.state = (openError == ERROR_FILE_NOT_FOUND || openError == ERROR_PATH_NOT_FOUND)
                               ? MemberState::Missing
                               : MemberState::Unsafe;
            snapshot.members.push_back(std::move(status));
            continue;
        }
        const DWORD attributes = GetFileAttributesW(guard.Path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) status.state = MemberState::Missing;
        else if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) status.state = MemberState::Unsafe;
        else status.state = MemberState::Installed;
        snapshot.members.push_back(std::move(status));
    }
    MemberCounts(snapshot);
    return snapshot;
}

// 慢速路径：对已就位成员计算哈希并核对清单（显式“检查状态”时在后台调用）。
// 安装记录覆盖的成员按记录里的哈希比对；安装时被跳过的成员按清单里的 sha256 比对。
inline void VerifySnapshotHashes(ClientSnapshot& snapshot, const CatalogBundle& catalog) {
    if (!snapshot.clientValid || !catalog.ok) return;
    if (snapshot.members.size() != catalog.catalog.members.size()) return;
    const auto captured = control_center::CaptureClientRecord(snapshot.clientRoot);
    if (captured.state != CapturedRecordState::Present) return;
    for (size_t i = 0; i < snapshot.members.size(); ++i) {
        auto& status = snapshot.members[i];
        const auto& member = catalog.catalog.members[i];
        if (status.state != MemberState::Installed) continue;
        std::string expected = member.sha256;
        for (const auto& candidate : captured.record.entries) {
            if (SameTarget(candidate.target, member.target)) {
                expected = candidate.sha256;
                break;
            }
        }
        if (expected.empty()) continue;
        control_center::GuardedPath guard;
        if (!guard.Open(snapshot.clientRoot, member.target)) {
            status.state = MemberState::Unsafe;
            continue;
        }
        const auto hash = control_center::HashFile(guard.Path());
        if (!hash) {
            status.state = (hash.error == control_center::HashError::Open &&
                            control_center::MissingPathError(hash.win32Error))
                               ? MemberState::Missing
                               : MemberState::Unsafe;
            continue;
        }
        status.state = control_center::HexSha256(hash.identity.sha256) == expected ? MemberState::InstalledVerified
                                                                                   : MemberState::Modified;
    }
    snapshot.hashesChecked = true;
    MemberCounts(snapshot);
}

// ---- 客户端扫描（后台线程调用，可能耗时数秒）----
struct ClientCandidate {
    std::wstring path, name;
    bool hasRecord = false;
    bool hasReshade = false;
    bool hasZhcn = false;
    FILETIME configTime{};
};

inline std::vector<ClientCandidate> ScanClientRoots(int maxComponents = 4, size_t maxDirs = 700) {
    std::vector<ClientCandidate> found;
    const std::vector<std::wstring> skip = {L"Windows", L"Program Files", L"Program Files (x86)", L"ProgramData",
                                            L"Users", L"$Recycle.Bin", L"System Volume Information", L"Recovery",
                                            L"PerfLogs", L"Windows.old", L"AppData", L"node_modules", L"_scratch"};
    const DWORD drives = GetLogicalDrives();
    for (int drive = 0; drive < 26; ++drive) {
        if (!(drives & (1u << drive))) continue;
        wchar_t root[4] = {static_cast<wchar_t>(L'A' + drive), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        std::vector<std::wstring> queue;
        queue.push_back(root);
        size_t visited = 0;
        size_t head = 0;
        while (head < queue.size() && visited < maxDirs) {
            const std::wstring dir = queue[head++];
            ++visited;
            if (TestClientRoot(dir)) {
                ClientCandidate candidate;
                candidate.path = dir;
                candidate.name = DisplayNameForClientPath(dir);
                candidate.hasRecord = FileExistsNoReparse(dir + L"\\host64\\dlss5-install-record.json");
                candidate.hasReshade = FileExistsNoReparse(dir + L"\\host64\\ReShade.ini");
                WIN32_FILE_ATTRIBUTE_DATA attributes{};
                if (GetFileAttributesExW((dir + L"\\Data\\zhCN").c_str(), GetFileExInfoStandard, &attributes))
                    candidate.hasZhcn = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (GetFileAttributesExW((dir + L"\\WTF\\Config.wtf").c_str(), GetFileExInfoStandard, &attributes))
                    candidate.configTime = attributes.ftLastWriteTime;
                found.push_back(std::move(candidate));
                continue;
            }
            const std::wstring pattern = dir.back() == L'\\' ? dir + L"*" : dir + L"\\*";
            WIN32_FIND_DATAW data{};
            HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
            if (handle == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (data.cFileName[0] == L'.') continue;
                bool skipped = false;
                for (const auto& name : skip) {
                    if (CompareStringOrdinal(data.cFileName, -1, name.c_str(), -1, TRUE) == CSTR_EQUAL) {
                        skipped = true;
                        break;
                    }
                }
                if (skipped) continue;
                std::wstring child = dir;
                if (child.back() != L'\\') child += L'\\';
                child += data.cFileName;
                int components = 0;
                for (size_t i = 3; i < child.size(); ++i) {
                    if (child[i] == L'\\') ++components;
                }
                if (components + 1 > maxComponents) continue;
                queue.push_back(std::move(child));
            } while (FindNextFileW(handle, &data));
            FindClose(handle);
        }
    }
    std::sort(found.begin(), found.end(), [](const ClientCandidate& a, const ClientCandidate& b) {
        if (a.hasRecord != b.hasRecord) return a.hasRecord;
        if (a.hasReshade != b.hasReshade) return a.hasReshade;
        if (a.hasZhcn != b.hasZhcn) return a.hasZhcn;
        const int timeOrder = CompareFileTime(&a.configTime, &b.configTime);
        if (timeOrder != 0) return timeOrder > 0;
        return a.path.size() < b.path.size();
    });
    if (found.size() > 8) found.resize(8);
    return found;
}

// ---- 内存日志（写入文件由调用方决定）----
class Log {
public:
    void Add(const std::wstring& line) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t stamp[24]{};
        swprintf_s(stamp, L"%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(std::wstring(stamp) + L"  " + line);
        if (lines_.size() > 300) lines_.erase(lines_.begin(), lines_.begin() + 100);
        revision_.fetch_add(1);
    }
    std::vector<std::wstring> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }
    std::uint64_t Revision() const { return revision_.load(); }
private:
    mutable std::mutex mutex_;
    std::vector<std::wstring> lines_;
    std::atomic<std::uint64_t> revision_{0};
};

}  // namespace cc
