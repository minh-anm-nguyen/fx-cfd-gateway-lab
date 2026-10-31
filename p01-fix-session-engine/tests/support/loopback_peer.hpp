// A FIX acceptor on a real loopback socket, used for the end-to-end scenario.
//
// Its wire handling comes from peer_wire.hpp, which is written independently of
// src/codec.cpp, so a round trip through this peer exercises two separate
// implementations of BodyLength and CheckSum over a real TCP connection. It is
// not a substitute for certification against a third-party engine (design doc
// 17.3 layer 6), and the test that uses it says so.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "peer_wire.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tt {

class LoopbackPeer {
public:
    LoopbackPeer() {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }

    ~LoopbackPeer() {
        stop();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // Bind an ephemeral port on the loopback interface and start serving.
    bool start(uint16_t* port) {
        listen_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (listen_fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(static_cast<SocketType>(listen_fd_), reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0)
            return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(static_cast<SocketType>(listen_fd_),
                          reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            return false;
        *port = ntohs(addr.sin_port);
        if (::listen(static_cast<SocketType>(listen_fd_), 1) != 0) return false;
        running_ = true;
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    void stop() {
        running_ = false;
        close_socket(conn_fd_);
        close_socket(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::vector<std::string> received() {
        std::lock_guard<std::mutex> g(mu_);
        return received_;
    }
    uint64_t next_out() {
        std::lock_guard<std::mutex> g(mu_);
        return next_out_;
    }
    bool logged_on() const { return logged_on_.load(); }

    // Queue an unsolicited probe. The peer builds it so the sequence number
    // comes from its own counter.
    void queue_test_request(const std::string& id) {
        std::lock_guard<std::mutex> g(mu_);
        queued_test_requests_.push_back(id);
    }

    std::string sender = "SIMLP01";
    std::string target = "TRADER01";
    int heartbeat_seconds = 30;

    // A real counterparty persists its own outbound sequence across sessions.
    // Setting this models that, which the engine requires: a peer that restarts
    // at sequence one after we have already committed its earlier messages is a
    // sequence-too-low condition and the engine is right to refuse it (E-04).
    void set_next_out(uint64_t seq) {
        std::lock_guard<std::mutex> g(mu_);
        next_out_ = seq;
    }

private:
#ifdef _WIN32
    using SocketType = SOCKET;
#else
    using SocketType = int;
#endif

    static void close_socket(int& fd) {
        if (fd < 0) return;
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(fd));
#else
        ::close(fd);
#endif
        fd = -1;
    }

    void send_frame(const std::string& frame) {
        size_t sent = 0;
        while (sent < frame.size() && running_) {
            const long n = static_cast<long>(::send(static_cast<SocketType>(conn_fd_),
                                                    frame.data() + sent,
                                                    static_cast<int>(frame.size() - sent), 0));
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
    }

    std::string build(const std::string& type, std::vector<peer::Field> extra) {
        peer::Msg m;
        m.msg_type = type;
        m.sender = sender;
        m.target = target;
        {
            std::lock_guard<std::mutex> g(mu_);
            m.seq = next_out_++;
        }
        m.sending_time = current_timestamp();
        m.extra = std::move(extra);
        return m.build();
    }

    static std::string current_timestamp();
    void serve();

    int listen_fd_ = -1;
    int conn_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> logged_on_{false};
    std::mutex mu_;
    std::vector<std::string> received_;
    std::vector<std::string> queued_test_requests_;
    uint64_t next_out_ = 1;
};

inline std::string LoopbackPeer::current_timestamp() {
    // The real UTC clock, because this peer talks to the engine over a real
    // socket and the engine checks SendingTime against its own wall clock.
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    const std::time_t t = static_cast<std::time_t>(secs.count());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch() - secs)
                        .count();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%03d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms));
    return std::string(buf);
}

inline void LoopbackPeer::serve() {
    conn_fd_ = static_cast<int>(::accept(static_cast<SocketType>(listen_fd_), nullptr, nullptr));
    if (conn_fd_ < 0) return;

    std::string buffer;
    char chunk[4096];
    while (running_) {
        // Flush anything the test queued for us.
        {
            std::vector<std::string> ids;
            {
                std::lock_guard<std::mutex> g(mu_);
                ids.swap(queued_test_requests_);
            }
            for (const std::string& id : ids) send_frame(build("1", {{112, id}}));
        }

        const long n = static_cast<long>(
            ::recv(static_cast<SocketType>(conn_fd_), chunk, sizeof(chunk), 0));
        if (n == 0) break;
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
            break;
        }
        buffer.append(chunk, static_cast<size_t>(n));

        const std::vector<std::string> frames = peer::split_frames(buffer);
        size_t consumed = 0;
        for (const std::string& f : frames) consumed += f.size();
        buffer.erase(0, consumed);

        for (const std::string& f : frames) {
            {
                std::lock_guard<std::mutex> g(mu_);
                received_.push_back(f);
            }
            const std::string type = peer::get(f, 35).value_or("");
            if (type == "A") {
                // Answer the Logon with our own, echoing the agreed interval.
                send_frame(build("A", {{98, "0"}, {108, std::to_string(heartbeat_seconds)}}));
                logged_on_ = true;
            } else if (type == "1") {
                const std::string id = peer::get(f, 112).value_or("");
                send_frame(build("0", {{112, id}}));
            } else if (type == "5") {
                send_frame(build("5", {{58, "logout acknowledged"}}));
                break;
            }
        }
    }
    close_socket(conn_fd_);
}

}  // namespace tt

