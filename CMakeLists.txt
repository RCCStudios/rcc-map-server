cmake_minimum_required(VERSION 3.28)

project(rm-server)

add_definitions(-DRM_DEBUG=true)
add_definitions(-DRM_VERSION="0.0.0")
add_definitions(-DRM_BUILD="260711b")
add_definitions(-DRM_BUILD_DATE=__DATE__)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -Wall")

file(GLOB_RECURSE RM_SRC
        "src/*.h"
        "src/*.cpp"
)

add_executable(rm-server ${RM_SRC})

add_subdirectory(lib/fmt)
add_subdirectory(lib/sqlitecpp)
add_subdirectory(lib/uuid_v4)
add_subdirectory(lib/yaml-cpp)

target_link_libraries(rm-server ${CMAKE_THREAD_LIBS_INIT} ${OPENSSL_LIBRARIES} ${CURL_LIBRARIES} fmt::fmt SQLiteCpp sqlite3 uuid_v4::uuid_v4 yaml-cpp::yaml-cpp)