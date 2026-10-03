cmake_minimum_required(VERSION 3.20)
project(ConcurrentCache LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(ENABLE_TSAN AND ENABLE_ASAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled simultaneously")
endif()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# 包含头文件目录
include_directories(${PROJECT_SOURCE_DIR}/src)

# ========== Base 模块 ==========
set(BASE_SOURCES
    src/base/config.cpp
    src/base/format.cpp
    src/base/lock.cpp
    src/base/log.cpp
    src/base/signal.cpp
    src/base/thread_pool.cpp
)

# ========== Cache 模块 ==========
set(CACHE_SOURCES
    src/cache/expiration_checker.cpp
    src/cache/expire_dict.cpp
    src/cache/storage.cpp
)

# ========== Command 模块 ==========
set(COMMAND_SOURCES
    src/command/command_factory.cpp
    src/command/cluster_cmd.cpp
    src/command/restore_cmd.cpp
    src/command/psync_cmd.cpp
)

# ========== Datatype 模块 ==========
set(DATATYPE_SOURCES
    src/datatype/object.cpp
)

# ========== MemoryPool 模块 ==========
set(MEMORYPOOL_SOURCES
    src/memorypool/central_cache.cpp
    src/memorypool/free_list.cpp
    src/memorypool/page_cache.cpp
    src/memorypool/size_class.cpp
    src/memorypool/span.cpp
    src/memorypool/thread_cache.cpp
)

# ========== Network 模块 ==========
set(NETWORK_SOURCES
    src/network/buffer.cpp
    src/network/channel.cpp
    src/network/connection.cpp
    src/network/event_loop.cpp
    src/network/main_reactor.cpp
    src/network/socket.cpp
    src/network/sub_reactor.cpp
    src/network/sub_reactor_pool.cpp
)

# ========== Persistence 模块 ==========
set(PERSISTENCE_SOURCES
    src/persistence/rdb.cpp
    src/persistence/rdb_scheduler.cpp
)

# ========== Protocol 模块 ==========
set(PROTOCOL_SOURCES
    src/protocol/resp.cpp
)

# ========== Cluster 模块 ==========
set(CLUSTER_SOURCES
    src/cluster/cluster_node.cpp
    src/cluster/cluster_state.cpp
    src/cluster/cluster_server.cpp
    src/cluster/cluster_link.cpp
    src/cluster/cluster_connection.cpp
    src/cluster/cluster_gossip.cpp
    src/cluster/replication_mgr.cpp
    src/cluster/cluster_bus.cpp
)

# ========== 主程序 ==========
add_executable(concurrentcache-server
    main.cpp
    ${BASE_SOURCES}
    ${CACHE_SOURCES}
    ${COMMAND_SOURCES}
    ${DATATYPE_SOURCES}
    ${MEMORYPOOL_SOURCES}
    ${NETWORK_SOURCES}
    ${PERSISTENCE_SOURCES}
    ${PROTOCOL_SOURCES}
    ${CLUSTER_SOURCES}
)

# ========== 链接库 ==========
if(UNIX)
    find_package(ZLIB REQUIRED)
    target_link_libraries(concurrentcache-server pthread ZLIB::ZLIB)
endif()

# ========== 编译选项 ==========
if(MSVC)
    target_compile_options(concurrentcache-server PRIVATE /W4 /WX)
else()
    target_compile_options(concurrentcache-server PRIVATE
        -Wall -Wextra -Werror
        -Wconversion -Wshadow -Wsign-conversion -Wdouble-promotion)
endif()

# ========== Sanitizer 标志 ==========
if(ENABLE_ASAN)
    target_compile_options(concurrentcache-server PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(concurrentcache-server PRIVATE -fsanitize=address -fno-omit-frame-pointer)
endif()

if(ENABLE_TSAN)
    target_compile_options(concurrentcache-server PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(concurrentcache-server PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
endif()

if(ENABLE_UBSAN)
    target_compile_options(concurrentcache-server PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(concurrentcache-server PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
endif()

# ========== 测试子目录 ==========
# enable_testing() 必须在顶层，否则 build/ 下没有顶层 CTestTestfile.cmake，
# ctest --test-dir build 会得到 "No tests were found"。
enable_testing()

# 测试 target 是 CI 门禁的载体，但源码树可能不含 test/（例如 docker 构建的
# .dockerignore 排除了它），因此这里必须允许缺席而不是 configure 失败。
option(BUILD_TESTING "Build the test targets" ON)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    add_subdirectory(test)
endif()
