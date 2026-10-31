// Identity keys and the uniqueness scopes they live in.
//
// Design doc 7.2 lists five indices. They are distinct types here so a request
// key can never be passed where an execution key is expected, which is the
// mistake behind cross-chain contamination (invariant I03).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace order_core {

// Local, immutable handle for an order chain. Never derived from a wire field.
struct OrderId {
    uint64_t value = 0;
    friend bool operator==(OrderId a, OrderId b) { return a.value == b.value; }
    friend bool operator<(OrderId a, OrderId b) { return a.value < b.value; }
    [[nodiscard]] bool valid() const { return value != 0; }
};

// (firm, business_day, ClOrdID): one client request across 35=D / 35=F / 35=G.
// The account is intentionally not part of the key in the strict v1 profile.
struct RequestKey {
    std::string firm;
    std::string business_day;
    std::string cl_ord_id;
    friend bool operator==(const RequestKey& a, const RequestKey& b) {
        return a.firm == b.firm && a.business_day == b.business_day &&
               a.cl_ord_id == b.cl_ord_id;
    }
    friend bool operator<(const RequestKey& a, const RequestKey& b) {
        if (a.firm != b.firm) return a.firm < b.firm;
        if (a.business_day != b.business_day) return a.business_day < b.business_day;
        return a.cl_ord_id < b.cl_ord_id;
    }
    [[nodiscard]] std::string text() const {
        return firm + "|" + business_day + "|" + cl_ord_id;
    }
};

// (peer, business namespace, ExecID): one logical execution report. Scoped by
// peer and namespace, never by MsgSeqNum (invariant I10).
struct ExecKey {
    std::string peer_id;
    std::string business_namespace;
    std::string exec_id;
    friend bool operator==(const ExecKey& a, const ExecKey& b) {
        return a.peer_id == b.peer_id && a.business_namespace == b.business_namespace &&
               a.exec_id == b.exec_id;
    }
    friend bool operator<(const ExecKey& a, const ExecKey& b) {
        if (a.peer_id != b.peer_id) return a.peer_id < b.peer_id;
        if (a.business_namespace != b.business_namespace)
            return a.business_namespace < b.business_namespace;
        return a.exec_id < b.exec_id;
    }
    [[nodiscard]] std::string text() const {
        return peer_id + "|" + business_namespace + "|" + exec_id;
    }
};

// (peer, OrderID): supplementary correlation only. An LP OrderID is not
// globally unique and must never be the sole key (design doc 3.3).
struct AliasKey {
    std::string peer_id;
    std::string order_id;
    friend bool operator<(const AliasKey& a, const AliasKey& b) {
        if (a.peer_id != b.peer_id) return a.peer_id < b.peer_id;
        return a.order_id < b.order_id;
    }
    friend bool operator==(const AliasKey& a, const AliasKey& b) {
        return a.peer_id == b.peer_id && a.order_id == b.order_id;
    }
};

// The key an applied event is recorded under so a redelivery is recognised
// after a crash. Either a request key or an execution key, flattened.
struct AppliedKey {
    std::string scope;         // "request" or "exec"
    std::string semantic_key;  // RequestKey::text() or ExecKey::text()
    uint64_t payload_hash = 0;
    [[nodiscard]] bool empty() const { return semantic_key.empty(); }
    friend bool operator<(const AppliedKey& a, const AppliedKey& b) {
        if (a.scope != b.scope) return a.scope < b.scope;
        return a.semantic_key < b.semantic_key;
    }
    friend bool operator==(const AppliedKey& a, const AppliedKey& b) {
        return a.scope == b.scope && a.semantic_key == b.semantic_key;
    }
};

// FNV-1a. Used for canonical state hashes and payload fingerprints: this is a
// change-detection digest for replay comparison, not a security primitive.
inline uint64_t fnv1a(const std::string& s, uint64_t seed = 1469598103934665603ULL) {
    uint64_t h = seed;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace order_core

namespace std {
template <>
struct hash<order_core::OrderId> {
    size_t operator()(order_core::OrderId id) const noexcept {
        return static_cast<size_t>(id.value * 1099511628211ULL);
    }
};
}  // namespace std
