#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../native/control_center/catalog.hpp"
#include "../native/control_center/file_identity.hpp"
#include "../native/control_center/manifest_list.hpp"

namespace {
int gFailures = 0;
void Fail(const char* label) { std::printf("FAIL %s\n", label); ++gFailures; }
void Check(bool condition, const char* label) { if (!condition) Fail(label); }
std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + (a.back() == L'\\' ? L"" : L"\\") + b;
}
bool MakeDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}
bool WriteAll(const std::wstring& path, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size();
    CloseHandle(file);
    return ok;
}
bool ReadAll(const std::wstring& path, std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return false; }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes.empty() ? nullptr : &bytes[0], static_cast<DWORD>(bytes.size()), &read,
                             nullptr) != 0;
    CloseHandle(file);
    return ok && read == bytes.size();
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
    return out;
}
bool HasW(const std::wstring& text, const wchar_t* needle) { return text.find(needle) != std::wstring::npos; }
bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

std::wstring HashOf(const std::wstring& path) {
    const control_center::HashResult hashed = control_center::HashFile(path);
    if (!hashed) return std::wstring();
    return control_center::diagnostics_detail::Widen(control_center::HexSha256(hashed.identity.sha256));
}

std::string ManifestText() {
    std::string manifest;
    manifest += "{\"entries\":[";
    manifest += "{\"target\":\"d3d9.dll\",\"folder\":\"客户端根目录\",\"purpose\":\"D3D9 代理\",\"required\":true},";
    manifest += "{\"target\":\"host64/nvngx.dll\",\"folder\":\"host64\",\"purpose\":\"超分库\",\"required\":false},";
    manifest += "{\"target\":\"ReShade.ini\",\"folder\":\"客户端根目录\",\"purpose\":\"配置\",\"required\":true}";
    manifest += "],\"residue\":[";
    manifest += "{\"path\":\"host64/logs\",\"kind\":\"tree\",\"policy\":\"delete\",\"note\":\"运行日志\"},";
    manifest += "{\"path\":\"WTF/Config.wtf\",\"kind\":\"file\",\"policy\":\"report\",\"note\":\"用户配置\"}";
    manifest += "]}";
    return manifest;
}

control_center::CatalogResult BuildCatalog(const std::wstring& clientRoot) {
    control_center::CatalogResult catalog;
    control_center::CatalogMember d3d9;
    d3d9.target = L"d3d9.dll";
    d3d9.source = L"stage\\d3d9.dll";
    d3d9.sourceKind = "stage";
    d3d9.sha256 = Utf8(HashOf(Join(clientRoot, L"d3d9.dll")));
    d3d9.replacesOriginal = true;
    catalog.members.push_back(d3d9);

    control_center::CatalogMember nvngx;
    nvngx.target = L"host64/nvngx.dll";
    nvngx.source = L"stage\\host64\\nvngx.dll";
    nvngx.sourceKind = "stage";
    nvngx.sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    nvngx.required = false;
    catalog.members.push_back(nvngx);

    control_center::CatalogMember ini;
    ini.target = L"ReShade.ini";
    ini.source = L"templates\\game-ReShade.ini";
    ini.sourceKind = "template";
    ini.sha256 = "1111111111111111111111111111111111111111111111111111111111111111";
    ini.replacesOriginal = true;
    catalog.members.push_back(ini);
    return catalog;
}

