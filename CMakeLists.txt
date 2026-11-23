cmake_minimum_required(VERSION 3.16)
project(pulse8_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(pulse8_engine server.cpp)
target_link_libraries(pulse8_engine PRIVATE Boost::system OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
