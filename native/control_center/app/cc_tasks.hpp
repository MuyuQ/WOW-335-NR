#pragma once

// 后台任务：安装、修复、卸载完整作业。所有函数在工作线程内执行，自行获取并
// 持有客户端锁（ClientLock 具有线程归属），不访问界面对象，只返回结果。

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cc_model.hpp"

#include "../capture_client_record.hpp"
#include "../catalog.hpp"
#include "../client_lock.hpp"
#include "../coordinated_install.hpp"
#include "../coordinated_uninstall.hpp"
#include "../install_job.hpp"
#include "../install_preview.hpp"
#include "../repair_job.hpp"
#include "../repair_preview.hpp"
#include "../start_with_recovery.hpp"
#include "../strict_json.hpp"
#include "../uninstall_finalize.hpp"
#include "../uninstall_finalize_prepare.hpp"
#include "../uninstall_member.hpp"
#include "../uninstall_plan.hpp"
#include "../uninstall_plan_read.hpp"
#include "../uninstall_preview.hpp"
#include "../ds5_schemes.hpp"
#include "../gpu_stats.hpp"
#include "../panel_process.hpp"
#include "../pipeline_switch.hpp"
#include "../diagnostics_export.hpp"
#include "../manifest_list.hpp"
#include "../restore_operation.hpp"
#include "../window_watch.hpp"

namespace cc {

struct TaskProgress {
    // 0=准备 1=执行成员 2=最终化 3=收尾
    std::atomic<int> stage{0};
    std::atomic<size_t> done{0};
    std::atomic<size_t> total{0};
};

inline std::wstring Win32ErrorText(DWORD error) {
    if (error == ERROR_SUCCESS) return L"";
    wchar_t buffer[256] = {};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
                                        0, buffer, 256, nullptr);
    std::wstring text = length ? std::wstring(buffer, length) : std::wstring();
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    wchar_t code[32] = {};
    swprintf_s(code, L"%lu", static_cast<unsigned long>(error));
    if (text.empty()) return std::wstring(L"Win32 错误 ") + code;
    return text + L"（" + code + L"）";
}

inline std::wstring JsonErrorText(control_center::JsonError error) {
    using control_center::JsonError;
    switch (error) {
        case JsonError::None: return L"无错误";
        case JsonError::DuplicateKey: return L"对象里存在重复键";
        case JsonError::Invalid: return L"不是合法 JSON";
        case JsonError::RootNotObject: return L"根节点不是对象";
    }
    return L"未知 JSON 错误";
}

inline std::string UtcTimestamp() {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay),
                  static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond));
    return buffer;
}

inline std::wstring NewOperationId() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return std::wstring();
    wchar_t text[33] = {};
    swprintf_s(text, L"%08lx%04hx%04hx%02x%02x%02x%02x%02x%02x%02x%02x",
               static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
               guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

// 产品自身数据目录下的三个事务根。启动时创建，失败不阻断界面。
inline std::wstring OperationsRoot(const AppPaths& paths) { return paths.dataDir + L"\\operations"; }
inline std::wstring RepairsRoot(const AppPaths& paths) { return paths.dataDir + L"\\repairs"; }
inline std::wstring UninstallsRoot(const AppPaths& paths) { return paths.dataDir + L"\\uninstalls"; }

inline bool EnsureTransactionRoots(const AppPaths& paths, std::wstring& error) {
    for (const auto& root : {OperationsRoot(paths), RepairsRoot(paths), UninstallsRoot(paths)}) {
        if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            error = L"无法创建事务目录：" + root + L"，" + Win32ErrorText(GetLastError());
            return false;
        }
    }
    return true;
}

inline bool LoadManifest(const AppPaths& paths, const CatalogBundle& bundle, control_center::Json& manifest,
                         std::wstring& error) {
    std::string bytes;
    DWORD readError = ERROR_SUCCESS;
    if (!ReadWholeFile(bundle.manifestPath, bytes, readError, 8u << 20)) {
        error = L"无法读取安装清单：" + bundle.manifestPath + L"，" + Win32ErrorText(readError);
        return false;
    }
    auto parsed = control_center::ParseJsonObject(bytes);
    if (!parsed) {
        error = L"安装清单不是严格 JSON：" + JsonErrorText(parsed.error);
        return false;
    }
    manifest = parsed.value;
    (void)paths;
    return true;
}

