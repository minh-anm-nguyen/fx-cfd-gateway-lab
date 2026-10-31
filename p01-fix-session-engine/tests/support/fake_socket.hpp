// Scripted socket layer for the A group of the what-if matrix.
//
// Everything the engine can observe about a socket is scriptable here: a
// refused connect, a connect that times out, a short write, a would-block
// write, an orderly FIN, an abrupt reset. The handle count is tracked so a
// descriptor leak across reconnect cycles shows up as a number (scenario
// I-11).
#pragma once

#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "fix/tcp_connector.hpp"
#include "peer_wire.hpp"

namespace tt {

class FakeSocketOps final : public fixse::ISocketOps {
public:
    enum class ConnectOutcome { Success, Refused, Timeout };

    // Queued connect outcomes; when the queue empties the default applies.
    std::deque<ConnectOutcome> connect_script;
    ConnectOutcome default_connect = ConnectOutcome::Success;

    // Bytes the peer will deliver, in order. An empty chunk means an orderly
    // FIN, and a chunk equal to kReset means an abrupt reset.
    static const std::string kReset;
    std::deque<std::string> recv_script;

    // Write behaviour.
    size_t max_bytes_per_send = static_cast<size_t>(-1);
    int would_block_sends = 0;  // next n sends report "would block"
    int failing_sends = 0;      // next n sends report a fatal error

    // Observability.
    std::string sent;
    int connect_attempts = 0;
    int open_handles = 0;
    int peak_open_handles = 0;
    int closes = 0;
    std::vector<std::string> connect_errors;

    int connect(const std::string&, uint16_t, int, std::string* err) override {
        ++connect_attempts;
        ConnectOutcome outcome = default_connect;
        if (!connect_script.empty()) {
            outcome = connect_script.front();
            connect_script.pop_front();
        }
        if (outcome != ConnectOutcome::Success) {
            const std::string text =
                outcome == ConnectOutcome::Refused ? "connection refused" : "connect timed out";
            if (err != nullptr) *err = text;
            connect_errors.push_back(text);
            return -1;
        }
        ++open_handles;
        if (open_handles > peak_open_handles) peak_open_handles = open_handles;
        return next_handle_++;
    }

    long send(int, std::string_view bytes) override {
        if (failing_sends > 0) {
            --failing_sends;
            return -1;
        }
        if (would_block_sends > 0) {
            --would_block_sends;
            return 0;
        }
        const size_t n = std::min(bytes.size(), max_bytes_per_send);
        sent.append(bytes.substr(0, n));
        return static_cast<long>(n);
    }

    long recv(int, char* buf, size_t len, bool* would_block) override {
        *would_block = false;
        if (recv_script.empty()) {
            *would_block = true;
            return 0;
        }
        std::string& front = recv_script.front();
        if (front.empty()) {
            recv_script.pop_front();
            return 0;  // orderly shutdown
        }
        if (front == kReset) {
            recv_script.pop_front();
            return -1;  // abrupt reset
        }
        const size_t n = std::min(len, front.size());
        std::memcpy(buf, front.data(), n);
        front.erase(0, n);
        if (front.empty()) recv_script.pop_front();
        return static_cast<long>(n);
    }

    int wait_readable(int, int) override { return recv_script.empty() ? 0 : 1; }

    void close(int) override {
        ++closes;
        if (open_handles > 0) --open_handles;
    }

    void deliver(const std::string& bytes) { recv_script.push_back(bytes); }
    void deliver_fin() { recv_script.push_back(std::string()); }
    void deliver_reset() { recv_script.push_back(kReset); }
    std::vector<std::string> sent_frames() const { return peer::split_frames(sent); }

private:
    int next_handle_ = 10;
};

inline const std::string FakeSocketOps::kReset = "<<RESET>>";

}  // namespace tt
