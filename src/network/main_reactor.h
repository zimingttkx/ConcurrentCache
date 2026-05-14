#ifndef CONCURRENTCACHE_MAIN_REACTOR_H
#define CONCURRENTCACHE_MAIN_REACTOR_H

#include "event_loop.h"
#include "socket.h"
#include "channel.h"
#include <atomic>

namespace cc_server {

    class MainReactor {
    public:
        MainReactor();
        ~MainReactor();

        MainReactor(const MainReactor&) = delete;
        MainReactor& operator=(const MainReactor&) = delete;

        bool init(int port);
        void start();
        void stop();

        EventLoop* event_loop() { return loop_.get(); }

    private:
        void handle_accept();
        void add_new_connection(int client_fd);

        std::unique_ptr<EventLoop> loop_;
        Socket listen_socket_;
        std::unique_ptr<Channel> listen_channel_;
        std::atomic<bool> initialized_{false};
        std::atomic<bool> running_{false};
    };
}

#endif
