// The P04 -> P01/P06 seam.
//
// P04's session port asks for two things P01 does not have: a sequence number
// bound to a command id, and a durable inbox it can acknowledge after its own
// commit. P06 has both, in `jrnl::StableSendStore`, written for this boundary.
// So this seam is a three-way one, and these cases are about the part that
// decides whether an order can be sent twice.
//
// The restart case is the one that matters. P04 reads `Binding::written` to
// choose between UnknownTransport and AcceptedDurable, and AcceptedDurable
// means "still safe to dispatch". P06 cannot say per key whether the bytes were
// confirmed, so the adapter answers `written = true` and the order is held for
// reconciliation rather than sent a second time (EI-09, EI-34).
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "deps/p01_session.hpp"
#include "gw/session_adapter.hpp"
#include "jrnl/stable_send.hpp"
#include "router/failpoint.hpp"
#include "router/hash.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* expr, int line, const std::string& detail) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("  FAIL line %d  %s%s%s\n", line, expr, detail.empty() ? "" : " -- ",
                detail.c_str());
}

#define CHECK(expr) check((expr), #expr, __LINE__, {})
#define CHECK_MSG(expr, detail) check((expr), #expr, __LINE__, (detail))

using State = router::p01::StableSendResult::State;

constexpr const char* kSession = "ROUTER04:LPSIM01:PRIMARY";

// One lab: a store on a scratch file, plus the wire callback P06 invokes once a
// sequence is durable. In the assembled gateway that callback is P01's
// submit_app_message; here it records, so a case can tell whether bytes were
// offered at all.
struct Lab {
    std::filesystem::path dir;
    std::unique_ptr<jrnl::StableSendStore> store;
    std::vector<std::pair<std::uint64_t, std::string>> wire;
    bool wire_succeeds = true;

    router::p01::SessionImage image;
    router::FailpointRegistry fp;
    std::unique_ptr<gw::SessionAdapter> adapter;

    explicit Lab(const std::string& name) {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path(ec) / ("p08-session-seam-" + name);
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        open();
    }

