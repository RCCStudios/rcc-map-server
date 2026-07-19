#pragma once

#include "Telemetry.h"
#include <uuid_v4.h>
#include <string>

struct User {
    enum State {
        RM_USER_STATE_UNDEFINED,
        RM_USER_STATE_PENDING_REGISTRATION,
        RM_USER_STATE_RETIRED,
        RM_USER_STATE_ACTIVE,
        RM_USER_STATE_INACTIVE,
        RM_USER_STATE_ENUM_COUNT,
    };

    UUIDv4::UUID token;
    uint32_t id;
    uint32_t key;

    std::string name;
    std::string pfpPath;

    State state = RM_USER_STATE_PENDING_REGISTRATION;
    Telemetry telemetry{};
};

