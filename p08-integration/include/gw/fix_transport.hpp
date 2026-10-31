// The bridge between P04's frames and P01's session.
//
// This is the last thing standing between the assembled gateway and the real
// LP: until now the "wire" was P04's in-tree simulator in the same process. P01
// owns a real TCP session (`fixse::TcpConnector` over `RealSocketOps`) and P02
// ships a real FIX 4.4 acceptor, and the two have already been shown to
// complete a session over loopback. What was missing is the translation in
// between, and it is not a pass-through in either direction.
//
// Outbound. P04 emits an application frame carrying 35 and the business tags
// only -- 11, 1, 55, 54, 60, 38, 40, 44, 59 for a New -- and no session tags at
// all. `fixse::SessionEngine::submit_app_message(msg_type, body)` wants exactly
// that shape, with 35 lifted out, because P01 owns 8, 9, 34, 49, 52, 56 and 10
// and will not accept a caller supplying them.
//
// Inbound. P01 hands the sink the whole raw frame, session tags included.
// P04's `parse_app_frame` refuses it: its rules are "35 first, no unknown
// must-understand tags", and a raw frame begins 8=FIX.4.4, 9=..., 35=... So the
// envelope has to come off and 35 has to move to the front.
//
// Which tags come off is a decision, not a formality. 43 (PossDupFlag) and 97
// (PossResend) look like session fields and are kept, because P04's
// NormalizedReport has a field for each and EI-03 turns on telling a session
// retransmission apart from a business retry. Dropping them would erase that
// distinction before the router could make it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fix/ports.hpp"
#include "fix/session_actor.hpp"
#include "gw/session_adapter.hpp"

namespace gw {

// The FIX 4.4 standard header and trailer tags P01 owns, in the sense that it
// writes them outbound and validates them inbound. Anything here is removed
// before a frame reaches P04.
//
// 43 and 97 are deliberately absent: see the note at the top of this file.
[[nodiscard]] bool is_session_tag(int tag);

// Raw inbound frame to something `router::fix::parse_app_frame` will accept:
// session envelope removed, 35 first, remaining fields in their original order.
//
// Returns nullopt when the frame has no 35 at all, which is not a message this
// bridge can describe and must not be guessed at.
[[nodiscard]] std::optional<std::string> strip_session_envelope(std::string_view raw);

// P04's application frame to the body `submit_app_message` takes, with 35
// returned separately. Session tags are dropped here too: if one ever appears
// in a P04 frame it is a defect on that side, and passing it to P01 would have
// P01 reject the whole message for a reason reported nowhere useful.
struct SubmitBody {
    std::string msg_type;
    std::vector<std::pair<int, std::string>> fields;
};
[[nodiscard]] std::optional<SubmitBody> to_submit_body(std::string_view app_frame);

// Routes P01's inbound application messages into P06's durable receipt store,
// which is where P04 reads them from.
//
// Returning false tells P01 "not yet" and it redelivers from its own durable
// inbox, so true must mean the receipt is durable somewhere. It does:
// `SessionAdapter::deliver_inbound` persists it to P06 first. P04 then
// acknowledges after its own commit, which is the second half of the
// at-least-once chain and the reason the two halves are kept apart.
class FixSink final : public fixse::IApplicationSink {
public:
    explicit FixSink(SessionAdapter& adapter) : adapter_(adapter) {}

    bool deliver(std::uint64_t seq, std::string_view msg_type, std::string_view payload) override;

    [[nodiscard]] std::uint64_t delivered() const { return delivered_; }
    // Frames P01 offered that could not be put into P04's shape. Not dropped
    // silently: a refused frame is left unacknowledged in P01's inbox and
    // counted here, so a dictionary mismatch surfaces as a stuck session
    // rather than as missing orders.
    [[nodiscard]] std::uint64_t unconvertible() const { return unconvertible_; }

private:
    SessionAdapter& adapter_;
    std::uint64_t delivered_ = 0;
    std::uint64_t unconvertible_ = 0;
};

// The outbound half, shaped as the callback `jrnl::StableSendStore` invokes
// once a sequence is durable. Returning false is a send failure, which is what
// leaves the binding unconfirmed and the outcome unknown.
[[nodiscard]] bool send_to_session(fixse::SessionEngine& engine, const std::string& app_frame);

}  // namespace gw
