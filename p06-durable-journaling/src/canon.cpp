#include "jrnl/canon.hpp"

#include <cstdio>

namespace jrnl {

CanonValue CanonValue::boolean(bool b) {
    CanonValue v;
    v.kind_ = Kind::Bool;
    v.bool_ = b;
    return v;
}

CanonValue CanonValue::integer(std::int64_t x) {
    CanonValue v;
    v.kind_ = Kind::Int;
    v.int_ = x;
    return v;
}

CanonValue CanonValue::str(std::string s) {
    CanonValue v;
    v.kind_ = Kind::Str;
    v.str_ = std::move(s);
    return v;
}

CanonValue CanonValue::list(CanonList l) {
    CanonValue v;
    v.kind_ = Kind::List;
    v.list_ = std::make_shared<CanonList>(std::move(l));
    return v;
}

CanonValue CanonValue::map(CanonMap m) {
    CanonValue v;
    v.kind_ = Kind::Map;
    v.map_ = std::make_shared<CanonMap>(std::move(m));
    return v;
}

bool CanonValue::as_bool(bool* out) const {
    if (kind_ != Kind::Bool) return false;
    if (out != nullptr) *out = bool_;
    return true;
}

bool CanonValue::as_int(std::int64_t* out) const {
    if (kind_ != Kind::Int) return false;
    if (out != nullptr) *out = int_;
    return true;
}

bool CanonValue::as_str(std::string* out) const {
    if (kind_ != Kind::Str) return false;
    if (out != nullptr) *out = str_;
    return true;
}

const CanonList* CanonValue::as_list() const {
    return kind_ == Kind::List ? list_.get() : nullptr;
}

const CanonMap* CanonValue::as_map() const { return kind_ == Kind::Map ? map_.get() : nullptr; }

const CanonValue* CanonValue::find(std::string_view key) const {
    if (kind_ != Kind::Map || map_ == nullptr) return nullptr;
    const auto it = map_->find(std::string(key));
    return it == map_->end() ? nullptr : &it->second;
}

bool CanonValue::get_int(std::string_view key, std::int64_t* out) const {
    const CanonValue* v = find(key);
    return v != nullptr && v->as_int(out);
}

bool CanonValue::get_str(std::string_view key, std::string* out) const {
    const CanonValue* v = find(key);
    return v != nullptr && v->as_str(out);
}

bool CanonValue::get_bool(std::string_view key, bool* out) const {
    const CanonValue* v = find(key);
    return v != nullptr && v->as_bool(out);
}

void CanonValue::set(std::string key, CanonValue v) {
    if (kind_ != Kind::Map || map_ == nullptr) {
        kind_ = Kind::Map;
        map_ = std::make_shared<CanonMap>();
    } else if (map_.use_count() > 1) {
        map_ = std::make_shared<CanonMap>(*map_);  // copy on write
    }
    (*map_)[std::move(key)] = std::move(v);
}

bool CanonValue::operator==(const CanonValue& o) const {
    if (kind_ != o.kind_) return false;
    switch (kind_) {
        case Kind::Null: return true;
        case Kind::Bool: return bool_ == o.bool_;
        case Kind::Int: return int_ == o.int_;
        case Kind::Str: return str_ == o.str_;
        case Kind::List: {
            const CanonList& a = *list_;
            const CanonList& b = *o.list_;
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (!(a[i] == b[i])) return false;
            }
            return true;
        }
        case Kind::Map: {
            const CanonMap& a = *map_;
            const CanonMap& b = *o.map_;
            if (a.size() != b.size()) return false;
            auto ia = a.begin();
            auto ib = b.begin();
            for (; ia != a.end(); ++ia, ++ib) {
                if (ia->first != ib->first) return false;
                if (!(ia->second == ib->second)) return false;
            }
            return true;
        }
    }
    return false;
}

namespace {

// Escapes the three things that must be escaped and nothing else. Any other
// byte, including UTF-8 continuation bytes, is emitted verbatim so that the
// encoding is a pure function of the bytes in the string.
void encode_string(const std::string& s, std::string* out) {
    out->push_back('"');
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"') {
            out->append("\\\"");
        } else if (c == '\\') {
            out->append("\\\\");
        } else if (u < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(u));
            out->append(buf);
        } else {
            out->push_back(c);
        }
    }
    out->push_back('"');
}

