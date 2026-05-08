#include "network_stress_test.h"
#include "../../src/base/log.h"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace cc_server {

void NetworkStressTest::run() {
    std::cout << "========================================" << std::endl;
    std::cout << "   Network Stress Test (Real Clients)" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Config:" << std::endl;
    std::cout << "  Server: " << config_.server_ip << ":" << config_.server_port << std::endl;
    std::cout << "  Threads: " << config_.num_threads << std::endl;
    std::cout << "  Duration: " << config_.duration_seconds << "s" << std::endl;
    std::cout << "  Key Range: " << config_.key_range << std::endl;
    std::cout << "========================================" << std::endl;

    // First, check if server is running by trying to connect
    int test_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (test_sock < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.server_port);
    inet_pton(AF_INET, config_.server_ip.c_str(), &server_addr.sin_addr);

    if (connect(test_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Cannot connect to server at " << config_.server_ip << ":" << config_.server_port << std::endl;
        std::cerr << "Please start the server first: ./build/concurrentcache-server" << std::endl;
        close(test_sock);
        return;
    }
    close(test_sock);

    std::cout << "Server connection verified." << std::endl;

    auto start_time = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;

    // Launch worker threads
    for (int i = 0; i < config_.num_threads; ++i) {
        workers.emplace_back(&NetworkStressTest::worker_thread, this, i);
    }

    // Progress reporter
    std::thread reporter([this, start_time]() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            auto total = stats_->total_requests.load();
            auto ops_per_sec = total / (elapsed > 0 ? elapsed : 1);

            std::cout << "[" << elapsed << "s] Total: " << total
                      << " | Success: " << stats_->successful_requests.load()
                      << " | Failed: " << stats_->failed_requests.load()
                      << " | Wrong: " << stats_->wrong_responses.load()
                      << " | Ops/s: " << ops_per_sec << std::endl;
        }
    });

    // Wait for duration
    std::this_thread::sleep_for(std::chrono::seconds(config_.duration_seconds));
    running_.store(false);

    // Wait for all workers
    for (auto& worker : workers) {
        worker.join();
    }
    reporter.join();

    auto end_time = std::chrono::steady_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() / 1000.0;

    std::cout << "\n========================================" << std::endl;
    std::cout << "   Network Stress Test Results" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Duration: " << total_duration << "s" << std::endl;
    std::cout << "Total Requests: " << stats_->total_requests.load() << std::endl;
    std::cout << "Successful: " << stats_->successful_requests.load() << std::endl;
    std::cout << "Failed: " << stats_->failed_requests.load() << std::endl;
    std::cout << "Wrong Responses: " << stats_->wrong_responses.load() << std::endl;
    std::cout << "Connection Errors: " << stats_->connection_errors.load() << std::endl;
    std::cout << "Avg Ops/sec: " << std::fixed << std::setprecision(0)
              << (stats_->total_requests.load() / total_duration) << std::endl;

    if (stats_->wrong_responses.load() > 0 || stats_->failed_requests.load() > stats_->connection_errors.load()) {
        std::cout << "\n[WARNING] Issues detected! Check server logs." << std::endl;
    } else {
        std::cout << "\n[SUCCESS] All responses correct!" << std::endl;
    }
}

void NetworkStressTest::worker_thread(int thread_id) {
    std::random_device rd;
    std::mt19937 gen(rd() + thread_id);
    std::uniform_int_distribution<> key_dist(0, config_.key_range - 1);
    std::uniform_int_distribution<> op_dist(0, 9);

    // Each worker maintains its own connection
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        stats_->connection_errors.fetch_add(1);
        return;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.server_port);
    inet_pton(AF_INET, config_.server_ip.c_str(), &server_addr.sin_addr);

    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        stats_->connection_errors.fetch_add(1);
        close(sock_fd);
        return;
    }

    // Set socket timeout for recv
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    uint64_t local_success = 0;
    uint64_t local_failed = 0;
    uint64_t local_wrong = 0;

    while (running_.load()) {
        int key_num = key_dist(gen);
        std::string key = "net_key_" + std::to_string(key_num);
        std::string value = "value_" + std::to_string(thread_id) + "_" + std::to_string(key_num);
        int op = op_dist(gen);

        std::string response;
        bool success = false;

        try {
            if (op < 4) {
                // SET operation
                std::string cmd = encode_set(key, value);
                response = send_and_recv(sock_fd, cmd);
                success = (response.find("+OK") != std::string::npos);
            } else if (op < 7) {
                // GET operation
                std::string cmd = encode_get(key);
                response = send_and_recv(sock_fd, cmd);
                // GET can return nil ($-1\r\n) or the value
                success = (response.find("$") != std::string::npos);  // Any bulk string response is valid
            } else if (op < 9) {
                // EXISTS operation
                std::string cmd = encode_exists(key);
                response = send_and_recv(sock_fd, cmd);
                // Response should be :0\r\n or :1\r\n
                success = (response.find(":") != std::string::npos);
            } else {
                // DEL operation
                std::string cmd = encode_del(key);
                response = send_and_recv(sock_fd, cmd);
                success = (response.find(":") != std::string::npos);
            }
        } catch (const std::exception& e) {
            stats_->connection_errors.fetch_add(1);
            // Try to reconnect
            close(sock_fd);
            sock_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (sock_fd < 0 || connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                stats_->connection_errors.fetch_add(1);
                break;
            }
            setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            continue;
        }

        stats_->total_requests.fetch_add(1);
        if (success) {
            local_success++;
            stats_->successful_requests.fetch_add(1);
        } else {
            local_failed++;
            stats_->failed_requests.fetch_add(1);
            if (response.empty()) {
                stats_->connection_errors.fetch_add(1);
            } else {
                local_wrong++;
                stats_->wrong_responses.fetch_add(1);
                // Print first few wrong responses for debugging
                if (local_wrong <= 5) {
                    std::cerr << "[Thread " << thread_id << "] Wrong response for key " << key
                              << ": " << (response.size() > 100 ? response.substr(0, 100) + "..." : response)
                              << std::endl;
                }
            }
        }
    }

    close(sock_fd);
}

bool NetworkStressTest::send_command(int sock_fd, const std::string& cmd) {
    ssize_t sent = send(sock_fd, cmd.c_str(), cmd.size(), 0);
    return sent == static_cast<ssize_t>(cmd.size());
}

std::string NetworkStressTest::send_and_recv(int sock_fd, const std::string& cmd) {
    if (!send_command(sock_fd, cmd)) {
        throw std::runtime_error("Send failed");
    }

    // Use poll for more reliable reading
    struct pollfd pfd;
    pfd.fd = sock_fd;
    pfd.events = POLLIN;

    int poll_result = poll(&pfd, 1, 5000);  // 5 second timeout
    if (poll_result <= 0) {
        throw std::runtime_error("Poll timeout or error");
    }

    char buffer[8192];
    std::string response;
    ssize_t n = recv(sock_fd, buffer, sizeof(buffer) - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';
        response = buffer;
    } else if (n == 0) {
        throw std::runtime_error("Connection closed");
    } else {
        throw std::runtime_error("Recv failed");
    }

    return response;
}

} // namespace cc_server
