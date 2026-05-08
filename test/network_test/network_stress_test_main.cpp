#include <iostream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <sys/wait.h>
#include "network_stress_test.h"

using namespace cc_server;

pid_t start_server() {
    std::cout << "Starting ConcurrentCache server..." << std::endl;

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork" << std::endl;
        return -1;
    }

    if (pid == 0) {
        // Child process - exec the server
        execl("./concurrentcache-server", "concurrentcache-server", nullptr);
        std::cerr << "Exec failed" << std::endl;
        exit(1);
    }

    // Parent - wait for server to start
    std::cout << "Waiting for server to start (PID: " << pid << ")..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    return pid;
}

void stop_server(pid_t pid) {
    std::cout << "Stopping server (PID: " << pid << ")..." << std::endl;
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
}

bool wait_for_server(int max_attempts = 30) {
    for (int i = 0; i < max_attempts; ++i) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(6379);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            close(sock);
            std::cout << "Server is ready!" << std::endl;
            return true;
        }
        close(sock);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "   ConcurrentCache Network Stress Test  " << std::endl;
    std::cout << "========================================" << std::endl;

    // Parse command line arguments
    NetworkStressTest::Config config;
    bool start_server_embedded = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            config.num_threads = std::stoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            config.duration_seconds = std::stoi(argv[++i]);
        } else if (arg == "--key-range" && i + 1 < argc) {
            config.key_range = std::stoi(argv[++i]);
        } else if (arg == "--external") {
            start_server_embedded = false;
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --threads N       Number of threads (default: 16)" << std::endl;
            std::cout << "  --duration N      Test duration in seconds (default: 60)" << std::endl;
            std::cout << "  --key-range N     Number of keys to use (default: 10000)" << std::endl;
            std::cout << "  --external        Don't start server, use existing" << std::endl;
            std::cout << "  --help            Show this help" << std::endl;
            return 0;
        }
    }

    pid_t server_pid = -1;

    if (start_server_embedded) {
        server_pid = start_server();
        if (server_pid < 0) {
            std::cerr << "Failed to start server" << std::endl;
            return 1;
        }

        if (!wait_for_server()) {
            std::cerr << "Server failed to start" << std::endl;
            stop_server(server_pid);
            return 1;
        }
    } else {
        std::cout << "Waiting for external server..." << std::endl;
        if (!wait_for_server()) {
            std::cerr << "Cannot connect to server" << std::endl;
            return 1;
        }
    }

    // Run the network stress test
    NetworkStressTest test(config);
    test.run();

    if (start_server_embedded) {
        stop_server(server_pid);
    }

    std::cout << "\nTest completed." << std::endl;
    return 0;
}
