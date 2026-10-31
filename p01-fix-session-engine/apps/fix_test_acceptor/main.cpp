// Scriptable FIX 4.4 acceptor for lab use.
//
// This is the test peer from the harness topology in design doc 17.1. It is
// deliberately small and deliberately scriptable: the fault switches below are
// how the A, C and D groups of the matrix are reproduced against the real
// client process rather than in a unit test.
//
// It is NOT a liquidity provider simulator. It does not match orders, keep an
// order book or persist anything: that is Project 02.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "fix/codec.hpp"
#include "fix/framer.hpp"
#include "fix/wire.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define FIXSE_BAD_SOCKET INVALID_SOCKET
#define FIXSE_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define FIXSE_BAD_SOCKET (-1)
#define FIXSE_CLOSE ::close
#endif

using namespace fixse;

namespace {

struct Options {
    uint16_t port = 9878;
    std::string sender = "SIMLP01";
    std::string target = "TRADER01";
    int heartbeat = 30;
    bool suppress_logon = false;
    bool logout_on_logon = false;
    bool wrong_sender = false;
    bool unsolicited_reset = false;
    uint64_t logon_seq_ahead = 0;
    int drop_after = 0;
    size_t byte_drip = 0;
    uint64_t start_seq = 1;
    bool verbose = true;
};

void usage() {
    std::printf(
        "usage: fix_test_acceptor [options]\n"
        "\n"
        "  --port <n>              listen port, default 9878\n"
        "  --sender <id>           our SenderCompID, default SIMLP01\n"
        "  --target <id>           expected client CompID, default TRADER01\n"
        "  --heartbeat <n>         HeartBtInt echoed in the Logon, default 30\n"
        "  --start-seq <n>         our first outbound sequence, default 1\n"
        "\n"
        "fault switches, lab only:\n"
        "  --suppress-logon        never answer the Logon            (A-06, C-02)\n"
        "  --logout-on-logon       answer the Logon with a Logout     (C-03)\n"
        "  --wrong-sender          answer with a bad SenderCompID     (C-04)\n"
        "  --logon-seq-ahead <n>   answer from n sequences ahead      (C-11)\n"
        "  --unsolicited-reset     set ResetSeqNumFlag in the Logon   (C-14, J-07)\n"
        "  --drop-after <n>        close after n received frames      (A-08)\n"
        "  --byte-drip <n>         send n bytes per write             (A-13, J-05)\n");
}

class Peer {
public:
    explicit Peer(const Options& o) : opt_(o), framer_(65536, 262144) {}

    std::string build(const std::string& type, const std::vector<std::pair<int, std::string>>& body,
                      bool poss_dup = false) {
        OutboundFields f;
        f.msg_type = type;
        f.seq_num = next_out_++;
        f.sender_comp_id = opt_.wrong_sender ? "IMPOSTOR" : opt_.sender;
        f.target_comp_id = opt_.target;
        f.sending_time = std::chrono::system_clock::now();
        f.poss_dup = poss_dup;
        if (poss_dup) f.orig_sending_time = format_utc_timestamp(f.sending_time);
        f.body = body;
        return encode(f);
    }

