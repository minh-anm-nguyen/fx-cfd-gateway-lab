// LP simulator: the FIX 4.4 acceptor process.
//
// Reference: design doc 13.2 (one simulator process) and 12.2 (safe lab
// design). The listener binds loopback by default and refuses to start with the
// unsafe capabilities enabled on any other address (L-05, L-06).
//
// Scope note: this is a single-session acceptor. It serves one authorised
// counterparty at a time, which is the v1 boundary stated in the design doc
// header; a second simultaneous connection for the same identity is closed
// rather than served (A-08).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

#include "lp/config.hpp"
#include "lp/failpoint.hpp"
#include "lp/metrics.hpp"
#include "lp/order_actor.hpp"
#include "lp/session.hpp"
#include "lp/sqlite_store.hpp"

using namespace lpsim;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

class SocketTransport final : public ITransport {
public:
    void attach(socket_t s) {
        fd_ = s;
        connected_ = s != kInvalidSocket;
    }

    std::size_t write(std::string_view bytes) override {
        if (!connected_) return 0;
        std::size_t sent = 0;
        while (sent < bytes.size()) {
#ifdef _WIN32
            const int n = ::send(fd_, bytes.data() + sent,
                                 static_cast<int>(bytes.size() - sent), 0);
#else
            const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, 0);
#endif
            if (n <= 0) {
                // A short write is reported honestly. The caller must not treat
                // the message as delivered (W7).
                return sent;
            }
            sent += static_cast<std::size_t>(n);
        }
        return sent;
    }

    void close() override {
        if (fd_ == kInvalidSocket) return;
#ifdef _WIN32
        ::closesocket(fd_);
#else
        ::close(fd_);
#endif
        fd_ = kInvalidSocket;
        connected_ = false;
    }

    bool connected() const override { return connected_; }
    socket_t fd() const { return fd_; }

private:
    socket_t fd_ = kInvalidSocket;
    bool connected_ = false;
};

bool set_nonblocking(socket_t s) {
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    return flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void usage() {
    std::printf(
        "usage: lp_simulator --config <file> [--db <path>] [--port <n>]\n"
        "                    [--metrics <file>]\n"
        "\n"
        "Binds loopback by default. Adversarial mode and wire faults require a\n"
        "loopback bind and are refused otherwise.\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::string db_override;
    std::string metrics_path;
    int port_override = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) db_override = argv[++i];
        else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port_override = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--metrics") == 0 && i + 1 < argc)
            metrics_path = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (config_path.empty()) {
        usage();
        return 2;
    }

    fp::arm_from_env();

    // --- configuration -----------------------------------------------------
    ConfLimits limits;
    ConfError cerr;
    std::string text;
    if (!read_conf_file(config_path, limits, &text, &cerr)) {
        std::fprintf(stderr, "config: %s\n", cerr.message.c_str());
        return 2;
    }
    ConfNode doc;
    if (!parse_conf(text, limits, &doc, &cerr)) {
        std::fprintf(stderr, "config line %d: %s\n", cerr.line, cerr.message.c_str());
        return 2;
    }
    SimulatorConfig cfg;
    if (!build_simulator_config(doc, &cfg, &cerr)) {
        std::fprintf(stderr, "config: %s\n", cerr.message.c_str());
        return 2;
    }
    if (!db_override.empty()) cfg.store.path = db_override;
    if (port_override != 0) cfg.listen_port = port_override;
    // Re-validate after the overrides: a command-line port must not be able to
    // move an adversarial instance off loopback.
    if (!validate_simulator_config(cfg, &cerr)) {
        std::fprintf(stderr, "config: %s\n", cerr.message.c_str());
        return 2;
    }

    // --- store and actor ---------------------------------------------------
    SqliteStore store;
    const StoreError se = store.open(cfg.store);
    if (se != StoreError::None) {
        std::fprintf(stderr, "store: %s\n", std::string(to_string(se)).c_str());
        return 3;
    }

    VirtualClock dummy_clock;  // unused in wall mode, kept for symmetry
    (void)dummy_clock;
    SystemClock clock;
    QuoteBook book;
    Scheduler sched(cfg.max_scheduled_events);
    OrderActor actor(cfg.actor, store, book, sched, clock);
    if (!actor.recover()) {
        std::fprintf(stderr, "HALTED on recovery: %s (%s)\n",
                     std::string(to_string(actor.halt_reason())).c_str(),
                     actor.halt_detail().c_str());
        return 4;
    }

    SocketTransport transport;
    AcceptorSession session(cfg.session, store, actor, clock, transport);
    if (!session.recover()) {
        std::fprintf(stderr, "HALTED on session recovery: %s (%s)\n",
                     std::string(to_string(session.halt_reason())).c_str(),
                     session.halt_detail().c_str());
        return 4;
    }

    // --- listener ----------------------------------------------------------
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 5;
    }
#endif
    const socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidSocket) {
        std::fprintf(stderr, "socket() failed\n");
        return 5;
    }
    {
        int yes = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                     sizeof(yes));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(cfg.listen_port));
    if (::inet_pton(AF_INET, cfg.listen_host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "listen host is not an IPv4 literal: %s\n",
                     cfg.listen_host.c_str());
        return 2;
    }
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "bind %s:%d failed\n", cfg.listen_host.c_str(), cfg.listen_port);
        return 5;
    }
    // Backlog 1: this profile serves one session, and a deeper queue would only
    // hide that a second peer is being refused (A-08).
    if (::listen(listener, 1) != 0) {
        std::fprintf(stderr, "listen() failed\n");
        return 5;
    }
    set_nonblocking(listener);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("lp_simulator %s listening on %s:%d (mode=%s, fill_trigger=%s)\n",
                cfg.instance_id.c_str(), cfg.listen_host.c_str(), cfg.listen_port,
                std::string(to_string(cfg.mode)).c_str(),
                std::string(to_string(cfg.actor.fill_trigger)).c_str());
    std::printf("session next_in=%llu next_out=%llu epoch=%llu\n",
                static_cast<unsigned long long>(session.next_in()),
                static_cast<unsigned long long>(session.next_out()),
                static_cast<unsigned long long>(session.epoch()));

    Metrics metrics;
    std::uint64_t generation = 0;
    char buf[8192];

    while (!g_stop.load()) {
        if (!transport.connected()) {
            sockaddr_in peer{};
#ifdef _WIN32
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            const socket_t s = ::accept(listener, reinterpret_cast<sockaddr*>(&peer),
                                        &peer_len);
            if (s == kInvalidSocket) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            {
                int yes = 1;
                ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                             reinterpret_cast<const char*>(&yes), sizeof(yes));
            }
            set_nonblocking(s);
            transport.attach(s);
            ++generation;
            session.on_connected(generation);
            metrics.sessions_active = 1;
            std::printf("connection %llu accepted\n",
                        static_cast<unsigned long long>(generation));
            continue;
        }

