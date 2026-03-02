cmake_minimum_required(VERSION 3.10)
project(HighPerfWebServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")

# 暂时只编译 main.cpp
add_executable(web_server src/main.cpp)

# 链接 pthread (多线程必备)
target_link_libraries(web_server pthread)
