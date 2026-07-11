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

    UUIDv4::UUIDGenerator<std::mt19937_64> UUIDGenerator;

    SQLite::Statement *validateUnregisteredUserByKeyQuery;
    SQLite::Statement *validateRegisteredUserByTokenQuery;

    SQLite::Statement *beginUserRegistrationQuery;
    SQLite::Statement *finishUserRegistrationQuery1;
    SQLite::Statement *finishUserRegistrationQuery2;
    SQLite::Statement *terminateUserRegistrationQuery;

    SQLite::Statement *getUserByTokenQuery;
    SQLite::Statement *removeUserByTokenQuery;

    SQLite::Statement *getAllUsersQuery;

    SQLite::Statement *updateTelemetryQuery;

    int code = 0;

#ifdef RM_DEBUG
    std::chrono::steady_clock::time_point begin;

    void beginElapsedTimer() {
        begin = std::chrono::steady_clock::now();
    }

    [[nodiscard]] int endElapsedTimer() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    }
#endif

public:
    int validateUnregisteredUserByKey(int64_t key, bool &success); // used in other methods
    int validateRegisteredUserByToken(UUIDv4::UUID token, bool &success); // used in other methods

    int beginUserRegistration(int64_t key = 0); // from terminal user new
    int finishUserRegistration(User &user); // from HTTP /register
    int terminateUserRegistration(int64_t key = 0); // from terminal user remove-new

    int getUserByToken(User &user); // reserve for later
    int removeUserByToken(User &user); // from terminal user remove

    int getAllUsers(std::vector<User> &users); // from HTTP /getData

    int updateTelemetry(User &user); // from HTTP /sendData
};
