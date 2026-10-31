#include "llx/store.hpp"

#include <cstdio>
#include <cstring>

#include "llx/failpoint.hpp"
#include "llx/hash.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace llx {

std::string_view to_string(OutboxState s) {
    switch (s) {
        case OutboxState::Ready: return "READY";
        case OutboxState::Claimed: return "CLAIMED";
        case OutboxState::Sent: return "SENT";
        case OutboxState::Ambiguous: return "AMBIGUOUS";
        case OutboxState::Reconciled: return "RECONCILED";
    }
    return "UNKNOWN";
}

namespace {

// Little-endian fixed-width encoding. Chosen over text so a record is a fixed
// shape and a torn tail is unambiguous, and over memcpy of the struct so the
// format does not depend on padding or on the compiler.
void put_u64(std::string& s, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_i64(std::string& s, std::int64_t v) {
    put_u64(s, static_cast<std::uint64_t>(v));
}
void put_u32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u8(std::string& s, std::uint8_t v) { s.push_back(static_cast<char>(v)); }

struct Reader {
    const std::string& s;
    std::size_t at = 0;
    bool ok = true;

    std::uint64_t u64() {
        if (at + 8 > s.size()) {
            ok = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[at + static_cast<std::size_t>(i)]))
                 << (8 * i);
        at += 8;
        return v;
    }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    std::uint32_t u32() {
        if (at + 4 > s.size()) {
            ok = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + static_cast<std::size_t>(i)]))
                 << (8 * i);
        at += 4;
        return v;
    }
    std::uint8_t u8() {
        if (at + 1 > s.size()) {
            ok = false;
            return 0;
        }
        return static_cast<std::uint8_t>(s[at++]);
    }
};

