#include "fix/tcp_connector.hpp"

#include <cstring>

#include "fix/session_actor.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define FIXSE_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define FIXSE_INVALID_SOCKET (-1)
#endif

namespace fixse {
namespace {

#ifdef _WIN32
bool would_block_error() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
void set_nonblocking(socket_t s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}
void close_socket(socket_t s) { closesocket(s); }
#else
bool would_block_error() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
void set_nonblocking(socket_t s) {
    const int flags = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
}
void close_socket(socket_t s) { ::close(s); }
#endif

}  // namespace

RealSocketOps::RealSocketOps() {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

RealSocketOps::~RealSocketOps() {
#ifdef _WIN32
    WSACleanup();
#endif
}

int RealSocketOps::connect(const std::string& host, uint16_t port, int timeout_ms,
                           std::string* err) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;  // both IPv4 and IPv6 candidates (A-18)
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    const std::string port_text = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &list);
    if (rc != 0 || list == nullptr) {
        if (err != nullptr) *err = "name resolution failed for " + host;
        return -1;
    }

    int result = -1;
    for (addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
        socket_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == FIXSE_INVALID_SOCKET) continue;
        set_nonblocking(s);
        int one = 1;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                     sizeof(one));

        int cr = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (cr != 0 && would_block_error()) {
            // Bounded wait: a blackholed SYN must not hang the engine (A-05).
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(s, &wset);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            const int sel = ::select(static_cast<int>(s) + 1, nullptr, &wset, nullptr, &tv);
            if (sel <= 0) {
                close_socket(s);
                if (err != nullptr) *err = "connect timed out";
                continue;
            }
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
            if (so_error != 0) {
                close_socket(s);
                if (err != nullptr) *err = "connect refused or failed";
                continue;
            }
        } else if (cr != 0) {
            close_socket(s);
            if (err != nullptr) *err = "connect failed";
            continue;
        }
        result = static_cast<int>(s);
        break;
    }
    ::freeaddrinfo(list);
    return result;
}

long RealSocketOps::send(int handle, std::string_view bytes) {
    for (;;) {
        const long n = static_cast<long>(
            ::send(static_cast<socket_t>(handle), bytes.data(),
                   static_cast<int>(bytes.size()), 0));
        if (n >= 0) return n;
#ifndef _WIN32
        if (errno == EINTR) continue;  // retry, never report a phantom failure
#endif
        return would_block_error() ? 0 : -1;
    }
}

long RealSocketOps::recv(int handle, char* buf, size_t len, bool* would_block) {
    *would_block = false;
    for (;;) {
        const long n = static_cast<long>(
            ::recv(static_cast<socket_t>(handle), buf, static_cast<int>(len), 0));
        if (n > 0) return n;
        if (n == 0) return 0;  // orderly shutdown
#ifndef _WIN32
        if (errno == EINTR) continue;
#endif
        if (would_block_error()) {
            *would_block = true;
            return 0;
        }
        return -1;
    }
}

int RealSocketOps::wait_readable(int handle, int timeout_ms) {
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(static_cast<socket_t>(handle), &rset);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return ::select(handle + 1, &rset, nullptr, nullptr, &tv);
}

void RealSocketOps::close(int handle) { close_socket(static_cast<socket_t>(handle)); }

TcpConnector::TcpConnector(const SessionConfig& cfg, IClock& clock, ISocketOps& ops)
    : cfg_(cfg), clock_(clock), ops_(ops) {
    read_buf_.resize(16384);
}

TcpConnector::~TcpConnector() {
    if (handle_ >= 0) ops_.close(handle_);
}

void TcpConnector::request_connect() { connect_pending_ = true; }

void TcpConnector::request_disconnect(uint64_t generation, std::string_view reason) {
    // A disconnect for a generation that is no longer current is ignored: the
    // engine may be tidying up after a socket this connector already replaced.
    if (generation != generation_ || handle_ < 0) return;
    drop(reason);
}

void TcpConnector::drop(std::string_view reason) {
    if (handle_ >= 0) {
        ops_.close(handle_);
        handle_ = -1;
    }
    if (engine_ != nullptr) engine_->on_disconnect(generation_, reason);
}

long TcpConnector::write(uint64_t generation, std::string_view bytes) {
    // Writing for an abandoned generation would put bytes from an old session
    // state onto a new connection (playbook PB-10).
    if (generation != generation_ || handle_ < 0) return -1;
    return ops_.send(handle_, bytes);
}

void TcpConnector::do_connect() {
    connect_pending_ = false;
    ++generation_;
    last_error_.clear();
    const int h = ops_.connect(cfg_.host, cfg_.port, cfg_.connect_timeout_ms, &last_error_);
    if (h < 0) {
        // The engine owns the backoff ladder, so a failed connect is reported
        // as a disconnect for this generation rather than retried here.
        if (engine_ != nullptr) engine_->on_disconnect(generation_, last_error_);
        return;
    }
    handle_ = h;
    if (engine_ != nullptr) engine_->on_connected(generation_);
}

bool TcpConnector::poll_once(int timeout_ms) {
    if (engine_ == nullptr) return false;

    // A stopped or faulted engine gets no sockets. Opening one anyway would
    // leak a descriptor and could put bytes on the wire from state the engine
    // no longer trusts (design doc 6).
    const SessionState engine_state = engine_->state();
    if (engine_state == SessionState::Stopped || engine_state == SessionState::Faulted) {
        connect_pending_ = false;
        if (handle_ >= 0) {
            ops_.close(handle_);
            handle_ = -1;
        }
        return false;
    }

    if (connect_pending_ && handle_ < 0) do_connect();

    if (handle_ >= 0) {
        const int ready = ops_.wait_readable(handle_, timeout_ms);
        if (ready < 0) {
            drop("select failed");
        } else if (ready > 0) {
            bool would_block = false;
            const long n = ops_.recv(handle_, read_buf_.data(), read_buf_.size(), &would_block);
            if (n > 0) {
                engine_->on_bytes(std::string_view(read_buf_.data(), static_cast<size_t>(n)),
                                  generation_);
            } else if (!would_block) {
                // Zero means the peer sent FIN; negative means a reset. Either
                // way the session is gone and a partially received frame is
                // discarded rather than guessed at (A-07, A-08, A-09).
                drop(n == 0 ? "peer closed the connection" : "socket error");
            }
        }
    }

    engine_->on_timer(clock_.steady_now());

    // The timer step is where the backoff expires and a reconnect is requested;
    // servicing it here keeps the reconnect from waiting for the next poll.
    if (connect_pending_ && handle_ < 0 && engine_->state() != SessionState::Stopped &&
        engine_->state() != SessionState::Faulted)
        do_connect();

    return engine_->state() != SessionState::Stopped;
}

}  // namespace fixse
