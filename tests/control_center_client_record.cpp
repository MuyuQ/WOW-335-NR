#include "../native/control_center/client_record.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>

int wmain(int argc, wchar_t** argv) {
    using control_center::Json;
    const std::string installedHash(64, 'a'), previousHash(64, 'b');
    Json present = {{"target", "d3d9.dll"}, {"sha256", installedHash}, {"replaces_original", true},
                    {"pre_state", "present"}, {"previous_sha256", previousHash}, {"backup", "d3d9.dll"}};
    Json absent = {{"target", "host64/component.dll"}, {"sha256", installedHash}, {"replaces_original", false},
                   {"pre_state", "absent"}, {"previous_sha256", nullptr}, {"backup", nullptr}};
    Json record = {{"schema", 1}, {"status", "installed"}, {"tool", "WOW-335-NR"},
                   {"entries", Json::array({present, absent})},
                   {"residue", Json::array({{{"path", "WTF/Config.wtf"}, {"kind", "file"}, {"policy", "report"}},
                                           {{"path", "host64/panel-logs"}, {"kind", "tree"}, {"policy", "delete"}}})}};
    auto parsed = control_center::ReadClientRecord(record.dump());
    if (!parsed || parsed.entries.size() != 2 || !parsed.entries[0].previouslyPresent ||
        parsed.entries[0].previousSha256 != previousHash || parsed.entries[1].previouslyPresent || parsed.residue.size() != 2) return 1;
    auto bad = record;
    bad["entries"].push_back(absent);
    if (control_center::ReadClientRecord(bad.dump())) return 2;
    bad = record; bad["entries"][0]["backup"] = "../outside";
    if (control_center::ReadClientRecord(bad.dump())) return 3;
    bad = record; bad["entries"][0]["previous_sha256"] = "bad";
    if (control_center::ReadClientRecord(bad.dump())) return 4;
    bad = record; bad["entries"][1]["backup"] = "unexpected";
    if (control_center::ReadClientRecord(bad.dump())) return 5;
    bad = record; bad["entries"][0]["pre_state"] = "same";
    if (control_center::ReadClientRecord(bad.dump())) return 6;
    bad = record; bad["residue"][0]["policy"] = "delete";
    if (control_center::ReadClientRecord(bad.dump())) return 7;
    bad = record; bad["residue"][1]["path"] = "../outside";
    if (control_center::ReadClientRecord(bad.dump())) return 8;
    bad = record; bad["schema"] = 2;
    if (control_center::ReadClientRecord(bad.dump())) return 9;
    bad = record; bad["entries"][0]["previous_sha256"] = nullptr;
    parsed = control_center::ReadClientRecord(bad.dump());
    if (!parsed || !parsed.entries[0].previouslyPresent || !parsed.entries[0].previousSha256.empty()) return 10;
    if (control_center::ReadClientRecord("{\"schema\":1,\"schema\":1}")) return 11;
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) return 12;
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        parsed = control_center::ReadClientRecord(text);
        if (!parsed || parsed.entries.empty()) { std::fprintf(stderr, "legacy record rejected: %s\n", parsed.error.c_str()); return 13; }
        std::printf("legacy schema 1 record: %zu entries accepted\n", parsed.entries.size());
    } else if (argc != 1) return 14;
    std::puts("client record: historical structure and unsafe paths validated");
    return 0;
}
