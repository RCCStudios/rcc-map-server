#include "UserDatabase.h"

#include "../server/Server.h"
#include "../common.h"

std::unique_ptr<UserDatabase> UserDatabase::singletonInstance = nullptr;
bool UserDatabase::isInitialized = false;

int UserDatabase::init() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing UserDatabase object");

    if (isInitialized) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot initialize a UserDatabase object since it is already initialized");
        return RM_ERROR_CODE_ALREADY_INITIALIZED;
    }

    isInitialized = true;
    parentServer = Server::getInstance();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing OTP pool");

    otpPool = std::make_unique<OTPPool>(parentServer->config.otpRegistrationPoolMaxSize, parentServer->config.otpRegistrationTimeToLive);

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "OTP pool initialized");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing SQLite DB");

    if (not std::filesystem::exists(parentServer->config.userDatabasePath)) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("No user database file found. A new one will be created at path \"{}\"", parentServer->config.userDatabasePath));

        try {
            std::filesystem::create_directory("data");
            db = std::make_unique<SQLite::Database>(parentServer->config.userDatabasePath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

            std::stringstream stream;
            stream << R"(CREATE TABLE "users" ("tokenX" INTEGER, "tokenY" INTEGER, "idX" INTEGER, "idY" INTEGER, "name" TEXT, "avatarPath" TEXT, )";
            for (const TelemetryProperty &prop: Telemetry::schema) {
                stream << "\"" << prop.name << "\" INTEGER, \"" << prop.name << "TS\" INTEGER, ";
            }
            stream << R"(PRIMARY KEY("tokenX", "tokenY"), UNIQUE("idX", "idY")))";

            RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("DB create table query: {}", stream.str()));

            db->exec(stream.str());
        } catch (std::exception &e) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to create db: {}", e.what()));
            return RM_ERROR_CODE_LIB_SQLITE;
        }
    } else {
        db = std::make_unique<SQLite::Database>(parentServer->config.userDatabasePath, SQLite::OPEN_READWRITE);
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "SQLite DB initialized");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Precompiling queries");

    try {
        std::stringstream stream;
        for (const TelemetryProperty &prop: Telemetry::schema) {
            stream << prop.name << ", " << prop.name << "TS, ";
        }
        std::string telemetrySchemaString = stream.str();
        if (not Telemetry::schema.empty()) {
            telemetrySchemaString = telemetrySchemaString.substr(0, telemetrySchemaString.size() - 2); // goofy ahh last comma workaround
        }

        validateUserQuery = std::make_unique<SQLite::Statement>(*db, "SELECT 1 FROM users WHERE tokenX = ? and tokenY = ?");

        finishUserRegistrationQuery = std::make_unique<SQLite::Statement>(*db, "INSERT INTO users (tokenX, tokenY, idX, idY, name) VALUES (?, ?, ?, ?, ?)");

        getUserByTokenQuery = std::make_unique<SQLite::Statement>(*db, "SELECT idX, idY name, avatarPath, " + telemetrySchemaString + " FROM users WHERE tokenX = ? and tokenY = ?");
        getUserIDByTokenQuery = std::make_unique<SQLite::Statement>(*db, "SELECT idX, idY FROM users WHERE tokenX = ? and tokenY = ?");
        removeUserByTokenQuery = std::make_unique<SQLite::Statement>(*db, "DELETE FROM users WHERE tokenX = ? and tokenY = ?");

        getAllUsersQuery = std::make_unique<SQLite::Statement>(*db, "SELECT tokenX, tokenY, idX, idY, name, avatarPath, " + telemetrySchemaString + " FROM users");
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to precompile queries: {}", e.what()));
        return RM_ERROR_CODE_LIB_SQLITE;
    }
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Queries precompiled");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "UserDatabase object initialized");

    return 0;
}

int UserDatabase::destroy() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Destroying UserDatabase object");

    if (not isInitialized) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot destroy a UserDatabase object since it is not initialized");
        return RM_ERROR_CODE_NOT_INITIALIZED;
    }

    isInitialized = false;

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "UserDatabase object destroyed");

    return 0;
}

