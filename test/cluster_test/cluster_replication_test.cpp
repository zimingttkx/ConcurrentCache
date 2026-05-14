/**
 * Cluster Replication Unit Tests
 *
 * Tests for the replication subsystem:
 * - Replication buffer operations
 * - Replica management (add/remove/get)
 * - PSYNC command full/incremental sync
 */

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cassert>
#include <cstring>
#include <sstream>

#include "cluster/cluster_state.h"
#include "cluster/cluster_node.h"
#include "cluster/replication_mgr.h"
#include "base/config.h"

using namespace cc_server;

// ============================================================================
// TEST FRAMEWORK
// ============================================================================

struct TestResults {
    std::atomic<int> passed{0};
    std::atomic<int> failed{0};
    std::mutex log_mutex;
    std::vector<std::string> failures;

    void pass(const std::string& test, const std::string& msg) {
        passed++;
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cout << "[  PASS  ] " << test << " - " << msg << "\n";
    }

    void fail(const std::string& test, const std::string& msg) {
        failed++;
        std::lock_guard<std::mutex> lock(log_mutex);
        std::string full_msg = test + " - " + msg;
        failures.push_back(full_msg);
        std::cout << "[  FAIL  ] " << full_msg << "\n";
    }

    void section(const std::string& name) {
        std::cout << "\n" << std::string(60, '=') << "\n";
        std::cout << "  " << name << "\n";
        std::cout << std::string(60, '=') << "\n";
    }

    void summary() {
        std::cout << "\n" << std::string(60, '=') << "\n";
        std::cout << "  TEST SUMMARY\n";
        std::cout << std::string(60, '=') << "\n";
        std::cout << "  Passed:  " << passed.load() << "\n";
        std::cout << "  Failed:  " << failed.load() << "\n";

        if (!failures.empty()) {
            std::cout << "\n  FAILURES:\n";
            for (const auto& f : failures) {
                std::cout << "    - " << f << "\n";
            }
        }
    }
};

TestResults g_results;

#define STRICT_ASSERT(condition, test_name, msg) \
    if (!(condition)) { \
        g_results.fail(test_name, msg); \
        return false; \
    }

#define STRICT_ASSERT_EQ(actual, expected, test_name, msg) \
    if ((actual) != (expected)) { \
        std::ostringstream oss; \
        oss << msg << " (expected " << (expected) << ", got " << (actual) << ")"; \
        g_results.fail(test_name, oss.str()); \
        return false; \
    }

#define STRICT_ASSERT_GT(a, b, test_name, msg) \
    if (!((a) > (b))) { \
        std::ostringstream oss; \
        oss << msg << " (expected " << #a << " > " << #b << ")"; \
        g_results.fail(test_name, oss.str()); \
        return false; \
    }

#define STRICT_ASSERT_NONNULL(ptr, test_name, msg) \
    if ((ptr) == nullptr) { \
        g_results.fail(test_name, msg); \
        return false; \
    }

#define TEST_CASE(name) static bool name()

// ============================================================================
// REPLICATION BUFFER TESTS
// ============================================================================