#ifdef _WIN32
        const int n = ::recv(transport.fd(), buf, static_cast<int>(sizeof(buf)), 0);
        const bool would_block = (n < 0) && (WSAGetLastError() == WSAEWOULDBLOCK);
#else
        const ssize_t n = ::recv(transport.fd(), buf, sizeof(buf), 0);
        const bool would_block = (n < 0) && (errno == EAGAIN || errno == EWOULDBLOCK);
#endif
        if (n > 0) {
            session.on_bytes(std::string_view(buf, static_cast<std::size_t>(n)), generation);
        } else if (n == 0 || (!would_block)) {
            std::printf("connection %llu closed\n",
                        static_cast<unsigned long long>(generation));
            transport.close();
            session.on_disconnect(generation);
            metrics.sessions_active = 0;
        }

        session.on_timer();
        session.dispatch_outbox();

        if (session.phase() == SessionPhase::Halted) {
            std::fprintf(stderr, "HALTED: %s (%s)\n",
                         std::string(to_string(session.halt_reason())).c_str(),
                         session.halt_detail().c_str());
            break;
        }
        if (actor.state() == ActorState::Halted) {
            std::fprintf(stderr, "HALTED: %s (%s)\n",
                         std::string(to_string(actor.halt_reason())).c_str(),
                         actor.halt_detail().c_str());
            break;
        }

        if (would_block) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    metrics.live_orders = actor.live_order_count();
    metrics.fills_total = actor.fill_count();
    metrics.outbox_pending = store.count_outbox_pending();
    if (!metrics_path.empty()) {
        std::FILE* f = std::fopen(metrics_path.c_str(), "wb");
        if (f != nullptr) {
            const std::string text_out = metrics.render();
            std::fwrite(text_out.data(), 1, text_out.size(), f);
            std::fclose(f);
        }
    }

    transport.close();
#ifdef _WIN32
    ::closesocket(listener);
    WSACleanup();
#else
    ::close(listener);
#endif
    store.close();
    std::printf("lp_simulator stopped (fills=%zu live_orders=%zu outbox_pending=%zu)\n",
                actor.fill_count(), actor.live_order_count(),
                static_cast<std::size_t>(metrics.outbox_pending));
    return 0;
}
