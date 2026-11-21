// 回归：重复安装时保留已登记的个性化 ReShade.ini（PreservedConfig）。
//
// 现场基准：客户端已有活动安装记录，25 个目标中 23 项哈希一致，另两个
// ReShade.ini（根目录与 host64）个人化后与模板不同。生产安装入口必须把这两项
// 识别为“保留现有配置”，报告无需写入且不产生新事务；其余冲突仍必须拦截。
#define CONTROL_CENTER_TESTING
#include "../native/control_center/app/cc_tasks.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace control_center;
namespace fs = std::filesystem;

namespace {
int failures = 0;
std::wstring failureText;
std::vector<std::wstring> failureLog;

void Check(bool condition, const std::wstring& what) {
    if (condition) return;
    ++failures;
    failureText = what;
    failureLog.push_back(what);
    std::fwprintf(stderr, L"FAIL: %s\n", what.c_str());
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), count, nullptr, nullptr);
    return result;
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::array<std::uint8_t, 32> HashBytesOf(const std::string& text) {
    std::array<std::uint8_t, 32> hash{};
    if (HashBytes(text, hash) < 0) { ++failures; failureText = L"hash fixture"; }
    return hash;
}

std::string ShaOf(const std::filesystem::path& path) {
    GuardedPath file;
    if (!file.Open(path.parent_path().wstring(), path.filename().wstring())) { ++failures; failureText = L"open fixture"; return {}; }
    const auto hash = HashFile(file.Path());
    if (!hash) { ++failures; failureText = L"hash fixture file"; return {}; }
    return HexSha256(hash.identity.sha256);
}

std::string BytesOf(const std::filesystem::path& path) { return ReadText(path); }

CatalogMember Member(const std::wstring& target, const std::wstring& source, const char* kind,
                     const std::string& sha, bool replaces) {
    CatalogMember member;
    member.target = target; member.source = source; member.sourceKind = kind;
    member.sha256 = sha; member.replacesOriginal = replaces;
    return member;
}

bool WriteManifest(const std::filesystem::path& path, const CatalogResult& catalog) {
    Json entries = Json::array();
    for (const auto& member : catalog.members) {
        std::wstring folder = member.target.find(L'/') == std::wstring::npos ? L"root" : L"host64";
        entries.push_back({{"target", Utf8(member.target)}, {"source_kind", member.sourceKind},
                           {"folder", Utf8(folder)}, {"purpose", "fixture"}, {"replaces_original", member.replacesOriginal},
                           {"required", true}});
    }
    const Json manifest = {{"schema", 1}, {"updated", "2026-11-19"}, {"entries", entries}};
    return WriteText(path, manifest.dump() + "\n");
}

std::wstring Child(const std::wstring& base, const std::wstring& name) {
    return (fs::path(base) / name).wstring();
}

// 统计 data\operations 下形如 32 位小写十六进制的操作目录；无需写入的安装
// 不应该留下任何新目录（空会话目录必须由本次操作自己清理）。
std::size_t CountOperationDirectories(const std::wstring& dataDir) {
    const fs::path operations = fs::path(dataDir) / "operations";
    std::error_code ec;
    if (!fs::exists(operations, ec)) return 0;
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(operations, ec)) {
        if (!entry.is_directory()) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.size() != 32) continue;
        bool hex = true;
        for (const wchar_t c : name) {
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) {
                hex = false;
                break;
            }
        }
        if (hex) ++count;
    }
    return count;
}

// 统计目录树中的普通文件数（用于证明卸载被拒绝时没有写入计划、备份或残留）。
std::size_t CountTreeFiles(const std::wstring& dir) {
    if (dir.empty()) return 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!ec && entry.is_regular_file()) ++count;
    }
    return count;
}

// 与 catalog 完全一致的客户端内容（不含活动安装记录），用于“文件已存在但
// 没有本工具安装记录”的场景。
bool WriteCurrentFiles(const std::wstring& client, const std::wstring& base, const std::wstring& fixtureRoot) {
    std::error_code ec;
    fs::create_directories(fs::path(client) / "host64", ec);
    if (!fs::copy_file(fs::path(fixtureRoot) / "client-x86" / "wow.exe", fs::path(client) / "wow.exe",
                       fs::copy_options::overwrite_existing, ec)) {
        return false;
    }
    if (!fs::copy_file(fs::path(base) / "stage" / "d3d9.dll", fs::path(client) / "d3d9.dll",
                       fs::copy_options::overwrite_existing, ec)) {
        return false;
    }
    if (!fs::copy_file(fs::path(base) / "template" / "ReShade.ini", fs::path(client) / "ReShade.ini",
                       fs::copy_options::overwrite_existing, ec)) {
        return false;
    }
    if (!fs::copy_file(fs::path(base) / "template" / "host64" / "ReShade.ini",
                       fs::path(client) / "host64" / "ReShade.ini", fs::copy_options::overwrite_existing, ec)) {
        return false;
    }
    return fs::copy_file(fs::path(base) / "template" / "host64" / "core.dll",
                         fs::path(client) / "host64" / "core.dll", fs::copy_options::overwrite_existing, ec) != false;
}