inline std::wstring StateText(control_center::InstallJobState state) {
    using control_center::InstallJobState;
    switch (state) {
        case InstallJobState::Completed: return L"已完成";
        case InstallJobState::AlreadyCurrent: return L"已是最新";
        case InstallJobState::RolledBack: return L"失败，已回滚";
        case InstallJobState::NeedsAttention: return L"需要人工检查";
        case InstallJobState::PreconditionFailed: return L"前置条件不满足";
    }
    return L"未知";
}

inline std::wstring StateText(control_center::RepairJobState state) {
    using control_center::RepairJobState;
    switch (state) {
        case RepairJobState::Completed: return L"已完成";
        case RepairJobState::AlreadyCurrent: return L"已是最新";
        case RepairJobState::RolledBack: return L"失败，已回滚";
        case RepairJobState::NeedsAttention: return L"需要人工检查";
        case RepairJobState::PreconditionFailed: return L"前置条件不满足";
    }
    return L"未知";
}

struct InstallTaskResult {
    bool ok = false;
    std::wstring error;
    control_center::InstallJobState state = control_center::InstallJobState::PreconditionFailed;
    size_t applied = 0;
    bool committedAfterRecovery = false;
};

struct RepairTaskResult {
    bool ok = false;
    std::wstring error;
    control_center::RepairJobState state = control_center::RepairJobState::PreconditionFailed;
    size_t repaired = 0;
    bool cleanupPending = false;
    bool committedAfterRecovery = false;
};

struct UninstallTaskResult {
    bool ok = false;
    bool recovered = false;
    std::wstring error;
    size_t processed = 0;
    bool finalized = false;
    control_center::UninstallFinalizeState finalizeState = control_center::UninstallFinalizeState::PreconditionFailed;
    control_center::UninstallFinalizeResult finalize;
};

inline std::wstring Utf8ErrorText(const std::string& text) {
    if (text.empty()) return L"";
    return Utf8ToWide(text);
}

inline std::wstring PreviewIssueReason(const std::string& issue) {
    if (issue == "source parent unavailable or unsafe") return L"源目录不可用或路径不安全";
    if (issue == "source missing or unreadable") return L"源文件缺失或不可读";
    if (issue == "source hash mismatch") return L"源文件哈希与清单不符";
    if (issue == "target parent unavailable or unsafe") return L"目标目录不可用或路径不安全";
    if (issue == "target unavailable or unsafe") return L"目标文件不可读";
    if (issue == "target conflicts with existing file") return L"目标位置已有内容不同的文件";
    return L"未知原因";
}

// 把安装预检失败翻译成可读的逐项原因；用于安装作业与检查步骤的提示。
inline std::wstring PreviewIssueText(const control_center::InstallPreview& preview) {
    std::vector<std::wstring> parts;
    size_t invalid = 0;
    for (const auto& item : preview.items) {
        if (item.state != control_center::PreviewState::Invalid) continue;
        ++invalid;
        if (parts.size() < 3) {
            parts.push_back(item.member.target + L"（" + PreviewIssueReason(item.issue) + L"）");
        }
    }
    if (invalid == 0) return L"部分文件不可用";
    std::wstring text = std::to_wstring(invalid) + L" 个文件未通过检查：";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) text += L"、";
        text += parts[i];
    }
    if (invalid > parts.size()) text += L" 等 " + std::to_wstring(invalid) + L" 项";
    return text;
}

