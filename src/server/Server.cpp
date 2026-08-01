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
    isInitialized = true;

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing threads");

    workerThread = std::thread(&Server::workerThreadLoop, this);
    std::stringstream workerThreadIdStringStream;
    workerThreadIdStringStream << workerThread.get_id();
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Worker thread: {}", workerThreadIdStringStream.str()));

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Doing initial jobs");

    nextDump = time(nullptr);

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

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing signal handlers");
    for (const int &sig: std::vector<int>{SIGTERM, SIGSEGV, SIGINT, SIGILL, SIGABRT, SIGFPE}) {
        signal(sig, [](const int signal) { Server::getInstance()->terminate(signal); });
    }
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Handlers initialized");

    return 0;
}

int Server::destroy() {
    if (not isInitialized) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot destroy a Server object since it is not initialized");
        return RM_ERROR_CODE_NOT_INITIALIZED;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Stopping signal handlers");
    for (const int &sig: std::vector<int>{SIGTERM, SIGSEGV, SIGINT, SIGILL, SIGABRT, SIGFPE}) {
        signal(sig, SIG_DFL);
    }
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Handlers stopped");

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
    inputThread.join();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Input thread joined");

    webServerCV.notify_all();
    webServerThread.join();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Web server thread joined");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Threads stopped");

    isInitialized = false;
    return 0;
}

void Server::terminate(int signal) {
    if (signal) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Got termination call by signal {}", signal));
    } else {
        RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Got termination call");
    }
    running = false;
    mainCV.notify_all(); // notify main thread of stopping execution
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
            terminate();
        }
        if (returnCode > RM_ERROR_CODE_THRESHOLD) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Worker thread finished job execution with unsatisfactory code {}. See above for the errors. Program execution will be terminated", static_cast<int>(returnCode)));
            code = returnCode;
            terminate();
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
        const int ret = poll(fds, 1, 200);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
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
    webServer->listen();
}

void Server::executeJobAsync(const std::function<int()> &job) {
    jobsToDo.emplace(job);
    workerCV.notify_all();
}

int Server::executeJob(const std::function<int()> &job) {
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

    if (inputArgs[0].empty()) {
        return;
    }

    if (inputArgs[0] == "stop") {
        terminate();
        return;
    }

    if (inputArgs[0] == "restart") {
        restartRequired = true;
        terminate();
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

        if (inputArgs[1] == "retire-new") {
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

        if (inputArgs[1] == "retire") {
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
            executeJob([this, token] { return userDatabase->retireUserByToken(token); });
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

            YAMLConfig["otp_pool_max_size"] = config.otpPoolMaxSize;
            YAMLConfig["otp_time_to_live"] = config.otpTimeToLive;

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

            config.otpPoolMaxSize = YAMLConfig["otp_pool_max_size"].as<uint32_t>();
            config.otpTimeToLive = YAMLConfig["otp_time_to_live"].as<uint32_t>();

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
    if ((code = userDatabase->init())) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Failed to initialize a UserDatabase object (error code {}). See above for the errors", static_cast<int>(code)));
        return code;
    }
    return 0;
}

int Server::loadWebServer() {
    webServer = WebServer::getInstance();
    if ((code = webServer->init())) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Failed to initialize a WebServer object (error code {}). See above for the errors", static_cast<int>(code)));
        return code;
    }
    return 0;
}

int Server::unloadDatabases() {
    if ((code = userDatabase->destroy())) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to destroy a UserDatabase object (error code {}). See above for the errors", static_cast<int>(code)));
        return code;
    }

    UserDatabase::releaseInstance();
    return 0;
}

int Server::unloadWebServer() {
    if ((code = webServer->destroy())) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to destroy a WebServer object (error code {}). See above for the errors", static_cast<int>(code)));
        return code;
    }

    WebServer::releaseInstance();
    return 0;
}

int Server::sendTelemetryUpdate(User &user) {
    if (not webServer->hasOpenWebsockets()) {
        return 0;
    }

    // since this DB call is inside a job it is OK to call it without worrying about thread safety
    if (int responseCode = 0; (responseCode = userDatabase->getUserIDByToken(user)) != RM_HTTP_CODE_OK) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Telemetry update for WebSockets failed: database call finished with unsatisfactory code {}", responseCode));
        return responseCode;
    }

    nlohmann::json data({{"id", user.id}});
    for (const TelemetryProperty &prop: user.telemetry.data) {
        switch (prop.type) {
            case nlohmann::json::value_t::number_integer:
                data[prop.name] = std::bit_cast<int64_t>(prop.value);
                break;
            case nlohmann::json::value_t::number_unsigned:
                data[prop.name] = std::bit_cast<uint64_t>(prop.value);
                break;
            case nlohmann::json::value_t::number_float:
                data[prop.name] = std::bit_cast<double>(prop.value);
                break;
            case nlohmann::json::value_t::boolean:
                data[prop.name] = static_cast<bool>(prop.value);
                break;
            default:
                data[prop.name] = nullptr;
        }
    }

    return webServer->sendToWebSockets(data.dump());
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
