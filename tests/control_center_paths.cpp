#include "../native/control_center/member_path.hpp"
#include <cstdio>

int main() {
    struct Case { const wchar_t* path; bool valid; };
    const Case cases[] = {
        {L"d3d9.dll", true}, {L"host64/组件 文件.dll", true},
        {L"host64\\ReShade.ini", true}, {L"", false}, {L"../d3d9.dll", false},
        {L"host64/../d3d9.dll", false}, {L"C:\\game\\file", false},
        {L"\\\\server\\share", false}, {L"file:stream", false},
        {L"host64//file", false}, {L"host64/file.", false},
        {L"host64/NUL.txt", false}, {L"host64/COM1", false},
        {L"host64/COM\u00b9.txt", false}, {L"host64/LPT\u00b2", false},
        {L"host64/CONIN$", false}, {L"host64/CONOUT$", false},
        {L"Data/file", false}, {L"wtf/config.wtf", false},
        {L"host64/bad?.dll", false}, {L"host64/", false},
    };
    for (const auto& item : cases) {
        if (control_center::ValidMemberPath(item.path) != item.valid) {
            std::fwprintf(stderr, L"Unexpected result for: %ls\n", item.path);
            return 1;
        }
    }
    std::printf("member path validation: %zu cases passed\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