    void send(socket_t s, const std::string& frame) {
        size_t sent = 0;
        while (sent < frame.size()) {
            const size_t want =
                opt_.byte_drip == 0 ? frame.size() - sent
                                    : std::min(opt_.byte_drip, frame.size() - sent);
            const long n = static_cast<long>(
                ::send(s, frame.data() + sent, static_cast<int>(want), 0));
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
        if (opt_.verbose)
            std::printf("out %s\n", printable(frame).c_str());
    }

    // Returns false when the connection should be closed.
    bool on_frame(socket_t s, std::string_view raw) {
        ++frames_seen_;
        DecodeOptions dopt;
        DecodedMessage msg;
        const WireError e = decode(raw, dopt, &msg);
        if (opt_.verbose)
            std::printf("in  %s%s\n", printable(raw).c_str(),
                        e == WireError::None ? "" : " [REJECTED]");
        if (e != WireError::None) return true;

        if (opt_.drop_after > 0 && frames_seen_ >= opt_.drop_after) {
            std::printf("closing the connection after %d frames\n", frames_seen_);
            return false;
        }

        if (msg.msg_type == msg_type::Logon) {
            if (opt_.suppress_logon) return true;
            if (opt_.logout_on_logon) {
                send(s, build(std::string(msg_type::Logout), {{tag::Text, "not today"}}));
                return false;
            }
            next_out_ += opt_.logon_seq_ahead;
            std::vector<std::pair<int, std::string>> body{
                {tag::EncryptMethod, "0"}, {tag::HeartBtInt, std::to_string(opt_.heartbeat)}};
            if (opt_.unsolicited_reset) body.emplace_back(tag::ResetSeqNumFlag, "Y");
            send(s, build(std::string(msg_type::Logon), body));
            return true;
        }
        if (msg.msg_type == msg_type::TestRequest) {
            const auto id = msg.find(tag::TestReqID);
            if (id.has_value())
                send(s, build(std::string(msg_type::Heartbeat),
                              {{tag::TestReqID, std::string(*id)}}));
            return true;
        }
        if (msg.msg_type == msg_type::ResendRequest) {
            // This peer keeps no history, so it answers with a GapFill over the
            // requested range. That is valid session behaviour for a range of
            // administrative messages and nothing more: it does NOT stand in
            // for a peer that can replay application messages.
            uint64_t begin = 0;
            msg.find_u64(tag::BeginSeqNo, &begin);
            const uint64_t saved = next_out_;
            next_out_ = begin == 0 ? 1 : begin;
            const std::string gap =
                build(std::string(msg_type::SequenceReset),
                      {{tag::GapFillFlag, "Y"}, {tag::NewSeqNo, std::to_string(saved)}}, true);
            next_out_ = saved;
            send(s, gap);
            return true;
        }
        if (msg.msg_type == msg_type::Logout) {
            send(s, build(std::string(msg_type::Logout), {{tag::Text, "acknowledged"}}));
            return false;
        }
        return true;
    }

    FixFramer& framer() { return framer_; }

private:
    Options opt_;
    FixFramer framer_;
    uint64_t next_out_ = 1;
    int frames_seen_ = 0;

public:
    void set_start_seq(uint64_t s) { next_out_ = s; }
};

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = i + 1 < argc;
        if (a == "--port" && has_next) opt.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--sender" && has_next) opt.sender = argv[++i];
        else if (a == "--target" && has_next) opt.target = argv[++i];
        else if (a == "--heartbeat" && has_next) opt.heartbeat = std::atoi(argv[++i]);
        else if (a == "--start-seq" && has_next) opt.start_seq = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--suppress-logon") opt.suppress_logon = true;
        else if (a == "--logout-on-logon") opt.logout_on_logon = true;
        else if (a == "--wrong-sender") opt.wrong_sender = true;
        else if (a == "--unsolicited-reset") opt.unsolicited_reset = true;
        else if (a == "--logon-seq-ahead" && has_next)
            opt.logon_seq_ahead = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--drop-after" && has_next) opt.drop_after = std::atoi(argv[++i]);
        else if (a == "--byte-drip" && has_next)
            opt.byte_drip = static_cast<size_t>(std::atoll(argv[++i]));
        else if (a == "--quiet") opt.verbose = false;
        else {
            usage();
            return 1;
        }
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    const socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == FIXSE_BAD_SOCKET) {
        std::fprintf(stderr, "cannot create a listening socket\n");
        return 2;
    }
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opt.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // lab only: never a public interface
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "cannot bind 127.0.0.1:%u\n", static_cast<unsigned>(opt.port));
        return 3;
    }
    if (::listen(listener, 4) != 0) {
        std::fprintf(stderr, "cannot listen\n");
        return 4;
    }
    std::printf("fix_test_acceptor listening on 127.0.0.1:%u as %s, expecting %s\n",
                static_cast<unsigned>(opt.port), opt.sender.c_str(), opt.target.c_str());

    for (;;) {
        const socket_t conn = ::accept(listener, nullptr, nullptr);
        if (conn == FIXSE_BAD_SOCKET) break;
        std::printf("client connected\n");

        Peer peer(opt);
        peer.set_start_seq(opt.start_seq);
        char chunk[8192];
        bool keep = true;
        while (keep) {
            const long n =
                static_cast<long>(::recv(conn, chunk, static_cast<int>(sizeof(chunk)), 0));
            if (n <= 0) break;
            if (peer.framer().append(std::string_view(chunk, static_cast<size_t>(n))) !=
                WireError::None) {
                std::printf("receive buffer limit reached, closing\n");
                break;
            }
            std::string_view frame;
            WireError err = WireError::None;
            while (peer.framer().next(&frame, &err)) {
                keep = peer.on_frame(conn, frame);
                if (!keep) break;
            }
            if (is_malformed(err)) {
                std::printf("framing error from the client: %s, closing\n",
                            std::string(to_string(err)).c_str());
                break;
            }
        }
        FIXSE_CLOSE(conn);
        std::printf("client disconnected\n");
    }

    FIXSE_CLOSE(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
