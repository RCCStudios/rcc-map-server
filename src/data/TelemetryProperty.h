#pragma once

#include <string>
#include <nlohmann/json.hpp>

struct TelemetryProperty {
    int64_t value = 0;
    std::time_t timestamp = 0;
    std::string name;
    nlohmann::json::value_t type = nlohmann::json::value_t::null;

    [[nodiscard]] inline bool isNull() const {
        return type == nlohmann::json::value_t::null;
    };
};
