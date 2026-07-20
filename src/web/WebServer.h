#pragma once

#include "OTPPool.h"
#include "../common.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <httplib.h>
#include <uuid_v4.h>

#include <string>
#include <chrono>
#include <list>

class Server;

class WebServer {
    RM_DECLARE_SINGLETON(WebServer)

private:
#ifdef RM_SSL_SUPPORT
    std::unique_ptr<httplib::SSLServer> webServer = nullptr;
#else
    std::unique_ptr<httplib::Server> webServer = nullptr;
#endif

    std::unique_ptr<OTPPool> otpPool = nullptr;
    std::list<httplib::ws::WebSocket *> openWebSockets{};

    Server *parentServer = nullptr;
    int code = 0;

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
    [[nodiscard]] bool hasOpenWebsockets() const { return not openWebSockets.empty(); }

    int listen();

    int sendToWebSockets(const std::string &message);
};
