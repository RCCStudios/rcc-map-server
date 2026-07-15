#include "common.h"
#include "server/Server.h"

int main() {
    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Starting up rcc-map-server {}-{} built on {}", RM_VERSION, RM_BUILD, RM_BUILD_DATE));

    int code = 0;

    do {
        RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Creating new a Server object");
        Server *server = Server::getInstance();

        if ((code = server->init())) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Failed to initialize the Server object (error code {}). See above for the errors", code));
            Server::releaseInstance();
            break;
        }

        if ((code = server->run())) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("The Server object finished execution with unsatisfactory code (error code {}). See above for the errors", code));
        }

        RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, "Stopping");

        if ((code = server->destroy())) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_FATAL, RM_LOG_AUTO_PREFIX, fmt::format("Failed to destroy the Server object (error code {}). See above for the errors", code));
        }
        Server::releaseInstance();
    } while (Server::restartRequired);

    RM_LOG(RM_LOG_LEVEL_PREFIX_INFO, RM_LOG_AUTO_PREFIX, fmt::format("Done ({})", code));

    return code;
}
