#include "../native/control_center/strict_json.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;
    using control_center::JsonError;
    struct Case { const char* text; JsonError expected; };
    const Case cases[] = {
        {R"({"schema":1,"entries":[{"x":1},{"x":2}]})", JsonError::None},
        {R"({"x":1,"x":2})", JsonError::DuplicateKey},
        {R"({"objects":[{"x":1,"x":2}]})", JsonError::DuplicateKey},
        {R"({"outer":{"x":1},"x":2})", JsonError::None},
        {R"({"a":1,"\u0061":2})", JsonError::DuplicateKey},
        {R"({"x":1} trailing)", JsonError::Invalid},
        {R"({"x":1,})", JsonError::Invalid},
        {R"({"x":1e9999})", JsonError::Invalid},
        {"{\"x\":\"\xff\"}", JsonError::Invalid},
        {R"([1,2])", JsonError::RootNotObject},
        {R"({"x":1)", JsonError::Invalid},
    };
    for (const auto& item : cases) {
        if (control_center::ParseJsonObject(item.text).error != item.expected) return 2;
    }
    for (int i = 1; i <= 2; ++i) {
        std::ifstream stream(argv[i], std::ios::binary);
        if (!stream) return 3;
        const std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const auto result = control_center::ParseJsonObject(source);
        if (!result || result.value.value("schema", 0) != 1) return 4;
    }
    std::puts("strict JSON: duplicate, nested, malformed, UTF-8 and actual manifest/lock passed");
    return 0;
}
