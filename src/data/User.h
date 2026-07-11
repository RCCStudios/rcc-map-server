#pragma once

#include <uuid_v4.h>

#include <string>

struct User {
    UUIDv4::UUID uuid;
    int32_t key;

    std::string login;
    std::string name;
};

