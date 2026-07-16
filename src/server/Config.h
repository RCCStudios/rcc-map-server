#pragma once

#include <uuid_v4.h>
#include <string>

struct Config {
    std::string path = "config/config.yml";
    std::string userDatabasePath = "data/userdb.sql";

    std::string serverCertPath = "web/cert.pem";
    std::string serverPrivateKeyPath = "web/key.pem";
    std::string serverListenToAddress = "0.0.0.0";
    uint16_t serverListenToPort = 443;

    uint32_t snapshotInterval = 180;
    uint32_t dumpInterval = 86400;
};
