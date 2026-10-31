// Canonical encoding and hashing.
//
// These are the foundations the whole journal rests on: if two renderings of
// the same payload can hash differently, every integrity claim above becomes
// noise. Scenario ids are from design doc section 24 group G01.
#include <fstream>
#include <map>

#include "jrnl/canon.hpp"
#include "jrnl/hash.hpp"
#include "tiny_test.hpp"

using namespace jrnl;

namespace {

SCENARIO("HASH-01", "P0", "SHA-256 matches the published test vectors") {
    // Without this, every other hash assertion in the suite is self-
    // referential: it would only prove the code agrees with itself.
    CHECK_EQ(to_hex(sha256("")),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(to_hex(sha256("abc")),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(to_hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    // A message that lands either side of the padding edge.
    const std::string edge(55, 'x');
    const std::string edge2(56, 'x');
    CHECK_NE(to_hex(sha256(edge)), to_hex(sha256(edge2)));
    CHECK_EQ(to_hex(sha256(std::string(1000, 'a'))).size(), std::size_t(64));
}

SCENARIO("HASH-02", "P0", "incremental hashing equals one-shot hashing") {
    Sha256 h;
    h.update("abcdbcdecdefdefgefghfghi");
    h.update("jhijkijkljklmklmnlmnomnopnopq");
    CHECK_EQ(to_hex(h.finish()),
             to_hex(sha256("abcdbcdecdefdefgefghfghijhijkijkljklmklmnlmnomnopnopq")));
}

SCENARIO("G01-11", "P0", "field order does not change the canonical hash") {
    // The literal scenario: "canonical JSON field order changes". Two maps
    // built in opposite orders must be byte-identical once encoded.
    CanonMap a;
    a["zulu"] = CanonValue::integer(1);
    a["alpha"] = CanonValue::str("x");
    a["mike"] = CanonValue::boolean(true);

    CanonMap b;
    b["mike"] = CanonValue::boolean(true);
    b["alpha"] = CanonValue::str("x");
    b["zulu"] = CanonValue::integer(1);

    const std::string ea = canonical_encode(CanonValue::map(a));
    const std::string eb = canonical_encode(CanonValue::map(b));
    CHECK_EQ(ea, eb);
    CHECK_EQ(to_hex(sha256(ea)), to_hex(sha256(eb)));
    CHECK_EQ(ea, std::string(R"({"alpha":"x","mike":true,"zulu":1})"));
}

SCENARIO("G01-11b", "P0", "a different value changes the hash") {
    const CanonValue a = Payload().i("qty", 100).s("sym", "EURUSD").build();
    const CanonValue b = Payload().i("qty", 101).s("sym", "EURUSD").build();
    CHECK_NE(to_hex(sha256(canonical_encode(a))), to_hex(sha256(canonical_encode(b))));
}

SCENARIO("G01-12", "P0", "out-of-range amounts are rejected before any commit") {
    CHECK(amount_in_range(0));
    CHECK(amount_in_range(kMaxAmount));
    CHECK(amount_in_range(-kMaxAmount));
    CHECK(!amount_in_range(kMaxAmount + 1));
    CHECK(!amount_in_range(-kMaxAmount - 1));

    // Two admitted amounts can be added without overflowing, which is what
    // lets the reducers do arithmetic without extra guards.
    const std::int64_t sum = kMaxAmount + kMaxAmount;
    CHECK(sum > 0);
}

SCENARIO("G01-16", "P0", "a payload with the wrong field type is refused, not coerced") {
    const CanonValue p = Payload().s("last_qty", "40000").build();
    std::int64_t q = 0;
    // A string where an integer belongs must not read as a number.
    CHECK(!p.get_int("last_qty", &q));
    CHECK_EQ(q, std::int64_t(0));

    std::string s;
    CHECK(p.get_str("last_qty", &s));
    CHECK_EQ(s, std::string("40000"));

    // An absent field is absent, not zero.
    CHECK(!p.get_int("missing", &q));
}

SCENARIO("G01-16b", "P0", "non-canonical text is refused by the decoder") {
    CanonValue v;
    std::string why;

    // Keys out of order did not come from canonical_encode.
    CHECK(!canonical_decode(R"({"b":1,"a":2})", &v, &why));
    CHECK(!why.empty());
    // Whitespace is not part of the canonical form.
    CHECK(!canonical_decode(R"({"a": 1})", &v, &why));
    // Floating point has no canonical integer rendering.
    CHECK(!canonical_decode(R"({"a":1.5})", &v, &why));
    // Leading zeros and negative zero are both non-canonical.
    CHECK(!canonical_decode(R"({"a":01})", &v, &why));
    CHECK(!canonical_decode(R"({"a":-0})", &v, &why));
    // Duplicate keys cannot be distinguished from an ordering error, and
    // either way the row was not produced by the encoder.
    CHECK(!canonical_decode(R"({"a":1,"a":2})", &v, &why));
    // Trailing bytes.
    CHECK(!canonical_decode(R"({"a":1} )", &v, &why));
}

SCENARIO("G01-11c", "P0", "canonical encoding round-trips exactly") {
    CanonList items;
    items.push_back(Payload().s("id", "E1").i("qty", 40000).build());
    items.push_back(Payload().s("id", "E2").i("qty", -5).build());

    const CanonValue original = Payload()
                                    .s("chain", "O1")
                                    .b("pending", false)
                                    .n("nothing")
                                    .i("version", 7)
                                    .l("fills", items)
                                    .s("quote", std::string("a\"b\\c"))
                                    .s("control", std::string("x\x01y"))
                                    .build();

    const std::string text = canonical_encode(original);
    CanonValue decoded;
    std::string why;
    CHECK_MSG(canonical_decode(text, &decoded, &why), why);
    CHECK(decoded == original);
    CHECK_EQ(canonical_encode(decoded), text);
}

SCENARIO("G01-19", "P0", "a malformed hash column is corruption, not an empty digest") {
    Digest d;
    CHECK(!from_hex("", &d));
    CHECK(!from_hex("abcd", &d));
    CHECK(!from_hex(std::string(64, 'z'), &d));
    CHECK(!from_hex(std::string(63, 'a'), &d));
    // Uppercase is not what the encoder writes, so it is not accepted back.
    CHECK(!from_hex(std::string(64, 'A'), &d));
    CHECK(from_hex(std::string(64, 'a'), &d));
}

SCENARIO("G01-10", "P0", "the chain preimage is unambiguous across its components") {
    // Length-prefixing matters: without it ("ab","c") and ("a","bc") would
    // collide and a substitution would be invisible to the verifier.
    const Digest prev = sha256("prev");
    const Digest x = chain_step(prev, "ab", "c", 1, "r");
    const Digest y = chain_step(prev, "a", "bc", 1, "r");
    CHECK_NE(to_hex(x), to_hex(y));

    // Every component is covered.
    CHECK_NE(to_hex(chain_step(prev, "p", "s", 1, "r")),
             to_hex(chain_step(prev, "p", "s", 2, "r")));
    CHECK_NE(to_hex(chain_step(prev, "p", "s", 1, "r1")),
             to_hex(chain_step(prev, "p", "s", 1, "r2")));
    CHECK_NE(to_hex(chain_step(sha256("other"), "p", "s", 1, "r")),
             to_hex(chain_step(prev, "p", "s", 1, "r")));
}

SCENARIO("G12-01", "P0", "the transaction manifest digest ignores member order") {
    std::vector<Digest> a{sha256("one"), sha256("two"), sha256("three")};
    std::vector<Digest> b{sha256("three"), sha256("one"), sha256("two")};
    CHECK_EQ(to_hex(members_digest(a)), to_hex(members_digest(b)));

    // But not membership.
    std::vector<Digest> c{sha256("one"), sha256("two")};
    CHECK_NE(to_hex(members_digest(a)), to_hex(members_digest(c)));

    // And not multiplicity: two copies of one member is a different set.
    std::vector<Digest> d{sha256("one"), sha256("one"), sha256("two")};
    CHECK_NE(to_hex(members_digest(a)), to_hex(members_digest(d)));
}

SCENARIO("GOLD-01", "P0", "the canonical encoding still matches the committed golden file") {
    // Every event ever written was hashed with these rules. If the codec
    // moves, every historical state_hash stops verifying, so this is the
    // one check that has to fail loudly rather than be quietly updated.
    std::ifstream f(std::string(JRNL_FIXTURES_DIR) +
                    "/canonical_events/order_accepted_v1.golden");
    CHECK_MSG(f.good(), std::string("cannot open the golden fixture"));

    std::map<std::string, std::string> golden;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        golden[line.substr(0, tab)] = line.substr(tab + 1);
    }
    CHECK_EQ(golden.size(), std::size_t(3));

    // Rebuild the same envelope the store would write.
    CanonMap m;
    m["tenant"] = CanonValue::str("t1");
    m["stream"] = CanonValue::str("ord:O1");
    m["version"] = CanonValue::integer(1);
    m["event_id"] = CanonValue::str("ev-0001");
    m["event_type"] = CanonValue::str("OrderAccepted");
    m["schema_version"] = CanonValue::integer(1);
    m["reducer_version"] = CanonValue::str("p03.v1");
    m["policy_version"] = CanonValue::integer(1);
    m["payload"] = Payload()
                       .s("account", "A1")
                       .s("chain", "O1")
                       .s("cl_ord_id", "K1-cl")
                       .s("command_id", "K1")
                       .i("limit_px", 10000)
                       .i("order_qty", 100000)
                       .s("risk_domain", "D1")
                       .s("side", "BUY")
                       .s("symbol", "EURUSD")
                       .build();

    const std::string text = canonical_encode(CanonValue::map(m));
    CHECK_EQ(text, golden["ENCODING"]);
    CHECK_EQ(to_hex(sha256(text)), golden["CANONICAL"]);

    const Digest chain =
        chain_step(zero_digest(), to_hex(sha256(text)) + to_hex(sha256("state")), "ord:O1", 1,
                   "p03.v1");
    CHECK_EQ(to_hex(chain), golden["CHAIN"]);

    // And the golden bytes decode back to exactly what produced them.
    CanonValue back;
    std::string why;
    CHECK_MSG(canonical_decode(golden["ENCODING"], &back, &why), why);
    CHECK_EQ(canonical_encode(back), golden["ENCODING"]);
}

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_canonical_hash"); }