std::uint64_t checksum(const std::string& payload) {
    std::uint64_t h = 1469598103934665603ULL;
    for (char c : payload) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace

// --- construction and journal I/O -----------------------------------------

JournalStore::JournalStore() : JournalStore(Options{}) {}

JournalStore::JournalStore(Options opts) : opts_(std::move(opts)) {
    epoch_ = opts_.initial_epoch;
    if (!opts_.journal_path.empty()) {
        // Open for append and read. An existing journal is replayed, which is
        // the restart path: a process killed mid-run leaves the file and the
        // next process rebuilds exactly the committed state from it.
        std::FILE* f = std::fopen(opts_.journal_path.c_str(), "a+b");
        if (f != nullptr) {
            file_ = f;
            load_journal();
            rebuild_from_journal();
        }
    }
}

JournalStore::~JournalStore() {
    if (file_ != nullptr) std::fclose(static_cast<std::FILE*>(file_));
}

std::string JournalStore::encode(const Record& r) const {
    std::string p;
    p.reserve(256);
    put_u8(p, static_cast<std::uint8_t>(r.kind));
    put_u64(p, r.version);
    put_u64(p, r.event_id.hi);
    put_u64(p, r.event_id.lo);
    put_u64(p, r.domain_seq);
    put_u32(p, r.risk_domain);
    put_u32(p, r.owner_epoch);
    put_u64(p, r.idempotency_key);
    put_u8(p, r.write_inbox ? 1 : 0);

    // Proposal post-image.
    const Proposal& e = r.effect;
    put_u8(p, e.accepted ? 1 : 0);
    put_u8(p, static_cast<std::uint8_t>(e.reason));
    put_u8(p, e.quarantine ? 1 : 0);
    put_u8(p, e.write_projection ? 1 : 0);
    put_u64(p, e.order.order_id);
    put_u64(p, e.order.orig_order_id);
    put_u32(p, e.order.symbol);
    put_u8(p, static_cast<std::uint8_t>(e.order.side));
    put_i64(p, e.order.qty);
    put_i64(p, e.order.px);
    put_u8(p, static_cast<std::uint8_t>(e.order.status));
    put_i64(p, e.order.cum_qty);
    put_i64(p, e.order.leaves_qty);
    put_i64(p, e.order.avg_px);
    put_u64(p, e.order.notional);
    put_u32(p, e.order.version);
    put_i64(p, e.order.pending_qty);
    put_i64(p, e.order.pending_px);
    put_u64(p, e.order.last_order_seq);
    put_u8(p, e.append_fill ? 1 : 0);
    put_u64(p, e.fill.exec_id);
    put_u64(p, e.fill.order_id);
    put_i64(p, e.fill.last_qty);
    put_i64(p, e.fill.last_px);
    put_u8(p, e.write_risk ? 1 : 0);
    put_u32(p, e.risk_domain);
    put_i64(p, e.reserved_buy_delta);
    put_i64(p, e.reserved_sell_delta);
    put_i64(p, e.position_delta);
    put_u8(p, e.outbox_kind);
    put_u64(p, e.outbox_cl_ord_id);
    put_u64(p, e.outbox_orig_cl_ord_id);
    put_u8(p, e.no_effect ? 1 : 0);

    // Outbox row carried by a business record, and the id carried by the
    // attempt/sent/ambiguous kinds.
    put_u64(p, r.outbox.id);
    put_u32(p, r.outbox.symbol);
    put_u8(p, static_cast<std::uint8_t>(r.outbox.side));
    put_i64(p, r.outbox.qty);
    put_i64(p, r.outbox.px);
    put_u64(p, r.outbox_id);
    put_u32(p, r.epoch);
    put_u32(p, r.config_domain);
    put_i64(p, r.config_limit);
    return p;
}

bool JournalStore::decode(const std::string& payload, Record& out) const {
    Reader rd{payload};
    out.kind = static_cast<RecordKind>(rd.u8());
    out.version = rd.u64();
    out.event_id.hi = rd.u64();
    out.event_id.lo = rd.u64();
    out.domain_seq = rd.u64();
    out.risk_domain = rd.u32();
    out.owner_epoch = rd.u32();
    out.idempotency_key = rd.u64();
    out.write_inbox = rd.u8() != 0;

    Proposal& e = out.effect;
    e.accepted = rd.u8() != 0;
    e.reason = static_cast<RejectReason>(rd.u8());
    e.quarantine = rd.u8() != 0;
    e.write_projection = rd.u8() != 0;
    e.order.order_id = rd.u64();
    e.order.orig_order_id = rd.u64();
    e.order.symbol = rd.u32();
    e.order.side = static_cast<Side>(rd.u8());
    e.order.qty = rd.i64();
    e.order.px = rd.i64();
    e.order.status = static_cast<OrderStatus>(rd.u8());
    e.order.cum_qty = rd.i64();
    e.order.leaves_qty = rd.i64();
    e.order.avg_px = rd.i64();
    e.order.notional = rd.u64();
    e.order.version = rd.u32();
    e.order.pending_qty = rd.i64();
    e.order.pending_px = rd.i64();
    e.order.last_order_seq = rd.u64();
    e.append_fill = rd.u8() != 0;
    e.fill.exec_id = rd.u64();
    e.fill.order_id = rd.u64();
    e.fill.last_qty = rd.i64();
    e.fill.last_px = rd.i64();
    e.write_risk = rd.u8() != 0;
    e.risk_domain = rd.u32();
    e.reserved_buy_delta = rd.i64();
    e.reserved_sell_delta = rd.i64();
    e.position_delta = rd.i64();
    e.outbox_kind = rd.u8();
    e.outbox_cl_ord_id = rd.u64();
    e.outbox_orig_cl_ord_id = rd.u64();
    e.no_effect = rd.u8() != 0;

    out.outbox.id = rd.u64();
    out.outbox.symbol = rd.u32();
    out.outbox.side = static_cast<Side>(rd.u8());
    out.outbox.qty = rd.i64();
    out.outbox.px = rd.i64();
    out.outbox_id = rd.u64();
    out.epoch = rd.u32();
    out.config_domain = rd.u32();
    out.config_limit = rd.i64();
    return rd.ok;
}

bool JournalStore::append(const Record& r) {
    records_.push_back(r);
    if (file_ == nullptr) return true;  // memory-only journal

    const std::string payload = encode(r);
    const auto len = static_cast<std::uint32_t>(payload.size());
    std::string frame;
    frame.reserve(payload.size() + 12);
    put_u32(frame, len);
    frame.append(payload);
    put_u64(frame, checksum(payload));

    auto* f = static_cast<std::FILE*>(file_);
    if (std::fwrite(frame.data(), 1, frame.size(), f) != frame.size()) return false;
    if (std::fflush(f) != 0) return false;
    if (opts_.sync_on_commit) {
        // Push the bytes past the OS cache. This is the strongest durability
        // claim this store makes, and it is still weaker than a database
        // commit: doc 15.1 keeps the two as separate span classes.
#if defined(_WIN32)
        const int fd = ::_fileno(f);
        if (fd >= 0) {
            auto h = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
            if (h != INVALID_HANDLE_VALUE) ::FlushFileBuffers(h);
        }
#else
        const int fd = ::fileno(f);
        if (fd >= 0) ::fsync(fd);
#endif
        ++stats_.syncs;
    }
    return true;
}

bool JournalStore::load_journal() {
    auto* f = static_cast<std::FILE*>(file_);
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_SET);
    records_.clear();
    for (;;) {
        unsigned char lenbuf[4];
        if (std::fread(lenbuf, 1, 4, f) != 4) break;  // clean end of file
        std::uint32_t len = 0;
        for (int i = 0; i < 4; ++i)
            len |= static_cast<std::uint32_t>(lenbuf[i]) << (8 * i);
        if (len == 0 || len > (1u << 20)) {
            // A length this record cannot have: the tail is garbage, so stop.
            ++stats_.records_discarded;
            break;
        }
        std::string payload(len, '\0');
        if (std::fread(payload.data(), 1, len, f) != len) {
            // Torn record: the process died between the length and the body.
            // Discarding it is what makes the transaction atomic (CW-16).
            ++stats_.records_discarded;
            break;
        }
        unsigned char ckbuf[8];
        if (std::fread(ckbuf, 1, 8, f) != 8) {
            ++stats_.records_discarded;
            break;
        }
        std::uint64_t ck = 0;
        for (int i = 0; i < 8; ++i)
            ck |= static_cast<std::uint64_t>(ckbuf[i]) << (8 * i);
        if (ck != checksum(payload)) {
            ++stats_.records_discarded;
            break;
        }
        Record r;
        if (!decode(payload, r)) {
            ++stats_.records_discarded;
            break;
        }
        records_.push_back(r);
        ++stats_.records_replayed;
    }
    std::fseek(f, 0, SEEK_END);
    return true;
}

