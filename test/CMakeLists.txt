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
    ${CMAKE_SOURCE_DIR}/src/command/psync_cmd.cpp
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
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_node.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_state.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_server.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_link.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_connection.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_gossip.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/replication_mgr.cpp
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_bus.cpp
    ${CMAKE_SOURCE_DIR}/src/command/cluster_cmd.cpp
    ${CMAKE_SOURCE_DIR}/src/command/restore_cmd.cpp
    # tests.cpp 的 MemoryPool 一致性/压力/边界用例需要这三层的实现；此前
    # COMMON_SOURCES 缺整个 memorypool，所以 tests.cpp 就算接线也链接不上。
    ${CMAKE_SOURCE_DIR}/src/memorypool/central_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/memorypool/free_list.cpp
    ${CMAKE_SOURCE_DIR}/src/memorypool/page_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/memorypool/size_class.cpp
    ${CMAKE_SOURCE_DIR}/src/memorypool/span.cpp
    ${CMAKE_SOURCE_DIR}/src/memorypool/thread_cache.cpp
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
    cluster_test/cluster_link_framing_test.cpp
    datatype_test/object_test.cpp
    memorypool_test/thread_cache_test.cpp
    network_test/connection_lifetime_test.cpp
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

# Cluster 测试
add_executable(cluster-tests
    cluster_test/cluster_test.cpp
)

# Cluster 复制 / 严格断言测试**不接入**：两者都调用
# ReplicationMgr::add_to_replication_buffer()，而它在 replication_mgr.h:128 之后
# 是 private（cluster_replication_test.cpp:118/123/140/167、
# cluster_strict_test.cpp 同源）。把它们改成能编译需要动产品可见性或重写测试，
# 都不属于"接线"这一步。
#
# Lock 压力测试（自带 main 且退出码正确）
add_executable(lock-stress-tests
    lock_test/lock_stress_test.cpp
)

# 并发 + 内存池测试：test_main.cpp 提供 main()，tests.cpp 提供
# run_thread_pool_tests / run_lock_tests / run_memory_pool_tests，两者是一对，
# 此前一起被漏在构建外。
# 注意它的判定强度：tests.cpp 只打印统计并依赖 assert()，没有走 EXPECT_ 宏，
# 而 CI 的 Release 构建会编译掉 assert —— 所以这里的"通过"只代表没抛异常、
# 没 abort，内存池的真实正确性要靠 daily 的 Debug/ASAN 跑它才算有牙齿。
add_executable(concurrency-tests
    test_main.cpp
    tests.cpp
)

# 语义契约测试：起真服务器、走真 socket 与真 RESP，断言 Redis 的（也是本仓库
# 文档承诺的）行为。自 #41 起整层连续绿（contract-tests 在 #40/#41/#42/#43 四次
# 运行里都是 7/7），所以本条已从 contract 收紧到 gate —— 收紧的判据是 main 上的
# 实际结果，不是"看起来稳"。contract 这一层现在只剩 ConcurrencyTests：它只打印
# 统计并依赖 assert()，而 Release 构型会把 assert 编掉，它的"通过"只代表没崩、
# 没 abort、没超时，拿来拦合并价值有限；要让它有牙齿得先把 tests.cpp 里的断言
# 换成 EXPECT_ 宏，那是改测试而不是改标签。
add_executable(contract-tests
    contract_test/contract_test.cpp
)

# 命令表探针（scripts/ci/check_consistency.py 用）：它是工具不是用例，所以进
# CC_TEST_TARGETS 拿到编译/链接参数，但不注册进 ctest。
add_executable(command-table-probe
    tools/command_table_probe.cpp
)

# 所有测试可执行文件。链接、包含目录、编译选项、sanitizer 与 ctest 注册
# 全部从这里派生，避免六份手写清单各自漂移。
set(CC_TEST_TARGETS
    concurrentcache-v3-tests
    atomic-tests
    lock-correctness-tests
    lock-deadlock-tests
    lock-race-tests
    lock-boundary-tests
    lock-stress-tests
    sync-primitives-tests
    stress-test
    long-running-stress-test
    load-limit-test
    network-stress-test
    cluster-tests
    concurrency-tests
    contract-tests
    command-table-probe
)

# ========== 链接公共源文件 ==========
foreach(test_target ${CC_TEST_TARGETS})
    target_sources(${test_target} PRIVATE ${COMMON_SOURCES})
endforeach()

# ========== 设置包含目录 ==========
foreach(test_target ${CC_TEST_TARGETS})
    target_include_directories(${test_target} PRIVATE ${TEST_INCLUDE_DIRS})
endforeach()

# ========== 链接库 ==========
if(UNIX)
    find_package(ZLIB REQUIRED)
    foreach(test_target ${CC_TEST_TARGETS})
        target_link_libraries(${test_target} pthread ZLIB::ZLIB)
    endforeach()