void encode_value(const CanonValue& v, std::string* out) {
    switch (v.kind()) {
        case CanonValue::Kind::Null:
            out->append("null");
            return;
        case CanonValue::Kind::Bool: {
            bool b = false;
            v.as_bool(&b);
            out->append(b ? "true" : "false");
            return;
        }
        case CanonValue::Kind::Int: {
            std::int64_t x = 0;
            v.as_int(&x);
            out->append(std::to_string(x));
            return;
        }
        case CanonValue::Kind::Str: {
            std::string s;
            v.as_str(&s);
            encode_string(s, out);
            return;
        }
        case CanonValue::Kind::List: {
            out->push_back('[');
            const CanonList* l = v.as_list();
            bool first = true;
            for (const CanonValue& e : *l) {
                if (!first) out->push_back(',');
                first = false;
                encode_value(e, out);
            }
            out->push_back(']');
            return;
        }
        case CanonValue::Kind::Map: {
            out->push_back('{');
            const CanonMap* m = v.as_map();
            bool first = true;
            // std::map already iterates in bytewise key order, which IS the
            // canonical order. Nothing else establishes it.
            for (const auto& entry : *m) {
                if (!first) out->push_back(',');
                first = false;
                encode_string(entry.first, out);
                out->push_back(':');
                encode_value(entry.second, out);
            }
            out->push_back('}');
            return;
        }
    }
}

struct Parser {
    std::string_view s;
    std::size_t i = 0;
    std::string why;

    bool fail(const char* m) {
        if (why.empty()) why = m;
        return false;
    }
    bool eof() const { return i >= s.size(); }

