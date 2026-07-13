#include "Server.h"

#include "../common.h"

#include "../data/User.h"

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <fstream>
#include <chrono>
#include <filesystem>

std::unique_ptr<Server> Server::singletonInstance = nullptr;
bool Server::isInitialized = false;

std::atomic_bool Server::running = false;
std::atomic_bool Server::restartRequired = false;

int Server::init() {
    if (isInitialized) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot initialize a Server object since it is already initialized");
        return RM_ERROR_CODE_ALREADY_INITIALIZED;
    }

    running = true;
    restartRequired = false;

    nextDump = time(nullptr);

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing threads");

    workerThread = std::thread(&Server::workerThreadLoop, this);
    std::stringstream workerThreadIdStringStream;
    workerThreadIdStringStream << workerThread.get_id();
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Worker thread: {}", workerThreadIdStringStream.str()));

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Doing initial jobs");

    // do some initial jobs
    if ((code = loadConfig())) {
        return code;
    }
    if ((code = loadDatabases())) {
        return code;
    }
    if ((code = loadWebServer())) {
        return code;
    }
    if ((code = scheduleNextDump())) {
        return code;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initial jobs done");

    inputThread = std::thread(&Server::inputThreadLoop, this);
    std::stringstream inputThreadIdStringStream;
    inputThreadIdStringStream << inputThread.get_id();
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Input thread: {}", inputThreadIdStringStream.str()));

    webServerThread = std::thread(&Server::webServerThreadLoop, this);
    std::stringstream webServerThreadIdStringStream;
    webServerThreadIdStringStream << webServerThread.get_id();
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Web server thread: {}", webServerThreadIdStringStream.str()));

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Threads initialized");

    isInitialized = true;

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initial jobs done");

    return 0;
}

int Server::destroy() {
    if (not isInitialized) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot destroy a Server object since it is not initialized");
        return RM_ERROR_CODE_NOT_INITIALIZED;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Doing final jobs");

    // do some final jobs
    unloadWebServer();
    unloadDatabases();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Final jobs done");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Stopping threads");

    workerCV.notify_all();
    workerThread.join();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Worker thread joined");

    inputCV.notify_all();
    inputThread.join(); // unable to join() because of std::getline()

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Input thread joined");

    webServerCV.notify_all();
    webServerThread.join(); // unable to join() because of std::getline()

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Web server thread joined");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Threads stopped");

    isInitialized = false;
    return 0;
}

int Server::run() {
    // main app cycle
    while (running) {
        std::unique_lock<std::mutex> uniqueLocker(mainMutex);
        mainCV.wait_until(uniqueLocker, std::chrono::system_clock::from_time_t(nextDump), [] { return not running; });

        if (std::time(nullptr) >= nextDump - 1) {
            executeJob([this] { return dumpTelemetry(); });
            scheduleNextDump();
        }
    }

    return code;
}

void Server::workerThreadLoop() {
    while (running) {
        std::unique_lock<std::mutex> uniqueLocker(workerMutex);
        workerCV.wait(uniqueLocker, [this] { return not jobsToDo.empty() or not running; });

        if (not running) {
            break;
        }

        Job currentJob = jobsToDo.front();
        jobsToDo.pop();
        uniqueLocker.unlock();

        int returnCode = 0;

        try {
            returnCode = currentJob.fun();
        } catch (std::exception &e) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Worker thread encountered an unhandled exception. Program execution will be terminated: {}", e.what()));
            code = RM_ERROR_CODE_UNKNOWN;
            running = false;
            mainCV.notify_all(); // notify main thread of stopping execution
        }
        if (returnCode > RM_ERROR_CODE_THRESHOLD) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Worker thread finished job execution with unsatisfactory code {}. See above for the errors. Program execution will be terminated", static_cast<int>(returnCode)));
            code = returnCode;
            running = false;
            mainCV.notify_all(); // notify main thread of stopping execution
        }

        if (currentJob.done != nullptr) {
            *currentJob.done = true;
        }
        if (currentJob.code != nullptr) {
            *currentJob.code = returnCode;
        }
        if (currentJob.cv != nullptr) {
            currentJob.cv->notify_all();
        }
    }
}

void Server::inputThreadLoop() {
    pollfd fds[1];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    while (running) {
        int ret = poll(fds, 1, 200);

        if (ret < 0) {
            if (errno == EINTR) {
                continue; // interrupted by signal, just retry
            }
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Input poll failed");
            continue;
        }

        if (ret == 0) {
            continue;
        }

        std::getline(std::cin, inputString);
        processInput();
    }
}