void TestBuild() {
    const std::wstring root = L"D:\\WOW-335-CN\\_scratch\\dlss5-aio\\manifest-list-test";
    const std::wstring client = Join(root, L"client");
    MakeDirectory(root);
    MakeDirectory(client);
    MakeDirectory(Join(client, L"host64"));
    Check(WriteAll(Join(client, L"d3d9.dll"), "abc"), "write d3d9");
    const control_center::CatalogResult catalog = BuildCatalog(client);
    const std::string manifest = ManifestText();

    const control_center::ManifestListBuild plain = control_center::BuildManifestListMarkdown(L"", manifest, catalog);
    Check(plain.ok, "plain ok");
    Check(plain.rows.size() == 3, "plain rows");
    Check(plain.residue.size() == 2, "plain residue");
    Check(HasW(plain.markdown, L"# WOW-335-NR 客户端安装清单"), "title");
    Check(HasW(plain.markdown, L"| 目标 | 目录 | 来源 | 必需 | 用途 |"), "header no status");
    Check(!HasW(plain.markdown, L"状态 |"), "no status column");
    Check(HasW(plain.markdown, L"组件暂存") && HasW(plain.markdown, L"模板"), "sources");
    Check(HasW(plain.markdown, L"## 卸载时会删除的运行产物"), "residue header");
    Check(HasW(plain.markdown, L"（删除）：运行日志"), "residue delete");
    Check(HasW(plain.markdown, L"（只报告，不删除）：用户配置"), "residue report");

    const control_center::ManifestListBuild withClient =
        control_center::BuildManifestListMarkdown(client, manifest, catalog);
    Check(withClient.ok, "client ok");
    Check(HasW(withClient.markdown, L" 状态 |"), "status column");
    Check(HasW(withClient.markdown, L"| `d3d9.dll` | 客户端根目录 | 组件暂存 | 必需 | D3D9 代理 | 已就位 |"), "row ready");
    Check(HasW(withClient.markdown, L"| `host64/nvngx.dll` | host64 | 组件暂存 | 可选 | 超分库 | 待移入 |"), "row pending");
    Check(HasW(withClient.markdown, L"| `ReShade.ini` | 客户端根目录 | 模板 | 必需 | 配置 | 待移入 |"), "row ini");

    // 目录冲突：同名目录存在时状态为“目录冲突”，且不参与哈希比较。
    const std::wstring conflictClient = Join(root, L"client-conflict");
    MakeDirectory(conflictClient);
    MakeDirectory(Join(conflictClient, L"d3d9.dll"));
    const control_center::ManifestListBuild conflict = control_center::BuildManifestListMarkdown(
        conflictClient, manifest, catalog);
    Check(conflict.ok, "conflict ok");
    Check(HasW(conflict.markdown, L"目录冲突"), "conflict word");

    // 内容不同：文件存在但哈希不符。
    const std::wstring differentClient = Join(root, L"client-different");
    MakeDirectory(differentClient);
    Check(WriteAll(Join(differentClient, L"d3d9.dll"), "different"), "write different");
    const control_center::ManifestListBuild different =
        control_center::BuildManifestListMarkdown(differentClient, manifest, catalog);
    Check(HasW(different.markdown, L"已存在（内容不同）"), "different word");

    // 错误路径。
    const control_center::ManifestListBuild badJson =
        control_center::BuildManifestListMarkdown(L"", "{oops", catalog);
    Check(!badJson.ok && badJson.error == L"安装清单不是合法 JSON。", "bad json");
    const control_center::ManifestListBuild missingEntries =
        control_center::BuildManifestListMarkdown(L"", "{}", catalog);
    Check(!missingEntries.ok && missingEntries.error == L"安装清单缺少 entries 数组。", "missing entries");
    control_center::CatalogResult invalidCatalog;
    invalidCatalog.error = "bad";
    const control_center::ManifestListBuild invalid =
        control_center::BuildManifestListMarkdown(L"", manifest, invalidCatalog);
    Check(!invalid.ok && invalid.error == L"安装清单无效。", "invalid catalog");

    // 写文件：UTF-8 无 BOM，重复写入拒绝。
    // 先清掉上一次运行的输出，否则重复运行会命中“已存在”分支。
    const std::wstring outputPath = Join(root, L"DLSS5移入清单.md");
    DeleteFileW(outputPath.c_str());
    const control_center::ManifestListWriteResult written =
        control_center::WriteManifestListFile(outputPath, withClient.markdown);
    Check(written.ok, "write ok");
    std::string bytes;
    Check(ReadAll(outputPath, bytes), "read output");
    Check(!bytes.empty() && !(bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF), "no bom");
    Check(Has(bytes, "# WOW-335-NR"), "utf8 content");
    Check(bytes == Utf8(withClient.markdown), "bytes equal markdown");
    const control_center::ManifestListWriteResult again =
        control_center::WriteManifestListFile(outputPath, withClient.markdown);
    Check(!again.ok && again.error == L"输出文件已存在。", "exists rejected");
    const control_center::ManifestListWriteResult empty =
        control_center::WriteManifestListFile(L"", withClient.markdown);
    Check(!empty.ok && empty.error == L"输出文件路径为空。", "empty path");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: manifest-list.exe <scratch-root>\n");
        return 2;
    }
    std::wstring root = argv[1];
    root += L"\\manifest-list";
    CreateDirectoryW(root.c_str(), nullptr);
    TestBuild();
    if (gFailures == 0) {
        std::printf("manifest list: markdown, statuses, residue, errors and output guards passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", gFailures);
    return 1;
}