int UserDatabase::validateUser(const UUIDv4::UUID &token) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    char bytes[16];
    token.bytes(bytes);

    int64_t tokenX = 0;
    int64_t tokenY = 0;
    memcpy(&tokenX, bytes + 8, 8);
    memcpy(&tokenY, bytes, 8);

    int responseCode = RM_HTTP_CODE_OK;

    try {
        validateUserQuery->bind(1, tokenX);
        validateUserQuery->bind(2, tokenY);
        if (not validateUserQuery->executeStep()) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with token {}: incorrect token. Somebody is trying to break in!", token.str()));
            responseCode = RM_HTTP_CODE_UNAUTHORIZED;
        }
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to find user with token {}: caught SQLIte exception: {}", token.str(), e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    validateUserQuery->tryReset();

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; token = {}", endElapsedTimer(), token.str()));

    return responseCode;
}

int UserDatabase::beginUserRegistration(OTP &otp) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    if ((otp = otpPool->getOTP(UUIDGenerator.getUUID())) == 0) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to get a registration OTP"));
        return RM_HTTP_CODE_INTERNAL_ERROR;
    }

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns", endElapsedTimer()));
    return RM_HTTP_CODE_OK;
}

int UserDatabase::finishUserRegistration(OTP otp, User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    std::string strOtp;
    intToHexString(otp, strOtp);

    if ((user.token = otpPool->getToken(otp)) == UUIDv4::UUID(0, 0)) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("User registration failed with OTP {}: incorrect OTP. Somebody is trying to break in!", strOtp));
        return RM_HTTP_CODE_UNAUTHORIZED;
    }

    char bytes[16];
    user.token.bytes(bytes);

    int64_t tokenX = 0;
    int64_t tokenY = 0;
    memcpy(&tokenX, bytes + 8, 8);
    memcpy(&tokenY, bytes, 8);

    user.id = UUIDGenerator.getUUID();
    user.id.bytes(bytes);

    int64_t idX = 0;
    int64_t idY = 0;
    memcpy(&idX, bytes + 8, 8);
    memcpy(&idY, bytes, 8);

    int responseCode = RM_HTTP_CODE_OK;

    try {
        finishUserRegistrationQuery->bind(1, tokenX);
        finishUserRegistrationQuery->bind(2, tokenY);
        finishUserRegistrationQuery->bind(3, idX);
        finishUserRegistrationQuery->bind(4, idY);
        finishUserRegistrationQuery->bind(5, user.name);
        finishUserRegistrationQuery->exec();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to insert user with OTP {}: caught SQLIte exception: {}", strOtp, e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    finishUserRegistrationQuery->tryReset();

    if (responseCode == RM_HTTP_CODE_OK) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("New user registered with OTP {} and token {}", strOtp, user.token.str()));
    }

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; OTP = {}", endElapsedTimer(), strOtp));

    return responseCode;
}

