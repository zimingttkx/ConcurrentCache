cmake_minimum_required(VERSION 3.28)
project(ConcurrentCache LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(${PROJECT_SOURCE_DIR}/src)

file(GLOB_RECURSE SOURCES
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/main.cpp"
)

add_executable(concurrentcache-server ${SOURCES})

if(UNIX)
    find_package(ZLIB REQUIRED)
    target_link_libraries(concurrentcache-server pthread ZLIB::ZLIB)
endif()

if(WIN32)
    find_package(ZLIB)
    if(ZLIB_FOUND)
        target_link_libraries(concurrentcache-server ZLIB::ZLIB)
    endif()
endif()

if(MSVC)
    target_compile_options(concurrentcache-server PRIVATE /W4 /WX)
else()
    target_compile_options(concurrentcache-server PRIVATE -Wall -Wextra -Werror)
endif()

# 添加测试子目录
# add_subdirectory(test)