void JournalStore::apply_record(const Record& r) {
    switch (r.kind) {
        case RecordKind::Business: {
            version_ = r.version;
            if (r.write_inbox) inbox_.insert(r.event_id);
            if (r.effect.append_fill && r.effect.fill.exec_id != 0)
                seen_exec_.insert(r.effect.fill.exec_id);
            apply(state_, r.effect, r.version);
            if (r.domain_seq > 0) {
                auto& cur = domain_seq_[r.risk_domain];
                if (r.domain_seq > cur) cur = r.domain_seq;
            }
            CommitReceipt rec;
            rec.event_id = r.event_id;
            rec.domain_seq = r.domain_seq;
            rec.risk_domain = r.risk_domain;
            rec.owner_epoch = r.owner_epoch;
            rec.outcome = CommitOutcome::Committed;
            rec.journal_version = r.version;
            rec.idempotency_key = r.idempotency_key;
            rec.outbox_id = r.outbox.id;
            idem_[r.idempotency_key] = rec;
            if (r.effect.outbox_kind != 0 && r.outbox.id != 0) {
                OutboxRecord ob;
                ob.id = r.outbox.id;
                ob.journal_version = r.version;
                ob.owner_epoch = r.owner_epoch;
                ob.kind = r.effect.outbox_kind;
                ob.cl_ord_id = r.effect.outbox_cl_ord_id;
                ob.orig_cl_ord_id = r.effect.outbox_orig_cl_ord_id;
                ob.symbol = r.outbox.symbol;
                ob.side = r.outbox.side;
                ob.qty = r.outbox.qty;
                ob.px = r.outbox.px;
                ob.state = OutboxState::Ready;
                outbox_pos_[ob.id] = outbox_.size();
                outbox_.push_back(ob);
                if (r.outbox.id >= next_outbox_id_) next_outbox_id_ = r.outbox.id + 1;
            }
            break;
        }
        case RecordKind::OutboxAttempt:
            if (OutboxRecord* ob_p = find_outbox_locked(r.outbox_id); ob_p != nullptr) {
                    OutboxRecord& ob = *ob_p;
                    ++ob.attempts;
                    // After a restart this is what distinguishes "never sent"
                    // from "may already be at the LP". The row does not go back
                    // to Ready: a second original send is exactly what LI-18
                    // forbids, so it comes back as Ambiguous and the
                    // reconciliation path owns it.
                    ob.state = OutboxState::Ambiguous;
            }
            break;
        case RecordKind::OutboxSent:
            if (OutboxRecord* ob_p = find_outbox_locked(r.outbox_id); ob_p != nullptr)
                ob_p->state = OutboxState::Sent;
            break;
        case RecordKind::OutboxAmbiguous:
            if (OutboxRecord* ob_p = find_outbox_locked(r.outbox_id); ob_p != nullptr)
                ob_p->state = OutboxState::Ambiguous;
            break;
        case RecordKind::EpochFence:
            if (r.epoch > epoch_) epoch_ = r.epoch;
            break;
        case RecordKind::RiskConfig: {
            DomainRisk& dr = state_.risk[r.config_domain];
            dr.domain = r.config_domain;
            dr.limit_qty = r.config_limit;
            break;
        }
    }
}

