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

    isInitialized = true;

    dbPath = path;

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing SQLite DB");

    if (not std::filesystem::exists(dbPath)) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("No user database file found. A new one will be created at path \"{}\"", dbPath));

        try {
            std::filesystem::create_directory("data");
            db = std::make_unique<SQLite::Database>(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

            std::stringstream stream;
            stream << R"(CREATE TABLE "users" ("tokenX" INTEGER, "tokenY" INTEGER, "id" INTEGER UNIQUE, "key" INTEGER, "name" TEXT, "pfpPath" TEXT, "state" INTEGER, )";
            for (const TelemetryProperty &prop: Telemetry::schema) {
                stream << "\"" << prop.name << "\" INTEGER, \"" << prop.name << "TS\" INTEGER, ";
            }
            stream << R"(PRIMARY KEY("tokenX", "tokenY")))";

            RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("DB create table query: {}", stream.str()));

            db->exec(stream.str());
        } catch (std::exception &e) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to create db: {}", e.what()));
            return RM_ERROR_CODE_LIB_SQLITE;
        }
    } else {
        db = std::make_unique<SQLite::Database>(dbPath, SQLite::OPEN_READWRITE);
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

        validateUnregisteredUserByKeyQuery = std::make_unique<SQLite::Statement>(*db, "SELECT state FROM users WHERE key = ?");
        validateRegisteredUserByTokenQuery = std::make_unique<SQLite::Statement>(*db, "SELECT state FROM users WHERE tokenX = ? and tokenY = ?");

        beginUserRegistrationQuery1 = std::make_unique<SQLite::Statement>(*db, "SELECT COUNT(*) FROM users");
        beginUserRegistrationQuery2 = std::make_unique<SQLite::Statement>(*db, "INSERT INTO users (tokenX, tokenY, id, key, state) VALUES (?, ?, ?, ?, ?)");
        finishUserRegistrationQuery1 = std::make_unique<SQLite::Statement>(*db, "SELECT tokenX, tokenY FROM users where key = ?");
        finishUserRegistrationQuery2 = std::make_unique<SQLite::Statement>(*db, "UPDATE users SET name = ?, state = ? WHERE key = ?");
        terminateUserRegistrationQuery = std::make_unique<SQLite::Statement>(*db, "UPDATE users set STATE = ? WHERE key = ?");

        getUserByTokenQuery = std::make_unique<SQLite::Statement>(*db, "SELECT id, key, name, pfpPath, state, " + telemetrySchemaString + " FROM users WHERE tokenX = ? and tokenY = ?");
        getUserIDByTokenQuery = std::make_unique<SQLite::Statement>(*db, "SELECT id FROM users WHERE tokenX = ? and tokenY = ?");
        retireUserByTokenQuery = std::make_unique<SQLite::Statement>(*db, "UPDATE users set STATE = ? WHERE tokenX = ? and tokenY = ?");

        getAllUsersQuery = std::make_unique<SQLite::Statement>(*db, "SELECT tokenX, tokenY, id, key, name, pfpPath, state, " + telemetrySchemaString + " FROM users");
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
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot destroy a Database object since it is not initialized");
        return RM_ERROR_CODE_NOT_INITIALIZED;
    }

    isInitialized = false;

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "UserDatabase object destroyed");

    return 0;
}

int UserDatabase::validateUnregisteredUserByKey(const uint32_t key) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    std::string strKey;
    intToHexString(key, strKey);

    int responseCode = RM_HTTP_CODE_OK;

    try {
        validateUnregisteredUserByKeyQuery->bind(1, key);
        if (validateUnregisteredUserByKeyQuery->executeStep()) {
            if (validateUnregisteredUserByKeyQuery->getColumn(0).getInt() != User::State::RM_USER_STATE_PENDING_REGISTRATION) {
                RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with key {}: the key was already used", strKey));
                responseCode = RM_HTTP_CODE_UNAUTHORIZED;
            }
        } else {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with key {}: incorrect key. Somebody is trying to break in!", strKey));
            responseCode = RM_HTTP_CODE_UNAUTHORIZED;
        }
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to find user with key {}: caught SQLIte exception: {}", strKey, e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    validateUnregisteredUserByKeyQuery->tryReset();

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; key = {}", endElapsedTimer(), strKey));

    return responseCode;
}

