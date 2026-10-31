// P02 -- the LP simulator, deliberately an independent implementation.
//
// Design doc 6.2 and 22.1 both make the same point: the oracle must not share a
// reducer with P03 or a parser with P01, because a shared bug would turn into a
// false PASS. So this file has its own tiny field splitter, its own order book
// and its own report encoder. Nothing here includes fix44.hpp.
//
// Two ways to drive it:
//   * auto mode, where the simulator behaves like a well-mannered LP and keeps
//     a book that a test can assert against (PB-02: "P02 co mot order O1");
//   * ReportSpec, where a test dictates the exact report, including malformed
//     ones, so the J, K and D series can inject precisely one defect at a time.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "deps/p01_session.hpp"
#include "router/ids.hpp"

namespace router::p02 {

// One report, described field by field. Defaults are a well-formed 35=8.
struct ReportSpec {
    char msg_type = '8';
    ExecType exec_type = ExecType::New;
    OrdStatus ord_status = OrdStatus::New;

    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string order_id = "LP-1";
    std::string exec_id = "EX-1";
    std::string exec_ref_id;
    std::string account = "ACC1";
    std::string symbol = "EUR/USD";
    Side side = Side::Buy;

    std::optional<std::int64_t> last_qty;
    std::optional<std::int64_t> last_px;
    std::int64_t cum_qty = 0;
    std::int64_t leaves_qty = 0;
    std::int64_t avg_px = 0;
    std::optional<std::int64_t> order_qty;
    int price_scale = 5;

    std::optional<CxlRejResponseTo> cxl_rej_response_to;
    std::optional<int> cxl_rej_reason;
    std::string text;

    bool poss_dup = false;
    bool poss_resend = false;
    std::optional<std::string> ord_status_req_id;

    // Escape hatches for the adversarial cases: `raw` replaces a tag value
    // verbatim (so "1.085e0" or "1,0850" can be injected), `omit` drops a tag
    // that would otherwise be written, and `extra` appends tags.
    std::map<int, std::string> raw;
    std::set<int> omit;
    std::vector<std::pair<int, std::string>> extra;

    // Encodes the application field block. Independent of router::fix on
    // purpose (design doc 22.1).
    std::string encode() const;
};

// What the simulator knows about an order. Its own model, with its own
// arithmetic, so a disagreement with P03 is a real finding.
struct LpOrder {
    std::string cl_ord_id;       // currently accepted client id
    std::string first_cl_ord_id; // the original D, for book identity
    std::string order_id;
    std::string account;
    std::string symbol;
    Side side = Side::Buy;
    std::int64_t qty = 0;
    std::int64_t price = 0;
    std::int64_t cum_qty = 0;
    std::int64_t leaves_qty = 0;
    std::int64_t weighted_px = 0;
    std::string status = "New";  // free text on purpose: not P03's enum
    bool pending_cancel = false;
    bool pending_replace = false;
    std::string pending_replace_id;
    std::int64_t pending_qty = 0;
    std::int64_t pending_price = 0;
};

struct LpImage {
    std::map<std::string, LpOrder> book;            // keyed by first ClOrdID
    std::map<std::string, std::string> id_to_order; // any ClOrdID -> book key
    std::uint64_t next_order_id = 1;
    std::uint64_t next_exec_id = 1;

    bool capability_status_request = false;  // 35=H, off until explicitly enabled
    bool auto_ack = true;                    // acknowledge a D with 8/150=0
    bool auto_pending_cancel = true;
    bool auto_pending_replace = true;
    bool suppress_reports = false;           // accept the order, say nothing
    bool reject_status_requests = false;     // BusinessMessageReject behaviour

    std::vector<std::string> received_frames;
    std::vector<std::string> rejected_frames;  // unparseable or unsupported
    std::size_t status_requests_seen = 0;
};

class LpSimulator final : public p01::IPeer {
public:
    LpSimulator(LpImage& image, p01::SessionEngine& session);

    // Receives an application frame from the session. In auto mode this updates
    // the book and emits the reports a real LP would.
    void on_app_frame(const std::string& frame) override;

    // Hands a report to the session, which makes it a durable inbound receipt.
    DurableReceiptId send(const ReportSpec& spec);

    // Convenience builders that fill the spec from the book, so a scenario only
    // has to say what it wants to differ.
    ReportSpec new_ack(const std::string& cl_ord_id) const;
    ReportSpec trade(const std::string& cl_ord_id, const std::string& exec_id, std::int64_t qty,
                     std::int64_t px) const;
    ReportSpec pending_cancel(const std::string& cancel_id, const std::string& orig) const;
    ReportSpec canceled(const std::string& cancel_id, const std::string& orig) const;
    ReportSpec pending_replace(const std::string& replace_id, const std::string& orig) const;
    ReportSpec replaced(const std::string& replace_id, const std::string& orig,
                        std::int64_t new_qty, std::int64_t new_px) const;
    ReportSpec rejected(const std::string& cl_ord_id, const std::string& reason) const;
    ReportSpec cancel_reject(const std::string& cl_ord_id, const std::string& orig,
                             CxlRejResponseTo kind, int reason) const;
    ReportSpec status_snapshot(const std::string& cl_ord_id,
                               const std::optional<std::string>& query_id) const;

    // Executes against the book: applies the fill to P02's own model and emits
    // the corresponding Trade. This is how an end-to-end test gets a fill
    // without hand-writing quantities.
    DurableReceiptId execute(const std::string& cl_ord_id, std::int64_t qty, std::int64_t px);
    DurableReceiptId accept_cancel(const std::string& cancel_id);
    DurableReceiptId accept_replace(const std::string& replace_id);

    const LpOrder* find(const std::string& any_cl_ord_id) const;
    std::size_t order_count() const { return image_.book.size(); }
    LpImage& image() { return image_; }
    const LpImage& image() const { return image_; }

private:
    // The simulator's own field splitter. Lenient where P04 is strict, because
    // its job is to model a peer, not to validate one.
    static std::map<int, std::string> split(const std::string& frame);

    LpImage& image_;
    p01::SessionEngine& session_;
};

}  // namespace router::p02
