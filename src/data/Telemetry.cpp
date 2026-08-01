#include "Telemetry.h"

std::vector<TelemetryProperty> Telemetry::schema = {
    {.name = "latitude", .type = nlohmann::json::value_t::number_float},
    {.name = "longitude", .type = nlohmann::json::value_t::number_float},
    {.name = "batteryStatus", .type = nlohmann::json::value_t::number_unsigned},
    {.name = "networkStatus", .type = nlohmann::json::value_t::number_unsigned},
    {.name = "screenLockStatus", .type = nlohmann::json::value_t::boolean},
};
