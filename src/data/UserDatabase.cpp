#include "UserDatabase.h"

#include "../common.h"

std::unique_ptr<UserDatabase> UserDatabase::singletonInstance = nullptr;
bool UserDatabase::isInitialized = false;

int UserDatabase::init(const std::string &path) {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing UserDatabase object");

    if (isInitialized) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot initialize a UserDatabase object since it is already initialized");
        return RM_ERROR_CODE_ALREADY_INITIALIZED;
    }

    dbPath = path;

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing SQLite db");

    if (not std::filesystem::exists(dbPath)) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("No user database file found. A new one will be created at path \"{}\"", dbPath));

        try {
            std::filesystem::create_directory("data");

            db = std::make_unique<SQLite::Database>(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
            db->exec("CREATE TABLE \"users\" (\"token_x\" INTEGER, \"token_y\" INTEGER, \"key\" INTEGER, \"name\" TEXT, \"pfp_path\" TEXT, \"state\" INTEGER, \"timestamp\" INTEGER, \"latitude\" REAL, \"longitude\" REAL, \"battery_level\" INTEGER, PRIMARY KEY(\"token_x\",\"token_y\"))");
        } catch (std::exception &e) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to create db: {}", e.what()));
            return RM_ERROR_CODE_LIB_SQLITE;
        }
    } else {
        db = std::make_unique<SQLite::Database>(dbPath, SQLite::OPEN_READWRITE);
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "SQLite db initialized");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Precompiling queries");

    validateUnregisteredUserByKeyQuery = std::make_unique<SQLite::Statement>(*db, "SELECT state FROM users WHERE key = ?");
    validateRegisteredUserByTokenQuery = std::make_unique<SQLite::Statement>(*db, "SELECT state FROM users WHERE token_x = ? and token_y = ?");

    beginUserRegistrationQuery = std::make_unique<SQLite::Statement>(*db, "INSERT INTO users (token_x, token_y, key, state) VALUES (?, ?, ?, ?)");
    finishUserRegistrationQuery1 = std::make_unique<SQLite::Statement>(*db, "SELECT token_x, token_y FROM users where key = ?");
    finishUserRegistrationQuery2 = std::make_unique<SQLite::Statement>(*db, "UPDATE users SET state = ? WHERE key = ?");
    terminateUserRegistrationQuery = std::make_unique<SQLite::Statement>(*db, "DELETE FROM users WHERE key = ?");

    getUserByTokenQuery = std::make_unique<SQLite::Statement>(*db, "SELECT key, name, pfp_path, state, timestamp, latitude, longitude, battery_level FROM users WHERE token_x = ? and token_y = ?");
    removeUserByTokenQuery = std::make_unique<SQLite::Statement>(*db, "DELETE FROM users WHERE token_x = ? and token_y = ?");

    getAllUsersQuery = std::make_unique<SQLite::Statement>(*db, "SELECT token_x, token_y, key, name, pfp_path, state, timestamp, latitude, longitude, battery_level FROM users");

    updateTelemetryQuery = std::make_unique<SQLite::Statement>(*db, "UPDATE users SET state = ?, timestamp = ?, latitude = ?, longitude = ?, battery_level = ? WHERE token_x = ? and token_y = ?");

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Queries precompiled");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "UserDatabase object initialized");

    isInitialized = true;

    return 0;
}

int UserDatabase::destroy() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Destroying UserDatabase object");

    if (not isInitialized) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot destroy a Database object since it is not initialized");
        return RM_ERROR_CODE_NOT_INITIALIZED;
    }

    isInitialized = false;

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "UserDatabase object destroyed");

    return 0;
}

int UserDatabase::validateUnregisteredUserByKey(uint32_t key, bool &success) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    success = false;

    std::string strKey;
    intToHexString(key, strKey);

    try {
        validateUnregisteredUserByKeyQuery->bind(1, key);
        if (validateUnregisteredUserByKeyQuery->executeStep()) {
            if (not((success = validateUnregisteredUserByKeyQuery->getColumn(0).getInt() == User::State::RM_USER_STATE_PENDING_REGISTRATION))) {
                RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with key {}: the key was already used", strKey));
            }
        } else {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with key {}: incorrect key. Somebody is trying to break in!", strKey));
        }
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to find user with key {}: caught SQLite exception: {}", strKey, e.what()));
        return RM_HTTP_CODE_INTERNAL_ERROR;
    }

    validateUnregisteredUserByKeyQuery->reset();

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; key = {}", endElapsedTimer(), strKey));
#endif

    return RM_HTTP_CODE_OK;
}

