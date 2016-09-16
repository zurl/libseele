# cmake_minimum_required(VERSION <specify CMake version here>)
project(TestCpp11)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -mthreads")

set(SOURCE_FILES main.cpp socket.h IOCPModel.cpp helper.cpp socket.h)
add_executable(TestCpp11 ${SOURCE_FILES})
target_link_libraries(TestCpp11 WS2_32.lib)