void Server::webServerThreadLoop() {
    try {
        webServer->listen(config.serverListenToAddress, config.serverListenToPort);
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Worker thread encountered an unhandled exception. Program execution will be terminated: {}", e.what()));
        code = RM_ERROR_CODE_UNKNOWN;
        running = false;
        mainCV.notify_all(); // notify main thread of stopping execution
    }
}

void Server::executeJobAsync(std::function<int()> job) {
    jobsToDo.emplace(job);
    workerCV.notify_all();
}

int Server::executeJob(std::function<int()> job) {
    int returnCode = 0;
    bool done = false;
    std::mutex mutex;
    std::unique_lock<std::mutex> jobLocker(mutex);
    std::condition_variable cv;

    jobsToDo.emplace(job, &cv, &returnCode, &done);
    workerCV.notify_all();

    cv.wait(jobLocker, [&done] { return done; });
    return returnCode;
}

void Server::processInput() {
    inputArgs = splitString(inputString, ' ');

    if (inputArgs[0] == "") {
        return;
    }

    if (inputArgs[0] == "stop") {
        running = false;
        mainCV.notify_all();
        return;
    }

    if (inputArgs[0] == "restart") {
        restartRequired = true;
        running = false;
        mainCV.notify_all();
        return;
    }

    if (inputArgs[0] == "hi") {
        RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Hello!");
        return;
    }

    if (inputArgs[0] == "user") {
        if (inputArgs.size() == 1) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse command \"{}\": no subcommand found", inputString));
            return;
        }

        if (inputArgs[1] == "new") {
            uint32_t key = 0;
            if (inputArgs.size() == 3) {
                if (not hexStringToInt(inputArgs[2], key, 8)) {
                    RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse command \"{}\": string to key conversion failed", inputString));
                    return;
                }
            }
            executeJob([this, key] { return userDatabase->beginUserRegistration(key); });
            return;
        }

        if (inputArgs[1] == "remove-new") {
            if (inputArgs.size() == 2) {
                RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse command \"{}\": no argument found", inputString));
                return;
            }
            uint32_t key = 0;
            if (not hexStringToInt(inputArgs[2], key, 8)) {
                RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse command \"{}\": string to key conversion failed", inputString));
                return;
            }
            executeJob([this, key] { return userDatabase->terminateUserRegistration(key); });
            return;
        }

        if (inputArgs[1] == "remove") {
            if (inputArgs.size() == 2) {
                RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse command \"{}\": no argument found", inputString));
                return;
            }
            UUIDv4::UUID token;
            try {
                token.fromStr(inputArgs[2].c_str());
            } catch (std::exception &e) {
                RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse command \"{}\": string to token conversion failed", inputString));
                return;
            }
            executeJob([this, token] { return userDatabase->removeUserByToken(token); });
            return;
        }

        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse command \"{}\": unknown subcommand", inputString));
        return;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse command \"{}\": unrecognized command", inputString));
}

int Server::loadConfig() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Loading config");

    YAML::Node YAMLConfig;

    if (not std::filesystem::exists(config.path)) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("No config file found. A new one will be created at path \"{}\"", config.path));

        try {
            YAMLConfig["user_db_path"] = config.userDatabasePath;

            YAMLConfig["server_cert_path"] = config.serverCertPath;
            YAMLConfig["server_private_key_path"] = config.serverPrivateKeyPath;
            YAMLConfig["server_listen_to_address"] = config.serverListenToAddress;
            YAMLConfig["server_listen_to_port"] = config.serverListenToPort;

            YAMLConfig["snapshot_interval"] = config.snapshotInterval;
            YAMLConfig["dump_interval"] = config.dumpInterval;

            std::filesystem::create_directories("config");

            std::ofstream configFile(config.path, std::ios::trunc);
            configFile << YAMLConfig;
            configFile.close();
        } catch (const std::exception &e) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to create config: {}", e.what()));
            return RM_ERROR_CODE_LIB_STD;
        }
    } else {
        try {
            YAMLConfig = YAML::LoadFile(config.path);

            config.userDatabasePath = YAMLConfig["user_db_path"].as<std::string>();

            config.serverCertPath = YAMLConfig["server_cert_path"].as<std::string>();
            config.serverPrivateKeyPath = YAMLConfig["server_private_key_path"].as<std::string>();
            config.serverListenToAddress = YAMLConfig["server_listen_to_address"].as<std::string>();
            config.serverListenToPort = YAMLConfig["server_listen_to_port"].as<uint16_t>();

            config.snapshotInterval = YAMLConfig["snapshot_interval"].as<uint32_t>();
            config.dumpInterval = YAMLConfig["dump_interval"].as<uint32_t>();
        } catch (const std::exception &e) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to load config: {}", e.what()));
            return RM_ERROR_CODE_LIB_YAML;
        }
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Config loaded");

    return 0;
}