int UserDatabase::validateRegisteredUserByToken(UUIDv4::UUID token, bool &success) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    success = false;

    char bytes[16];
    token.bytes(bytes);

    int64_t tokenx = 0;
    int64_t tokeny = 0;
    memcpy(&tokenx, bytes + 8, 8);
    memcpy(&tokeny, bytes, 8);

    try {
        validateRegisteredUserByTokenQuery->bind(1, tokenx);
        validateRegisteredUserByTokenQuery->bind(2, tokeny);
        if (validateRegisteredUserByTokenQuery->executeStep()) {
            if (not((success = validateRegisteredUserByTokenQuery->getColumn(0).getInt() != User::State::RM_USER_STATE_PENDING_REGISTRATION))) {
                RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with token {}: there is a user with such token, but their state is PENDING_REGISTRATION. Ensure that you sent the key before", token.str()));
            }
        }
        else {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with token {}: incorrect token. Somebody is trying to break in!", token.str()));
        }
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to find user with token {}: caught SQLite exception: {}", token.str(), e.what()));
        return RM_HTTP_CODE_INTERNAL_ERROR;
    }

    validateRegisteredUserByTokenQuery->reset();

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; token = {}", endElapsedTimer(), token.str()));
#endif

    return RM_HTTP_CODE_OK;
}

int UserDatabase::beginUserRegistration(uint32_t key) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    if (key == 0) {
        UUIDv4::UUID randomKey = UUIDGenerator.getUUID();
        char bytes[16];
        randomKey.bytes(bytes);

        memcpy(&key, bytes, 4);
    }

    UUIDv4::UUID token = UUIDGenerator.getUUID();
    char bytes[16];
    token.bytes(bytes);

    int64_t tokenx = 0;
    int64_t tokeny = 0;
    memcpy(&tokenx, bytes + 8, 8);
    memcpy(&tokeny, bytes, 8);

    std::string strKey;
    intToHexString(key, strKey);

    try {
        beginUserRegistrationQuery->bind(1, tokenx);
        beginUserRegistrationQuery->bind(2, tokeny);
        beginUserRegistrationQuery->bind(3, key);
        beginUserRegistrationQuery->bind(4, User::State::RM_USER_STATE_PENDING_REGISTRATION);
        beginUserRegistrationQuery->exec();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to insert user with key {}: caught SQLite exception: {}", strKey, e.what()));
        return RM_HTTP_CODE_INTERNAL_ERROR;
    }

    beginUserRegistrationQuery->reset();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("New user registered with key {}. Enter it in the app to finish registration", strKey));

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; key = {}", endElapsedTimer(), strKey));
#endif

    return RM_HTTP_CODE_OK;
}

int UserDatabase::finishUserRegistration(User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    std::string strKey;
    intToHexString(user.key, strKey);

    bool success;
    if ((code = validateUnregisteredUserByKey(user.key, success)) != RM_HTTP_CODE_OK) {
        return code;
    }

    if (not success) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("User registration failed with key {}", strKey));
        return RM_HTTP_CODE_UNAUTHORIZED;
    }

    try {
        finishUserRegistrationQuery1->bind(1, user.key);
        finishUserRegistrationQuery1->executeStep();

        user.token = UUIDv4::UUID(finishUserRegistrationQuery1->getColumn(0).getInt64(), finishUserRegistrationQuery1->getColumn(1).getInt64());

        finishUserRegistrationQuery2->bind(1, User::State::RM_USER_STATE_UNDEFINED);
        finishUserRegistrationQuery2->bind(2, user.key);
        finishUserRegistrationQuery2->exec();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to insert user with key {}: caught SQLite exception: {}", strKey, e.what()));
        return RM_HTTP_CODE_INTERNAL_ERROR;
    }

    finishUserRegistrationQuery1->reset();
    finishUserRegistrationQuery2->reset();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("New user registered with {} and token {}", strKey, user.token.str()));

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; key = {}", endElapsedTimer(), strKey));
#endif

    return RM_HTTP_CODE_OK;
}

int UserDatabase::terminateUserRegistration(uint32_t key) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    std::string strKey;
    intToHexString(key, strKey);

    bool success;
    if ((code = validateUnregisteredUserByKey(key, success)) != RM_HTTP_CODE_OK) {
        return code;
    }

    if (not success) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("User registration termination failed with key {}", strKey));
        return RM_HTTP_CODE_UNAUTHORIZED;
    }

    try {
        terminateUserRegistrationQuery->bind(1, key);
        terminateUserRegistrationQuery->exec();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to find user with key {}: caught SQLite exception: {}", strKey, e.what()));
        return RM_HTTP_CODE_INTERNAL_ERROR;
    }

    terminateUserRegistrationQuery->reset();

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; key = {}", endElapsedTimer(), strKey));
#endif

    return RM_HTTP_CODE_OK;
}