int UserDatabase::validateRegisteredUserByToken(const UUIDv4::UUID &token) {
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
        validateRegisteredUserByTokenQuery->bind(1, tokenX);
        validateRegisteredUserByTokenQuery->bind(2, tokenY);
        if (validateRegisteredUserByTokenQuery->executeStep()) {
            if (validateRegisteredUserByTokenQuery->getColumn(0).getInt() == User::State::RM_USER_STATE_PENDING_REGISTRATION) {
                RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with token {}: there is a user with such token, but their state is PENDING_REGISTRATION. Ensure that you sent the key before", token.str()));
                responseCode = RM_HTTP_CODE_UNAUTHORIZED;
            }
        } else {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("User validation failed with token {}: incorrect token. Somebody is trying to break in!", token.str()));
            responseCode = RM_HTTP_CODE_UNAUTHORIZED;
        }
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to find user with token {}: caught SQLIte exception: {}", token.str(), e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    validateRegisteredUserByTokenQuery->tryReset();

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; token = {}", endElapsedTimer(), token.str()));

    return responseCode;
}

int UserDatabase::beginUserRegistration(uint32_t key) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    if (key == 0) {
        const UUIDv4::UUID randomKey = UUIDGenerator.getUUID();
        char bytes[16];
        randomKey.bytes(bytes);

        memcpy(&key, bytes, 4);
    }

    const UUIDv4::UUID token = UUIDGenerator.getUUID();
    char bytes[16];
    token.bytes(bytes);

    int64_t tokenX = 0;
    int64_t tokenY = 0;
    memcpy(&tokenX, bytes + 8, 8);
    memcpy(&tokenY, bytes, 8);

    std::string strKey;
    intToHexString(key, strKey);

    int responseCode = RM_HTTP_CODE_OK;

    try {
        uint32_t userID = beginUserRegistrationQuery1->executeStep();
        userID = beginUserRegistrationQuery1->getColumn(0);

        beginUserRegistrationQuery2->bind(1, tokenX);
        beginUserRegistrationQuery2->bind(2, tokenY);
        beginUserRegistrationQuery2->bind(3, userID);
        beginUserRegistrationQuery2->bind(4, key);
        beginUserRegistrationQuery2->bind(5, User::State::RM_USER_STATE_PENDING_REGISTRATION);
        beginUserRegistrationQuery2->exec();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to insert user with key {}: caught SQLIte exception: {}", strKey, e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    beginUserRegistrationQuery1->tryReset();
    beginUserRegistrationQuery2->tryReset();

    if (responseCode == RM_HTTP_CODE_OK) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("New user registered with key {}. Enter it in the app to finish registration", strKey));
    }

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; key = {}", endElapsedTimer(), strKey));

    return responseCode;
}

int UserDatabase::finishUserRegistration(User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    std::string strKey;
    intToHexString(user.key, strKey);

    int responseCode = RM_HTTP_CODE_OK;

    if ((responseCode = validateUnregisteredUserByKey(user.key)) != RM_HTTP_CODE_OK) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("User registration failed with key {}", strKey));
        return responseCode;
    }

    try {
        finishUserRegistrationQuery1->bind(1, user.key);
        finishUserRegistrationQuery1->executeStep();

        user.token = UUIDv4::UUID(finishUserRegistrationQuery1->getColumn(0).getInt64(), finishUserRegistrationQuery1->getColumn(1).getInt64());

        finishUserRegistrationQuery2->bind(1, user.name);
        finishUserRegistrationQuery2->bind(2, User::State::RM_USER_STATE_UNDEFINED);
        finishUserRegistrationQuery2->bind(3, user.key);
        finishUserRegistrationQuery2->exec();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to insert user with key {}: caught SQLIte exception: {}", strKey, e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    finishUserRegistrationQuery1->tryReset();
    finishUserRegistrationQuery2->tryReset();

    if (responseCode == RM_HTTP_CODE_OK) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("New user registered with key {} and token {}", strKey, user.token.str()));
    }

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; key = {}", endElapsedTimer(), strKey));

    return responseCode;
}

