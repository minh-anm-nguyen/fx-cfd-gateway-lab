// Canonical payload encoding.
//
// Design doc G01-11 requires that re-ordering the fields of a payload does not
// change its hash, and section 8 note 3 requires the hash preimage to be
// canonical bytes. Both fall out of one rule: a payload is an ordered map
// serialised with its keys sorted bytewise, no insignificant whitespace, and
// integers only.
//
// There is deliberately no floating point type. G01-12 asks that an
// out-of-range quantity or price is rejected before the journal commit rather
// than silently wrapping, and a binary float would make "the same number" a
// question about rounding. Quantities and prices travel as integers in minor
// units with the scale fixed by the policy version.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jrnl {

class CanonValue;
using CanonMap = std::map<std::string, CanonValue>;
using CanonList = std::vector<CanonValue>;

class CanonValue {
public:
    enum class Kind { Null, Bool, Int, Str, List, Map };

    CanonValue() = default;
    static CanonValue null() { return CanonValue(); }
    static CanonValue boolean(bool b);
    static CanonValue integer(std::int64_t v);
    static CanonValue str(std::string s);
    static CanonValue list(CanonList v);
    static CanonValue map(CanonMap v);

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::Null; }

    // Typed accessors. Each returns false when the value is absent or of
    // another kind: a reducer that reads a field of the wrong type must fail
    // the event rather than coerce it (G01-16).
    bool as_bool(bool* out) const;
    bool as_int(std::int64_t* out) const;
    bool as_str(std::string* out) const;
    const CanonList* as_list() const;
    const CanonMap* as_map() const;

    // Map field lookup. Returns nullptr when this is not a map or the key is
    // absent.
    const CanonValue* find(std::string_view key) const;

    // Convenience readers used all over the reducers. The bool result is
    // "present and of the right type".
    bool get_int(std::string_view key, std::int64_t* out) const;
    bool get_str(std::string_view key, std::string* out) const;
    bool get_bool(std::string_view key, bool* out) const;

    void set(std::string key, CanonValue v);

    bool operator==(const CanonValue& o) const;
    bool operator!=(const CanonValue& o) const { return !(*this == o); }

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    std::int64_t int_ = 0;
    std::string str_;
    std::shared_ptr<CanonList> list_;
    std::shared_ptr<CanonMap> map_;
};

// Canonical serialisation. Deterministic for a given value: this is the hash
// preimage and the bytes stored in the payload column.
std::string canonical_encode(const CanonValue& v);

// Exact inverse of canonical_encode for any value it produced. Returns false
// on anything else, including valid JSON that is not in canonical form, so a
// hand-edited row is caught by the decoder before the hash check even runs.
bool canonical_decode(std::string_view text, CanonValue* out, std::string* why);

// Small builder so call sites read like the event they are describing.
class Payload {
public:
    Payload& i(std::string key, std::int64_t v);
    Payload& s(std::string key, std::string v);
    Payload& b(std::string key, bool v);
    Payload& n(std::string key);
    Payload& l(std::string key, CanonList v);
    Payload& m(std::string key, CanonMap v);
    CanonValue build() const { return CanonValue::map(fields_); }
    operator CanonValue() const { return build(); }

private:
    CanonMap fields_;
};

// Range guard for quantities, prices and limits (G01-12). The bound is well
// inside int64 so that a reducer can add two admitted values without the sum
// overflowing.
inline constexpr std::int64_t kMaxAmount = 1000000000000000ll;  // 1e15
bool amount_in_range(std::int64_t v);

}  // namespace jrnl
