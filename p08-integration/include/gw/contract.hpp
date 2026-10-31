// The compatibility matrix.
//
// Section 2 of the design doc closes with the list of things that must be
// pinned before P01-P07 may run as one system -- "module commit hashes, schema
// migration version, event codec version, FIX dictionary, RoE, price scale,
// account/symbol policy, FIX session identity, test harness version" -- and
// then the rule for when they disagree: "Mismatch -> BLOCKED hoac fail closed,
// khong best effort deserialize".
//
// This header is that list in code, for the seven components as they actually
// exist in this repository rather than as the document imagines them. The
// checks are not ceremony. The components were built separately and in places
// agree only by coincidence:
//
//   * P07 carries the price scale as a compile-time constant (llx::kPxScale)
//     while P04 carries one per symbol (router::SymbolProfile::price_scale).
//     A symbol quoted to anything but five digits is representable in P04 and
//     silently wrong in P07. EI-15 requires a scale mismatch to be rejected,
//     so the disagreement has to fail the gate rather than be rounded away.
//   * P01, P02 and P06 each own a separate durable store with its own
//     independent kSchemaVersion. Nothing today stops one from being migrated
//     without the others.
//
// A failure here is a BLOCKED run, which EI-54 forbids a release gate from
// counting as a PASS.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gw {

// What one component declares about itself. `store_schema` is -1 for the
// components that own no durable store of their own, which is every one of them
// except P01, P02 and P06.
struct ComponentPin {
    std::string_view id;       // "P01" .. "P07"
    std::string_view library;  // the CMake target, which is also the namespace
    std::string_view owns;     // the state this component is the single owner of
    int store_schema;
};

// The numeric contract across the whole assembled system.
//
// Five representations meet here and none converts to another for free: P02
// holds ticks plus a per-symbol PriceFormat, P03 holds a mantissa and its own
// scale per value, P04 holds int64 plus a per-symbol digit count, P05 holds
// exact rationals, and P07 holds int64 against a fixed global scale. P07's is
// the binding one because it cannot be reconfigured without a recompile.
struct NumericContract {
    int price_digits;           // fraction digits carried on the wire
    std::int64_t px_scale;      // must equal 10^price_digits
    std::int64_t qty_scale;
};

// The one profile this lab is pinned to. Changing a value here is a schema
// change: it has to be accompanied by the migration and the recompile that make
// every component agree again, which is the point of asserting it.
inline constexpr NumericContract kNumericContract{
    .price_digits = 5,
    .px_scale = 100000,
    .qty_scale = 1000000,
};

inline constexpr int kHarnessVersion = 1;

enum class GateStatus : std::uint8_t {
    // Every pin agreed. The lab may start.
    Ready,
    // At least one pin disagreed. Section 1.4 and EI-54: this is neither a PASS
    // nor a FAIL of any scenario, because no scenario ran.
    Blocked,
};

std::string_view to_string(GateStatus s);

struct Finding {
    std::string_view check;  // stable identifier, safe to assert on in CI
    std::string detail;      // what disagreed, with both values
};

struct GateReport {
    GateStatus status = GateStatus::Blocked;
    std::vector<Finding> findings;  // empty if and only if status is Ready
};

// The seven components, in the order section 2 tabulates them, with the state
// each one is the single owner of. The ownership column is what resolves the
// three overlapping gateway layers the assembly inherited: P06 owns the
// transaction, so the stores inside P03 and P05 write through it; P03 owns
// order status and the fill ledger, so P04's local reducer is demoted to an
// oracle rather than kept on the write path.
[[nodiscard]] std::vector<ComponentPin> components();

// Reads the constants the seven libraries actually compiled with and compares
// them against the pins above. Linking this function at all is half the check:
// it forces all seven libraries into the binary, so a component that cannot
// coexist with the others fails at link time rather than mid-scenario.
[[nodiscard]] GateReport verify_contracts();

}  // namespace gw
