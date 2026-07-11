#pragma once

#include "../common.h"

#include "User.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <uuid_v4.h>

#include <string>
#include <chrono>

class UserDatabase {
    RM_DECLARE_SINGLETON(UserDatabase, const std::string& path)

private:
    std::string dbPath;
    SQLite::Database *db;

    // publications

    SQLite::Statement *insertUserQuery;
    SQLite::Statement *getUserByUUIDQuery;

    UUIDv4::UUIDGenerator<std::mt19937_64> UUIDGenerator;

    int code = 0;

#ifdef RM_DEBUG
    std::chrono::steady_clock::time_point begin;

    void beginElapsedTimer() {
        begin = std::chrono::steady_clock::now();
    }

    int endElapsedTimer() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    }
#endif

public:
    int insertUser(User &user);

    int getUserByUUID(const UUIDv4::UUID& uuid, User &user);
};
