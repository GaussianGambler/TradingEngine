cmake_minimum_required(VERSION 3.15)
project(MatchingEngine CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native")
endif()

add_executable(matching_engine engine.cpp)
target_link_libraries(matching_engine pthread)
