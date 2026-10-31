// Outbox dispatch.
//
// Section 10 draws the state machine and then says the part that matters in
// bold: a claim timing out does not prove the send never happened. The
// dispatcher is therefore built around one rule — before it calls P01 it
// durably records that it is about to, in its own committed write. After a
// crash the recorded intent is the difference between "retry this safely"
// and "this is UNKNOWN, hold it".
//
// The dispatcher runs AFTER the business commit, never inside it (section
// 9.1). It holds no transaction while talking to P01.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "jrnl/stable_send.hpp"
#include "jrnl/store.hpp"

namespace jrnl {

struct DispatchStats {
    std::size_t claimed = 0;
    std::size_t bound = 0;
    std::size_t unknown = 0;
    std::size_t held = 0;
    std::size_t skipped_not_owner = 0;
    std::size_t released_expired = 0;
};

class OutboxDispatcher {
public:
    struct Options {
        std::string owner = "dispatcher-1";
        std::size_t claim_limit = 100;
        std::int64_t lease_ms = 5000;
        // Section 10: if the stable-send contract cannot be shown to be
        // durable, a retry after an ambiguous attempt is not allowed and the
        // row goes to HELD instead. Turning this off is how a test shows the
        // difference between the safe and the unsafe policy.
        bool safe_retry_requires_stable_send = true;
    };

    OutboxDispatcher(SqliteAtomicStore& store, IStableFixSend& link, Options opt)
        : store_(store), link_(link), opt_(std::move(opt)) {}

    // One pass: expire stale claims, claim READY work, and attempt each one.
    // The fence token is checked by the store on every mutation, so a
    // dispatcher that lost ownership mid-pass stops affecting anything.
    DispatchStats dispatch_once(const TenantId& tenant, FenceToken token);

    // Enumerates the rows a human has to deal with. Section 22 makes the age
    // and count of these an alerting signal.
    std::vector<OutboxRow> unresolved(const TenantId& tenant) const;

    // Reconciliation after a restart: for every UNKNOWN row, ask P01 whether
    // a sequence was ever bound to the key. A bound sequence does not prove
    // the LP acted, so the row becomes HELD with the evidence attached, not
    // CONFIRMED (JI-20, JI-40).
    std::size_t reconcile_unknown(const TenantId& tenant, StableSendStore& link);

private:
    SqliteAtomicStore& store_;
    IStableFixSend& link_;
    Options opt_;
};

}  // namespace jrnl
