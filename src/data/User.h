#pragma once

#include "Telemetry.h"
#include <uuid_v4.h>
#include <string>

struct User {
    enum State {
        RM_USER_STATE_UNDEFINED,
        RM_USER_STATE_PENDING_REGISTRATION,
        RM_USER_STATE_ACTIVE,
        RM_USER_STATE_INACTIVE,
    };

    UUIDv4::UUID token;
    int64_t key;

    std::string name;
    std::string pfpPath;

    State state = RM_USER_STATE_PENDING_REGISTRATION;
    Telemetry telemetry{};
};

