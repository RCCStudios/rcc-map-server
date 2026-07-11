#include "UserDatabase.h"

#include "../common.h"

UserDatabase *UserDatabase::singletonInstance = nullptr;
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

            db = new SQLite::Database(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
            db->exec("CREATE TABLE \"users\" (\"token_x\" INTEGER, \"token_y\" INTEGER, \"key\" INTEGER, \"name\" TEXT, \"pfp_path\" TEXT, \"state\" INTEGER, \"timestamp\" INTEGER, \"latitude\" REAL, \"longitude\" REAL, \"battery_level\" INTEGER, PRIMARY KEY(\"token_x\",\"token_y\"))");
        } catch (std::exception &e) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to create db: {}", e.what()));
            return RM_ERROR_CODE_LIB_SQLITE;
        }
    } else {
        db = new SQLite::Database(dbPath, SQLite::OPEN_READWRITE);
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "SQLite db initialized");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Precompiling queries");

    validateUnregisteredUserByKeyQuery = new SQLite::Statement(*db, "SELECT state FROM users WHERE key = ?");
    validateRegisteredUserByTokenQuery = new SQLite::Statement(*db, "SELECT state FROM users WHERE token_x = ? and token_y = ?");

    beginUserRegistrationQuery = new SQLite::Statement(*db, "INSERT INTO users (token_x, token_y, key, state) VALUES (?, ?, ?, ?)");
    finishUserRegistrationQuery1 = new SQLite::Statement(*db, "SELECT token_x, token_y FROM users where key = ?");
    finishUserRegistrationQuery2 = new SQLite::Statement(*db, "UPDATE users SET state = ? WHERE key = ?");
    terminateUserRegistrationQuery = new SQLite::Statement(*db, "DELETE FROM users WHERE key = ?");

    getUserByTokenQuery = new SQLite::Statement(*db, "SELECT key, name, pfp_path, state, timestamp, latitude, longitude, battery_level FROM users WHERE token_x = ? and token_y = ?");
    removeUserByTokenQuery = new SQLite::Statement(*db, "DELETE FROM users WHERE token_x = ? and token_y = ?");

    getAllUsersQuery = new SQLite::Statement(*db, "SELECT token_x, token_y, key, name, pfp_path, state, timestamp, latitude, longitude, battery_level FROM users");

    updateTelemetryQuery = new SQLite::Statement(*db, "UPDATE users SET state = ?, timestamp = ?, latitude = ?, longitude = ?, battery_level = ? WHERE token_x = ? and token_y = ?");

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

    delete validateUnregisteredUserByKeyQuery;
    delete validateRegisteredUserByTokenQuery;

    delete beginUserRegistrationQuery;
    delete finishUserRegistrationQuery1;
    delete finishUserRegistrationQuery2;
    delete terminateUserRegistrationQuery;

    delete getUserByTokenQuery;
    delete removeUserByTokenQuery;

    delete getAllUsersQuery;

    delete updateTelemetryQuery;

    delete db;

    isInitialized = false;

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "UserDatabase object destroyed");

    return 0;
}

int UserDatabase::validateUnregisteredUserByKey(int64_t key, bool &success) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    try {
        validateUnregisteredUserByKeyQuery->bind(1, key);
        validateUnregisteredUserByKeyQuery->tryExecuteStep();

        success = false;

        if (not validateRegisteredUserByTokenQuery->isColumnNull(0)) {
            if (not((success = validateUnregisteredUserByKeyQuery->getColumn(0).getInt() == User::State::RM_USER_STATE_PENDING_REGISTRATION))) {
                RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with key {}, while there were initiated registrations. Ensure that you sent the key correctly", key));
            }
        } else {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with key {}, while no registrations were initiated. Somebody is trying to break in!", key));
        }

        validateUnregisteredUserByKeyQuery->reset();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to find user with key {}: caught SQLite exception: {}", key, e.what()));
        return 0;
    }

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Operation done in {}ns; key = {}", endElapsedTimer(), key));
#endif

    return 0;
}

int UserDatabase::validateRegisteredUserByToken(UUIDv4::UUID token, bool &success) {
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
        validateRegisteredUserByTokenQuery->bind(1, tokenx);
        validateRegisteredUserByTokenQuery->bind(2, tokeny);
        validateRegisteredUserByTokenQuery->tryExecuteStep();

        success = false;

        if (not validateRegisteredUserByTokenQuery->isColumnNull(0)) {
            if (not((success = validateUnregisteredUserByKeyQuery->getColumn(0).getInt() != User::State::RM_USER_STATE_PENDING_REGISTRATION))) {
                RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with token {}, while there was a user with such token, but their state is PENDING_REGISTRATION. Ensure that you sent the key before", token.str()));
            }
        } else {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with token {}. Somebody is trying to break in!", token.str()));
        }

        validateRegisteredUserByTokenQuery->reset();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to find user with token {}: caught SQLite exception: {}", token.str(), e.what()));
        return 0;
    }

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Operation done in {}ns; token = {}", endElapsedTimer(), token.str()));
#endif

    return 0;
}