inline InstallTaskResult RunInstallTask(const std::wstring& clientRoot, const std::wstring& stageRoot,
                                        const std::wstring& templateRoot, const control_center::CatalogResult& catalog,
                                        const std::wstring& manifestPath, const std::vector<std::wstring>& protectedRoots,
                                        const AppPaths& paths, const std::shared_ptr<TaskProgress>& progress) {
    using namespace control_center;
    InstallTaskResult result;
    if (!clientRoot.empty() && !stageRoot.empty()) {
        std::string bytes;
        DWORD readError = ERROR_SUCCESS;
        if (!ReadWholeFile(manifestPath, bytes, readError, 8u << 20)) {
            result.error = L"无法读取安装清单：" + Win32ErrorText(readError);
            return result;
        }
        auto parsed = ParseJsonObject(bytes);
        if (!parsed) {
            result.error = L"安装清单不是严格 JSON：" + JsonErrorText(parsed.error);
            return result;
        }
        ClientLock lock;
        if (!lock.Acquire(clientRoot)) {
            result.error = L"无法获取客户端锁：" +
                           (lock.Error() == ClientLockError::Busy ? std::wstring(L"另一个操作正在进行") : Win32ErrorText(lock.Win32Error()));
            return result;
        }
        progress->stage.store(0);
        const auto preview = PreviewInstall(clientRoot, stageRoot, templateRoot, catalog);
        if (!preview.error.empty() || !preview.filesReady) {
            result.error = L"安装源文件校验失败：" +
                           (preview.error.empty() ? PreviewIssueText(preview) : Utf8ToWide(preview.error));
            return result;
        }
        MemberInstallContext context;
        context.clientRoot = clientRoot;
        context.stageRoot = stageRoot;
        context.templateRoot = templateRoot;
        context.protectedRoots = protectedRoots;
        context.operationId = NewOperationId();
        if (!ValidOperationId(context.operationId)) {
            result.error = L"无法生成事务编号。";
            return result;
        }
        context.journalRoot = OperationsRoot(paths) + L"\\" + context.operationId;
        progress->total.store(preview.items.size());
        progress->stage.store(1);
        std::wstring rootError;
        if (!EnsureTransactionRoots(paths, rootError)) {
            result.error = rootError;
            return result;
        }
        const auto run = RunCoordinatedInstall(OperationsRoot(paths), RepairsRoot(paths), UninstallsRoot(paths),
                                               OperationsRoot(paths), context, catalog, parsed.value, preview,
                                               UtcTimestamp(), lock);
        result.state = run.installation.state;
        result.applied = run.installation.applied;
        result.committedAfterRecovery = run.installation.committedAfterRecovery;
        result.ok = run.attemptedInstallation && run.installation.state == InstallJobState::Completed;
        if (!result.ok) {
            if (!run.attemptedInstallation)
                result.error = L"恢复检查未通过：" + Utf8ErrorText(run.recovery.reason);
            else
                result.error = StateText(run.installation.state) + L"：" + Utf8ErrorText(run.installation.reason);
        }
        progress->done.store(result.applied);
        progress->stage.store(3);
    } else {
        result.error = L"没有可用的客户端或安装来源。";
    }
    return result;
}

inline RepairTaskResult RunRepairTask(const std::wstring& clientRoot, const control_center::CatalogResult& catalog,
                                      const std::wstring& manifestPath, const std::vector<std::wstring>& protectedRoots,
                                      const AppPaths& paths, const std::shared_ptr<TaskProgress>& progress) {
    using namespace control_center;
    RepairTaskResult result;
    std::string bytes;
    DWORD readError = ERROR_SUCCESS;
    if (!ReadWholeFile(manifestPath, bytes, readError, 8u << 20)) {
        result.error = L"无法读取安装清单：" + Win32ErrorText(readError);
        return result;
    }
    auto parsed = ParseJsonObject(bytes);
    if (!parsed) {
        result.error = L"安装清单不是严格 JSON：" + JsonErrorText(parsed.error);
        return result;
    }
    ClientLock lock;
    if (!lock.Acquire(clientRoot)) {
        result.error = L"无法获取客户端锁：" +
                       (lock.Error() == ClientLockError::Busy ? std::wstring(L"另一个操作正在进行") : Win32ErrorText(lock.Win32Error()));
        return result;
    }
    const auto captured = CaptureClientRecord(clientRoot);
    if (captured.state != CapturedRecordState::Present) {
        result.error = L"客户端没有可修复的安装记录。";
        result.state = RepairJobState::PreconditionFailed;
        return result;
    }
    std::wstring operation;
    const auto active = captured.record.original.find("operation");
    if (active == captured.record.original.end() || !active->is_string() ||
        !Utf8Wide(active->get<std::string>(), operation)) {
        result.error = L"安装记录未绑定操作目录，无法定位修复来源。";
        return result;
    }
    std::wstring canonicalOperation, canonicalRoot;
    if (!CanonicalLocalPath(operation, canonicalOperation) || !CanonicalLocalPath(OperationsRoot(paths), canonicalRoot) ||
        !WithinRoot(canonicalOperation, canonicalRoot)) {
        result.error = L"安装记录中的操作目录不安全。";
        return result;
    }
    progress->stage.store(0);
    const auto preview = PreviewRepair(clientRoot, OperationsRoot(paths), canonicalOperation, protectedRoots, catalog, lock);
    if (!preview.valid) {
        result.error = L"修复预检失败：" + Utf8ToWide(preview.error);
        return result;
    }
    const std::wstring repairId = NewOperationId();
    if (!ValidOperationId(repairId)) {
        result.error = L"无法生成修复编号。";
        return result;
    }
    std::wstring rootError;
    if (!EnsureTransactionRoots(paths, rootError)) {
        result.error = rootError;
        return result;
    }
    progress->stage.store(1);
    const auto run = RunRepairJob(OperationsRoot(paths), RepairsRoot(paths), UninstallsRoot(paths), OperationsRoot(paths),
                                  canonicalOperation, repairId, clientRoot, protectedRoots, catalog, parsed.value, preview, lock);
    result.state = run.state;
    result.repaired = run.repaired;
    result.cleanupPending = run.cleanupPending;
    result.committedAfterRecovery = run.committedAfterRecovery;
    result.ok = run.state == RepairJobState::Completed || run.state == RepairJobState::AlreadyCurrent;
    if (!result.ok) result.error = StateText(run.state) + L"：" + Utf8ErrorText(run.reason);
    progress->done.store(run.repaired);
    progress->stage.store(3);
    return result;
}

