#include "WebServer.h"

#include <nlohmann/json.hpp>

#include "../data/User.h"

#include "../server/Server.h"
#include "../common.h"

std::unique_ptr<WebServer> WebServer::singletonInstance = nullptr;
bool WebServer::isInitialized = false;

int WebServer::init() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing WebServer object");

    if (isInitialized) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot initialize a WebServer object since it is already initialized");
        return RM_ERROR_CODE_ALREADY_INITIALIZED;
    }

    isInitialized = true;
    parentServer = Server::getInstance();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing OTP pool");

    otpPool = std::make_unique<OTPPool>(parentServer->config.otpPoolMaxSize, parentServer->config.otpTimeToLive);

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "OTP pool initialized");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Initializing HTTPlib WebServer");

    try {
#ifdef RM_SSL_SUPPORT
        if (not std::filesystem::exists(Server::getInstance()->config.serverCertPath)) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("No certificate file found at path \"{}\"", Server::getInstance()->config.serverCertPath));
        }
        if (not std::filesystem::exists(Server::getInstance()->config.serverPrivateKeyPath)) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, fmt::format("No private key file found at path \"{}\"", Server::getInstance()->config.serverPrivateKeyPath));
        }

        webServer = std::make_unique<httplib::SSLServer>(Server::getInstance()->config.serverCertPath.c_str(), Server::getInstance()->config.serverPrivateKeyPath.c_str());
#else
        webServer = std::make_unique<httplib::Server>();