endif()

# ========== 编译选项 ==========
if(MSVC)
    foreach(test_target ${CC_TEST_TARGETS})
        target_compile_options(${test_target} PRIVATE /W4)
    endforeach()
else()
    foreach(test_target ${CC_TEST_TARGETS})
        target_compile_options(${test_target} PRIVATE
            -Wall -Wextra
            -Wconversion -Wshadow -Wsign-conversion -Wdouble-promotion)
    endforeach()
endif()

# ========== Sanitizer 标志 ==========
if(ENABLE_ASAN)
    foreach(test_target ${CC_TEST_TARGETS})
        target_compile_options(${test_target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${test_target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    endforeach()
endif()

if(ENABLE_TSAN)
    foreach(test_target ${CC_TEST_TARGETS})
        target_compile_options(${test_target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${test_target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    endforeach()
endif()

if(ENABLE_UBSAN)
    foreach(test_target ${CC_TEST_TARGETS})
        target_compile_options(${test_target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${test_target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    endforeach()
endif()

# ========== 测试 ==========
# enable_testing() 在根 CMakeLists.txt，否则 ctest --test-dir build 找不到测试。
#
# 注册表格式：名称|target|ctest LABELS|TIMEOUT(秒)
#   gate     —— 必过门禁（ci.yml: ctest -L gate）
#   contract —— 结果未知或断言正确语义，失败可见但不拦合并
#   slow     —— 只在 daily.yml 跑
# TIMEOUT 是硬要求：没有它的用例会一直挂到 GitHub 的 6 小时上限。
set(CC_TESTS
    "AtomicTests|atomic-tests|gate|60"
    "SyncPrimitivesTests|sync-primitives-tests|gate|120"
    "ClusterTests|cluster-tests|gate|60"
    "LockCorrectnessTests|lock-correctness-tests|gate|120"
    "LockDeadlockTests|lock-deadlock-tests|gate|120"
    "LockRaceTests|lock-race-tests|gate|120"
    "LockBoundaryTests|lock-boundary-tests|gate|120"
    "V3Tests|concurrentcache-v3-tests|gate|600"
    "ContractTests|contract-tests|gate|180"
    "ConcurrencyTests|concurrency-tests|contract|300"
    "LockStressTests|lock-stress-tests|slow|300"
    "StressTest|stress-test|slow|1800"
    "LoadLimitTest|load-limit-test|slow|3600"
    "LongRunningStressTest|long-running-stress-test|slow|7200"
    "NetworkStressTest|network-stress-test|slow|900"
)

# 每个用例在自己的空目录里跑：stress 类会往工作目录写 persistence_test.rdb /
# bgsave_dump.rdb，而仓库里已经有被跟踪的 dump.rdb，隔离后才能用 git diff
# 检查测试有没有污染源码树。
set(CC_TEST_RUN_DIR "${CMAKE_CURRENT_BINARY_DIR}/run")

foreach(spec ${CC_TESTS})
    string(REPLACE "|" ";" parts "${spec}")
    list(GET parts 0 test_name)
    list(GET parts 1 test_target)
    list(GET parts 2 test_labels)
    list(GET parts 3 test_timeout)

    set(test_run_dir "${CC_TEST_RUN_DIR}/${test_name}")
    file(MAKE_DIRECTORY "${test_run_dir}")

    add_test(NAME ${test_name} COMMAND ${test_target})
    set_tests_properties(${test_name} PROPERTIES
        LABELS "${test_labels}"
        TIMEOUT ${test_timeout}
        WORKING_DIRECTORY "${test_run_dir}"
    )
endforeach()

# network-stress-test 自己 fork 出并发服务器并占端口，不与其它用例并行。
# 它用 execl("./concurrentcache-server") 相对工作目录找可执行文件，所以这里
# 把它的 WORKING_DIRECTORY 指回 build/（该目录下没有 conf/，服务器会落到
# main.cpp 的默认端口 6379，与 network_stress_test.h 的探测端口一致）。
set_tests_properties(NetworkStressTest PROPERTIES
    RESOURCE_LOCK "cc_server_port"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# 契约测试要起真服务器：把 server 二进制的绝对路径用生成器表达式交给它，
# 这样同一份测试在 build/、ASAN/TSan、Debug 构型下都能找到被测物。
set_tests_properties(ContractTests PROPERTIES
    ENVIRONMENT "CC_SERVER_BIN=$<TARGET_FILE:concurrentcache-server>"
    RESOURCE_LOCK "cc_server_port"
)

# command_test/command_test.cpp 故意不接入任何 target：它已和产品 API 脱节
# （构造 CommandFactory(storage)、调用 create_command()，而现在只有 instance()
# 与 create()；且用局部 GlobalStorage 而不是 GlobalStorage::instance()）。
# 要恢复命令层覆盖必须先重写这个文件，属于缺陷队列而不是接线工作。
