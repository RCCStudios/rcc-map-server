#pragma once

#include "Telemetry.h"
#include <uuid_v4.h>
#include <string>

struct User {
    UUIDv4::UUID token;
    UUIDv4::UUID id;

    std::string username;
    std::string telegram;

    Telemetry telemetry{};
};

