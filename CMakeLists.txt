cmake_minimum_required(VERSION 3.28)

project(rcc-map-server)

option(SSL_SUPPORT "will use HTTPS if ON, else will use HTTP" ON)

if (SSL_SUPPORT)
    add_definitions(-DRM_SSL_SUPPORT=true)
endif ()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DRM_DEBUG=true)
endif ()

add_definitions(-DRM_VERSION="0.0.4")
add_definitions(-DRM_BUILD="260806b")
add_definitions(-DRM_BUILD_DATE=__DATE__)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64 -Wall")

file(GLOB_RECURSE RM_SRC
        "src/*.h"
        "src/*.cpp"
)

add_executable(rcc-map-server ${RM_SRC})

add_subdirectory(lib/cpp-httplib)
add_subdirectory(lib/fmt)
add_subdirectory(lib/json)
add_subdirectory(lib/sqlitecpp)
add_subdirectory(lib/uuid_v4)
add_subdirectory(lib/yaml-cpp)

target_link_libraries(rcc-map-server ${CMAKE_THREAD_LIBS_INIT} ${OPENSSL_LIBRARIES} ${CURL_LIBRARIES} httplib fmt::fmt nlohmann_json::nlohmann_json SQLiteCpp sqlite3 uuid_v4::uuid_v4 yaml-cpp::yaml-cpp)