bool InterruptAfterIntent(MemberCheckpoint point) { return point == MemberCheckpoint::Intent; }
ProcessCheck StoppedProcesses() { return ProcessCheck{ProcessState::Stopped, ERROR_SUCCESS, L""}; }
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1];
    const std::wstring base = Child(root, L"preserved-config");
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    const std::wstring client = Child(base, L"client");
    const std::wstring stage = Child(base, L"stage");
    const std::wstring templ = Child(base, L"template");
    const std::wstring data = Child(base, L"data");
    fs::create_directories(client, ec);
    fs::create_directories(stage, ec);
    fs::create_directories(fs::path(templ) / "host64", ec);
    fs::create_directories(data, ec);
    if (!fs::copy_file(fs::path(root) / "client-x86" / "wow.exe", fs::path(client) / "wow.exe",
                       fs::copy_options::overwrite_existing, ec)) {
        std::fwprintf(stderr, L"FAIL: copy wow.exe\n");
        return 2;
    }

    if (!WriteText(fs::path(stage) / "d3d9.dll", "dxvk-fixture-dll") ||
        !WriteText(fs::path(templ) / "ReShade.ini", "template-reshade\n") ||
        !WriteText(fs::path(templ) / "host64" / "ReShade.ini", "template-host64-reshade\n") ||
        !WriteText(fs::path(templ) / "host64" / "core.dll", "template-core\n")) {
        std::fwprintf(stderr, L"FAIL: write fixture sources\n");
        return 3;
    }

    CatalogResult catalog;
    catalog.members.push_back(Member(L"d3d9.dll", L"d3d9.dll", "stage", ShaOf(fs::path(stage) / "d3d9.dll"), true));
    catalog.members.push_back(Member(L"ReShade.ini", L"ReShade.ini", "template", ShaOf(fs::path(templ) / "ReShade.ini"), false));
    catalog.members.push_back(Member(L"host64/ReShade.ini", L"host64/ReShade.ini", "template",
                                     ShaOf(fs::path(templ) / "host64" / "ReShade.ini"), false));
    catalog.members.push_back(Member(L"host64/core.dll", L"host64/core.dll", "template",
                                     ShaOf(fs::path(templ) / "host64" / "core.dll"), false));
    if (!catalog) { std::fwprintf(stderr, L"FAIL: catalog fixture\n"); return 4; }

    const std::wstring manifestPath = Child(base, L"install-manifest.json");
    if (!WriteManifest(manifestPath, catalog)) return 5;

    cc::AppPaths paths;
    paths.dataDir = data;
    auto progress = std::make_shared<cc::TaskProgress>();
    const std::vector<std::wstring> protectedRoots;

    // 场景 1：首次安装（全部目标缺失），走生产安装任务入口。
    {
        const auto first = cc::RunInstallTask(client, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(first.ok && !first.noChange && first.applied == 4, L"首次安装应写入 4 项");
        Check(first.preserved == 0, L"首次安装不应有保留配置");
        if (!first.ok) return 6;
    }

    const auto recordPath = fs::path(client) / "host64" / "dlss5-install-record.json";
    const auto rootIni = fs::path(client) / "ReShade.ini";
    const auto hostIni = fs::path(client) / "host64" / "ReShade.ini";
    Check(fs::exists(recordPath), L"首次安装后应生成活动安装记录");

    // 现场状态：两份 INI 被玩家/载体个性化。
    const std::string rootIniPersonal = ReadText(rootIni) + "[OVERLAY]\r\nShowFps=1\r\n[STYLE]\r\nAccent=7\r\n";
    const std::string hostIniPersonal =
        ReadText(hostIni) + "FrameGeneration=1\r\nModel=3\r\nIntensity=0.3500\r\n";
    WriteText(rootIni, rootIniPersonal);
    WriteText(hostIni, hostIniPersonal);
    const auto rootIniHash = ShaOf(rootIni);
    const auto hostIniHash = ShaOf(hostIni);
    const auto recordText = ReadText(recordPath);
    const auto coreHashAfterInstall = ShaOf(fs::path(client) / "host64" / "core.dll");
    std::string evidence;
    const auto Append = [&](const std::string& key, const std::string& value) { evidence += key + "=" + value + "\n"; };
    Append("root_ini_personal_sha256", rootIniHash);
    Append("host_ini_personal_sha256", hostIniHash);
    Append("record_after_first_install_sha256", ShaOf(recordPath));

    // 场景 2：现场重装 —— 23 项一致 + 2 份已登记配置不同，应成功且无需写入。
    // 连续两次都必须保持记录、INI 与备份不变，且不得留下空会话目录。
    const auto operationsBeforeReinstall = CountOperationDirectories(data);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = cc::RunInstallTask(client, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(result.ok, L"现场重装应成功");
        Check(result.noChange, L"现场重装应报告无需写入");
        Check(result.preserved == 2, L"现场重装应保留 2 份已登记配置");
        Check(result.applied == 0, L"现场重装不应写入成员");
        Check(result.state == InstallJobState::AlreadyCurrent, L"现场重装状态应为 AlreadyCurrent");
        Check(cc::InstallOutcomeDetail(result).rfind(L"无需写入：", 0) == 0 &&
                  cc::InstallOutcomeDetail(result).find(L"保留 2 份已登记配置") != std::wstring::npos,
              L"结果文案应显示无需写入并包含保留 2 份配置");
        Check(cc::InstallOutcomeStateText(result) == L"无需变更（安装记录与备份保持原样）",
              L"事务状态文案应为无需变更");
        if (attempt == 0) {
            Append("reinstall_detail", Utf8(cc::InstallOutcomeDetail(result)));
            Append("reinstall_state_text", Utf8(cc::InstallOutcomeStateText(result)));
            Append("reinstall_ok", result.ok ? "true" : "false");
            Append("reinstall_no_change", result.noChange ? "true" : "false");
            Append("reinstall_preserved", std::to_string(result.preserved));
            Append("reinstall_applied", std::to_string(result.applied));
            Append("root_ini_after_reinstall_sha256", ShaOf(rootIni));
            Append("host_ini_after_reinstall_sha256", ShaOf(hostIni));
            Append("record_after_reinstall_sha256", ShaOf(recordPath));
        }
        Check(ShaOf(rootIni) == rootIniHash, L"根 ReShade.ini 不应被改写");
        Check(ShaOf(hostIni) == hostIniHash, L"host64 ReShade.ini 不应被改写");
        Check(ReadText(recordPath) == recordText, L"活动安装记录不应被改写");
        Check(!fs::exists(fs::path(client) / "host64" / "dlss5-backup"),
              L"无需写入时不应创建/改动备份目录");
        Check(ShaOf(fs::path(client) / "host64" / "core.dll") == coreHashAfterInstall,
              L"一致组件不应被改写");
    }

    const auto operationsAfterReinstall = CountOperationDirectories(data);
    Check(operationsAfterReinstall == operationsBeforeReinstall,
          L"两次无需写入的重装都不应留下新事务目录（本次空会话须自行清理）");
    Append("operations_before_noop_reinstall", std::to_string(operationsBeforeReinstall));
    Append("operations_after_noop_reinstall", std::to_string(operationsAfterReinstall));

    // 文件清单页输入：同一预检 + 活动记录策略必须把两项标为 PreservedConfig（而不是冲突）。
    {
        const auto registered = LoadRegisteredInstallConfig(client, catalog);
        Check(registered.trusted, L"活动安装记录应可用于保留配置判定");
        const auto preview = PreviewInstall(client, stage, templ, catalog, registered.trusted ? &registered.policy : nullptr);
        size_t preserved = 0, invalid = 0;
        for (const auto& item : preview.items) {
            if (item.state == PreviewState::PreservedConfig) ++preserved;
            if (item.state == PreviewState::Invalid) ++invalid;
        }
        Check(preview.filesReady, L"现场重装预检应文件齐全");
        Check(preserved == 2, L"文件清单应把两份已登记 INI 标为保留配置");
        Check(invalid == 0, L"文件清单不应出现冲突项");
        Append("preview_preserved_items", std::to_string(preserved));
        Append("preview_invalid_items", std::to_string(invalid));
    }
    // 场景 3：混合安装 —— 再缺一个普通组件，只补该组件并保留两份配置。
    fs::remove(fs::path(client) / "host64" / "core.dll", ec);
    {
        const auto result = cc::RunInstallTask(client, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(result.ok && !result.noChange, L"混合安装应成功提交");
        Check(result.applied == 1, L"混合安装只应补写 1 个组件");
        Check(result.preserved == 2, L"混合安装应保留 2 份已登记配置");
        Append("mixed_detail", Utf8(cc::InstallOutcomeDetail(result)));
        Append("mixed_state_text", Utf8(cc::InstallOutcomeStateText(result)));
        Append("mixed_applied", std::to_string(result.applied));
        Append("root_ini_after_mixed_sha256", ShaOf(rootIni));
        Append("host_ini_after_mixed_sha256", ShaOf(hostIni));
        Append("record_after_mixed_sha256", ShaOf(recordPath));
        Check(ShaOf(rootIni) == rootIniHash && ShaOf(hostIni) == hostIniHash, L"混合安装不应改写保留配置");
        Check(ShaOf(fs::path(client) / "host64" / "core.dll") == catalog.members[3].sha256,
              L"补写的组件应恢复为模板内容");
        Check(fs::exists(recordPath), L"混合安装后安装记录应保留");
        const auto record = ReadClientRecord(ReadText(recordPath));
        Check(static_cast<bool>(record), L"混合安装后记录应可解析");
        if (record) {
            bool rootEntryOk = false, hostEntryOk = false;
            for (const auto& entry : record.entries) {
                if (SameTarget(entry.target, L"ReShade.ini")) rootEntryOk = entry.sha256 == catalog.members[1].sha256;
                if (SameTarget(entry.target, L"host64/ReShade.ini")) hostEntryOk = entry.sha256 == catalog.members[2].sha256;
            }
            Check(rootEntryOk && hostEntryOk, L"保留配置应维持原有登记源哈希");
        }
    }

    // 场景 3b：混合安装过程中写入中断，恢复后再次安装应保留配置并正常提交。
    {
        fs::remove(fs::path(client) / "host64" / "core.dll", ec);
        const auto registered = LoadRegisteredInstallConfig(client, catalog);
        const auto preview = PreviewInstall(client, stage, templ, catalog, registered.trusted ? &registered.policy : nullptr);
        bool found = false;
        PreviewItem coreItem;
        for (const auto& item : preview.items) {
            if (SameTarget(item.member.target, L"host64/core.dll") && item.state == PreviewState::Absent) {
                coreItem = item; found = true;
            }
        }
        Check(found, L"中断夹具应找到缺失的普通组件");
        const std::wstring operationId = cc::NewOperationId();
        MemberInstallContext context;
        context.clientRoot = client;
        context.stageRoot = stage;
        context.templateRoot = templ;
        context.journalRoot = fs::path(data) / "operations" / operationId;
        context.operationId = operationId;
        context.testProcesses = &StoppedProcesses;
        context.testInterrupt = &InterruptAfterIntent;
        std::error_code mk;
        fs::create_directories(context.journalRoot, mk);
        ClientLock lock;
        if (!lock.Acquire(client)) { Check(false, L"中断夹具应能获取客户端锁"); }
        else {
            const auto interrupted = InstallMember(context, catalog, coreItem, lock);
            Check(interrupted.state == MemberInstallState::RecoveryRequired, L"写入中断应留下需要恢复的成员事务");
            Append("interrupt_state", std::to_string(static_cast<int>(interrupted.state)));
        }
        const auto recovered = cc::RunInstallTask(client, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(recovered.ok && !recovered.noChange, L"中断恢复后再次安装应成功提交");
        Check(recovered.applied == 1 && recovered.preserved == 2, L"恢复安装只补缺失组件并保留两份配置");
        Check(ShaOf(rootIni) == rootIniHash && ShaOf(hostIni) == hostIniHash, L"恢复安装不应改写保留配置");
        Check(ShaOf(fs::path(client) / "host64" / "core.dll") == catalog.members[3].sha256,
              L"恢复安装应写入完整模板组件");
        Append("interrupt_recovery_ok", recovered.ok ? "true" : "false");
        Append("interrupt_recovery_applied", std::to_string(recovered.applied));
        Append("interrupt_recovery_preserved", std::to_string(recovered.preserved));
    }
    // 场景 4：不同内容但没有可信登记的第三方 INI，首次安装必须报目标冲突。
    {
        const std::wstring other = Child(base, L"client-conflict");
        fs::create_directories(fs::path(other) / "host64", ec);
        fs::copy_file(fs::path(root) / "client-x86" / "wow.exe", fs::path(other) / "wow.exe",
                      fs::copy_options::overwrite_existing, ec);
        WriteText(fs::path(other) / "ReShade.ini", "third-party-reshade\n");
        const auto result = cc::RunInstallTask(other, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(!result.ok, L"无可信登记的第三方 INI 不应被静默接管");
        Check(result.error.find(L"安装目标文件冲突：") == 0, L"冲突提示应明确为目标冲突");
        Check(result.error.find(L"ReShade.ini") != std::wstring::npos, L"冲突提示应包含具体路径");
        Append("conflict_error", Utf8(result.error));
        Check(ReadText(fs::path(other) / "ReShade.ini") == "third-party-reshade\n", L"第三方 INI 应保持原样");
        Check(!fs::exists(fs::path(other) / "host64" / "dlss5-install-record.json"), L"冲突时不应发布记录");
    }

    // 场景 5：模板源损坏仍按源校验失败处理，且不写入目标。
    {
        const std::wstring other = Child(base, L"client-corrupt");
        fs::create_directories(other, ec);
        fs::copy_file(fs::path(root) / "client-x86" / "wow.exe", fs::path(other) / "wow.exe",
                      fs::copy_options::overwrite_existing, ec);
        WriteText(fs::path(templ) / "host64" / "core.dll", "corrupted-template\n");
        const auto result = cc::RunInstallTask(other, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(!result.ok, L"损坏模板不应通过预检");
        Check(result.error.find(L"安装源文件校验失败：") == 0, L"源损坏应报告源文件校验失败");
        Append("source_corrupt_error", Utf8(result.error));
        Check(!fs::exists(fs::path(other) / "host64" / "core.dll"), L"源校验失败时不应写入目标");
        WriteText(fs::path(templ) / "host64" / "core.dll", "template-core\n");
    }

    // 场景 5b：组件文件全部匹配，但客户端没有活动安装记录。生产入口必须给出
    // 独立结果：不得自动接管、不得赋予卸载删除权限、不得留下空事务目录。
    {
        const std::wstring other = Child(base, L"client-norecord");
        Check(WriteCurrentFiles(other, base, root), L"无记录全匹配夹具应可写入");
        const auto noRecordPath = fs::path(other) / "host64" / "dlss5-install-record.json";
        const auto dllBefore = ShaOf(fs::path(other) / "d3d9.dll");
        const auto rootIniBefore = ShaOf(fs::path(other) / "ReShade.ini");
        const auto operationsBeforeNoRecord = CountOperationDirectories(data);
        const auto result =
            cc::RunInstallTask(other, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(!result.ok, L"无记录全匹配必须报告安装未完成");
        Check(result.recordMissing, L"无记录全匹配应标记为安装记录缺失");
        Check(!result.noChange, L"无记录全匹配不得报告无需写入（首页与结果页必须一致显示未安装）");
        Check(result.applied == 0 && result.preserved == 0, L"无记录全匹配不应写入任何组件");
        Check(result.error.rfind(L"安装记录缺失：", 0) == 0, L"错误文案应以“安装记录缺失：”开头");
        Check(result.error.find(L"当前没有本工具的安装记录") != std::wstring::npos,
              L"错误文案应说明当前没有本工具的安装记录");
        Check(cc::StateText(result.state) == L"需要人工检查",
              L"无记录全匹配的事务状态应为需要人工检查：" + cc::StateText(result.state));
        Check(!fs::exists(noRecordPath), L"无记录全匹配不得自动生成安装记录（不静默接管）");
        Check(CountOperationDirectories(data) == operationsBeforeNoRecord,
              L"无记录全匹配不应留下本次的空事务目录");
        Check(ShaOf(fs::path(other) / "d3d9.dll") == dllBefore, L"无记录全匹配不应改写已有组件");
        Check(ShaOf(fs::path(other) / "ReShade.ini") == rootIniBefore, L"无记录全匹配不应改写现有配置");
        Append("no_record_ok", result.ok ? "true" : "false");
        Append("no_record_record_missing", result.recordMissing ? "true" : "false");
        Append("no_record_no_change", result.noChange ? "true" : "false");
        Append("no_record_error", Utf8(result.error));
        Append("no_record_operations_before", std::to_string(operationsBeforeNoRecord));
        Append("no_record_operations_after", std::to_string(CountOperationDirectories(data)));
    }

    // 场景 5c：组件文件全部匹配，但现有安装记录属于另一个客户端目录。生产入口
    // 必须保留该记录与文件、不得判为“无需写入”，也不能把首页显示成已安装。
    {
        const std::wstring other = Child(base, L"client-foreign-record");
        Check(WriteCurrentFiles(other, base, root), L"外来记录夹具应可写入组件");
        Check(fs::exists(recordPath), L"来源客户端应已有安装记录");
        const auto otherRecordPath = fs::path(other) / "host64" / "dlss5-install-record.json";
        fs::copy_file(recordPath, otherRecordPath, fs::copy_options::overwrite_existing, ec);
        Check(!ec, L"应可把另一个客户端的记录复制到夹具客户端");
        const auto recordBefore = ShaOf(otherRecordPath);
        const auto dllBefore = ShaOf(fs::path(other) / "d3d9.dll");
        const auto rootIniBefore = ShaOf(fs::path(other) / "ReShade.ini");
        const auto operationsBefore = CountOperationDirectories(data);
        const auto result =
            cc::RunInstallTask(other, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(!result.ok, L"外来安装记录不得报告安装成功");
        Check(result.recordForeign, L"外来安装记录应标记为记录属于其他客户端");
        Check(!result.recordMissing, L"外来记录不应被归为完全无记录");
        Check(!result.noChange, L"外来记录不得报告无需写入");
        Check(result.applied == 0 && result.preserved == 0, L"外来记录场景不应写入任何组件");
        Check(result.error.rfind(L"安装记录属于其他客户端：", 0) == 0,
              L"错误文案应以“安装记录属于其他客户端：”开头");
        Check(cc::StateText(result.state) == L"需要人工检查",
              L"外来记录的事务状态应为需要人工检查：" + cc::StateText(result.state));
        Check(ShaOf(otherRecordPath) == recordBefore, L"外来记录必须原样保留，不得被改写或删除");
        Check(CountOperationDirectories(data) == operationsBefore,
              L"外来记录场景不应留下本次的空事务目录");
        Check(ShaOf(fs::path(other) / "d3d9.dll") == dllBefore, L"外来记录场景不应改写已有组件");
        Check(ShaOf(fs::path(other) / "ReShade.ini") == rootIniBefore, L"外来记录场景不应改写现有配置");
        Append("foreign_record_ok", result.ok ? "true" : "false");
        Append("foreign_record_record_foreign", result.recordForeign ? "true" : "false");
        Append("foreign_record_no_change", result.noChange ? "true" : "false");
        Append("foreign_record_error", Utf8(result.error));
        Append("foreign_record_operations_before", std::to_string(operationsBefore));
        Append("foreign_record_operations_after", std::to_string(CountOperationDirectories(data)));
    }

    // 场景 5d：B 持有 A 的有效安装记录，同时缺少 host64/core.dll。生产安装入口
    // 必须在创建事务和写入任何成员之前拒绝；缺失文件仍缺失，外来记录与其余
    // 文件逐字节不变，且不留下事务目录。
    {
        const std::wstring other = Child(base, L"client-foreign-record");
        const auto otherRecordPath = fs::path(other) / "host64" / "dlss5-install-record.json";
        const auto otherCore = fs::path(other) / "host64" / "core.dll";
        fs::remove(otherCore, ec);
        Check(!fs::exists(otherCore), L"5d 夹具应先缺少 host64/core.dll");
        const auto recordBefore = ShaOf(otherRecordPath);
        const auto dllBefore = ShaOf(fs::path(other) / "d3d9.dll");
        const auto rootIniBefore = ShaOf(fs::path(other) / "ReShade.ini");
        const auto hostIniBefore = ShaOf(fs::path(other) / "host64" / "ReShade.ini");
        const auto operationsBefore = CountOperationDirectories(data);
        const auto result =
            cc::RunInstallTask(other, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(!result.ok, L"5d 外来记录不得报告安装成功");
        Check(result.recordForeign, L"5d 应标记记录属于其他客户端");
        Check(!result.recordMissing, L"5d 外来记录不应被归为完全无记录");
        Check(!result.noChange && result.applied == 0 && result.preserved == 0, L"5d 不得写入任何成员");
        Check(result.error.rfind(L"安装记录属于其他客户端：", 0) == 0,
              L"5d 错误应以“安装记录属于其他客户端：”开头：" + result.error);
        Check(cc::StateText(result.state) == L"需要人工检查", L"5d 状态应为需要人工检查");
        Check(!fs::exists(otherCore), L"5d 缺失的 host64/core.dll 仍应缺失");
        Check(ShaOf(otherRecordPath) == recordBefore, L"5d 外来记录应逐字节不变");
        Check(ShaOf(fs::path(other) / "d3d9.dll") == dllBefore &&
                  ShaOf(fs::path(other) / "ReShade.ini") == rootIniBefore &&
                  ShaOf(fs::path(other) / "host64" / "ReShade.ini") == hostIniBefore,
              L"5d 其余文件应逐字节不变");
        Check(CountOperationDirectories(data) == operationsBefore, L"5d 不应创建事务目录");
        Append("missing_component_foreign_ok", result.ok ? "true" : "false");
        Append("missing_component_foreign_record_foreign", result.recordForeign ? "true" : "false");
        Append("missing_component_foreign_error", Utf8(result.error));
        Append("missing_component_foreign_core_present", fs::exists(otherCore) ? "true" : "false");
        Append("missing_component_foreign_operations_before", std::to_string(operationsBefore));
        Append("missing_component_foreign_operations_after", std::to_string(CountOperationDirectories(data)));
    }

    // 场景 5e 对照：本客户端有效记录 + 缺少一个组件，正常归属仍应补齐并成功，
    // 两份已登记配置保持原样。
    {
        const auto ownCore = fs::path(client) / "host64" / "core.dll";
        fs::remove(ownCore, ec);
        Check(!fs::exists(ownCore), L"5e 夹具应先缺少 host64/core.dll");
        const auto result =
            cc::RunInstallTask(client, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(result.ok, L"5e 本客户端有效记录应能补齐缺失组件：" + result.error);
        Check(!result.recordForeign && !result.recordMissing, L"5e 不应判为外来或缺失记录");
        Check(result.applied == 1, L"5e 应只补写 1 个组件");
        Check(result.preserved == 2, L"5e 应保留 2 份已登记配置");
        Check(fs::exists(ownCore) && ShaOf(ownCore) == catalog.members[3].sha256, L"5e 缺失组件应被正确写入");
        Check(ShaOf(rootIni) == rootIniHash && ShaOf(hostIni) == hostIniHash, L"5e 不应改写保留配置");
        Append("own_record_missing_component_ok", result.ok ? "true" : "false");
        Append("own_record_missing_component_applied", std::to_string(result.applied));
        Append("own_record_missing_component_preserved", std::to_string(result.preserved));
    }

    // 场景 7：卸载生产入口必须重新核对记录归属。页面预检时记录属于本客户端；
    // 执行前换成另一个客户端的有效记录，实际卸载任务必须在建立计划、移动文件
    // 或删除记录之前明确拒绝，夹具文件/备份/外来记录保持原样。
    {
        const std::wstring swapped = Child(base, L"client-uninstall-foreign");
        fs::create_directories(swapped, ec);
        Check(static_cast<bool>(fs::copy_file(fs::path(root) / "client-x86" / "wow.exe", fs::path(swapped) / "wow.exe",
                                              fs::copy_options::overwrite_existing, ec)),
              L"7 卸载夹具应可复制 wow.exe");
        const auto installed =
            cc::RunInstallTask(swapped, stage, templ, catalog, manifestPath, protectedRoots, paths, progress);
        Check(installed.ok && installed.applied == 4, L"7 应先在夹具客户端完成一次正常安装");
        const auto swappedRecord = fs::path(swapped) / "host64" / "dlss5-install-record.json";
        Check(fs::exists(swappedRecord), L"7 正常安装后应有记录");

        std::string manifestBytes;
        DWORD readError = ERROR_SUCCESS;
        cc::ReadWholeFile(manifestPath, manifestBytes, readError, 8u << 20);
        auto manifestJson = ParseJsonObject(manifestBytes);
        Check(static_cast<bool>(manifestJson), L"7 应可解析清单");
        // 页面预检（只读、持锁）此时应通过：记录确实属于该客户端。
        {
            ClientLock previewLock;
            Check(previewLock.Acquire(swapped), L"7 预检应能获取客户端锁");
            const auto preview = PreviewUninstall(swapped, protectedRoots, catalog, manifestJson.value, previewLock);
            Check(preview.valid && preview.canUninstall, L"7 本客户端记录的有效预检应通过：" + cc::Utf8ToWide(preview.error));
        }

        const auto dllBefore = ShaOf(fs::path(swapped) / "d3d9.dll");
        const auto rootIniBefore = ShaOf(fs::path(swapped) / "ReShade.ini");
        const auto hostIniBefore = ShaOf(fs::path(swapped) / "host64" / "ReShade.ini");
        const auto operationsBefore = CountOperationDirectories(data);
        const auto uninstallsBefore = CountTreeFiles((fs::path(data) / "uninstalls").wstring());
        const auto backupBefore = fs::exists(fs::path(swapped) / "host64" / "dlss5-backup");

        fs::copy_file(recordPath, swappedRecord, fs::copy_options::overwrite_existing, ec);
        Check(!ec, L"7 应可把外来记录换入夹具客户端");
        const auto swappedRecordHash = ShaOf(swappedRecord);

        const auto result = cc::RunUninstallTask(swapped, catalog, manifestPath, protectedRoots, paths, progress);
        Check(!result.ok, L"7 卸载任务不得在记录被换成外来记录后执行");
        Check(result.recordForeign, L"7 卸载任务应标记记录属于其他客户端");
        Check(result.error.rfind(L"卸载预检失败：安装记录属于其他客户端", 0) == 0,
              L"7 卸载错误应明确提示记录属于其他客户端：" + result.error);
        Check(ShaOf(swappedRecord) == swappedRecordHash, L"7 外来记录应原样保留");
        Check(ShaOf(fs::path(swapped) / "d3d9.dll") == dllBefore &&
                  ShaOf(fs::path(swapped) / "ReShade.ini") == rootIniBefore &&
                  ShaOf(fs::path(swapped) / "host64" / "ReShade.ini") == hostIniBefore,
              L"7 卸载拒绝后夹具文件应逐字节不变");
        Check(CountOperationDirectories(data) == operationsBefore, L"7 卸载拒绝不应创建事务目录");
        Check(CountTreeFiles((fs::path(data) / "uninstalls").wstring()) == uninstallsBefore,
              L"7 卸载拒绝不应创建卸载计划或备份");
        Check(fs::exists(fs::path(swapped) / "host64" / "dlss5-backup") == backupBefore,
              L"7 卸载拒绝不应改动备份目录");
        Append("uninstall_foreign_ok", result.ok ? "true" : "false");
        Append("uninstall_foreign_record_foreign", result.recordForeign ? "true" : "false");
        Append("uninstall_foreign_error", Utf8(result.error));
        Append("uninstall_foreign_record_sha256", swappedRecordHash);
        Append("uninstall_foreign_operations_before", std::to_string(operationsBefore));
        Append("uninstall_foreign_operations_after", std::to_string(CountOperationDirectories(data)));
        Append("uninstall_foreign_uninstall_files_before", std::to_string(uninstallsBefore));
        Append("uninstall_foreign_uninstall_files_after",
               std::to_string(CountTreeFiles((fs::path(data) / "uninstalls").wstring())));
    }

    // 场景 6：保留配置后正常卸载，按原记录处理用户改动：客户端内还原安装前状态（删除本工具装过的
    // 文件），个性化内容先保存到 uninstalls/<id>/modified，并在结果中报告 kept_modified。
    {
        const auto result = cc::RunUninstallTask(client, catalog, manifestPath, protectedRoots, paths, progress);
        Check(result.ok, L"卸载应成功");
        Check(result.finalize.keptModified == 2, L"卸载应把两份个性化 INI 记为已保存的用户改动");
        Check(!fs::exists(rootIni) && !fs::exists(hostIni), L"卸载应按记录还原到安装前状态");
        bool rootSaved = false, hostSaved = false;
        const auto uninstalls = fs::path(data) / "uninstalls";
        if (fs::exists(uninstalls)) {
            for (const auto& entry : fs::recursive_directory_iterator(uninstalls)) {
                if (!entry.is_regular_file()) continue;
                const auto relative = fs::relative(entry.path(), uninstalls).generic_string();
                if (relative.find("/modified/ReShade.ini") != std::string::npos) rootSaved = true;
                if (relative.find("/modified/host64/ReShade.ini") != std::string::npos) hostSaved = true;
            }
        }
        Check(rootSaved && hostSaved, L"卸载应保存两份个性化 INI 的文件副本");
        Append("uninstall_kept_modified", std::to_string(result.finalize.keptModified));
        Append("uninstall_root_ini_saved", rootSaved ? "true" : "false");
        Append("uninstall_host_ini_saved", hostSaved ? "true" : "false");
        Append("record_after_uninstall_exists", fs::exists(recordPath) ? "true" : "false");
        Check(!fs::exists(recordPath), L"卸载成功后应移除安装记录");
    }

    WriteText(fs::path(base) / "preserved-evidence.txt", evidence);

    if (failures) {
        std::string report;
        for (const auto& item : failureLog) report += Utf8(item) + "\\n";
        std::FILE* file = nullptr;
        if (_wfopen_s(&file, (fs::path(base) / "preserved-failures.txt").c_str(), L"wb") == 0 && file) {
            std::fwrite(report.data(), 1, report.size(), file);
            std::fclose(file);
        }
        std::fwprintf(stderr, L"preserved-config: %d failure(s), last=%s\n", failures, failureText.c_str());
        return 20;
    }
    std::printf("preserved-config: all scenarios passed\n");
    return 0;
}