TEST_CASE(test_replication_buffer_basic) {
    g_results.section("Replication Buffer Basic Operations");

    auto& repl_mgr = ReplicationMgr::instance();
    int64_t start = repl_mgr.get_master_repl_offset();

    // Add commands to buffer and verify offset advances
    repl_mgr.add_to_replication_buffer("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n");
    int64_t after_first = repl_mgr.get_master_repl_offset();
    STRICT_ASSERT_GT(after_first, start, "replication_buffer_basic",
                     "Offset should advance after adding first command");

    repl_mgr.add_to_replication_buffer("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    int64_t after_second = repl_mgr.get_master_repl_offset();
    STRICT_ASSERT_GT(after_second, after_first, "replication_buffer_basic",
                     "Offset should advance after adding second command");

    g_results.pass("replication_buffer_basic", "Buffer add operations work correctly");
    return true;
}

TEST_CASE(test_replication_buffer_offset) {
    g_results.section("Replication Buffer Offset Tracking");

    auto& repl_mgr = ReplicationMgr::instance();
    int64_t start_offset = repl_mgr.get_master_repl_offset();

    // Add one command and check offset advances
    std::string cmd = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
    repl_mgr.add_to_replication_buffer(cmd);
    int64_t new_offset = repl_mgr.get_master_repl_offset();

    STRICT_ASSERT_GT(new_offset, start_offset, "replication_buffer_offset",
                     "Master offset should advance after adding command");
    STRICT_ASSERT_EQ(new_offset, start_offset + static_cast<int64_t>(cmd.size()),
                     "replication_buffer_offset",
                     "Offset should advance by command size");

    g_results.pass("replication_buffer_offset", "Offset tracking works correctly");
    return true;
}

TEST_CASE(test_replication_buffer_multiple) {
    g_results.section("Replication Buffer Multiple Commands");

    auto& repl_mgr = ReplicationMgr::instance();
    int64_t before = repl_mgr.get_master_repl_offset();

    // Add 10 commands
    std::vector<std::string> commands;
    for (int i = 0; i < 10; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string val = "val" + std::to_string(i);
        std::string cmd = "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" +
                          key + "\r\n$" + std::to_string(val.size()) + "\r\n" + val + "\r\n";
        commands.push_back(cmd);
        repl_mgr.add_to_replication_buffer(cmd);
    }

    int64_t after = repl_mgr.get_master_repl_offset();
    STRICT_ASSERT_GT(after, before, "replication_buffer_multiple",
                     "Offset should advance after adding multiple commands");

    // Calculate total expected offset advance
    int64_t total_size = 0;
    for (const auto& cmd : commands) {
        total_size += static_cast<int64_t>(cmd.size());
    }
    STRICT_ASSERT_EQ(after, before + total_size, "replication_buffer_multiple",
                     "Offset should advance by total command size");

    g_results.pass("replication_buffer_multiple", "Buffer handles multiple commands correctly");
    return true;
}

// ============================================================================
// REPLICA MANAGEMENT TESTS
// ============================================================================

TEST_CASE(test_replica_add_remove) {
    g_results.section("Replica Add and Remove");

    ClusterState state;
    ReplicationMgr::instance().init(&state);

    auto node = std::make_shared<ClusterNode>("replica1:6379", "127.0.0.1", 6379, NodeRole::kReplica);

    // Add replica
    ReplicationMgr::instance().add_replica("replica1", "127.0.0.1", 6379, node);

    // Verify replica exists
    auto replicas = ReplicationMgr::instance().get_all_replicas();
    STRICT_ASSERT_GT(replicas.size(), static_cast<size_t>(0), "replica_add_remove",
                     "Should have at least one replica after add");
    STRICT_ASSERT(replicas[0]->name == "replica1", "replica_add_remove",
                  "Replica name should match");

    // Remove replica
    ReplicationMgr::instance().remove_replica("replica1");
    replicas = ReplicationMgr::instance().get_all_replicas();
    STRICT_ASSERT_EQ(replicas.size(), static_cast<size_t>(0), "replica_add_remove",
                     "Should have no replicas after remove");

    g_results.pass("replica_add_remove", "Replica add/remove operations work correctly");
    return true;
}

TEST_CASE(test_replica_multiple_add) {
    g_results.section("Replica Multiple Add");

    ClusterState state;
    ReplicationMgr::instance().init(&state);

    for (int i = 0; i < 3; ++i) {
        std::string name = "replica" + std::to_string(i);
        int port = 6380 + i;
        auto node = std::make_shared<ClusterNode>(name, "127.0.0.1", port, NodeRole::kReplica);
        ReplicationMgr::instance().add_replica(name, "127.0.0.1", port, node);
    }

    auto replicas = ReplicationMgr::instance().get_all_replicas();
    STRICT_ASSERT_EQ(replicas.size(), static_cast<size_t>(3), "replica_multiple_add",
                     "Should have exactly 3 replicas");

    g_results.pass("replica_multiple_add", "Multiple replica add works correctly");
    return true;
}

// ============================================================================
// PSYNC COMMAND TESTS
// ============================================================================

TEST_CASE(test_psync_full_sync_request) {
    g_results.section("PSYNC Full Sync Request");

    ClusterState state;
    ReplicationMgr::instance().init(&state);
    ReplicationMgr::instance().set_master("127.0.0.1", 6379, "");

    // Simulate a full sync request (PSYNC ? -1)
    ReplicationMgr::instance().set_sync_state(SyncState::kWaiting);

    SyncState state_after = ReplicationMgr::instance().get_sync_state();
    STRICT_ASSERT_EQ(static_cast<int>(state_after), static_cast<int>(SyncState::kWaiting),
                     "psync_full_sync", "Should be in waiting state after full sync request");

    g_results.pass("psync_full_sync", "Full sync state transition works correctly");
    return true;
}

TEST_CASE(test_psync_runid_generation) {
    g_results.section("PSYNC Runid Generation");

    std::string runid1 = ReplicationMgr::generate_runid();
    std::string runid2 = ReplicationMgr::generate_runid();

    STRICT_ASSERT_EQ(runid1.size(), static_cast<size_t>(40), "psync_runid_gen",
                     "Runid should be 40 characters");
    STRICT_ASSERT(!runid1.empty(), "psync_runid_gen", "Runid should not be empty");
    // Two generated runids should be different (probabilistic, but highly likely)
    STRICT_ASSERT(runid1 != runid2, "psync_runid_gen",
                  "Two generated runids should be different");

    g_results.pass("psync_runid_gen", "Runid generation is valid");
    return true;
}

TEST_CASE(test_replication_master_info) {
    g_results.section("Replication Master Info");

    ClusterState state;
    ReplicationMgr::instance().init(&state);

    // Set master info
    ReplicationMgr::instance().set_master("192.168.1.100", 6379, "test-runid-12345");

    std::string ip = ReplicationMgr::instance().get_master_ip();
    int port = ReplicationMgr::instance().get_master_port();
    std::string runid = ReplicationMgr::instance().get_master_runid();

    STRICT_ASSERT_EQ(ip, std::string("192.168.1.100"), "repl_master_info", "Master IP should match");
    STRICT_ASSERT_EQ(port, 6379, "repl_master_info", "Master port should match");
    STRICT_ASSERT_EQ(runid, std::string("test-runid-12345"), "repl_master_info", "Master runid should match");

    g_results.pass("repl_master_info", "Master info set/get works correctly");
    return true;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Cluster Replication Unit Tests       " << std::endl;
    std::cout << "========================================" << std::endl;

    // Load config
    Config::instance().load("conf/concurrentcache.conf");

    bool all_passed = true;

    auto tests = {
        &test_replication_buffer_basic,
        &test_replication_buffer_offset,
        &test_replication_buffer_multiple,
        &test_replica_add_remove,
        &test_replica_multiple_add,
        &test_psync_full_sync_request,
        &test_psync_runid_generation,
        &test_replication_master_info,
    };

    for (auto test : tests) {
        try {
            if (!test()) {
                all_passed = false;
            }
        } catch (const std::exception& e) {
            g_results.fail("exception", std::string("Exception: ") + e.what());
            all_passed = false;
        } catch (...) {
            g_results.fail("exception", "Unknown exception occurred");
            all_passed = false;
        }
    }

    g_results.summary();
    return all_passed ? 0 : 1;
}
