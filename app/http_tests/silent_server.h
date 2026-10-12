// SPDX-License-Identifier: MIT
#pragma once

// A loopback TCP server for the tests of the HTTP client that accepts one connection and then says
// nothing: a request to it is "in flight" until the client gives up or closes. POSIX only (the
// Windows leg of the same behaviour is covered by the provider_mock test, which drives the real
// client against the mock server).
#if !defined(_WIN32)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace curia::testing {

class SilentServer {
public:
    SilentServer() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        socklen_t length = sizeof(address);
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listenFd_, 4) != 0 ||
            ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(listenFd_);
            listenFd_ = -1;
            return;
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { acceptLoop(); });
    }

    ~SilentServer() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
        if (connectionFd_ >= 0) {
            ::close(connectionFd_);
        }
        if (listenFd_ >= 0) {
            ::close(listenFd_);
        }
    }

    SilentServer(const SilentServer&) = delete;
    SilentServer& operator=(const SilentServer&) = delete;

    bool ok() const { return port_ != 0; }
    int port() const { return port_; }
    std::string endpointUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1/messages";
    }

    // True once a client has connected.
    bool waitAccepted(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return connectionFd_ >= 0; });
    }

    // Reads what the client sends until the end of its header block (a blank line) or the timeout,
    // and returns what was read (the head, possibly with the first bytes of the body).
    std::string readRequestHead(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int fd = -1;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            fd = connectionFd_;
        }
        std::string head;
        if (fd < 0) {
            return head;
        }
        char buffer[4096];
        while (head.find("\r\n\r\n") == std::string::npos) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                break;
            }
            pollfd watched{fd, POLLIN, 0};
            if (::poll(&watched, 1, static_cast<int>(left.count())) <= 0) {
                continue;
            }
            const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                break;
            }
            head.append(buffer, static_cast<std::size_t>(got));
        }
        return head;
    }

    // Reads (and discards) what the client sent until it closes the connection; true if the close
    // was seen within the timeout. This is the socket-close assertion.
    bool waitPeerClosed(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int fd = -1;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            fd = connectionFd_;
        }
        if (fd < 0) {
            return false;
        }
        char buffer[4096];
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return false;
            }
            pollfd watched{fd, POLLIN, 0};
            const int ready = ::poll(&watched, 1, static_cast<int>(left.count()));
            if (ready <= 0) {
                continue;
            }
            const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
            if (got == 0) {
                return true;
            }
            if (got < 0) {
                return true;  // a reset is a close too
            }
        }
    }

private:
    void acceptLoop() {
        while (!stop_) {
            pollfd watched{listenFd_, POLLIN, 0};
            if (::poll(&watched, 1, 50) <= 0) {
                continue;
            }
            const int fd = ::accept(listenFd_, nullptr, nullptr);
            if (fd >= 0) {
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    connectionFd_ = fd;
                }
                changed_.notify_all();
                return;
            }
        }
    }

    int listenFd_ = -1;
    int connectionFd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread thread_;
};

}  // namespace curia::testing

#endif  // !_WIN32
