// Identity vocabulary from design doc section 7.
//
// These are aliases rather than wrapper types on purpose: every one of them is
// a short opaque string that crosses an SQL boundary, and the places where
// mixing them up would matter are the places where the composite primary keys
// in the schema already reject the mistake. What the doc does demand is that
// the *scope* of each identity is explicit, so the names carry it.
#pragma once

#include <cstdint>
#include <string>

namespace jrnl {

using TenantId = std::string;       // tenant boundary; part of every canonical PK
using AccountId = std::string;      // risk scope inside a tenant
using CommandId = std::string;      // client API idempotency key
using OrderChainId = std::string;   // immutable across New/Replace
using ClOrdId = std::string;        // FIX-scoped identity, changes on Replace
using RiskDomainId = std::string;   // unit of exposure ownership and fencing
using SessionKey = std::string;     // FIX session identity at P01
using StreamId = std::string;       // aggregate stream: "ord:<chain>" / "rsk:<domain>"
using TxId = std::string;           // business transaction manifest id
using EventId = std::string;        // unique event identity, NOT a commit clock
using ExecId = std::string;         // counterparty execution identity
using LpNamespace = std::string;    // scope that makes an ExecId meaningful
using ReservationId = std::string;  // risk reservation identity
using OutboxId = std::string;       // side effect intent identity
using CaseId = std::string;         // reconciliation case identity

// Per-aggregate version. Total order WITHIN one stream only: the doc is
// explicit (section 7) that this is not a global commit order.
using Version = std::uint64_t;

// Monotonic owner epoch handed out by the database (section 16). A mutation
// carrying a token older than the row's current token must not commit.
using FenceToken = std::uint64_t;

using PolicyVersion = std::uint64_t;
using SchemaVersion = std::int32_t;

// Stream naming. Kept in one function each so a test and the store cannot
// disagree about what a stream is called.
inline StreamId order_stream(const OrderChainId& chain) { return "ord:" + chain; }
inline StreamId risk_stream(const RiskDomainId& domain) { return "rsk:" + domain; }

inline bool is_order_stream(const StreamId& s) { return s.rfind("ord:", 0) == 0; }
inline bool is_risk_stream(const StreamId& s) { return s.rfind("rsk:", 0) == 0; }

// The part after the prefix. Empty when the stream is not of that kind.
inline std::string stream_suffix(const StreamId& s) {
    return s.size() > 4 ? s.substr(4) : std::string();
}

// Largest stream version the engine will mint. BIGINT in PostgreSQL and
// INTEGER in SQLite are both signed 64-bit, so the ceiling is the signed
// maximum, and admission stops well before it rather than wrapping (G01-20).
inline constexpr Version kMaxStreamVersion = 9223372036854775807ull;

}  // namespace jrnl
