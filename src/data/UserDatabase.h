#pragma once

#include "../common.h"

#include "User.h"
#include "TelemetryProperty.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <uuid_v4.h>

#include <string>
#include <chrono>

class UserDatabase {
    RM_DECLARE_SINGLETON(UserDatabase, const std::string& path)

private:
    std::string dbPath;
    std::unique_ptr<SQLite::Database> db;

    UUIDv4::UUIDGenerator<std::mt19937_64> UUIDGenerator;

    std::unique_ptr<SQLite::Statement> validateUnregisteredUserByKeyQuery;
    std::unique_ptr<SQLite::Statement> validateRegisteredUserByTokenQuery;

    std::unique_ptr<SQLite::Statement> beginUserRegistrationQuery;
    std::unique_ptr<SQLite::Statement> finishUserRegistrationQuery1;
    std::unique_ptr<SQLite::Statement> finishUserRegistrationQuery2;
    std::unique_ptr<SQLite::Statement> terminateUserRegistrationQuery;

    std::unique_ptr<SQLite::Statement> getUserByTokenQuery;
    std::unique_ptr<SQLite::Statement> removeUserByTokenQuery;

    std::unique_ptr<SQLite::Statement> getAllUsersQuery;

    // no updateTelemetryQuery for you, it is compiled in-place

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
    int validateUnregisteredUserByKey(uint32_t key, bool &success); // used in other methods
    int validateRegisteredUserByToken(const UUIDv4::UUID& token, bool &success); // used in other methods

    int beginUserRegistration(uint32_t key = 0); // from terminal user new
    int finishUserRegistration(User &user); // from HTTP /api/register
    int terminateUserRegistration(uint32_t key = 0); // from terminal user remove-new

    int getUserByToken(User &user); // reserve for later
    int removeUserByToken(const UUIDv4::UUID& token); // from terminal user remove

    int getAllUsers(std::vector<User> &users); // from HTTP /getData

    int updateTelemetry(const User &user); // from HTTP /api/sendTelemetry
};