void JournalStore::rebuild_from_journal() {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = BusinessState{};
    inbox_.clear();
    seen_exec_.clear();
    idem_.clear();
    domain_seq_.clear();
    outbox_.clear();
    outbox_pos_.clear();
    next_ready_hint_ = 0;
    version_ = 0;
    next_outbox_id_ = 1;
    epoch_ = opts_.initial_epoch;
    for (const Record& r : records_) apply_record(r);
}

void JournalStore::configure_risk_domain(DomainId domain, Qty limit_qty) {
    std::lock_guard<std::mutex> lk(mu_);
    Record r;
    r.kind = RecordKind::RiskConfig;
    r.version = ++version_;
    r.config_domain = domain;
    r.config_limit = limit_qty;
    append(r);
    apply_record(r);
}

void JournalStore::fence_to_epoch(std::uint32_t epoch) {
    std::lock_guard<std::mutex> lk(mu_);
    if (epoch <= epoch_) return;
    Record r;
    r.kind = RecordKind::EpochFence;
    r.version = ++version_;
    r.epoch = epoch;
    append(r);
    apply_record(r);
}

// --- commit ----------------------------------------------------------------

CommitOutcome JournalStore::commit(const CommitRequest& req, CommitReceipt& out) {
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.commits_attempted;
    out = CommitReceipt{};
    out.event_id = req.intent.event_id;
    out.domain_seq = req.intent.domain_seq;
    out.risk_domain = req.intent.risk_domain;
    out.owner_epoch = req.intent.owner_epoch;
    out.idempotency_key = req.intent.idempotency_key;
    out.commit_begin_ns = 0;

    // Fencing first. A commit from a displaced owner is refused before anything
    // else happens, which is what keeps a late callback from mutating state
    // (LI-14, LI-47).
    if (req.intent.owner_epoch < epoch_) {
        ++stats_.stale_epoch_refusals;
        out.outcome = CommitOutcome::Rejected;
        ++stats_.commits_rejected;
        return CommitOutcome::Rejected;
    }

    // Inbox dedup. An inbound business proof that has already been committed
    // produces no second effect, whatever the session layer replayed.
    if (req.intent.write_inbox && inbox_.count(req.intent.event_id) != 0) {
        ++stats_.inbox_duplicates;
        const auto it = idem_.find(req.intent.idempotency_key);
        if (it != idem_.end()) {
            out = it->second;
            return CommitOutcome::Committed;
        }
        out.outcome = CommitOutcome::Committed;
        out.journal_version = version_;
        ++stats_.commits_committed;
        return CommitOutcome::Committed;
    }

    // Causal order. Doc 7's worked example: New(10), PartialFill(11),
    // Cancel(12) where 11's commit is slow. 12 must not commit first.
    if (opts_.enforce_domain_sequence && req.intent.domain_seq > 0) {
        const std::uint64_t last = committed_domain_seq_locked(req.intent.risk_domain);
        if (req.intent.domain_seq != last + 1) {
            ++stats_.out_of_order_refusals;
            out.outcome = CommitOutcome::Rejected;
            ++stats_.commits_rejected;
            return CommitOutcome::Rejected;
        }
    }

    // Sub-step boundaries. Nothing is applied yet: these failpoints model a
    // crash part-way through a multi-table transaction, and the oracle for
    // every one of them is that no effect becomes visible (CW-11..CW-16).
    LLX_FP(fp::point::kStoreAfterBegin);
    if (req.intent.write_inbox) LLX_FP(fp::point::kStoreAfterInbox);
    LLX_FP(fp::point::kStoreAfterJournal);
    if (req.intent.write_projection) LLX_FP(fp::point::kStoreAfterProjection);
    if (req.intent.write_risk) LLX_FP(fp::point::kStoreAfterRiskRow);
    if (req.intent.write_outbox) LLX_FP(fp::point::kStoreAfterOutbox);
    if (LLX_FP(fp::point::kStoreCommitStall) == fp::Action::ReturnError) {
        out.outcome = CommitOutcome::Rejected;
        ++stats_.commits_rejected;
        return CommitOutcome::Rejected;
    }

    Record r;
    r.kind = RecordKind::Business;
    r.version = version_ + 1;
    r.event_id = req.intent.event_id;
    r.domain_seq = req.intent.domain_seq;
    r.risk_domain = req.intent.risk_domain;
    r.owner_epoch = req.intent.owner_epoch;
    r.idempotency_key = req.intent.idempotency_key;
    r.write_inbox = req.intent.write_inbox;
    r.effect = req.effect;
    if (req.intent.write_outbox && req.effect.outbox_kind != 0) {
        r.outbox.id = next_outbox_id_;
        r.outbox.symbol = req.intent.outbox_symbol;
        r.outbox.side = req.intent.outbox_side;
        r.outbox.qty = req.intent.outbox_qty;
        r.outbox.px = req.intent.outbox_px;
    }

    if (!append(r)) {
        // The write failed and nothing was applied.
        out.outcome = CommitOutcome::Rejected;
        ++stats_.commits_rejected;
        return CommitOutcome::Rejected;
    }

    // The record is durable from here on. A failure after this point is
    // COMMIT_UNKNOWN from the caller's side, not COMMIT_FAILED: the
    // transaction did commit and the resolution is a lookup by idempotency
    // key, never a reissued business command (LI-17, CW-17).
    if (LLX_FP(fp::point::kStoreCommitLostAck) == fp::Action::ReturnError) {
        apply_record(r);
        ++stats_.commits_unknown;
        out.outcome = CommitOutcome::Unknown;
        out.journal_version = r.version;
        return CommitOutcome::Unknown;
    }

    apply_record(r);
    LLX_FP(fp::point::kStoreAfterCommit);

    out.outcome = CommitOutcome::Committed;
    out.journal_version = r.version;
    out.outbox_id = r.outbox.id;
    ++stats_.commits_committed;
    return CommitOutcome::Committed;
}

