#include "Telemetry.h"

std::vector<TelemetryProperty> Telemetry::schema = {
    {.name = "latitude", .type = nlohmann::json::value_t::number_float},
    {.name = "longitude", .type = nlohmann::json::value_t::number_float},
    {.name = "batteryLevel", .type = nlohmann::json::value_t::number_integer},
    {.name = "network", .type = nlohmann::json::value_t::number_integer},
    {.name = "screenLock", .type = nlohmann::json::value_t::number_integer},
};