    bool parse_string(std::string* out) {
        if (eof() || s[i] != '"') return fail("expected string");
        ++i;
        out->clear();
        while (true) {
            if (eof()) return fail("unterminated string");
            const char c = s[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (c == '\\') {
                ++i;
                if (eof()) return fail("truncated escape");
                const char e = s[i];
                if (e == '"') {
                    out->push_back('"');
                    ++i;
                } else if (e == '\\') {
                    out->push_back('\\');
                    ++i;
                } else if (e == 'u') {
                    if (i + 4 >= s.size()) return fail("truncated unicode escape");
                    unsigned v = 0;
                    for (int k = 1; k <= 4; ++k) {
                        const char h = s[i + k];
                        int d;
                        if (h >= '0' && h <= '9') d = h - '0';
                        else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                        else return fail("bad unicode escape");
                        v = (v << 4) | static_cast<unsigned>(d);
                    }
                    // Canonical form only ever emits an escape for C0
                    // controls, so anything else is a non-canonical encoding.
                    if (v >= 0x20) return fail("non-canonical unicode escape");
                    out->push_back(static_cast<char>(v));
                    i += 5;
                } else {
                    return fail("non-canonical escape");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return fail("raw control byte in string");
            } else {
                out->push_back(c);
                ++i;
            }
        }
    }

    bool parse_value(CanonValue* out) {
        if (eof()) return fail("unexpected end of input");
        const char c = s[i];
        if (c == 'n') {
            if (s.compare(i, 4, "null") != 0) return fail("bad literal");
            i += 4;
            *out = CanonValue::null();
            return true;
        }
        if (c == 't') {
            if (s.compare(i, 4, "true") != 0) return fail("bad literal");
            i += 4;
            *out = CanonValue::boolean(true);
            return true;
        }
        if (c == 'f') {
            if (s.compare(i, 5, "false") != 0) return fail("bad literal");
            i += 5;
            *out = CanonValue::boolean(false);
            return true;
        }
        if (c == '"') {
            std::string v;
            if (!parse_string(&v)) return false;
            *out = CanonValue::str(std::move(v));
            return true;
        }
        if (c == '[') {
            ++i;
            CanonList l;
            if (!eof() && s[i] == ']') {
                ++i;
                *out = CanonValue::list(std::move(l));
                return true;
            }
            while (true) {
                CanonValue e;
                if (!parse_value(&e)) return false;
                l.push_back(std::move(e));
                if (eof()) return fail("unterminated array");
                if (s[i] == ',') {
                    ++i;
                    continue;
                }
                if (s[i] == ']') {
                    ++i;
                    break;
                }
                return fail("bad array separator");
            }
            *out = CanonValue::list(std::move(l));
            return true;
        }
        if (c == '{') {
            ++i;
            CanonMap m;
            if (!eof() && s[i] == '}') {
                ++i;
                *out = CanonValue::map(std::move(m));
                return true;
            }
            std::string prev_key;
            bool have_prev = false;
            while (true) {
                std::string k;
                if (!parse_string(&k)) return false;
                // Canonical means sorted and unique. A row whose keys arrive
                // in another order did not come out of canonical_encode.
                if (have_prev && !(prev_key < k)) {
                    return fail("object keys not in canonical order");
                }
                prev_key = k;
                have_prev = true;
                if (eof() || s[i] != ':') return fail("expected colon");
                ++i;
                CanonValue v;
                if (!parse_value(&v)) return false;
                m.emplace(std::move(k), std::move(v));
                if (eof()) return fail("unterminated object");
                if (s[i] == ',') {
                    ++i;
                    continue;
                }
                if (s[i] == '}') {
                    ++i;
                    break;
                }
                return fail("bad object separator");
            }
            *out = CanonValue::map(std::move(m));
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const std::size_t start = i;
            if (c == '-') ++i;
            if (eof() || s[i] < '0' || s[i] > '9') return fail("bad number");
            // No leading zeros: "01" is not canonical output.
            if (s[i] == '0' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
                return fail("leading zero");
            }
            while (!eof() && s[i] >= '0' && s[i] <= '9') ++i;
            if (!eof() && (s[i] == '.' || s[i] == 'e' || s[i] == 'E')) {
                return fail("non-integer number");
            }
            const std::string num(s.substr(start, i - start));
            // Canonical output for zero is "0", never "-0".
            if (num == "-0") return fail("non-canonical negative zero");
            std::int64_t v = 0;
            try {
                std::size_t used = 0;
                v = std::stoll(num, &used);
                if (used != num.size()) return fail("bad number");
            } catch (...) {
                return fail("integer out of range");
            }
            *out = CanonValue::integer(v);
            return true;
        }
        return fail("unexpected character");
    }
};

}  // namespace

std::string canonical_encode(const CanonValue& v) {
    std::string out;
    encode_value(v, &out);
    return out;
}

bool canonical_decode(std::string_view text, CanonValue* out, std::string* why) {
    Parser p{text, 0, {}};
    CanonValue v;
    if (!p.parse_value(&v)) {
        if (why != nullptr) *why = p.why;
        return false;
    }
    if (p.i != text.size()) {
        if (why != nullptr) *why = "trailing bytes after value";
        return false;
    }
    if (out != nullptr) *out = std::move(v);
    return true;
}

Payload& Payload::i(std::string key, std::int64_t v) {
    fields_[std::move(key)] = CanonValue::integer(v);
    return *this;
}

Payload& Payload::s(std::string key, std::string v) {
    fields_[std::move(key)] = CanonValue::str(std::move(v));
    return *this;
}

Payload& Payload::b(std::string key, bool v) {
    fields_[std::move(key)] = CanonValue::boolean(v);
    return *this;
}

Payload& Payload::n(std::string key) {
    fields_[std::move(key)] = CanonValue::null();
    return *this;
}

Payload& Payload::l(std::string key, CanonList v) {
    fields_[std::move(key)] = CanonValue::list(std::move(v));
    return *this;
}

Payload& Payload::m(std::string key, CanonMap v) {
    fields_[std::move(key)] = CanonValue::map(std::move(v));
    return *this;
}

bool amount_in_range(std::int64_t v) { return v >= -kMaxAmount && v <= kMaxAmount; }

}  // namespace jrnl
