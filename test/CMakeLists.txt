cmake_minimum_required(VERSION 3.20)

# 公共源文件
set(COMMON_SOURCES
    ${CMAKE_SOURCE_DIR}/src/base/config.cpp
    ${CMAKE_SOURCE_DIR}/src/base/format.cpp
    ${CMAKE_SOURCE_DIR}/src/base/lock.cpp
    ${CMAKE_SOURCE_DIR}/src/base/log.cpp
    ${CMAKE_SOURCE_DIR}/src/base/signal.cpp
    ${CMAKE_SOURCE_DIR}/src/base/thread_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/expiration_checker.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/expire_dict.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/storage.cpp
    ${CMAKE_SOURCE_DIR}/src/command/command_factory.cpp
    ${CMAKE_SOURCE_DIR}/src/datatype/object.cpp
    ${CMAKE_SOURCE_DIR}/src/network/buffer.cpp
    ${CMAKE_SOURCE_DIR}/src/network/channel.cpp
    ${CMAKE_SOURCE_DIR}/src/network/connection.cpp
    ${CMAKE_SOURCE_DIR}/src/network/event_loop.cpp
    ${CMAKE_SOURCE_DIR}/src/network/main_reactor.cpp
    ${CMAKE_SOURCE_DIR}/src/network/socket.cpp
    ${CMAKE_SOURCE_DIR}/src/network/sub_reactor.cpp
    ${CMAKE_SOURCE_DIR}/src/network/sub_reactor_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/rdb.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/rdb_scheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/protocol/resp.cpp
)

# 包含目录
set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/test
)

# ========== 测试可执行文件 ==========

# V3 测试
add_executable(concurrentcache-v3-tests
    test_v3_main.cpp
    datatype_test/object_test.cpp
    persistence_test/rdb_test.cpp
    storage_test/storage_v3_test.cpp
)

# Atomic 测试
add_executable(atomic-tests
    atomic_test/atomic_correctness_test.cpp
)

# Lock 测试
add_executable(lock-correctness-tests
    lock_test/lock_correctness_test.cpp
)

add_executable(lock-deadlock-tests
    lock_test/lock_deadlock_test.cpp
)

add_executable(lock-race-tests
    lock_test/lock_race_test.cpp
)

add_executable(lock-boundary-tests
    lock_test/lock_boundary_test.cpp
)

# 同步原语测试
add_executable(sync-primitives-tests
    sync_primitives_test/sync_primitives_test.cpp
)

# Stress 测试
add_executable(stress-test
    stress_test/stress_test.cpp
)

# Long-Running Stress 测试
add_executable(long-running-stress-test
    stress_test/long_running_stress_test.cpp
)

# Load Limit 测试
add_executable(load-limit-test
    stress_test/load_limit_test.cpp
)

# Network Stress 测试
add_executable(network-stress-test
    network_test/network_stress_test_main.cpp
    network_test/network_stress_test.cpp
)

# ========== 链接公共源文件 ==========
foreach(test_target atomic-tests lock-correctness-tests lock-deadlock-tests
                  lock-race-tests lock-boundary-tests sync-primitives-tests
                  stress-test long-running-stress-test load-limit-test network-stress-test)
    target_sources(${test_target} PRIVATE ${COMMON_SOURCES})
endforeach()

# ========== 设置包含目录 ==========
foreach(test_target concurrentcache-v3-tests atomic-tests lock-correctness-tests
                  lock-deadlock-tests lock-race-tests lock-boundary-tests
                  sync-primitives-tests stress-test long-running-stress-test
                  load-limit-test network-stress-test)
    target_include_directories(${test_target} PRIVATE ${TEST_INCLUDE_DIRS})
endforeach()

# ========== 链接库 ==========
if(UNIX)
    find_package(ZLIB REQUIRED)
    target_link_libraries(concurrentcache-v3-tests pthread ZLIB::ZLIB)
    target_link_libraries(atomic-tests pthread ZLIB::ZLIB)
    target_link_libraries(lock-correctness-tests pthread ZLIB::ZLIB)
    target_link_libraries(lock-deadlock-tests pthread ZLIB::ZLIB)
    target_link_libraries(lock-race-tests pthread ZLIB::ZLIB)
    target_link_libraries(lock-boundary-tests pthread ZLIB::ZLIB)
    target_link_libraries(sync-primitives-tests pthread ZLIB::ZLIB)
    target_link_libraries(stress-test pthread ZLIB::ZLIB)
    target_link_libraries(long-running-stress-test pthread ZLIB::ZLIB)
    target_link_libraries(load-limit-test pthread ZLIB::ZLIB)
    target_link_libraries(network-stress-test pthread ZLIB::ZLIB)
endif()

# ========== 编译选项 ==========
if(MSVC)
    foreach(test_target concurrentcache-v3-tests atomic-tests lock-correctness-tests
                      lock-deadlock-tests lock-race-tests lock-boundary-tests
                      sync-primitives-tests stress-test long-running-stress-test
                      load-limit-test network-stress-test)
        target_compile_options(${test_target} PRIVATE /W4)
    endforeach()
else()
    foreach(test_target concurrentcache-v3-tests atomic-tests lock-correctness-tests
                      lock-deadlock-tests lock-race-tests lock-boundary-tests
                      sync-primitives-tests stress-test long-running-stress-test
                      load-limit-test network-stress-test)
        target_compile_options(${test_target} PRIVATE -Wall -Wextra)
    endforeach()
endif()

# ========== 测试 ==========
enable_testing()
add_test(NAME AtomicTests COMMAND atomic-tests)
add_test(NAME LockCorrectnessTests COMMAND lock-correctness-tests)
add_test(NAME LockDeadlockTests COMMAND lock-deadlock-tests)
add_test(NAME LockRaceTests COMMAND lock-race-tests)