int UserDatabase::getUserByToken(User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    char bytes[16];
    user.token.bytes(bytes);

    int64_t tokenX = 0;
    int64_t tokenY = 0;
    memcpy(&tokenX, bytes + 8, 8);
    memcpy(&tokenY, bytes, 8);

    int responseCode = RM_HTTP_CODE_OK;

    try {
        getUserByTokenQuery->bind(1, tokenX);
        getUserByTokenQuery->bind(2, tokenY);
        getUserByTokenQuery->executeStep();

        user.id = UUIDv4::UUID(getUserByTokenQuery->getColumn(0).getInt(), getUserByTokenQuery->getColumn(1).getInt());
        user.name = getUserByTokenQuery->getColumn(2).getString();
        user.avatarPath = getUserByTokenQuery->isColumnNull(3) ? "" : getUserByTokenQuery->getColumn(3).getString();

        for (int propIndex = 0; propIndex < Telemetry::schema.size(); propIndex++) {
            if (getUserByTokenQuery->getColumn(propIndex * 2 + 4).isNull() or getUserByTokenQuery->getColumn(propIndex * 2 + 5).isNull()) {
                user.telemetry.data.push_back({0, 0, Telemetry::schema[propIndex].name, nlohmann::json::value_t::null});
                continue;
            }
            user.telemetry.data.push_back({getUserByTokenQuery->getColumn(propIndex * 2 + 4).getInt64(), getUserByTokenQuery->getColumn(propIndex * 2 + 5).getInt64(), Telemetry::schema[propIndex].name, Telemetry::schema[propIndex].type});
        }
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to get user with token {}: caught SQLIte exception: {}", user.token.str(), e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    getUserByTokenQuery->tryReset();

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; token = {}", endElapsedTimer(), user.token.str()));

    return responseCode;
}

int UserDatabase::getUserIDByToken(User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    char bytes[16];
    user.token.bytes(bytes);

    int64_t tokenX = 0;
    int64_t tokenY = 0;
    memcpy(&tokenX, bytes + 8, 8);
    memcpy(&tokenY, bytes, 8);

    int responseCode = RM_HTTP_CODE_OK;

    try {
        getUserIDByTokenQuery->bind(1, tokenX);
        getUserIDByTokenQuery->bind(2, tokenY);
        getUserIDByTokenQuery->executeStep();

        user.id = UUIDv4::UUID(getUserIDByTokenQuery->getColumn(0).getInt(), getUserIDByTokenQuery->getColumn(1).getInt());
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to get user with token {}: caught SQLIte exception: {}", user.token.str(), e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    getUserIDByTokenQuery->tryReset();

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; token = {}", endElapsedTimer(), user.token.str()));

    return responseCode;
}

int UserDatabase::retireUserByToken(const UUIDv4::UUID &token) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    char bytes[16];
    token.bytes(bytes);

    int64_t tokenX = 0;
    int64_t tokenY = 0;
    memcpy(&tokenX, bytes + 8, 8);
    memcpy(&tokenY, bytes, 8);

    int responseCode = RM_HTTP_CODE_OK;

    try {
        removeUserByTokenQuery->bind(1, tokenX);
        removeUserByTokenQuery->bind(2, tokenY);
        removeUserByTokenQuery->exec();
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to retire user with token {}: caught SQLIte exception: {}", token.str(), e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    removeUserByTokenQuery->tryReset();

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; token = {}", endElapsedTimer(), token.str()));

    return responseCode;
}

int UserDatabase::getAllUsers(std::vector<User> &users) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    int responseCode = RM_HTTP_CODE_OK;

    try {
        while (getAllUsersQuery->executeStep()) {
            User user{};
            user.token = UUIDv4::UUID(getAllUsersQuery->getColumn(0).getInt(), getAllUsersQuery->getColumn(1).getInt());
            user.id = UUIDv4::UUID(getAllUsersQuery->getColumn(2).getInt(), getAllUsersQuery->getColumn(3).getInt());
            user.name = getAllUsersQuery->getColumn(4).getString();
            user.avatarPath = getAllUsersQuery->isColumnNull(5) ? "" : getAllUsersQuery->getColumn(5).getString();

            for (int propIndex = 0; propIndex < Telemetry::schema.size(); propIndex++) {
                if (getAllUsersQuery->getColumn(propIndex * 2 + 6).isNull() or getAllUsersQuery->getColumn(propIndex * 2 + 7).isNull()) {
                    user.telemetry.data.push_back({0, 0, Telemetry::schema[propIndex].name, nlohmann::json::value_t::null});
                    continue;
                }
                user.telemetry.data.push_back({getAllUsersQuery->getColumn(propIndex * 2 + 6).getInt64(), getAllUsersQuery->getColumn(propIndex * 2 + 7).getInt64(), Telemetry::schema[propIndex].name, Telemetry::schema[propIndex].type});
            }

            users.push_back(user);
        }
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to get users: caught SQLIte exception: {}", e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    getAllUsersQuery->tryReset();

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns", endElapsedTimer()));

    return responseCode;
}

int UserDatabase::updateTelemetry(const User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif


    if (user.telemetry.data.empty()) {
        return RM_HTTP_CODE_BAD_REQUEST;
    }

    int responseCode = RM_HTTP_CODE_OK;

    if ((responseCode = validateUser(user.token)) != RM_HTTP_CODE_OK) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("User telemetry update failed with invalid token {}", user.token.str()));
        return responseCode;
    }

    char bytes[16];
    user.token.bytes(bytes);

    int64_t tokenX = 0;
    int64_t tokenY = 0;
    memcpy(&tokenX, bytes + 8, 8);
    memcpy(&tokenY, bytes, 8);

    std::stringstream stream;
    stream << "UPDATE users SET ";
    for (const TelemetryProperty &prop: user.telemetry.data) {
        stream << prop.name << " = " << prop.value << ", " << prop.name << "TS = " << prop.timestamp << ", ";
    }
    if (not user.telemetry.data.empty()) {
        stream.seekp(-2, std::stringstream::cur); // goofy ahh last comma workaround: electric boogaloo
    }
    stream << " WHERE tokenX = " << tokenX << " and tokenY = " << tokenY;
    std::string query = stream.str();

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Update telemetry query: {}", query));

    try {
        db->exec(query);
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to update telemetry: caught SQLIte exception: {}", e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns, token = {}", endElapsedTimer(), user.token.str()));

    return responseCode;
}
