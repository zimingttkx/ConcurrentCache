#ifndef NETWORK_STRESS_TEST_H
#define NETWORK_STRESS_TEST_H

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <poll.h>

namespace cc_server {

class NetworkStressTest {
public:
    struct Stats {
        std::atomic<uint64_t> total_requests{0};
        std::atomic<uint64_t> successful_requests{0};
        std::atomic<uint64_t> failed_requests{0};
        std::atomic<uint64_t> wrong_responses{0};
        std::atomic<uint64_t> connection_errors{0};
    };

    struct Config {
        std::string server_ip = "127.0.0.1";
        int server_port = 6379;
        int num_threads = 16;
        int duration_seconds = 60;
        int key_range = 10000;
        int operations_per_connection = 1000;
    };

    NetworkStressTest(const Config& config) : config_(config), stats_(std::make_shared<Stats>()) {}

    void run();

private:
    void worker_thread(int thread_id);
    bool send_command(int sock_fd, const std::string& cmd);
    std::string send_and_recv(int sock_fd, const std::string& cmd);

    // RESP protocol helpers
    std::string encode_get(const std::string& key) {
        return "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
    }
    std::string encode_set(const std::string& key, const std::string& value) {
        return "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
    }
    std::string encode_del(const std::string& key) {
        return "*2\r\n$3\r\nDEL\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
    }
    std::string encode_exists(const std::string& key) {
        return "*2\r\n$6\r\nEXISTS\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
    }

    std::string encode_hset(const std::string& key, const std::string& field, const std::string& value) {
        return "*4\r\n$4\r\nHSET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n$" + std::to_string(field.size()) + "\r\n" + field + "\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
    }
    std::string encode_hget(const std::string& key, const std::string& field) {
        return "*3\r\n$4\r\nHGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n$" + std::to_string(field.size()) + "\r\n" + field + "\r\n";
    }

    std::string encode_zadd(const std::string& key, double score, const std::string& member) {
        return "*4\r\n$4\r\nZADD\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n$" + std::to_string(std::to_string(score).size()) + "\r\n" + std::to_string(score) + "\r\n$" + std::to_string(member.size()) + "\r\n" + member + "\r\n";
    }
    std::string encode_zrange(const std::string& key, int start, int stop) {
        return "*4\r\n$6\r\nZRANGE\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n$" + std::to_string(std::to_string(start).size()) + "\r\n" + std::to_string(start) + "\r\n$" + std::to_string(std::to_string(stop).size()) + "\r\n" + std::to_string(stop) + "\r\n";
    }

    Config config_;
    std::shared_ptr<Stats> stats_;
    std::atomic<bool> running_{true};
};

} // namespace cc_server

#endif // NETWORK_STRESS_TEST_H
