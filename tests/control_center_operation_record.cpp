#include "../native/control_center/operation_record.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>

int wmain(int argc, wchar_t** argv) {
    using control_center::Json;
    const std::string candidate(64, 'a'), previous(64, 'b');
    const Json present = {{"target", "d3d9.dll"}, {"sha256", candidate}, {"replaces_original", true},
                          {"pre_state", "present"}, {"previous_sha256", previous}, {"backup", "d3d9.dll"}};
    const Json absent = {{"target", "host64/component.dll"}, {"sha256", candidate}, {"replaces_original", false},
                         {"pre_state", "absent"}, {"previous_sha256", nullptr}, {"backup", nullptr}};
    Json record = {{"schema", 2}, {"status", "in_progress"}, {"client", "D:\\client with spaces"},
                   {"entries", Json::array({present, absent})}, {"applied", Json::array({"d3d9.dll"})}};
    auto parsed = control_center::ReadOperationRecord(record.dump());
    if (!parsed || parsed.entries.size() != 2 || parsed.applied.size() != 1 || parsed.status != "in_progress") return 1;
    auto bad = record; bad["applied"].push_back("../outside");
    if (control_center::ReadOperationRecord(bad.dump())) return 2;
    bad = record; bad["applied"].push_back("unknown.dll");
    if (control_center::ReadOperationRecord(bad.dump())) return 3;
    bad = record; bad["applied"].push_back("D3D9.DLL");
    if (control_center::ReadOperationRecord(bad.dump())) return 4;
    bad = record; bad["client"] = "../outside";
    if (control_center::ReadOperationRecord(bad.dump())) return 5;
    bad = record; bad["entries"][0]["backup"] = "../outside";
    if (control_center::ReadOperationRecord(bad.dump())) return 6;
    bad = record; bad["members"] = Json::array({present});
    if (control_center::ReadOperationRecord(bad.dump())) return 7;
    bad = record; bad["status"] = "unsupported";
    if (control_center::ReadOperationRecord(bad.dump())) return 8;
    Json oldMember = {{"target", "d3d9.dll"}, {"sha256", candidate}, {"replaces_original", true}};
    Json oldRecord = {{"schema", 1}, {"status", "completed"}, {"client", "D:\\old-client"},
                      {"members", Json::array({oldMember})}};
    parsed = control_center::ReadOperationRecord(oldRecord.dump());
    if (!parsed || parsed.schema != 1 || !parsed.entries[0].previouslyPresent ||
        parsed.entries[0].backup != L"d3d9.dll" || !parsed.entries[0].previousSha256.empty()) return 9;
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) return 10;
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        parsed = control_center::ReadOperationRecord(text);
        if (!parsed || parsed.entries.empty()) { std::fprintf(stderr, "legacy operation rejected: %s\n", parsed.error.c_str()); return 11; }
        std::printf("legacy operation schema %u: %zu entries accepted\n", parsed.schema, parsed.entries.size());
    } else if (argc != 1) return 12;
    std::puts("operation record: schema 1/2 and untrusted fields validated");
    return 0;
}
