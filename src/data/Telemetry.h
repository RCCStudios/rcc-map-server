#pragma once

#include "TelemetryProperty.h"

struct Telemetry {
    std::vector<TelemetryProperty> data;
    static std::vector<TelemetryProperty> schema;
};
