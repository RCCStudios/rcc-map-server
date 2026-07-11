#pragma once

#include "../common.h"

#include "../data/UserDatabase.h"

#include <ctime>
#include <string>
#include <queue>
#include <functional>
#include <thread>
#include <condition_variable>
#include <atomic>

#include "Config.h"

class Server {

    RM_DECLARE_SINGLETON(Server)

private:
    UserDatabase *userDatabase = nullptr;

    void inputThreadLoop();
    void workerThreadLoop();

    void processInput();

    // load jobs

    int loadConfig();
    int loadDatabases();

    // unload jobs

    int unloadDatabases();

    // main jobs

    int scheduleNextDump();
    int dumpTelemetry();

    bool hasJobsToDo = false;
    std::mutex workerMutex;
    std::thread workerThread;
    std::condition_variable workerCV;

    bool hasInputToProcess = false;
    std::mutex inputMutex;
    std::thread inputThread;
    std::condition_variable inputCV;

    std::mutex mainMutex;
    std::condition_variable mainCV;
    std::condition_variable mainIntialCV;

    std::queue<std::function<int()>> jobsToDo;

    std::string inputString;
    std::vector<std::string> inputArgs;

    bool hasAnythingToPublish = false;
    bool hasScheduled = false;
    std::time_t nextDump = 0;
    std::time_t timeDifference = 0;

    Config config{};

    int code = 0;

public:
    int run();

    static std::atomic_bool running;
    static std::atomic_bool restartRequired;
};
