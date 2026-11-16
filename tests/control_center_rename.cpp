#include "../native/control_center/copy_verified.hpp"
#include "../native/control_center/rename_verified.hpp"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1];
    control_center::GuardedPath sample, source, destination, occupied, invalid;
    if (!sample.Open(root, L"real/sample.bin") || !source.Open(root, L"real/rename-source.bin") ||
        !destination.Open(root, L"real/rename-destination.bin") || !occupied.Open(root, L"real/occupied.bin")) return 2;
    const auto sampleHash = control_center::HashFile(sample.Path());
    const auto occupiedHash = control_center::HashFile(occupied.Path());
    if (!sampleHash || !occupiedHash || !control_center::CopyVerifiedNew(sample, source, sampleHash.identity.sha256)) return 3;
    auto wrong = sampleHash.identity.sha256; wrong[0] ^= 1;
    auto result = control_center::RenameVerifiedNew(source, destination, wrong);
    if (result.error != control_center::RenameError::Mismatch || result.renamed ||
        GetFileAttributesW(source.Path().c_str()) == INVALID_FILE_ATTRIBUTES) return 4;
    result = control_center::RenameVerifiedNew(source, occupied, sampleHash.identity.sha256);
    const auto occupiedAfter = control_center::HashFile(occupied.Path());
    if (result.error != control_center::RenameError::Rename || result.renamed || !occupiedAfter ||
        occupiedAfter.identity.sha256 != occupiedHash.identity.sha256) return 5;
    result = control_center::RenameVerifiedNew(source, destination, sampleHash.identity.sha256);
    if (!result || !result.renamed || GetFileAttributesW(source.Path().c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "rename failed stage=%d win32=%lu renamed=%d\n", static_cast<int>(result.error), result.win32Error, result.renamed);
        return 6;
    }
    const auto destHash = control_center::HashFile(destination.Path());
    if (!destHash || destHash.identity.sha256 != sampleHash.identity.sha256 ||
        destHash.identity.indexLow != result.identity.indexLow || destHash.identity.indexHigh != result.identity.indexHigh) return 7;
    result = control_center::RenameVerifiedNew(invalid, destination, sampleHash.identity.sha256);
    if (result.error != control_center::RenameError::InvalidPath) return 8;
    for (int i = 0; i < 128; ++i) {
        control_center::GuardedPath from, to;
        const std::wstring suffix = std::to_wstring(i) + std::wstring(static_cast<size_t>(i % 7), L'x') + L"文件.bin";
        if (!from.Open(root, L"real/from-" + suffix) || !to.Open(root, L"real/to-" + suffix)) return 9;
        const auto created = control_center::CopyVerifiedNew(sample, from, sampleHash.identity.sha256);
        if (!created) return 10;
        result = control_center::RenameVerifiedNew(from, to, sampleHash.identity.sha256, &created.target);
        if (!result || !result.renamed) return 11;
        const auto exact = control_center::HashFile(to.Path());
        if (!exact || exact.identity.sha256 != sampleHash.identity.sha256 ||
            exact.identity.indexHigh != created.target.indexHigh || exact.identity.indexLow != created.target.indexLow) return 12;
    }
    std::puts("verified rename: hash, occupied destination, file identity and 128 exact paths passed");
    return 0;
}
