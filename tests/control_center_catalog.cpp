#include "../native/control_center/catalog.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

static std::string Read(const wchar_t* path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;
    const auto manifestText = Read(argv[1]), lockText = Read(argv[2]);
    const auto manifest = control_center::ParseJsonObject(manifestText);
    const auto lock = control_center::ParseJsonObject(lockText);
    if (!manifest || !lock) return 2;
    const auto catalog = control_center::ReadCatalog(manifestText, lockText);
    if (!catalog || catalog.members.size() != lock.value["members"].size()) {
        std::fprintf(stderr, "actual catalog rejected: %s\n", catalog.error.c_str()); return 3;
    }
    auto bad = lock.value;
    bad["members"][0]["sha256"] = "bad";
    if (control_center::ReadCatalog(manifestText, bad.dump())) return 4;
    bad = lock.value;
    bad["members"][0]["target"] = "../escape";
    if (control_center::ReadCatalog(manifestText, bad.dump())) return 5;
    bad = lock.value;
    bad["members"].push_back(bad["members"][0]);
    if (control_center::ReadCatalog(manifestText, bad.dump())) return 6;
    bad = manifest.value;
    bad["entries"][0]["source_kind"] = "different";
    if (control_center::ReadCatalog(bad.dump(), lockText)) return 7;
    bad = manifest.value;
    bad["entries"].erase(0);
    if (control_center::ReadCatalog(bad.dump(), lockText)) return 8;
    bad = manifest.value;
    bad["entries"][0]["required"] = "false";
    if (control_center::ReadCatalog(bad.dump(), lockText)) return 9;
    bad = lock.value;
    bad["members"][0]["replaces_original"] = true;
    bad["members"][0]["target"] = "somewhere.dll";
    if (control_center::ReadCatalog(manifestText, bad.dump())) return 10;
    if (control_center::ReadCatalog("{\"schema\":1,\"schema\":1}", lockText)) return 11;
    std::printf("catalog: %zu real members and invalid catalog cases passed\n", catalog.members.size());
    return 0;
}