bool JournalStore::lookup_by_idempotency(std::uint64_t key, CommitReceipt& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = idem_.find(key);
    if (it == idem_.end()) return false;
    out = it->second;
    return true;
}

std::uint64_t JournalStore::committed_domain_seq(DomainId d) const {
    std::lock_guard<std::mutex> lk(mu_);
    return committed_domain_seq_locked(d);
}

std::uint64_t JournalStore::committed_domain_seq_locked(DomainId d) const {
    const auto it = domain_seq_.find(d);
    return it == domain_seq_.end() ? 0 : it->second;
}

// --- outbox ----------------------------------------------------------------

bool JournalStore::claim_next(std::uint32_t owner_epoch, OutboxRecord& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (owner_epoch < epoch_) return false;  // fenced out
    while (next_ready_hint_ < outbox_.size() &&
           outbox_[next_ready_hint_].state != OutboxState::Ready)
        ++next_ready_hint_;
    for (std::size_t i = next_ready_hint_; i < outbox_.size(); ++i) {
        OutboxRecord& ob = outbox_[i];
        if (ob.state != OutboxState::Ready) continue;
        // The attempt marker is durable before the record is handed out, so a
        // crash between the claim and the socket write cannot look like "never
        // attempted" after a restart (CW-19..CW-22).
        Record r;
        r.kind = RecordKind::OutboxAttempt;
        r.version = version_ + 1;
        r.outbox_id = ob.id;
        if (!append(r)) return false;
        version_ = r.version;
        ++ob.attempts;
        ob.state = OutboxState::Claimed;
        out = ob;
        LLX_FP(fp::point::kDispatchAfterClaim);
        return true;
    }
    return false;
}

bool JournalStore::mark_sent(std::uint64_t outbox_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (OutboxRecord* found = find_outbox_locked(outbox_id); found != nullptr) {
        OutboxRecord& ob = *found;
        Record r;
        r.kind = RecordKind::OutboxSent;
        r.version = version_ + 1;
        r.outbox_id = outbox_id;
        if (!append(r)) return false;
        version_ = r.version;
        ob.state = OutboxState::Sent;
        return true;
    }
    return false;
}

bool JournalStore::mark_ambiguous(std::uint64_t outbox_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (OutboxRecord* found = find_outbox_locked(outbox_id); found != nullptr) {
        OutboxRecord& ob = *found;
        Record r;
        r.kind = RecordKind::OutboxAmbiguous;
        r.version = version_ + 1;
        r.outbox_id = outbox_id;
        if (!append(r)) return false;
        version_ = r.version;
        ob.state = OutboxState::Ambiguous;
        return true;
    }
    return false;
}

OutboxRecord* JournalStore::find_outbox_locked(std::uint64_t id) {
    const auto it = outbox_pos_.find(id);
    if (it == outbox_pos_.end() || it->second >= outbox_.size()) return nullptr;
    return &outbox_[it->second];
}

std::vector<OutboxRecord> JournalStore::outbox_snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return outbox_;
}

}  // namespace llx
