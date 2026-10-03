// 契约测试用的最小 RESP 客户端：真 socket + 真协议，不走任何产品内部接口。
//
// 为什么要自己写而不是链接产品的 Buffer/RespParser：契约测试的意义是
// "从外面看服务器做对了没"，用被测代码去解析被测代码的输出，parser 一坏
// 测试就同时失明。
#ifndef CONCURRENTCACHE_TEST_RESP_CLIENT_H
#define CONCURRENTCACHE_TEST_RESP_CLIENT_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace cc_server {
namespace testing {

// 一条回复。type 是 RESP 的类型字节；nil 覆盖 $-1 与 *-1。
struct Reply {
    char type = '?';
    std::string str;
    long long integer = 0;
    bool nil = false;
    std::vector<Reply> elements;

    bool is_error() const { return type == '-'; }
    bool starts_with(const char* prefix) const { return str.rfind(prefix, 0) == 0; }
};

class RespClient {
public:
    RespClient() = default;
    ~RespClient() { close(); }

    // 必须显式写移动并删掉拷贝：类里有用户声明的析构函数，隐式移动不会被生成，
    // 而隐式拷贝会把 fd_ 复制一份、源对象析构时就把连接关掉了——C++20 只会给
    // "deprecated copy"警告，测试则表现为莫名断连。
    RespClient(const RespClient&) = delete;
    RespClient& operator=(const RespClient&) = delete;

    RespClient(RespClient&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

    RespClient& operator=(RespClient&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    bool connect_to(const std::string& host, int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        // 读超时：回复没按时到达就让测试失败，而不是把 ctest 拖到 TIMEOUT。
        timeval tv{};
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            close();
            return false;
        }

        // 就绪轮询由调用方做，这里只试一次连接。
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool is_connected() const { return fd_ >= 0; }

    bool send_command(const std::vector<std::string>& args) {
        std::string out = "*" + std::to_string(args.size()) + "\r\n";
        for (const auto& arg : args) {
            out += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
        }
        return write_all(out.data(), out.size());
    }

    // 阻塞读一条完整回复；失败（超时/断链/协议不符）返回 false。
    bool read_reply(Reply& out) { return read_reply_impl(out, 0); }

private:
    static constexpr int kMaxDepth = 8;

    bool read_reply_impl(Reply& out, int depth) {
        std::string line;
        if (!read_line(line)) return false;
        if (line.empty()) return false;

        const char type = line[0];
        out = Reply{};
        out.type = type;

        switch (type) {
            case '+':
            case '-':
                out.str = line.substr(1);
                return true;
            case ':': {
                try {
                    out.integer = std::stoll(line.substr(1));
                } catch (...) {
                    return false;
                }
                return true;
            }
            case '$': {
                long long len = 0;
                try {
                    len = std::stoll(line.substr(1));
                } catch (...) {
                    return false;
                }
                if (len < 0) {
                    out.nil = true;
                    return true;
                }
                if (len > static_cast<long long>(64 * 1024 * 1024)) return false;
                if (!read_exact(out.str, static_cast<size_t>(len))) return false;
                std::string crlf;
                if (!read_line(crlf) || !crlf.empty()) return false;
                return true;
            }
            case '*': {
                if (depth >= kMaxDepth) return false;
                long long count = 0;
                try {
                    count = std::stoll(line.substr(1));
                } catch (...) {
                    return false;
                }
                if (count < 0) {
                    out.nil = true;
                    return true;
                }
                out.elements.reserve(static_cast<size_t>(count));
                for (long long i = 0; i < count; ++i) {
                    Reply elem;
                    if (!read_reply_impl(elem, depth + 1)) return false;
                    out.elements.push_back(std::move(elem));
                }
                return true;
            }
            default:
                return false;
        }
    }

    bool read_line(std::string& out) {
        out.clear();
        char c = 0;
        while (true) {
            if (!read_byte(c)) return false;
            if (c == '\r') {
                if (!read_byte(c) || c != '\n') return false;
                return true;
            }
            out.push_back(c);
        }
    }

    bool read_exact(std::string& out, size_t n) {
        out.assign(n, '\0');
        size_t done = 0;
        while (done < n) {
            ssize_t r = ::recv(fd_, &out[done], n - done, 0);
            if (r <= 0) return false;
            done += static_cast<size_t>(r);
        }
        return true;
    }

    bool read_byte(char& c) {
        ssize_t r = ::recv(fd_, &c, 1, 0);
        return r == 1;
    }

    bool write_all(const char* data, size_t n) {
        size_t done = 0;
        while (done < n) {
            ssize_t w = ::send(fd_, data + done, n - done, MSG_NOSIGNAL);
            if (w <= 0) return false;
            done += static_cast<size_t>(w);
        }
        return true;
    }

    int fd_ = -1;
};

}  // namespace testing
}  // namespace cc_server

#endif  // CONCURRENTCACHE_TEST_RESP_CLIENT_H
