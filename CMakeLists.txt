cmake_minimum_required(VERSION 3.16)
project(lmtop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_executable(lmtop
  src/main.cpp
  src/json.cpp
  src/procfs.cpp
  src/stats.cpp
  src/monitor.cpp
  src/server.cpp
  src/client.cpp
)

target_include_directories(lmtop PRIVATE src)
target_link_libraries(lmtop PRIVATE Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lmtop PRIVATE -Wall -Wextra -Wpedantic)
endif()
