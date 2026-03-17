cmake_minimum_required(VERSION 3.10)
project(HighPerfWebServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Release
    Debug
    RelWithDebInfo
    MinSizeRel
    AsanUbsan
)

string(TOLOWER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_LOWER)

if(BUILD_TYPE_LOWER STREQUAL "asanubsan")
    add_compile_options(
        -O1
        -g
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    add_link_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
elseif(BUILD_TYPE_LOWER STREQUAL "release")
    add_compile_options(-O2 -DNDEBUG)
endif()

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
    src/timer_utils.cpp
)

target_link_libraries(web_server pthread)
