#pragma once

#include "../common.h"

#include "../server/OTPPool.h"
#include "User.h"
#include "TelemetryProperty.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <uuid_v4.h>

#include <string>
#include <chrono>

class Server;

class UserDatabase {
    RM_DECLARE_SINGLETON(UserDatabase)

private:
    std::unique_ptr<SQLite::Database> db;

    UUIDv4::UUIDGenerator<std::mt19937_64> UUIDGenerator;

    std::unique_ptr<OTPPool> otpPool = nullptr;

    std::unique_ptr<SQLite::Statement> validateUserQuery;

    std::unique_ptr<SQLite::Statement> finishUserRegistrationQuery;

    std::unique_ptr<SQLite::Statement> getUserByTokenQuery;
    std::unique_ptr<SQLite::Statement> getUserIDByTokenQuery;
    std::unique_ptr<SQLite::Statement> removeUserByTokenQuery;

    std::unique_ptr<SQLite::Statement> getAllUsersQuery;

    Server* parentServer = nullptr;
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
    int validateUser(const UUIDv4::UUID& token); // used in other methods

    int beginUserRegistration(OTP &otp); // from terminal user new
    int finishUserRegistration(OTP otp, User &user); // from HTTP /api/register

    int getUserByToken(User &user); // reserve for later
    int getUserIDByToken(User &user); // from job sendTelemetryUpdate
    int retireUserByToken(const UUIDv4::UUID& token); // from terminal user retire

    int getAllUsers(std::vector<User> &users); // from HTTP /api/ws

    int updateTelemetry(const User &user); // from HTTP /api/sendTelemetry
};