int UserDatabase::getUserByToken(User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    char bytes[16];
    user.token.bytes(bytes);

    int64_t tokenx = 0;
    int64_t tokeny = 0;
    memcpy(&tokenx, bytes + 8, 8);
    memcpy(&tokeny, bytes, 8);

    try {
        getUserByTokenQuery->bind(1, tokenx);
        getUserByTokenQuery->bind(2, tokeny);
        getUserByTokenQuery->executeStep();

        user.key = getUserByTokenQuery->getColumn(0).getInt();
        user.name = getUserByTokenQuery->getColumn(1).getString();
        user.pfpPath = getUserByTokenQuery->getColumn(2).getString();
        user.state = static_cast<User::State>(getUserByTokenQuery->getColumn(3).getInt());
        user.telemetry.timestamp = static_cast<std::time_t>(getUserByTokenQuery->getColumn(4).getInt());
        user.telemetry.latitude = getUserByTokenQuery->getColumn(5).getDouble();
        user.telemetry.longitude = getUserByTokenQuery->getColumn(6).getDouble();
        user.telemetry.batteryLevel = getUserByTokenQuery->getColumn(7).getInt();
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to get user with token {}: caught SQLite exception: {}", user.token.str(), e.what()));
        return RM_HTTP_CODE_INTERNAL_ERROR;
    }

    getUserByTokenQuery->reset();

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; token = {}", endElapsedTimer(), user.token.str()));
#endif

    return RM_HTTP_CODE_OK;
}

int UserDatabase::removeUserByToken(UUIDv4::UUID token) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    char bytes[16];
    token.bytes(bytes);

    int64_t tokenx = 0;
    int64_t tokeny = 0;
    memcpy(&tokenx, bytes + 8, 8);
    memcpy(&tokeny, bytes, 8);

    try {
        removeUserByTokenQuery->bind(1, tokenx);
        removeUserByTokenQuery->bind(2, tokeny);
        removeUserByTokenQuery->exec();
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to remove user with token {}: caught SQLite exception: {}", token.str(), e.what()));
        return RM_HTTP_CODE_INTERNAL_ERROR;
    }

    removeUserByTokenQuery->reset();

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; token = {}", endElapsedTimer(), token.str()));
#endif

    return RM_HTTP_CODE_OK;
}

int UserDatabase::getAllUsers(std::vector<User> &users) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    try {
        while (getAllUsersQuery->executeStep()) {
            User user{};
            user.token = UUIDv4::UUID(getAllUsersQuery->getColumn(0).getInt(), getAllUsersQuery->getColumn(1).getInt());
            user.key = getAllUsersQuery->getColumn(2).getInt();
            user.name = getAllUsersQuery->getColumn(3).getString();
            user.pfpPath = getAllUsersQuery->getColumn(4).getString();
            user.state = static_cast<User::State>(getAllUsersQuery->getColumn(5).getInt());
            user.telemetry.timestamp = static_cast<std::time_t>(getAllUsersQuery->getColumn(6).getInt());
            user.telemetry.latitude = getAllUsersQuery->getColumn(7).getDouble();
            user.telemetry.longitude = getAllUsersQuery->getColumn(8).getDouble();
            user.telemetry.batteryLevel = getAllUsersQuery->getColumn(9).getInt();

            users.push_back(user);
        }
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to get users: caught SQLite exception: {}", e.what()));
        return RM_HTTP_CODE_INTERNAL_ERROR;
    }

    getAllUsersQuery->reset();

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns", endElapsedTimer()));
#endif

    return RM_HTTP_CODE_OK;
}

int UserDatabase::updateTelemetry(User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    bool success;
    if ((code = validateRegisteredUserByToken(user.token, success)) != RM_HTTP_CODE_OK) {
        return code;
    }

    if (not success) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("User telemetry update failed with invalid token {}", user.token.str()));
        return RM_HTTP_CODE_UNAUTHORIZED;
    }

    char bytes[16];
    user.token.bytes(bytes);

    int64_t tokenx = 0;
    int64_t tokeny = 0;
    memcpy(&tokenx, bytes + 8, 8);
    memcpy(&tokeny, bytes, 8);

    try {
        updateTelemetryQuery->bind(1, user.state);
        updateTelemetryQuery->bind(2, user.telemetry.timestamp);
        updateTelemetryQuery->bind(3, user.telemetry.latitude);
        updateTelemetryQuery->bind(4, user.telemetry.longitude);
        updateTelemetryQuery->bind(5, user.telemetry.batteryLevel);
        updateTelemetryQuery->bind(6, tokenx);
        updateTelemetryQuery->bind(7, tokeny);
        updateTelemetryQuery->exec();
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to update telemetry: caught SQLite exception: {}", e.what()));
        return RM_HTTP_CODE_INTERNAL_ERROR;
    }

    updateTelemetryQuery->reset();

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns, token = {}", endElapsedTimer(), user.token.str()));
#endif

    return RM_HTTP_CODE_OK;
}
