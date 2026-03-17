cmake_minimum_required(VERSION 3.10)
project(HighPerfWebServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")

include_directories(include)

add_executable(web_server
    src/main.cpp
    src/ip_rate_limit.cpp
    src/connection.cpp
    src/buffer_utils.cpp
    src/queue_utils.cpp
    src/io_utils.cpp
    src/http_logic.cpp
    src/accept_utils.cpp
    src/startup_utils.cpp
)

target_link_libraries(web_server pthread)
