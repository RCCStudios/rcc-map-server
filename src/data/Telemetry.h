#pragma once

#include <ctime>

struct Telemetry {
    std::time_t timestamp = 0;

    float latitude = 0;
    float longitude = 0;

    int batteryLevel = 0;
};