int UserDatabase::beginUserRegistration(int64_t key) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    if (key == 0) {
        UUIDv4::UUID randomKey = UUIDGenerator.getUUID();
        char bytes[16];
        randomKey.bytes(bytes);

        memcpy(bytes, &key, 8);
    }

    UUIDv4::UUID token = UUIDGenerator.getUUID();
    char bytes[16];
    token.bytes(bytes);

    int64_t tokenx = 0;
    int64_t tokeny = 0;
    memcpy(&tokenx, bytes + 8, 8);
    memcpy(&tokeny, bytes, 8);

    try {
        beginUserRegistrationQuery->bind(1, tokenx);
        beginUserRegistrationQuery->bind(2, tokeny);
        beginUserRegistrationQuery->bind(3, key);
        beginUserRegistrationQuery->bind(4, User::State::RM_USER_STATE_PENDING_REGISTRATION);

        beginUserRegistrationQuery->exec();
        beginUserRegistrationQuery->reset();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to insert user with key {}: caught SQLite exception: {}", key, e.what()));
        return 0;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("New user registered with key {}. Enter it in the app to finish registration", key));

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Operation done in {}ns; key = {}", endElapsedTimer(), key));
#endif

    return 0;
}

int UserDatabase::finishUserRegistration(User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    bool success;
    validateUnregisteredUserByKey(user.key, success);

    if (not success) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("User registration failed with key {}. A null token will be sent", user.key));
        user.token = UUIDv4::UUID(0, 0);

        return 0;
    }

    try {
        finishUserRegistrationQuery1->bind(1, user.key);

        finishUserRegistrationQuery1->exec();

        user.token = UUIDv4::UUID(finishUserRegistrationQuery1->getColumn(0).getInt64(), finishUserRegistrationQuery1->getColumn(1).getInt64());

        finishUserRegistrationQuery1->tryReset();

        finishUserRegistrationQuery2->bind(1, User::State::RM_USER_STATE_UNDEFINED);
        finishUserRegistrationQuery2->bind(2, user.key);

        finishUserRegistrationQuery1->exec();
        finishUserRegistrationQuery1->reset();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to insert user with key {}: caught SQLite exception: {}", user.key, e.what()));
        return 0;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("New user registered with {} and token {}", user.key, user.token.str()));

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Operation done in {}ns; key = {}", endElapsedTimer(), user.key));
#endif

    return 0;
}

int UserDatabase::terminateUserRegistration(int64_t key) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    bool success;
    validateUnregisteredUserByKey(key, success);

    if (not success) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("User registration termination failed with key {}", key));
        return 0;
    }

    try {
        terminateUserRegistrationQuery->bind(1, key);

        terminateUserRegistrationQuery->exec();
        terminateUserRegistrationQuery->reset();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to find user with key {}: caught SQLite exception: {}", key, e.what()));
        return 0;
    }

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Operation done in {}ns; key = {}", endElapsedTimer(), key));
#endif

    return 0;
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

        getUserByTokenQuery->exec();

        user.key = getUserByTokenQuery->getColumn(0).getInt();
        user.name = getUserByTokenQuery->getColumn(1).getString();
        user.pfpPath = getUserByTokenQuery->getColumn(2).getString();
        user.state = static_cast<User::State>(getUserByTokenQuery->getColumn(3).getInt());
        user.telemetry.timestamp = static_cast<std::time_t>(getUserByTokenQuery->getColumn(4).getInt());
        user.telemetry.latitude = getUserByTokenQuery->getColumn(5).getDouble();
        user.telemetry.longitude = getUserByTokenQuery->getColumn(6).getDouble();
        user.telemetry.batteryLevel = getUserByTokenQuery->getColumn(7).getInt();

        getUserByTokenQuery->reset();
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to get user with token {}: caught SQLite exception: {}", user.token.str(), e.what()));
        return 0;
    }

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Operation done in {}ns; token = {}", endElapsedTimer(), user.token.str()));
#endif

    return 0;
}

int UserDatabase::removeUserByToken(User &user) {
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
        removeUserByTokenQuery->bind(1, tokenx);
        removeUserByTokenQuery->bind(2, tokeny);

        getUserByTokenQuery->exec();
        getUserByTokenQuery->reset();
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to remove user with token {}: caught SQLite exception: {}", user.token.str(), e.what()));
        return 0;
    }

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Operation done in {}ns; token = {}", endElapsedTimer(), user.token.str()));
#endif

    return 0;
}

int UserDatabase::getAllUsers(std::vector<User> &users) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    try {
        while (getAllUsersQuery->executeStep()) {
            User user{};
            user.token = UUIDv4::UUID(getUserByTokenQuery->getColumn(0).getInt(), getUserByTokenQuery->getColumn(1).getInt());
            user.key = getUserByTokenQuery->getColumn(2).getInt();
            user.name = getUserByTokenQuery->getColumn(3).getString();
            user.pfpPath = getUserByTokenQuery->getColumn(4).getString();
            user.state = static_cast<User::State>(getUserByTokenQuery->getColumn(5).getInt());
            user.telemetry.timestamp = static_cast<std::time_t>(getUserByTokenQuery->getColumn(6).getInt());
            user.telemetry.latitude = getUserByTokenQuery->getColumn(7).getDouble();
            user.telemetry.longitude = getUserByTokenQuery->getColumn(8).getDouble();
            user.telemetry.batteryLevel = getUserByTokenQuery->getColumn(9).getInt();

            users.push_back(user);
        }

        getAllUsersQuery->reset();
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to get users: caught SQLite exception: {}", e.what()));
        return 0;
    }

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Operation done in {}ns", endElapsedTimer()));
#endif

    return 0;
}

int UserDatabase::updateTelemetry(User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    bool success;
    validateRegisteredUserByToken(user.token, success);

    if (not success) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("User telemetry update failed with invalid token {}", user.token.str()));
        return 0;
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
        updateTelemetryQuery->reset();
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to update telemetry: caught SQLite exception: {}", e.what()));
        return 0;
    }

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Operation done in {}ns, token = {}", endElapsedTimer(), user.token.str()));
#endif

    return 0;
}