inline UninstallTaskResult RunUninstallTask(const std::wstring& clientRoot, const control_center::CatalogResult& catalog,
                                           const std::wstring& manifestPath, const std::vector<std::wstring>& protectedRoots,
                                           const AppPaths& paths, const std::shared_ptr<TaskProgress>& progress) {
    using namespace control_center;
    UninstallTaskResult result;
    std::string bytes;
    DWORD readError = ERROR_SUCCESS;
    if (!ReadWholeFile(manifestPath, bytes, readError, 8u << 20)) {
        result.error = L"无法读取安装清单：" + Win32ErrorText(readError);
        return result;
    }
    auto parsed = ParseJsonObject(bytes);
    if (!parsed) {
        result.error = L"安装清单不是严格 JSON：" + JsonErrorText(parsed.error);
        return result;
    }
    ClientLock lock;
    if (!lock.Acquire(clientRoot)) {
        result.error = L"无法获取客户端锁：" +
                       (lock.Error() == ClientLockError::Busy ? std::wstring(L"另一个操作正在进行") : Win32ErrorText(lock.Win32Error()));
        return result;
    }
    std::wstring rootError;
    if (!EnsureTransactionRoots(paths, rootError)) {
        result.error = rootError;
        return result;
    }
    progress->stage.store(0);
    const auto preview = PreviewUninstall(clientRoot, protectedRoots, catalog, parsed.value, lock);
    if (!preview.valid) {
        result.error = L"卸载预检失败：" + Utf8ToWide(preview.error);
        return result;
    }
    if (!preview.canUninstall) {
        result.error = L"卸载预检发现无法安全还原的文件，已停止。";
        return result;
    }
    const std::wstring uninstallId = NewOperationId();
    if (!ValidOperationId(uninstallId)) {
        result.error = L"无法生成卸载编号。";
        return result;
    }
    const auto start = StartCoordinatedUninstall(OperationsRoot(paths), RepairsRoot(paths), UninstallsRoot(paths),
                                                 OperationsRoot(paths), clientRoot, uninstallId, protectedRoots, catalog,
                                                 parsed.value, preview, lock);
    // Recovery may have completed an interrupted uninstall before planning. When the client
    // record is gone afterwards the remaining items are already handled, so this run is done.
    if (start.recovery.ready && start.recovery.recoveredUninstall && start.plan.state != UninstallPlanState::Ready) {
        result.ok = true;
        result.recovered = true;
        result.finalized = true;
        result.finalizeState = UninstallFinalizeState::AlreadyCompleted;
        result.processed = 0;
        progress->stage.store(3);
        return result;
    }
    if (!start.attemptedPlan || start.plan.state != UninstallPlanState::Ready) {
        result.error = L"无法建立卸载计划：" +
                       (start.recovery.ready ? Utf8ErrorText(start.plan.reason) : Utf8ErrorText(start.recovery.reason));
        return result;
    }
    progress->total.store(start.plan.items);
    progress->stage.store(1);
    for (size_t index = 0; index < start.plan.items; ++index) {
        const auto applied = ApplyUninstallMember(clientRoot, UninstallsRoot(paths), uninstallId, protectedRoots,
                                                  catalog, parsed.value, index, lock);
        if (applied.state != UninstallMemberState::Applied && applied.state != UninstallMemberState::NoWork) {
            result.error = L"卸载成员失败：" + Utf8ErrorText(applied.reason);
            result.processed = index;
            return result;
        }
        result.processed = index + 1;
        progress->done.store(result.processed);
    }
    progress->stage.store(2);
    const auto prepared = PrepareUninstallFinalization(clientRoot, UninstallsRoot(paths), uninstallId, protectedRoots,
                                                       catalog, parsed.value, lock);
    if (!prepared.ready) {
        result.error = L"卸载最终化准备失败：" + Utf8ErrorText(prepared.reason);
        return result;
    }
    const auto finalized = FinalizeUninstall(clientRoot, UninstallsRoot(paths), uninstallId, protectedRoots, catalog,
                                             parsed.value, lock, UtcTimestamp());
    result.finalize = finalized;
    result.finalizeState = finalized.state;
    result.finalized = finalized.state == UninstallFinalizeState::Completed ||
                       finalized.state == UninstallFinalizeState::AlreadyCompleted;
    result.ok = result.finalized;
    if (!result.ok) result.error = L"卸载最终化失败：" + Utf8ErrorText(finalized.reason);
    progress->stage.store(3);
    return result;
}

