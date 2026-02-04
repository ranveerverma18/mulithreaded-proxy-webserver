cmake_minimum_required(VERSION 3.10)

project(multithreaded_proxy_web_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(proxy_server
    src/main.cpp
)
