#pragma once

#include "../common.h"

#include "../data/UserDatabase.h"
#include "Config.h"
#include "Job.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <ctime>
#include <string>
#include <queue>
#include <functional>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <csignal>

class Server {

    RM_DECLARE_SINGLETON(Server)

private:
    UserDatabase *userDatabase = nullptr;

#ifdef RM_SSL_SUPPORT
    std::unique_ptr<httplib::SSLServer> webServer = nullptr;
#else
    std::unique_ptr<httplib::Server> webServer = nullptr;
#endif


    void inputThreadLoop();
    void workerThreadLoop();
    void webServerThreadLoop();

    void processInput();

    void executeJobAsync(const std::function<int()>& job);
    int executeJob(const std::function<int()>& job);
    // load jobs

    int loadConfig();
    int loadDatabases();
    int loadWebServer();

    // unload jobs

    int unloadDatabases();
    int unloadWebServer();

    // main jobs

    int scheduleNextDump();
    int dumpTelemetry();

    std::mutex workerMutex;
    std::thread workerThread;
    std::condition_variable workerCV;

    std::mutex inputMutex;
    std::thread inputThread;
    std::condition_variable inputCV;

    std::mutex webServerMutex;
    std::thread webServerThread;
    std::condition_variable webServerCV;

    std::mutex mainMutex;
    std::condition_variable mainCV;

    std::queue<Job> jobsToDo;

    std::string inputString;
    std::vector<std::string> inputArgs;

    std::atomic<std::time_t> nextDump = 0;
    std::time_t timeDifference = 0;

    Config config{};

    std::atomic_int code = 0;

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
    void terminate(int signal = 0);
    int run();

    static std::atomic_bool running;
    static std::atomic_bool restartRequired;
};