#endif

        webServer->Post("/api/register", [this](const httplib::Request &request, httplib::Response &response) {
#ifdef RM_DEBUG
            beginElapsedTimer();
#endif

            User user{};
            std::string keyStr;

            try {
                nlohmann::json data = nlohmann::json::parse(request.body);
                keyStr = data["key"].get<std::string>();
                user.name = data["name"].get<std::string>();
            } catch (std::exception &e) {
                RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request parsing failed: {}", e.what()));
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_BAD_REQUEST);
                return;
            }

            if (not hexStringToInt(keyStr, user.key, 8)) {
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_BAD_REQUEST);
                return;
            }

            if ((response.status = static_cast<httplib::StatusCode>(parentServer->executeJob([this, &user] { return parentServer->userDatabase->finishUserRegistration(user); }))) == RM_HTTP_CODE_OK) {
                response.set_content(nlohmann::json({{"token", user.token.str()}}).dump(), "application/json");
            }

            RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request satisfied in {}ns; token = {}", endElapsedTimer(), user.token.str()));
        });

        webServer->Post("/api/sendTelemetry", [this](const httplib::Request &request, httplib::Response &response) {
#ifdef RM_DEBUG
            beginElapsedTimer();
#endif

            response.set_content(nlohmann::json({{"timeToNextSnapshot", parentServer->config.snapshotInterval}}).dump(), "application/json");
            User user{};

            try {
                if (request.get_header_value("Authorization").substr(0, 7) != "Bearer ") {
                    throw std::runtime_error(std::string("Invalid authorization header \"") + request.get_header_value("Authorization").substr(0, 7) + std::string("\""));
                }
                user.token = UUIDv4::UUID::fromStrFactory(request.get_header_value("Authorization").substr(7));
            } catch (std::exception &e) {
                RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request auth failed: {}", e.what()));
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_UNAUTHORIZED);
                return;
            }

            try {
                user.state = User::RM_USER_STATE_ACTIVE;
                const std::time_t now = time(nullptr);

                nlohmann::json data = nlohmann::json::parse(request.body);

                for (const TelemetryProperty &prop: Telemetry::schema) {
                    if (data[prop.name].is_null()) {
                        continue;
                    } else if (data[prop.name].type() != prop.type) {
                        throw std::runtime_error(std::string("Wrong type for ") + prop.name);
                    }
                    switch (prop.type) {
                        case nlohmann::json::value_t::number_integer:
                            user.telemetry.data.push_back({std::bit_cast<int64_t>(data[prop.name].get<int64_t>()), now, prop.name, prop.type});
                            break;
                        case nlohmann::json::value_t::number_unsigned:
                            user.telemetry.data.push_back({std::bit_cast<int64_t>(data[prop.name].get<uint64_t>()), now, prop.name, prop.type});
                            break;
                        case nlohmann::json::value_t::number_float:
                            user.telemetry.data.push_back({std::bit_cast<int64_t>(data[prop.name].get<double>()), now, prop.name, prop.type});
                            break;
                        default:
                            throw std::runtime_error(std::string("Unsupported type ") + data[prop.name].type_name());
                    }
                }

                // if (data["state"].get<int>() >= User::State::RM_USER_STATE_ENUM_COUNT) {
                //     throw std::exception();
                // }
            } catch (std::exception &e) {
                RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request parsing failed: {}", e.what()));
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_BAD_REQUEST);
                return;
            }

            response.status = static_cast<httplib::StatusCode>(parentServer->executeJob([this, &user] { return parentServer->userDatabase->updateTelemetry(user); }));
            parentServer->executeJob([this, &user] { return parentServer->sendTelemetryUpdate(user); });

            RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request satisfied in {}ns; token = {}", endElapsedTimer(), user.token.str()));
        });

        webServer->Get("/api/getTelemetry", [this](const httplib::Request &request, httplib::Response &response) {
#ifdef RM_DEBUG
            beginElapsedTimer();
#endif

            UUIDv4::UUID token;

            try {
                if (request.get_header_value("Authorization").substr(0, 7) != "Bearer ") {
                    throw std::runtime_error(std::string("Invalid authorization header \"") + request.get_header_value("Authorization").substr(0, 7) + std::string("\""));
                }
                token = UUIDv4::UUID::fromStrFactory(request.get_header_value("Authorization").substr(7));
            } catch (std::exception &e) {
                RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request auth failed: {}", e.what()));
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_UNAUTHORIZED);
                return;
            }

            if ((response.status = static_cast<httplib::StatusCode>(parentServer->executeJob([this, &token] { return parentServer->userDatabase->validateRegisteredUserByToken(token); }))) != RM_HTTP_CODE_OK) {
                return;
            }

            std::vector<User> users;
            if ((response.status = static_cast<httplib::StatusCode>(parentServer->executeJob([this, &users] { return parentServer->userDatabase->getAllUsers(users); }))) != RM_HTTP_CODE_OK) {
                return;
            }

            nlohmann::json data = nlohmann::json::array();

            for (const User &user: users) {
                if (user.state != User::State::RM_USER_STATE_ACTIVE) {
                    continue;
                }

                nlohmann::json userData({
                    {"id", user.id},
                    {"name", user.name},
                    {"pfpPath", user.pfpPath}
                });

                for (const TelemetryProperty &prop: user.telemetry.data) {
                    switch (prop.type) {
                        case nlohmann::json::value_t::number_integer:
                            userData[prop.name] = {{"value", std::bit_cast<int64_t>(prop.value)}, {"timestamp", prop.timestamp}};
                            break;
                        case nlohmann::json::value_t::number_unsigned:
                            userData[prop.name] = {{"value", std::bit_cast<uint64_t>(prop.value)}, {"timestamp", prop.timestamp}};
                            break;
                        case nlohmann::json::value_t::number_float:
                            userData[prop.name] = {{"value", std::bit_cast<double>(prop.value)}, {"timestamp", prop.timestamp}};
                            break;
                        default:
                            userData[prop.name] = nullptr;
                    }
                }

                data.push_back(userData);
            }

            response.set_content(data.dump(), "application/json");

            RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request satisfied in {}ns; token = {}", endElapsedTimer(), token.str()));
        });

        webServer->Get("/api/getOtp", [this](const httplib::Request &request, httplib::Response &response) {
#ifdef RM_DEBUG
            beginElapsedTimer();
#endif

            UUIDv4::UUID token;

            try {
                if (request.get_header_value("Authorization").substr(0, 7) != "Bearer ") {
                    throw std::runtime_error(std::string("Invalid authorization header \"") + request.get_header_value("Authorization").substr(0, 7) + std::string("\""));
                }
                token = UUIDv4::UUID::fromStrFactory(request.get_header_value("Authorization").substr(7));
            } catch (std::exception &e) {
                RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request auth failed: {}", e.what()));
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_UNAUTHORIZED);
                return;
            }

            if ((response.status = static_cast<httplib::StatusCode>(parentServer->executeJob([this, &token] { return parentServer->userDatabase->validateRegisteredUserByToken(token); }))) != RM_HTTP_CODE_OK) {
                return;
            }

            OTP otp;
            if ((otp = otpPool->getOTP(token)) == 0) {
                response.status = RM_HTTP_CODE_INTERNAL_ERROR;
                return;
            }
            std::string otpString;
            intToHexString(otp, otpString);
            response.set_content(nlohmann::json({{"otp", otpString}}).dump(), "application/json");

            RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request satisfied in {}ns; token = {}", endElapsedTimer(), token.str()));
        });

        webServer->Get("/api/getToken", [this](const httplib::Request &request, httplib::Response &response) {
#ifdef RM_DEBUG
            beginElapsedTimer();
#endif

            OTP otp;
            UUIDv4::UUID token;

            try {
                if (request.get_header_value("Authorization").substr(0, 7) != "Bearer ") {
                    throw std::runtime_error(std::string("Invalid authorization header \"") + request.get_header_value("Authorization").substr(0, 7) + std::string("\""));
                }
                if (not hexStringToInt(request.get_header_value("Authorization").substr(7), otp, 8)) {
                    throw std::runtime_error(std::string("Invalid OTP \"") + request.get_header_value("Authorization").substr( 7) + std::string("\""));
                }
                if ((token = otpPool->getToken(otp)) == UUIDv4::UUID(0, 0)) {
                    throw std::runtime_error(std::string("Incorrect OTP \"") + request.get_header_value("Authorization").substr( 7) + std::string("\""));
                }
            } catch (std::exception &e) {
                RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request auth failed: {}", e.what()));
                response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_UNAUTHORIZED);
                return;
            }

            response.status = static_cast<httplib::StatusCode>(RM_HTTP_CODE_OK);
            response.set_content(nlohmann::json({{"token", token.str()}}).dump(), "application/json");

            RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Request satisfied in {}ns; token = {}", endElapsedTimer(), token.str()));
        });