    ~Lab() {
        adapter.reset();
        store.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // Opens the store and attaches a fresh adapter over it. Kept out of the
    // constructor because the restart case needs it again: the file outlives
    // both objects, which is the whole subject of this test.
    void open() {
        jrnl::StableSendStore::Options o;
        o.path = (dir / "stable_send.db").string();
        o.wire = [this](const jrnl::SessionKey&, std::uint64_t seq, const std::string& payload) {
            wire.emplace_back(seq, payload);
            return wire_succeeds;
        };
        std::string why;
        const jrnl::StoreError e = jrnl::StableSendStore::open(o, &store, &why);
        CHECK_MSG(e == jrnl::StoreError::None, why);
        if (e != jrnl::StoreError::None) return;

        image.identity = {"ROUTER04", "LPSIM01", "PRIMARY"};
        image.state = router::p01::SessionState::Active;
        adapter = std::make_unique<gw::SessionAdapter>(image, fp, *store,
                                                       gw::SessionAdapter::Options{kSession});
    }

    void restart() {
        adapter.reset();
        store.reset();
        wire.clear();
        image = router::p01::SessionImage{};
        open();
    }
};

router::Hash hash_of(const std::string& s) { return gw::to_router_hash(jrnl::sha256(s)); }

// ---------------------------------------------------------------------------

void a_command_gets_one_sequence_however_often_it_is_sent() {
    std::printf("a_command_gets_one_sequence_however_often_it_is_sent\n");
    Lab lab("issue-once");
    if (!lab.adapter) return;

    const router::CommandId cmd("C-1");
    const std::string frame = "35=D|11=O-1|55=EUR/USD|";

    const router::p01::StableSendResult first = lab.adapter->send_once(cmd, hash_of(frame), frame);
    CHECK(first.state == State::AssignedOriginalSeq);
    CHECK(first.original_seq.has_value());

    // The retry. P04 cannot tell whether the first attempt reached anyone, so
    // it asks again -- and must get the same number back, never a second one
    // (JI-19, EI-09).
    const router::p01::StableSendResult again = lab.adapter->send_once(cmd, hash_of(frame), frame);
    CHECK(again.state == State::AlreadyAssigned);
    CHECK(again.original_seq == first.original_seq);

    // The release gate: sequences handed out never exceed distinct commands.
    CHECK(lab.adapter->allocated_sequences() == 1);
}

void the_same_command_with_different_bytes_is_fatal() {
    std::printf("the_same_command_with_different_bytes_is_fatal\n");
    Lab lab("conflict");
    if (!lab.adapter) return;

    const router::CommandId cmd("C-2");
    const std::string first = "35=D|11=O-1|38=10000|";
    const std::string second = "35=D|11=O-1|38=99000|";

    CHECK(lab.adapter->send_once(cmd, hash_of(first), first).state == State::AssignedOriginalSeq);

    // One accepted command id yields one original intent, so two payloads
    // cannot both be it. A defect to surface, not a retry to absorb.
    CHECK(lab.adapter->send_once(cmd, hash_of(second), second).state == State::Fatal);
    CHECK(lab.adapter->allocated_sequences() == 1);
}

void a_down_session_binds_nothing() {
    std::printf("a_down_session_binds_nothing\n");
    Lab lab("not-ready");
    if (!lab.adapter) return;

    lab.adapter->disconnect();
    CHECK(!lab.adapter->ready_for_app());

    const router::CommandId cmd("C-3");
    const std::string frame = "35=D|11=O-1|";
    const router::p01::StableSendResult r = lab.adapter->send_once(cmd, hash_of(frame), frame);

    // NOT_READY has to mean nothing was bound: an original sequence spent on a
    // message that cannot leave is a hole in the outbound numbering.
    CHECK(r.state == State::NotReady);
    CHECK(!r.original_seq.has_value());
    CHECK(lab.adapter->allocated_sequences() == 0);
    CHECK(!lab.adapter->lookup_binding(cmd).has_value());
    CHECK(lab.wire.empty());
}

void after_a_restart_a_bound_command_is_unknown_not_resendable() {
    std::printf("after_a_restart_a_bound_command_is_unknown_not_resendable\n");
    Lab lab("restart");
    if (!lab.adapter) return;

    const router::CommandId cmd("C-4");
    const std::string frame = "35=D|11=O-1|38=10000|";
    const router::p01::StableSendResult sent = lab.adapter->send_once(cmd, hash_of(frame), frame);
    CHECK(sent.state == State::AssignedOriginalSeq);
    CHECK(lab.wire.size() == 1);

    // The gateway dies and comes back. The adapter and its in-memory image are
    // gone; the store is not.
    lab.restart();
    if (!lab.adapter) return;

    const std::optional<router::p01::SessionImage::Binding> b = lab.adapter->lookup_binding(cmd);
    CHECK(b.has_value());
    if (b.has_value()) {
        CHECK(b->original_seq == sent.original_seq);
        // The load-bearing assertion. P04 maps written=false to
        // AcceptedDurable, which is "go ahead and dispatch". P06 cannot prove
        // this key's bytes were confirmed, so the adapter says written=true and
        // P04 holds it as UnknownTransport for reconciliation instead.
        CHECK(b->written);
    }

    // And asking to send it again still yields the same sequence.
    const router::p01::StableSendResult retry = lab.adapter->send_once(cmd, hash_of(frame), frame);
    CHECK(retry.state == State::AlreadyAssigned);
    CHECK(retry.original_seq == sent.original_seq);
    CHECK(lab.adapter->allocated_sequences() == 1);
}

void an_inbound_receipt_is_redelivered_until_it_is_acknowledged() {
    std::printf("an_inbound_receipt_is_redelivered_until_it_is_acknowledged\n");
    Lab lab("inbound");
    if (!lab.adapter) return;

    const std::string report = "35=8|17=E-1|39=0|";
    const router::DurableReceiptId id = lab.adapter->deliver_inbound(report);
    CHECK(id != 0);
    CHECK(lab.adapter->pending_inbound() == 1);

    const std::optional<router::p01::DurableFixInbound> first = lab.adapter->peek_inbound();
    CHECK(first.has_value());
    if (first.has_value()) {
        CHECK(first->durable_inbox_id == id);
        CHECK(first->frame == report);
        CHECK(first->raw_sha256 == hash_of(report));
    }

    // Peeked but not acknowledged, so it comes back. That is what at-least-once
    // means, and P04's port says the same: nothing is removed until the
    // consumer acknowledges after its own commit.
    const std::optional<router::p01::DurableFixInbound> again = lab.adapter->peek_inbound();
    CHECK(again.has_value());
    if (again.has_value()) CHECK(again->durable_inbox_id == id);

    lab.adapter->ack_inbound_after_commit(id);
    CHECK(lab.adapter->pending_inbound() == 0);
    CHECK(!lab.adapter->peek_inbound().has_value());
}

void an_acknowledged_receipt_id_is_never_handed_out_twice() {
    std::printf("an_acknowledged_receipt_id_is_never_handed_out_twice\n");
    Lab lab("receipt-ids");
    if (!lab.adapter) return;

    const router::DurableReceiptId first = lab.adapter->deliver_inbound("35=8|17=E-1|");
    lab.adapter->ack_inbound_after_commit(first);

    // A restart has to resume the counter past the acknowledged row, which is
    // invisible through undelivered(). Reusing the id would make persist_raw's
    // ON CONFLICT DO NOTHING swallow the next message, and an accepted event
    // would simply stop existing (EI-45).
    lab.restart();
    if (!lab.adapter) return;

    const router::DurableReceiptId second = lab.adapter->deliver_inbound("35=8|17=E-2|");
    CHECK(second != 0);
    CHECK(second != first);
    CHECK(lab.adapter->pending_inbound() == 1);

    const std::optional<router::p01::DurableFixInbound> peeked = lab.adapter->peek_inbound();
    CHECK(peeked.has_value());
    if (peeked.has_value()) {
        CHECK(peeked->durable_inbox_id == second);
        CHECK(peeked->frame == "35=8|17=E-2|");
    }
}

}  // namespace

int main() {
    std::printf("P08 seam: P04 session port -> P06 stable send + durable inbox\n\n");

    a_command_gets_one_sequence_however_often_it_is_sent();
    the_same_command_with_different_bytes_is_fatal();
    a_down_session_binds_nothing();
    after_a_restart_a_bound_command_is_unknown_not_resendable();
    an_inbound_receipt_is_redelivered_until_it_is_acknowledged();
    an_acknowledged_receipt_id_is_never_handed_out_twice();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
