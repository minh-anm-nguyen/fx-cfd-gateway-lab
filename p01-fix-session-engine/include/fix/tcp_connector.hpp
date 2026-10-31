// TCP transport and connection controller.
//
// Socket syscalls sit behind ISocketOps so that the A group of the what-if
// matrix can inject partial writes, EAGAIN, EINTR and abrupt resets without a
// real network (scenarios A-13, A-14). Each connection attempt gets a fresh
// generation number, which is how a late callback from an abandoned socket is
// recognised and discarded (A-15, playbook PB-10).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "fix/clock.hpp"
#include "fix/config.hpp"
#include "fix/ports.hpp"

namespace fixse {

class SessionEngine;

// Return conventions are explicit rather than errno-based so a fake can honour
// them exactly.
class ISocketOps {
public:
    virtual ~ISocketOps() = default;

    // Resolve and connect. Returns a non-negative handle, or -1 with *err set.
    virtual int connect(const std::string& host, uint16_t port, int timeout_ms,
                        std::string* err) = 0;
    // Returns bytes sent (may be short), 0 when the socket would block, or -1
    // on a fatal error. An interrupted call must be retried internally, never
    // reported as a short write of zero with bytes actually sent.
    virtual long send(int handle, std::string_view bytes) = 0;
    // Returns bytes read, 0 on orderly peer shutdown, -1 on fatal error.
    // *would_block is set when there is simply nothing to read yet.
    virtual long recv(int handle, char* buf, size_t len, bool* would_block) = 0;
    // 1 readable, 0 timeout, -1 error.
    virtual int wait_readable(int handle, int timeout_ms) = 0;
    virtual void close(int handle) = 0;
};

// Real Winsock or POSIX sockets.
class RealSocketOps final : public ISocketOps {
public:
    RealSocketOps();
    ~RealSocketOps() override;
    int connect(const std::string& host, uint16_t port, int timeout_ms, std::string* err) override;
    long send(int handle, std::string_view bytes) override;
    long recv(int handle, char* buf, size_t len, bool* would_block) override;
    int wait_readable(int handle, int timeout_ms) override;
    void close(int handle) override;
};

class TcpConnector final : public ITransport, public IConnectionController {
public:
    TcpConnector(const SessionConfig& cfg, IClock& clock, ISocketOps& ops);
    ~TcpConnector() override;

    void bind_engine(SessionEngine* engine) { engine_ = engine; }

    // IConnectionController
    void request_connect() override;
    void request_disconnect(uint64_t generation, std::string_view reason) override;

    // ITransport
    long write(uint64_t generation, std::string_view bytes) override;

    // One step of the driver loop: service a pending connect, read whatever is
    // available, then run the engine timers. Returns false once the engine has
    // stopped.
    bool poll_once(int timeout_ms);

    uint64_t generation() const { return generation_; }
    bool connected() const { return handle_ >= 0; }
    const std::string& last_error() const { return last_error_; }

private:
    void do_connect();
    void drop(std::string_view reason);

    const SessionConfig& cfg_;
    IClock& clock_;
    ISocketOps& ops_;
    SessionEngine* engine_ = nullptr;
    int handle_ = -1;
    uint64_t generation_ = 0;
    bool connect_pending_ = false;
    std::string last_error_;
    std::string read_buf_;
};

}  // namespace fixse