// 档位 / 高级参数 / 方案（ReShade.ini [Standalone.DLSSNR]）。
// 读取与方案目录扫描同样放在工作线程，界面线程只消费结果。
struct AdvancedReadTaskResult {
    bool ok = false;
    std::wstring error;
    control_center::Ds5AdvancedSettings settings;
    std::wstring tierName = L"自定义";
    std::wstring modelName = L"默认";
};

struct AdvancedSaveTaskResult {
    bool ok = false;
    std::wstring error;
    control_center::Ds5WriteResult write;
};

struct SchemesTaskResult {
    bool ok = false;
    std::wstring error;
    std::vector<control_center::Ds5Scheme> schemes;
};

struct SchemeSaveTaskResult {
    bool ok = false;
    std::wstring error;
    std::wstring path;
};

struct SchemeApplyTaskResult {
    bool ok = false;
    std::wstring error;
    control_center::Ds5WriteResult write;
};

inline AdvancedReadTaskResult RunAdvancedReadTask(const std::wstring& clientRoot) {
    using namespace control_center;
    AdvancedReadTaskResult result;
    if (clientRoot.empty()) {
        result.error = L"未选择客户端。";
        return result;
    }
    result.settings = ReadDs5AdvancedSettings(clientRoot);
    if (!result.settings.exists) {
        result.error = L"客户端内没有 host64\\ReShade.ini；请先安装组件。";
        return result;
    }
    if (!result.settings.readable) {
        result.error = L"读取 host64\\ReShade.ini 失败：配置节缺失、重复或编码无效。";
        return result;
    }
    result.tierName = DetectDs5Tier(result.settings);
    result.modelName = Ds5ModelName(result.settings.model);
    result.ok = true;
    return result;
}

inline AdvancedSaveTaskResult RunAdvancedSaveTask(const std::wstring& clientRoot, const std::wstring& operationsRoot,
                                                  const control_center::Ds5Values& values, bool detached,
                                                  bool opticalFlow) {
    using namespace control_center;
    AdvancedSaveTaskResult result;
    result.write = SaveDs5AdvancedSettings(clientRoot, operationsRoot, values, detached, opticalFlow);
    result.ok = static_cast<bool>(result.write);
    if (!result.ok) result.error = Ds5WriteErrorText(result.write.error);
    return result;
}

inline SchemesTaskResult RunSchemesListTask(const std::wstring& builtinRoot, const std::wstring& userRoot) {
    using namespace control_center;
    SchemesTaskResult result;
    const auto list = ListDs5Schemes(builtinRoot, userRoot);
    result.ok = list.ok;
    result.schemes = list.schemes;
    if (!list.ok) result.error = list.error;
    return result;
}

