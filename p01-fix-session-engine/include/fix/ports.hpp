// Ports the session engine talks to. Keeping every side effect behind an
// interface is what makes the scenario matrix testable without a network: the
// engine is driven purely by on_connected / on_bytes / on_timer / on_disconnect
// (design doc 12).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fixse {

// Byte sink for one TCP connection. The engine owns frame ordering and retries
// partial writes itself, so this interface stays as thin as a socket.
class ITransport {
public:
    virtual ~ITransport() = default;

    // Accept as many bytes as possible for this connection generation.
    //   >= 0 : number of bytes accepted (may be fewer than offered)
    //   -1   : fatal error on this connection
    // A generation that is no longer current must be rejected with -1 so a late
    // completion cannot write into a newer connection (scenario A-15, PB-10).
    virtual long write(uint64_t generation, std::string_view bytes) = 0;
};

// Connection lifecycle driver. The engine never opens sockets itself; it asks.
class IConnectionController {
public:
    virtual ~IConnectionController() = default;
    virtual void request_connect() = 0;
    virtual void request_disconnect(uint64_t generation, std::string_view reason) = 0;
};

// Application boundary. The engine hands over application messages that are
// sequenced AND durably committed, in increasing MsgSeqNum order.
class IApplicationSink {
public:
    virtual ~IApplicationSink() = default;

    // Return true once the application has accepted the event. Returning false
    // means not yet: the engine retries from the durable inbox and never drops
    // the event. Delivery is at-least-once, so the sink must be idempotent on a
    // business key of its own (invariant I-11, scenarios G-07, L-06).
    virtual bool deliver(uint64_t seq, std::string_view msg_type, std::string_view payload) = 0;
};

}  // namespace fixse
