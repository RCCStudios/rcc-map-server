#pragma once

#include <ctime>

struct Telemetry {
    std::time_t timestamp = 0;

    double latitude = 0;
    double longitude = 0;

    int batteryLevel = 0;
};
