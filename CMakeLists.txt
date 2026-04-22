cmake_minimum_required(VERSION 3.31)
project(ConcurrentCache LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(${PROJECT_SOURCE_DIR}/src)

file(GLOB_RECURSE SOURCES
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/main.cpp"
)

add_executable(concurrentcache-server ${SOURCES}
        src/network/event_loop.cpp
        src/network/connection.h
        src/protocol/resp.h
        src/protocol/resp.cpp
        src/cache/storage.h
        src/cache/storage.cpp
        src/command/command.h
        src/command/string_cmd.h
        src/command/command_factory.h
        src/command/command_factory.cpp)

if(UNIX)
    target_link_libraries(concurrentcache-server pthread)
endif()

if(MSVC)
    target_compile_options(concurrentcache-server PRIVATE /W4 /WX)
else()
    target_compile_options(concurrentcache-server PRIVATE -Wall -Wextra -Werror)
endif()