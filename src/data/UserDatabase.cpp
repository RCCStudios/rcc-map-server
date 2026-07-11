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
            db->exec(R"(CREATE TABLE users (uuid_x INTEGER, uuid_y INTEGER, key INTEGER, login TEXT UNIQUE, name TEXT, PRIMARY KEY(uuid_x, uuid_y)))");
        }
        catch (std::exception &e) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to create db: {}", e.what()));
            return RM_ERROR_CODE_LIB_SQLITE;
        }
    }

    else {
        db = new SQLite::Database(dbPath, SQLite::OPEN_READWRITE);
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "SQLite db initialized");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Precompiling queries");


    insertUserQuery = new SQLite::Statement(*db, "INSERT INTO users (uuid_x, uuid_y, key, login, name) VALUES (?, ?, ?, ?, ?)");
    getUserByUUIDQuery = new SQLite::Statement(*db, "SELECT key, login, name FROM users WHERE uuid_x = ? and uuid_y = ?");

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

    delete insertUserQuery;
    delete getUserByUUIDQuery;

    delete db;

    isInitialized = false;

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "UserDatabase object destroyed");

    return 0;
}

int UserDatabase::insertUser(User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    user.uuid = UUIDGenerator.getUUID();
    char bytes[16];
    user.uuid.bytes(bytes);

    int64_t uuidx = 0;
    int64_t uuidy = 0;
    memcpy(&uuidx, bytes + 8, 8);
    memcpy(&uuidy, bytes, 8);

    try {
        insertUserQuery->bind(1, uuidx);
        insertUserQuery->bind(2, uuidy);
        insertUserQuery->bind(3, user.key);
        insertUserQuery->bind(4, user.login);
        insertUserQuery->bind(5, user.name);

        insertUserQuery->exec();
        insertUserQuery->tryReset();
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to insert user with login {}: caught SQLite exception: {}", user.login, e.what()));
        return 1;
    }

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Inserted user with login {} in {}ns", user.login, endElapsedTimer()));
#endif

    return 0;
}

int UserDatabase::getUserByUUID(const UUIDv4::UUID& uuid, User &user) {
#ifdef RM_DEBUG
    beginElapsedTimer();
#endif

    user.uuid = uuid;
    char bytes[16];
    user.uuid.bytes(bytes);

    int64_t UUIDx = 0;
    int64_t UUIDy = 0;
    memcpy(&UUIDx, bytes + 8, 8);
    memcpy(&UUIDy, bytes, 8);

    try {
        getUserByUUIDQuery->bind(1, UUIDx);
        getUserByUUIDQuery->bind(2, UUIDy);

        getUserByUUIDQuery->executeStep();

        user.key = getUserByUUIDQuery->getColumn(0).getInt();
        user.login = getUserByUUIDQuery->getColumn(1).getString();
        user.name = getUserByUUIDQuery->getColumn(2).getString();

        getUserByUUIDQuery->tryReset();
    } catch (const std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to get user with UUID {}: caught SQLite exception: {}", user.uuid.str(), e.what()));
        return 1;
    }

#ifdef RM_DEBUG
    RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Got user with UUID {} in {}ns", user.uuid.str(), endElapsedTimer()));
#endif

    return 0;
}