inline SchemeSaveTaskResult RunSchemeSaveTask(const std::wstring& clientRoot, const std::wstring& userRoot,
                                              const std::wstring& name, const std::wstring& description) {
    using namespace control_center;
    SchemeSaveTaskResult result;
    const auto saved = SaveDs5Scheme(clientRoot, userRoot, name, description);
    result.ok = static_cast<bool>(saved);
    result.path = saved.path;
    if (!result.ok) result.error = saved.error;
    return result;
}

inline SchemeApplyTaskResult RunSchemeApplyTask(const std::wstring& clientRoot, const std::wstring& operationsRoot,
                                                const std::wstring& path, const std::wstring& builtinRoot,
                                                const std::wstring& userRoot) {
    using namespace control_center;
    SchemeApplyTaskResult result;
    const auto applied = ApplyDs5Scheme(clientRoot, operationsRoot, path, builtinRoot, userRoot);
    result.ok = static_cast<bool>(applied);
    result.write = applied.write;
    if (!result.ok) {
        result.error = applied.error.empty() ? Ds5WriteErrorText(applied.write.error) : applied.error;
    }
    return result;
}
// ---- 阶段二工具任务：GPU 统计、面板/游戏启动、日志尾读、诊断与清单导出、
//      恢复安装前文件、AIO 管线开关、窗口助手 ----

struct GpuStatsTaskResult {
    bool ok = false;
    control_center::GpuStats stats;
};

inline GpuStatsTaskResult RunGpuStatsTask() {
    GpuStatsTaskResult result;
    result.stats = control_center::ReadGpuStats();
    result.ok = result.stats.valid;
    return result;
}

struct PanelStartTaskResult {
    bool ok = false;
    std::wstring error;
    std::wstring logDir;
};

inline PanelStartTaskResult RunPanelStartTask(const std::wstring& exeDir, const std::wstring& dataDir,
                                             const std::wstring& clientRoot) {
    PanelStartTaskResult result;
    const auto started = control_center::panel_detail::StartPanel(exeDir, dataDir, clientRoot);
    result.ok = started.ok;
    result.logDir = started.logDir;
    if (!result.ok) result.error = started.error.empty() ? L"启动游戏内面板失败。" : started.error;
    return result;
}

struct GameStartTaskResult {
    bool ok = false;
    std::wstring error;
};

inline GameStartTaskResult RunStartGameTask(const std::wstring& clientRoot) {
    GameStartTaskResult result;
    const auto started = control_center::StartClientGame(clientRoot);
    result.ok = started.ok;
    if (!result.ok) result.error = started.error.empty() ? L"启动游戏失败。" : started.error;
    return result;
}

struct LogTailTaskResult {
    bool ok = false;
    std::wstring error;
    std::vector<std::wstring> lines;
};

inline LogTailTaskResult RunLogTailTask(const std::wstring& logDir) {
    LogTailTaskResult result;
    const auto tail = control_center::ReadLogTail(logDir);
    result.ok = tail.ok;
    result.lines = tail.lines;
    if (!result.ok) result.error = tail.error.empty() ? L"读取日志失败。" : tail.error;
    return result;
}

struct DiagnosticsTaskResult {
    bool ok = false;
    std::wstring error;
    std::wstring path;
};

inline DiagnosticsTaskResult RunDiagnosticsExportTask(const std::wstring& clientRoot,
                                                      const control_center::CatalogResult& catalog,
                                                      const std::wstring& targetPath,
                                                      const std::vector<std::uint32_t>& gameProcesses,
                                                      const std::vector<std::uint32_t>& hostProcesses,
                                                      int unidentifiedProcesses,
                                                      const control_center::TelemetrySample& telemetry,
                                                      const std::vector<std::wstring>& graphicsDevices) {
    using namespace control_center;
    DiagnosticsTaskResult result;
    result.path = targetPath;
    const DiagnosticsStatus status = BuildDiagnosticsStatus(clientRoot, catalog, gameProcesses, hostProcesses,
                                                            unidentifiedProcesses, telemetry, graphicsDevices);
    const auto written = WriteDiagnosticsFile(targetPath, status);
    result.ok = written.ok;
    if (!result.ok) result.error = written.error.empty() ? L"写入诊断文件失败。" : written.error;
    return result;
}

