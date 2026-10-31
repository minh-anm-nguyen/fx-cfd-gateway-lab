// P04's session port, wired to P06's stable-send store.
//
// This seam is not where it first looks. P04's port asks for issue-once keyed
// by command id -- send_once(), lookup_binding() -- and for a durable inbox it
// can acknowledge after its own commit. Its header says that state belongs to
// P01, "because it is P01's own store rather than P04's".
//
// The real P01 does not have it. `fixse::SessionEngine::submit_app_message()`
// returns Accepted or a refusal and no sequence number, and
// `fixse::OutboundRecord` is {seq, msg_type, payload, original_sending_time}
// with no command id anywhere, so nothing in P01 can answer "did I already
// issue this command?" after a restart. P01 owns the FIX session rules, the
// sequence numbers and the socket, which is exactly what §2's ownership row
// gives it, and nothing more.
//
// P06 has it, and wrote it for this boundary. jrnl/stable_send.hpp opens with
// the problem: "If P01's session store is a different database from P06's
// journal, there is no single transaction spanning the two, and the contract
// degrades to: P01 persists a stable (session, command_id, payload_hash) ->
// original MsgSeqNum mapping BEFORE the socket write, and delivers inbound
// receipts at least once." `jrnl::StableSendStore` is that mapping plus the
// receipt half, and §2 gives P06 the journal, the durable inbox and the outbox.
//
// So the adapter routes the issue-once question to P06 and leaves the wire to
// P01, which is the ownership table rather than a convenience. The two commits
// that result are not hidden: P06's own header calls them "exactly the hazard
// the doc describes", and they are what makes crash windows W13-W21
// reproducible instead of asserted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "deps/p01_session.hpp"
#include "jrnl/hash.hpp"
#include "jrnl/stable_send.hpp"
#include "router/failpoint.hpp"
#include "router/hash.hpp"

namespace gw {

// P04's Hash and P06's Digest are both a 32-byte SHA-256, so these are copies
// rather than conversions. They are still named functions: the types are
// distinct, and a reinterpret_cast between them would keep compiling if either
// side ever changed width.
[[nodiscard]] jrnl::Digest to_digest(const router::Hash& h);
[[nodiscard]] router::Hash to_router_hash(const jrnl::Digest& d);

class SessionAdapter final : public router::p01::SessionEngine {
public:
    struct Options {
        // P06 keys every binding and receipt by this, and P04 builds the same
        // string from its SessionIdentity. They have to agree, so the caller
        // passes one value and both sides use it.
        jrnl::SessionKey session_key;
    };

    // `image` and `fp` go to the base class, which keeps answering the state
    // accessors from them -- identity, epoch, state, wire_log -- so the parts
    // of P04 that read the image still see a coherent session. `store` is the
    // durable half and must outlive this object.
    SessionAdapter(router::p01::SessionImage& image, router::FailpointRegistry& fp,
                   jrnl::StableSendStore& store, Options options);

    // --- outbound ----------------------------------------------------------

    // Asks P06 for the binding. P06 calls the wire callback it was opened with
    // once the sequence is durable, and the caller wires that callback to P01,
    // so the ordering P06's header demands -- sequence durable, then bytes --
    // is a property of the store rather than of this adapter remembering to do
    // things in the right order.
    router::p01::StableSendResult send_once(const router::CommandId& command_id,
                                            const router::Hash& payload_hash,
                                            const std::string& app_frame) override;

    std::optional<router::p01::SessionImage::Binding> lookup_binding(
        const router::CommandId& command_id) const override;

    // --- inbound -----------------------------------------------------------

    std::optional<router::p01::DurableFixInbound> peek_inbound() const override;
    void ack_inbound_after_commit(router::DurableReceiptId id) override;
    [[nodiscard]] std::size_t pending_inbound() const override;
    router::DurableReceiptId deliver_inbound(const std::string& frame) override;
    void redeliver(router::DurableReceiptId id) override;

    // --- lifecycle ---------------------------------------------------------

    void disconnect() override;
    void reconnect(bool reset_sequences) override;

    // --- what P06 knows and P04's port has no room for ---------------------

    // Keys whose sequence is durable but whose bytes were never confirmed
    // written. After a restart these are the UNKNOWN_OUTCOME candidates, and
    // EI-34 forbids resending any of them on a timer. P04's port cannot ask the
    // question, so it is exposed here for reconciliation to ask instead.
    [[nodiscard]] std::size_t unconfirmed_sends() const;

    // JI-19's release gate: distinct sequences handed out must never exceed
    // distinct commands. A scenario asserts on this rather than trusting that
    // send_once() was called correctly everywhere.
    [[nodiscard]] std::uint64_t allocated_sequences() const;

private:
    jrnl::StableSendStore& store_;
    Options options_;
};

}  // namespace gw
