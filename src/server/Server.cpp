#include "Server.h"

#include "../common.h"

#include "../data/User.h"

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <fstream>
#include <chrono>
#include <filesystem>

Server *Server::singletonInstance = nullptr;
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

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing threads");

    workerThread = std::thread(&Server::workerThreadLoop, this);
    std::stringstream workerThreadIdStringStream;
    workerThreadIdStringStream << workerThread.get_id();
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Worker thread: {}", workerThreadIdStringStream.str()));

    inputThread = std::thread(&Server::inputThreadLoop, this);
    std::stringstream inputThreadIdStringStream;
    inputThreadIdStringStream << inputThread.get_id();
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Input thread: {}", inputThreadIdStringStream.str()));

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Threads initialized");

    time_t now = time(0);
    tm *ptmgm = gmtime(&now);
    time_t gmnow = mktime(ptmgm);
    timeDifference = gmnow - now;
    if (ptmgm->tm_isdst > 0) {
        timeDifference -= 3600;
    }

    nextDump = now;

    isInitialized = true;

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Waiting for worker thread to do initial jobs");

    // do some initial jobs
    jobsToDo.emplace([this] { return loadConfig(); });
    jobsToDo.emplace([this] { return loadDatabases(); });
    jobsToDo.emplace([this] { return scheduleNextDump(); });

    hasJobsToDo = true;
    workerCV.notify_all();

    std::unique_lock<std::mutex> uniqueLocker(mainMutex);
    mainCV.wait(uniqueLocker, [this] { return not hasJobsToDo; }); // wait for worker thread to finish initial jobs

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initial jobs done");

    return 0;
}

int Server::destroy() {
    if (not isInitialized) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot destroy a Server object since it is not initialized");
        return RM_ERROR_CODE_NOT_INITIALIZED;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Waiting for worker thread to do final jobs");

    // do some initial jobs
    jobsToDo.emplace([this] { return unloadDatabases(); });

    hasJobsToDo = true;
    workerCV.notify_all();

    std::unique_lock<std::mutex> uniqueLocker(mainMutex);
    mainCV.wait(uniqueLocker, [this] { return not hasJobsToDo; }); // wait for worker thread to finish final jobs

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Final jobs done");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Stopping threads");

    workerCV.notify_all();
    workerThread.join();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Worker thread joined");

    inputCV.notify_all();
    inputThread.detach(); // unable to join() because of std::getline()

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Input thread detached");

    workerThread.~thread();
    inputThread.~thread();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Threads stopped");

    isInitialized = false;
    return 0;
}

int Server::run() {
    // main app cycle
    while (true) {
        std::unique_lock<std::mutex> uniqueLocker(mainMutex); // wait for input
        mainCV.wait_until(uniqueLocker, std::chrono::system_clock::from_time_t(nextDump), [this] { return hasInputToProcess or (not running); }); // jump from Server::readInput() OR next dump

        if (not running) {
            break;
        }

        if (hasInputToProcess) {
            processInput();
            hasInputToProcess = false;
        }

        if (not running) {
            break;
        }

        if (std::time(nullptr) >= nextDump - 1) {
            jobsToDo.emplace([this] { return dumpTelemetry(); });
            scheduleNextDump();
        }

        hasJobsToDo = not jobsToDo.empty();
        workerCV.notify_all(); // jump to Server::workerThreadLoop()

        uniqueLocker.unlock();
        inputCV.notify_all(); // jump to Server::inputThreadLoop()
    }

    return code;
}

void Server::workerThreadLoop() {
    while (true) {
        if (jobsToDo.size() == 0) {
            hasJobsToDo = false;
            std::unique_lock<std::mutex> uniqueLocker(workerMutex); // wait for new tasks
            workerCV.wait(uniqueLocker, [this] { return hasJobsToDo or (not running); }); // jump from Server::run()
        }

        if (not running and not hasJobsToDo) {
            mainCV.notify_all();
            break;
        }

        // execute first job from the queue
        auto currentJob = jobsToDo.front();
        jobsToDo.pop();
        code = currentJob();
        if (code) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Worker thread encountered an error while executing one of the jobs (error code {}). See above for the errors", code));

            // notify main thread
            running = false;
            mainCV.notify_all();
        }
    }
}

void Server::inputThreadLoop() {
    while (true) {
        std::getline(std::cin, inputString); // wait for user
        hasInputToProcess = true;
        mainCV.notify_all(); // jump to Server::run()

        std::unique_lock<std::mutex> uniqueLocker(inputMutex); // wait for parsing
        inputCV.wait(uniqueLocker, [this] { return (not hasInputToProcess) or (not running); }); // jump from Server::run()
    }
}

void Server::processInput() {
    inputArgs = splitString(inputString, ' ');

    if (inputArgs[0] == "") {
        return;
    }
    if (inputArgs[0] == "stop") {
        running = false;
        return;
    }
    if (inputArgs[0] == "restart") {
        restartRequired = true;
        running = false;
        return;
    }
    if (inputArgs[0] == "hi") {
        RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Hello!");
        return;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse command \"{}\": unrecognized command", inputString));
}

int Server::loadConfig() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Loading config");

    YAML::Node YAMLConfig;

    if (not std::filesystem::exists(config.path)) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("No config file found. A new one will be created at path \"{}\"", config.path));

        try {
            YAMLConfig["user_db_path"] = config.userDatabasePath;

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
        RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Failed to initialize a UserDatabase object (error code {}). See above for the errors", code));
        return code;
    }
    return 0;
}

int Server::unloadDatabases() {
    if ((code = userDatabase->destroy())) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Failed to initialize a UserDatabase object (error code {}). See above for the errors", code));
        return code;
    }

    delete userDatabase;
    return 0;
}

int Server::scheduleNextDump() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Scheduling next telemetry dump");

    nextDump += 60;
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Next dump at {}", unixtimeToIso8601(nextDump)));

    hasScheduled = true;
    mainIntialCV.notify_all(); // jump to Mango::init()

    return 0;
}

int Server::dumpTelemetry() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "NOT IMPLEMENTED");

    return 0;
}