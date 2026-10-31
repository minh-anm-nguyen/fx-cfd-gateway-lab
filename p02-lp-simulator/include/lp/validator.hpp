// FIX 4.4 application-message validation for 35=D, 35=F and 35=G.
//
// Reference: design doc 7.2 and the whole of matrix group B.
//
// Division of labour, which design doc 7.2 insists on keeping explicit:
//
//   * malformed FRAMES and bad standard headers never reach here. The framer
//     and the codec have already refused them, and the session layer answers
//     with 35=3 or closes the connection;
//   * a message that is well formed at the FIX layer but illegal for this
//     counterparty profile is refused HERE, and the answer is an application
//     message: 35=8 with ExecType=8 for a New, 35=9 for a Cancel or Replace;
//   * an unsupported application MsgType is answered with 35=j, not with a
//     session-level reject (B-17).
//
// The validator is pure: it reads a decoded message and produces either a typed
// command or a named refusal. It touches no order state, which is what lets
// every B-group case assert that the ledger is byte-identical after a reject.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lp/codec.hpp"
#include "lp/domain.hpp"

namespace lpsim {

struct ValidatorPolicy {
    // TransactTime(60) tolerance. Both bounds are pinned so that C-17 and C-18
    // have a deterministic expected answer rather than a clock-dependent one.
    std::int64_t transact_time_past_tolerance_s = 120;
    std::int64_t transact_time_future_tolerance_s = 60;

    // Business bound on identifier length, below the wire bound enforced by the
    // codec. A 10 MB ClOrdID is stopped at the wire; a 200-character one is
    // stopped here (B-19).
    std::size_t max_cl_ord_id_len = 64;

    // When non-empty, Account(1) must be present and in the list (C-16).
    std::vector<std::string> allowed_accounts;
    bool require_account = false;
};

// A validated application request. Identity fields are populated on a best
// effort basis EVEN WHEN validation fails, so that a reject report can echo the
// ClOrdID and OrigClOrdID the client sent. FIX 4.4 requires 11 and 41 on a
// cancel reject (design doc 7.3), and a reject that omits them is unusable for
// reconciliation.
struct AppCommand {
    RequestType type = RequestType::New;
    InboundIdentity inbound;

    BusinessKey key;                 // firm / business_day / ClOrdID(11)
    std::string orig_cl_ord_id;      // OrigClOrdID(41), for F and G
    std::string account;             // Account(1)
    std::string symbol;              // Symbol(55)
    Side side = Side::Buy;
    OrdType ord_type = OrdType::Limit;
    Tif tif = Tif::Day;
    Units qty = 0;
    Ticks limit_ticks = 0;
    WallTime transact_time{};

    // Stable hash of the semantic payload. Two requests that share a business
    // key are the same request when the hashes match and a conflict when they
    // do not, which is exactly the C-02..C-06 distinction.
    std::uint64_t canonical_hash = 0;

    // Which optional identity fields survived parsing, so the report builder
    // knows whether it may echo them.
    bool has_cl_ord_id = false;
    bool has_orig_cl_ord_id = false;
    bool has_symbol = false;
    bool has_side = false;
    bool has_qty = false;
};

class AppValidator {
public:
    AppValidator(std::vector<SymbolProfile> symbols, ValidatorPolicy policy)
        : symbols_(std::move(symbols)), policy_(std::move(policy)) {}

    // Validate a decoded application message. Returns BizReject::None when the
    // command is well formed for this profile. On refusal *out still carries
    // whatever identity could be recovered.
    BizReject validate(const DecodedMessage& m, const InboundIdentity& id,
                       std::string_view firm, std::string_view business_day, WallTime now,
                       AppCommand* out) const;

    const SymbolProfile* find_symbol(std::string_view symbol) const;

private:
    std::vector<SymbolProfile> symbols_;
    ValidatorPolicy policy_;
};

// Stable 64-bit hash of a canonical payload string. FNV-1a, pinned here rather
// than taken from a standard library container hash: std::hash is allowed to
// vary between runs and builds, which would make the duplicate-versus-conflict
// decision non-reproducible across a restart (C-04, M-05).
std::uint64_t canonical_hash64(std::string_view s);

}  // namespace lpsim
