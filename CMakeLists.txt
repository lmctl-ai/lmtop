cmake_minimum_required(VERSION 3.16)
project(lmtop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Single-file binaries: static CRT on Windows, fully static on Linux GCC.
if(WIN32)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

find_package(Threads REQUIRED)

add_executable(lmtop
  src/main.cpp
  src/json.cpp
  src/stats.cpp
  src/monitor.cpp
  src/server.cpp
  src/client.cpp
)

# One sys backend per OS, conditional compilation for full parity.
if(WIN32)
  target_sources(lmtop PRIVATE src/sys_win.cpp)
  target_link_libraries(lmtop PRIVATE pdh)
elseif(APPLE)
  target_sources(lmtop PRIVATE src/sys_mac.cpp)
  target_link_libraries(lmtop PRIVATE "-framework IOKit" "-framework CoreFoundation")
else()
  target_sources(lmtop PRIVATE src/procfs.cpp)
endif()

target_include_directories(lmtop PRIVATE src)
target_link_libraries(lmtop PRIVATE Threads::Threads)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(lmtop PRIVATE -static)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lmtop PRIVATE -Wall -Wextra -Wpedantic)
endif()