struct ManifestListTaskResult {
    bool ok = false;
    std::wstring error;
    std::wstring path;
};

inline ManifestListTaskResult RunManifestListTask(const std::wstring& clientRoot, const std::wstring& manifestPath,
                                                  const control_center::CatalogResult& catalog,
                                                  const std::wstring& targetPath) {
    using namespace control_center;
    ManifestListTaskResult result;
    result.path = targetPath;
    std::string manifestText;
    DWORD readError = ERROR_SUCCESS;
    if (!ReadWholeFile(manifestPath, manifestText, readError, 8u << 20)) {
        result.error = L"读取安装清单失败（Win32 " + std::to_wstring(readError) + L"）。";
        return result;
    }
    const ManifestListBuild build = BuildManifestListMarkdown(clientRoot, manifestText, catalog);
    if (!build.ok) {
        result.error = build.error.empty() ? L"安装清单无效。" : build.error;
        return result;
    }
    const auto written = WriteManifestListFile(targetPath, build.markdown);
    result.ok = written.ok;
    if (!result.ok) result.error = written.error.empty() ? L"写入移入清单失败。" : written.error;
    return result;
}

struct RestoreTaskResult {
    bool ok = false;
    std::wstring error;
    std::wstring status;
    std::wstring report;
    size_t restored = 0, deleted = 0, skipped = 0;
};

inline RestoreTaskResult RunRestoreTask(const std::wstring& clientRoot, const std::wstring& operationsRoot,
                                        const std::vector<std::wstring>& protectedRoots,
                                        const control_center::CatalogResult& catalog) {
    using namespace control_center;
    RestoreTaskResult result;
    const RestoreResult restored = RestoreLastInstallOperation(clientRoot, operationsRoot, protectedRoots, catalog);
    result.ok = restored.ok;
    result.error = restored.error;
    result.status = restored.status;
    result.report = restored.report;
    result.restored = restored.restored;
    result.deleted = restored.deleted;
    result.skipped = restored.skipped;
    return result;
}

struct PipelineToggleTaskResult {
    bool ok = false;
    std::wstring error;
    bool changed = false;
    int enabled = -1;  // 写入后的值（0/1）
    std::wstring backupPath;
};

inline PipelineToggleTaskResult RunPipelineToggleTask(const std::wstring& clientRoot,
                                                     const std::wstring& operationsRoot) {
    using namespace control_center;
    PipelineToggleTaskResult result;
    const PipelineSwitchState state = ReadPipelineSwitch(clientRoot);
    if (!state.exists) {
        result.error = PipelineSwitchErrorText(PipelineSwitchError::MissingFile);
        return result;
    }
    // 文件存在但读不出来要报读失败，不能误导成“缺少文件”。
    if (!state.readable) {
        result.error = PipelineSwitchErrorText(PipelineSwitchError::Read);
        return result;
    }
    if (state.enabled < 0) {
        result.error = PipelineSwitchErrorText(PipelineSwitchError::Ambiguous);
        return result;
    }
    const int target = state.enabled == 1 ? 0 : 1;
    const PipelineSwitchResult saved = SetPipelineSwitch(clientRoot, operationsRoot, target);
    result.ok = static_cast<bool>(saved);
    result.changed = saved.changed;
    result.backupPath = saved.backupPath;
    result.enabled = result.ok ? target : -1;
    if (!result.ok) result.error = PipelineSwitchErrorText(saved.error);
    return result;
}

struct WindowWatchTaskResult {
    bool ok = false;
    std::wstring error;
    std::wstring logDir;
    std::wstring message;
};

inline WindowWatchTaskResult RunWindowWatchTask(const std::wstring& productRoot, const std::wstring& operationsRoot,
                                                const std::wstring& clientRoot) {
    using namespace control_center;
    WindowWatchTaskResult result;
    const WindowWatchStartResult started = StartWindowWatch(productRoot, operationsRoot, clientRoot);
    result.ok = started.ok;
    result.logDir = started.logDir;
    result.message = started.message;
    if (!result.ok) result.error = started.error.empty() ? L"启动窗口助手失败。" : started.error;
    return result;
}
}  // namespace cc