int UserDatabase::terminateUserRegistration(const uint32_t key) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    std::string strKey;
    intToHexString(key, strKey);

    int responseCode = RM_HTTP_CODE_OK;

    if ((responseCode = validateUnregisteredUserByKey(key)) != RM_HTTP_CODE_OK) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("User registration termination failed with key {}", strKey));
        return responseCode;
    }

    try {
        terminateUserRegistrationQuery->bind(1, User::State::RM_USER_STATE_RETIRED);
        terminateUserRegistrationQuery->bind(2, key);
        terminateUserRegistrationQuery->exec();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to find user with key {}: caught SQLIte exception: {}", strKey, e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    terminateUserRegistrationQuery->tryReset();

    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Call done in {}ns; key = {}", endElapsedTimer(), strKey));

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

        user.id = getUserByTokenQuery->getColumn(0).getInt();
        user.key = getUserByTokenQuery->getColumn(1).getInt();
        user.name = getUserByTokenQuery->getColumn(2).getString();
        user.pfpPath = getUserByTokenQuery->getColumn(3).getString();
        user.state = static_cast<User::State>(getUserByTokenQuery->getColumn(4).getInt());

        for (int propIndex = 0; propIndex < Telemetry::schema.size(); propIndex++) {
            if (getUserByTokenQuery->getColumn(propIndex * 2 + 5).isNull() or getUserByTokenQuery->getColumn(propIndex * 2 + 6).isNull()) {
                user.telemetry.data.push_back({0, 0, Telemetry::schema[propIndex].name, nlohmann::json::value_t::null});
                continue;
            }
            user.telemetry.data.push_back({getUserByTokenQuery->getColumn(propIndex * 2 + 5).getInt64(), getUserByTokenQuery->getColumn(propIndex * 2 + 6).getInt64(), Telemetry::schema[propIndex].name, Telemetry::schema[propIndex].type});
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

        user.id = getUserIDByTokenQuery->getColumn(0).getInt();
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
        retireUserByTokenQuery->bind(1, User::State::RM_USER_STATE_RETIRED);
        retireUserByTokenQuery->bind(2, tokenX);
        retireUserByTokenQuery->bind(3, tokenY);
        retireUserByTokenQuery->exec();
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to retire user with token {}: caught SQLIte exception: {}", token.str(), e.what()));
        responseCode = RM_HTTP_CODE_INTERNAL_ERROR;
    }

    retireUserByTokenQuery->tryReset();

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
            user.id = getAllUsersQuery->getColumn(2).getInt();
            user.key = getAllUsersQuery->getColumn(3).getInt();
            user.name = getAllUsersQuery->getColumn(4).getString();
            user.pfpPath = getAllUsersQuery->isColumnNull(5) ? "" : getAllUsersQuery->getColumn(5).getString();
            user.state = static_cast<User::State>(getAllUsersQuery->getColumn(6).getInt());

            for (int propIndex = 0; propIndex < Telemetry::schema.size(); propIndex++) {
                if (getAllUsersQuery->getColumn(propIndex * 2 + 7).isNull() or getAllUsersQuery->getColumn(propIndex * 2 + 8).isNull()) {
                    user.telemetry.data.push_back({0, 0, Telemetry::schema[propIndex].name, nlohmann::json::value_t::null});
                    continue;
                }
                user.telemetry.data.push_back({getAllUsersQuery->getColumn(propIndex * 2 + 7).getInt64(), getAllUsersQuery->getColumn(propIndex * 2 + 8).getInt64(), Telemetry::schema[propIndex].name, Telemetry::schema[propIndex].type});
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

    if ((responseCode = validateRegisteredUserByToken(user.token)) != RM_HTTP_CODE_OK) {
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
    stream << "UPDATE users SET state = " << user.state << ", ";
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
