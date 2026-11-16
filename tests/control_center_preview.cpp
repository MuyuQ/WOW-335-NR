#include "../native/control_center/install_preview.hpp"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring root = argv[1];
    control_center::CatalogResult catalog;
    control_center::CatalogMember item;
    item.source = L"real/sample.bin";
    item.sourceKind = "stage";
    item.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    auto expect = [&](const wchar_t* target, control_center::PreviewState state, bool ready) {
        item.target = target;
        catalog.members = {item};
        const auto preview = control_center::PreviewInstall(root, root, root, catalog);
        return preview.items.size() == 1 && preview.filesReady == ready && preview.items[0].state == state;
    };
    using control_center::PreviewState;
    if (!expect(L"real/copy.bin", PreviewState::Same, true)) return 2;
    if (!expect(L"new-directory/file.bin", PreviewState::Absent, true)) return 3;
    if (!expect(L"real/uncreated.bin", PreviewState::Absent, true)) return 4;
    if (!expect(L"real/occupied.bin", PreviewState::Invalid, false)) return 5;
    item.replacesOriginal = true;
    if (!expect(L"real/occupied.bin", PreviewState::Present, true)) return 6;
    item.replacesOriginal = false;
    if (!expect(L"link/uncreated.bin", PreviewState::Invalid, false)) return 7;
    if (!expect(L"real", PreviewState::Invalid, false)) return 8;
    item.source = L"real/missing.bin";
    item.required = false;
    if (!expect(L"real/uncreated.bin", PreviewState::MissingOptional, true)) return 9;
    item.required = true;
    if (!expect(L"real/uncreated.bin", PreviewState::Invalid, false)) return 10;
    item.source = L"real/sample.bin";
    item.sha256.assign(64, '0');
    if (!expect(L"real/uncreated.bin", PreviewState::Invalid, false)) return 11;
    const auto missingRoot = control_center::PreviewInstall(root + L"\\missing", root, root, catalog);
    if (missingRoot.filesReady || missingRoot.error.empty()) return 12;
    std::puts("preview: same, absent, collision, replacement, junction, optional and source mismatch passed");
    return 0;
}
