#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "cluster/cluster_server.h"
#include "cluster/cluster_node.h"
#include "cluster/cluster_state.h"

using namespace cc_server;

bool test_passed = true;

#define CHECK(condition, msg) \
    if (!(condition)) { \
        std::cout << "[FAIL] " << msg << std::endl; \
        test_passed = false; \
    } else { \
        std::cout << "[PASS] " << msg << std::endl; \
    }

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "       Cluster Module Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    // ========== ClusterNode Tests ==========
    std::cout << "\n--- ClusterNode Tests ---" << std::endl;

    {
        ClusterNode node("test_node", "127.0.0.1", 6379, NodeRole::kMaster);
        CHECK(node.getName() == "test_node", "getName");
        CHECK(node.getInfo().ip == "127.0.0.1", "getInfo().ip");
        CHECK(node.getInfo().port == 6379, "getInfo().port");
        CHECK(node.isMaster() == true, "isMaster");
        CHECK(node.isReplica() == false, "isReplica");
    }

    {
        ClusterNode node("test_node", "127.0.0.1", 6379, NodeRole::kMaster);
        CHECK(node.isMaster() == true, "initial role is master");
        node.setRole(NodeRole::kReplica);
        CHECK(node.isMaster() == false, "after setRole to replica");
        CHECK(node.isReplica() == true, "isReplica after change");
    }

    {
        ClusterNode node("test_node", "127.0.0.1", 6379, NodeRole::kMaster);
        node.addSlot(100);
        node.addSlot(200);
        node.addSlot(300);
        CHECK(node.hasSlot(100) == true, "hasSlot(100)");
        CHECK(node.hasSlot(200) == true, "hasSlot(200)");
        CHECK(node.hasSlot(300) == true, "hasSlot(300)");
        CHECK(node.hasSlot(400) == false, "hasSlot(400) - not added");

        node.delSlot(200);
        CHECK(node.hasSlot(100) == true, "hasSlot(100) after del");
        CHECK(node.hasSlot(200) == false, "hasSlot(200) after del");
        CHECK(node.hasSlot(300) == true, "hasSlot(300) after del");
    }

    {
        ClusterNode node("test_node", "127.0.0.1", 6379, NodeRole::kReplica);
        node.setMaster("127.0.0.2", 6380);
        CHECK(node.hasMaster() == true, "hasMaster after setMaster");
        CHECK(node.getInfo().replicaof_ip == "127.0.0.2", "replicaof_ip");
        CHECK(node.getInfo().replicaof_port == 6380, "replicaof_port");
        CHECK(node.isReplica() == true, "isReplica after setMaster");
    }

    // ========== ClusterState Tests ==========
    std::cout << "\n--- ClusterState Tests ---" << std::endl;

    {
        ClusterState state;
        auto node1 = std::make_shared<ClusterNode>("node1", "127.0.0.1", 6379, NodeRole::kMaster);
        auto node2 = std::make_shared<ClusterNode>("node2", "127.0.0.2", 6380, NodeRole::kMaster);

        state.addNode(node1);
        state.addNode(node2);
        CHECK(state.size() == 2, "size after adding 2 nodes");

        auto found = state.getNode("node1");
        CHECK(found != nullptr, "getNode(\"node1\") != nullptr");
        if (found) {
            CHECK(found->getName() == "node1", "found node name");
        }

        state.delNode("node1");
        CHECK(state.size() == 1, "size after deleting 1 node");

        auto notFound = state.getNode("node1");
        CHECK(notFound == nullptr, "getNode(\"node1\") after delete == nullptr");
    }

    {
        ClusterState state;
        auto node1 = std::make_shared<ClusterNode>("node1", "127.0.0.1", 6379, NodeRole::kMaster);
        state.addNode(node1);
        CHECK(state.size() == 1, "size after adding node1");

        // 添加同名节点应该被拒绝
        auto node1_dup = std::make_shared<ClusterNode>("node1", "127.0.0.1", 6379, NodeRole::kMaster);
        state.addNode(node1_dup);
        CHECK(state.size() == 1, "size unchanged after duplicate add");
    }

    {
        ClusterState state;
        auto node1 = std::make_shared<ClusterNode>("node1", "127.0.0.1", 6379, NodeRole::kMaster);
        auto node2 = std::make_shared<ClusterNode>("node2", "127.0.0.2", 6380, NodeRole::kMaster);

        node1->addSlot(0);
        node1->addSlot(1);
        node1->addSlot(2);

        state.addNode(node1);
        state.addNode(node2);
        state.setNodeForSlot(0, node1);
        state.setNodeForSlot(1, node1);
        state.setNodeForSlot(100, node2);

        auto owner0 = state.getNodeForSlot(0);
        CHECK(owner0 != nullptr, "getNodeForSlot(0) != nullptr");
        if (owner0) {
            CHECK(owner0->getName() == "node1", "owner of slot 0 is node1");
        }

        auto owner100 = state.getNodeForSlot(100);
        CHECK(owner100 != nullptr, "getNodeForSlot(100) != nullptr");
        if (owner100) {
            CHECK(owner100->getName() == "node2", "owner of slot 100 is node2");
        }

        auto owner9999 = state.getNodeForSlot(9999);
        CHECK(owner9999 == nullptr, "getNodeForSlot(9999) == nullptr (unassigned)");
    }

    {
        ClusterState state;
        auto node1 = std::make_shared<ClusterNode>("node1", "127.0.0.1", 6379, NodeRole::kMaster);
        node1->addSlot(100);
        node1->addSlot(200);

        state.addNode(node1);
        state.setNodeForSlot(100, node1);
        state.setNodeForSlot(200, node1);

        CHECK(state.getNodeForSlot(100) != nullptr, "slot 100 assigned before delete");
        CHECK(state.getNodeForSlot(200) != nullptr, "slot 200 assigned before delete");

        state.delNode("node1");

        CHECK(state.getNodeForSlot(100) == nullptr, "slot 100 cleared after delNode");
        CHECK(state.getNodeForSlot(200) == nullptr, "slot 200 cleared after delNode");
    }

    {
        ClusterState state;
        auto node1 = std::make_shared<ClusterNode>("node1", "127.0.0.1", 6379, NodeRole::kMaster);
        auto node2 = std::make_shared<ClusterNode>("node2", "127.0.0.2", 6380, NodeRole::kMaster);

        state.addNode(node1);
        state.addNode(node2);

        auto found = state.getNodeByIpPort("127.0.0.1", 6379);
        CHECK(found != nullptr, "getNodeByIpPort found node1");
        if (found) {
            CHECK(found->getName() == "node1", "getNodeByIpPort returns correct name");
        }

        auto notFound = state.getNodeByIpPort("127.0.0.3", 6379);
        CHECK(notFound == nullptr, "getNodeByIpPort returns nullptr for unknown");
    }

    // ========== ClusterServer Tests ==========
    std::cout << "\n--- ClusterServer Tests ---" << std::endl;

    {
        auto& instance1 = ClusterServer::instance();
        auto& instance2 = ClusterServer::instance();
        CHECK(&instance1 == &instance2, "Singleton returns same instance");
    }

    {
        auto& server = ClusterServer::instance();

        int slot1 = server.keyToSlot("key1");
        CHECK(slot1 >= 0 && slot1 <= 16383, "keyToSlot(\"key1\") in range [0, 16383]");

        int slot2 = server.keyToSlot("another_key");
        CHECK(slot2 >= 0 && slot2 <= 16383, "keyToSlot(\"another_key\") in range");

        int slot3 = server.keyToSlot("third_key");
        CHECK(slot3 >= 0 && slot3 <= 16383, "keyToSlot(\"third_key\") in range");
    }

    {
        auto& server = ClusterServer::instance();

        // 相同 tag 应该映射到相同槽
        int slot1 = server.keyToSlot("{mytag}.somekey");
        int slot2 = server.keyToSlot("{mytag}.anotherkey");
        CHECK(slot1 == slot2, "same tag produces same slot");
        CHECK(slot1 >= 0 && slot1 <= 16383, "tagged slot in range");
    }

    // ========== Summary ==========
    std::cout << "\n========================================" << std::endl;
    if (test_passed) {
        std::cout << "       ALL TESTS PASSED" << std::endl;
    } else {
        std::cout << "       SOME TESTS FAILED" << std::endl;
    }
    std::cout << "========================================" << std::endl;

    return test_passed ? 0 : 1;
}
