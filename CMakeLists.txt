cmake_minimum_required(VERSION 3.22)
project(eyefight CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SmartSpectra CONFIG REQUIRED)

add_executable(eyefight src/main.cc)
target_include_directories(eyefight PRIVATE third_party)
target_link_libraries(eyefight PRIVATE SmartSpectra::SDK)
