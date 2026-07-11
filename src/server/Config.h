#pragma once

#include <uuid_v4.h>
#include <string>

struct Config {
    std::string path = "config/common.yml";
    std::string userDatabasePath = "data/userdb.sql";
};
