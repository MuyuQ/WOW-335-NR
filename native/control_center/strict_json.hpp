#pragma once

#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "vendor/nlohmann/json.hpp"

namespace control_center {
using Json = nlohmann::json;
enum class JsonError { None, DuplicateKey, Invalid, RootNotObject };
struct JsonResult {
    JsonError error = JsonError::None;
    Json value;
    explicit operator bool() const { return error == JsonError::None; }
};
class DuplicateJsonKey : public std::runtime_error {
public:
    DuplicateJsonKey() : std::runtime_error("duplicate JSON key") {}
};

// Parse the entire UTF-8 input. Maintain one set per currently open object;
// duplicate detection must happen during parsing, before the DOM overwrites keys.
inline JsonResult ParseJsonObject(const std::string& source) {
    JsonResult result;
    std::vector<std::set<std::string>> keys;
    auto callback = [&keys](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            if (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second) throw DuplicateJsonKey();
        } else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    try {
        result.value = Json::parse(source, callback);
        if (!result.value.is_object()) { result.value = nullptr; result.error = JsonError::RootNotObject; }
    } catch (const DuplicateJsonKey&) {
        result.value = nullptr; result.error = JsonError::DuplicateKey;
    } catch (const Json::exception&) {
        result.value = nullptr; result.error = JsonError::Invalid;
    }
    return result;
}
}