int Server::loadDatabases() {
    userDatabase = UserDatabase::getInstance();
    if ((code = userDatabase->init(config.userDatabasePath))) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Failed to initialize a UserDatabase object (error code {}). See above for the errors", static_cast<int>(code)));
        return code;
    }
    return 0;
}

int Server::loadWebServer() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Loading web server");

    try {
        if (not std::filesystem::exists(config.serverCertPath)) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("No certificate file found at path \"{}\"", config.serverCertPath));
        }
        if (not std::filesystem::exists(config.serverPrivateKeyPath)) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("No private key file found at path \"{}\"", config.serverPrivateKeyPath));
        }

        webServer = std::make_unique<httplib::SSLServer>(config.serverCertPath.c_str(), config.serverPrivateKeyPath.c_str());

        webServer->Post("/sendData", [this](const httplib::Request &request, httplib::Response &response) {
#ifdef RM_DEBUG
            beginElapsedTimer();
#endif

            response.set_content(std::to_string(config.snapshotInterval), "text/plain");
            int responseCode = 0;
            User user{};

            const std::vector<std::string> args = splitString(request.body, ' ');
            if (args.size() < 5) {
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_BAD_REQUEST);
                return;
            }

            try {
                user.token = UUIDv4::UUID::fromStrFactory(args[0]);
                user.state = static_cast<User::State>(std::stoi(args[1]));
                user.telemetry.timestamp = time(nullptr);
                user.telemetry.latitude = std::stod(args[2]);
                user.telemetry.longitude = std::stod(args[3]);
                user.telemetry.batteryLevel = std::stoi(args[4]);
            } catch (std::exception &e) {
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_BAD_REQUEST);
                return;
            }

            response.status = static_cast<httplib::StatusCode>(executeJob([this, &user] { return userDatabase->updateTelemetry(user); }));

#ifdef RM_DEBUG
            RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request satisfied in {}ns; token = {}", endElapsedTimer(), user.token.str()));
#endif
        });

        webServer->Post("/register", [this](const httplib::Request &request, httplib::Response &response) {
#ifdef RM_DEBUG
            beginElapsedTimer();
#endif

            int responseCode = 0;
            User user{.token = UUIDv4::UUID(0, 0)};

            const std::vector<std::string> args = splitString(request.body, ' ');
            if (args.size() < 2) {
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_BAD_REQUEST);
                response.set_content(user.token.str(), "text/plain");
                return;
            }

            if (not checkStringForValidHex(args[0], 8)) {
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_BAD_REQUEST);
                response.set_content(user.token.str(), "text/plain");
                return;
            }

            try {
                hexStringToInt(args[0], user.key, 8);
                user.name = args[1];
            } catch (std::exception &e) {
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_BAD_REQUEST);
                response.set_content(user.token.str(), "text/plain");
                return;
            }

            response.status = static_cast<httplib::StatusCode>(executeJob([this, &user] { return userDatabase->finishUserRegistration(user); }));
            response.set_content(user.token.str(), "text/plain");

#ifdef RM_DEBUG
            RM_LOG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request satisfied in {}ns; token = {}", endElapsedTimer(), user.token.str()));
#endif
        });
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Failed to load the web server: {}", e.what()));
        return RM_ERROR_CODE_LIB_HTTP;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Web server loaded");

    return 0;
}

int Server::unloadDatabases() {
    if ((code = userDatabase->destroy())) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Failed to destroy a UserDatabase object (error code {}). See above for the errors", static_cast<int>(code)));
        return code;
    }

    UserDatabase::releaseInstance();
    return 0;
}

int Server::unloadWebServer() {
    if (webServer == nullptr) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, "Cannot destroy a httplib::SSLServer object since it is not initialized");
        return RM_ERROR_CODE_NOT_INITIALIZED;
    }

    webServer->stop();
    return 0;
}

int Server::scheduleNextDump() {
    nextDump += config.dumpInterval;
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Next dump at {}", unixtimeToIso8601(nextDump)));

    return 0;
}

int Server::dumpTelemetry() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "NOT IMPLEMENTED");

    return 0;
}
