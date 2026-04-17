cmake_minimum_required(VERSION 3.31)
project(ConcurrentCache)

# C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ConcurrentCache main.cpp
        src/base/log.h
        src/base/config.h
        src/base/signal.h
        src/network/buffer.h)

# 包含目录
include_directories(${CMAKE_SOURCE_DIR}/src)

# 搜集源文件
file(GLOB_RECURSE SOURCES "src/*.cpp"})

# 生成可执行文件
add_executable(concurrentcache-server ${SOURCES}
        src/base/log.h)

# 链接必要的库
target_link_libraries(concurrentcache-server pthread)