#ifdef RM_SSL_SUPPORT
        std::string webSocketEndpoint = "/api/wss";
#else
        std::string webSocketEndpoint = "/api/ws";
#endif
        webServer->WebSocket(
            webSocketEndpoint,
            [this](const httplib::Request &request, httplib::ws::WebSocket &webSocket) {
                UUIDv4::UUID token;

                try {
                    if (request.get_header_value("Sec-WebSocket-Protocol").substr(0, 7) != "bearer.") {
                        throw std::runtime_error(std::string("Invalid authorization header \"") + request.get_header_value("Sec-WebSocket-Protocol").substr(0, 7) + std::string("\""));
                    }
                    token = UUIDv4::UUID::fromStrFactory(request.get_header_value("Sec-WebSocket-Protocol").substr(7));
                } catch (std::exception &e) {
                    RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("Websocket handshake auth failed: {}", e.what()));
                    webSocket.close(httplib::ws::CloseStatus::PolicyViolation, fmt::format("Authorization failed: {}", RM_HTTP_CODE_UNAUTHORIZED));
                    return;
                }

                if (int responseCode = 0; (responseCode = parentServer->executeJob([this, &token] { return parentServer->userDatabase->validateRegisteredUserByToken(token); })) != RM_HTTP_CODE_OK) {
                    webSocket.close(httplib::ws::CloseStatus::PolicyViolation, fmt::format("Authorization failed: {}", responseCode));
                    return;
                }

                openWebSockets.push_back(&webSocket);
                RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("WebSocket connection opened (pointer {})", static_cast<void *>(&webSocket)));

                std::string _;
                while (webSocket.is_open()) {
                    if (webSocket.read(_) == httplib::ws::ReadResult::Fail) {
                        break;
                    }
                }

                openWebSockets.remove(&webSocket);
                webSocket.close(httplib::ws::CloseStatus::Normal, "Shutting down gracefully");
                RM_LOG_DEBUG(RM_LOG_LEVEL_PREFIX_DEBUG, RM_LOG_AUTO_PREFIX, fmt::format("WebSocket connection closed (pointer {})", static_cast<void *>(&webSocket)));
            },
            [](const std::vector<std::string> &protocols) { return protocols[0]; });
    } catch (std::exception &e) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Failed to load the web server: {}", e.what()));
        return RM_ERROR_CODE_LIB_HTTP;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "HTTPlib WebServer initialized");
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "WebServer object initialized");

    return 0;
}

int WebServer::destroy() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Destroying WebServer object");

    if (not isInitialized) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, "Cannot destroy a WebServer object since it is not initialized");
        return RM_ERROR_CODE_NOT_INITIALIZED;
    }

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Stopping web interactions");
    for (httplib::ws::WebSocket *const webSocket: openWebSockets) {
        webSocket->close(httplib::ws::CloseStatus::GoingAway, "Server is shutting down");
    }
    openWebSockets.clear();

    webServer->wait_until_ready();
    webServer->stop();

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Web interactions stopped");
    isInitialized = false;
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "WebServer object destroyed");

    return 0;
}

int WebServer::listen() {
#ifdef RM_SSL_SUPPORT
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Using HTTPS protocol");
#else
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Using HTTP protocol");
#endif

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Listening on {}:{}", parentServer->config.serverListenToAddress, parentServer->config.serverListenToPort));
    webServer->listen(parentServer->config.serverListenToAddress, parentServer->config.serverListenToPort);

    return 0;
}

int WebServer::sendToWebSockets(const std::string &message) {
    for (httplib::ws::WebSocket *const webSocket: openWebSockets) {
        if (webSocket->is_open()) {
            webSocket->send(message);
        }
    }
    return 0;
}
