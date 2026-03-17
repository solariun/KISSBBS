// =============================================================================
// test_ax25lib.cpp — Unit tests for ax25lib using GoogleTest
//
// Compile & run:
//   make test
//
// Or manually:
//   g++ -std=c++17 -O2 -o test_ax25lib test_ax25lib.cpp ax25lib.cpp \
//       $(pkg-config --cflags --libs gtest_main)
//   ./test_ax25lib
//
// NOTE: ax25lib itself is C++11.  The test binary requires -std=c++17 only
// because modern GoogleTest (>=1.13) uses C++17 internally.  Your production
// code that links ax25lib can be compiled with -std=c++11.
// =============================================================================
#include "ax25lib.hpp"
#include "ax25dump.hpp"
#include "ini.hpp"
#include "basic.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <vector>

using namespace ax25;

// ─────────────────────────────────────────────────────────────────────────────
// VirtualWire — deferred delivery queue connecting two Kiss objects.
//
// Why deferred?  The synchronous wire causes re-entrancy in flush_window():
//   send I-frame → wire → peer sends RR → wire → process_nr() → flush_window()
//   ...all while the original flush_window() is still executing, before it
//   pushes the frame to unacked_.  The deferred queue breaks that cycle by
//   delivering each frame only after the current send_frame() call returns.
// ─────────────────────────────────────────────────────────────────────────────
class VirtualWire {
public:
    Kiss& a;
    Kiss& b;

    explicit VirtualWire(Kiss& ka, Kiss& kb) : a(ka), b(kb) {
        a.on_send_hook = [this](const std::vector<uint8_t>& f) {
            queue_.push_back({&b, f});
            flush();
            return true;
        };
        b.on_send_hook = [this](const std::vector<uint8_t>& f) {
            queue_.push_back({&a, f});
            flush();
            return true;
        };
    }

    // Manually deliver all pending frames (useful for deferred-start tests)
    void flush() {
        if (flushing_) return;
        flushing_ = true;
        while (!queue_.empty()) {
            Kiss* tgt = queue_.front().target;
            std::vector<uint8_t> frm = std::move(queue_.front().frame);
            queue_.erase(queue_.begin());
            tgt->test_inject(frm);
        }
        flushing_ = false;
    }

private:
    struct Pending { Kiss* target; std::vector<uint8_t> frame; };
    std::vector<Pending> queue_;
    bool flushing_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a minimal Config
// ─────────────────────────────────────────────────────────────────────────────
static Config make_cfg(const std::string& call) {
    Config c;
    c.mycall  = Addr::make(call);
    c.t1_ms   = 500;
    c.t3_ms   = 5000;
    c.n2      = 3;
    c.txdelay = 0;    // no TX pacing in tests (synchronous wire)
    return c;
}

// =============================================================================
// 1. Intrusive List — ObjNode<T> / ObjList<T>
//
// ObjNode<T> is the new self-managing design:
//   • default constructor DELETED — must bind to an ObjList<T>
//   • ObjNode(ObjList<T>&) constructor auto-inserts into the list
//   • ~ObjNode() auto-removes from the list
//   • developers never call push_back / remove manually
// =============================================================================

/// Test fixture object.  Takes the list in its constructor (mandatory).
struct Item : ObjNode<Item> {
    int val;
    Item(ObjList<Item>& l, int v) : ObjNode<Item>(l), val(v) {}
};

TEST(ObjList, AutoInsertOnConstruction) {
    ObjList<Item> lst;
    EXPECT_TRUE(lst.empty());
    EXPECT_EQ(lst.size(), 0u);

    Item a(lst, 1), b(lst, 2), c(lst, 3);
    EXPECT_EQ(lst.size(), 3u);
    EXPECT_FALSE(lst.empty());
}

TEST(ObjList, AutoRemoveOnDestruction) {
    ObjList<Item> lst;
    Item a(lst, 10);
    {
        Item b(lst, 20);
        Item c(lst, 30);
        EXPECT_EQ(lst.size(), 3u);
    }   // b and c destroyed → auto-removed (reverse order: c then b)
    EXPECT_EQ(lst.size(), 1u);
    EXPECT_EQ(lst.begin()->val, 10);
}

TEST(ObjList, IterationOrder) {
    ObjList<Item> lst;
    Item a(lst, 10), b(lst, 20), c(lst, 30);

    std::vector<int> vals;
    for (auto& x : lst) vals.push_back(x.val);
    EXPECT_EQ(vals, (std::vector<int>{10, 20, 30}));
}

TEST(ObjList, RemoveMiddleViaDelete) {
    ObjList<Item> lst;
    Item a(lst, 1);
    auto* b = new Item(lst, 2);
    Item c(lst, 3);

    EXPECT_EQ(lst.size(), 3u);
    delete b;                   // ← destructor auto-removes from list
    EXPECT_EQ(lst.size(), 2u);

    std::vector<int> vals;
    for (auto& x : lst) vals.push_back(x.val);
    EXPECT_EQ(vals, (std::vector<int>{1, 3}));
}

TEST(ObjList, RemoveHeadViaDelete) {
    ObjList<Item> lst;
    auto* a = new Item(lst, 1);
    Item b(lst, 2);

    delete a;

    std::vector<int> vals;
    for (auto& x : lst) vals.push_back(x.val);
    EXPECT_EQ(vals, (std::vector<int>{2}));
}

TEST(ObjList, RemoveTailViaDelete) {
    ObjList<Item> lst;
    Item a(lst, 1);
    auto* b = new Item(lst, 2);

    delete b;

    std::vector<int> vals;
    for (auto& x : lst) vals.push_back(x.val);
    EXPECT_EQ(vals, (std::vector<int>{1}));
}

TEST(ObjList, ScopedLifetimeManagement) {
    // This IS the idiomatic ObjNode pattern: scope controls list membership.
    ObjList<Item> lst;
    {
        Item x(lst, 1), y(lst, 2), z(lst, 3);
        EXPECT_EQ(lst.size(), 3u);
        {
            Item w(lst, 4);
            EXPECT_EQ(lst.size(), 4u);
        }                           // w destroyed → auto-removed
        EXPECT_EQ(lst.size(), 3u);
    }                               // x, y, z destroyed
    EXPECT_EQ(lst.size(), 0u);
    EXPECT_TRUE(lst.empty());
}

TEST(ObjList, Snapshot) {
    ObjList<Item> lst;
    Item a(lst, 1), b(lst, 2), c(lst, 3);

    auto snap = lst.snapshot();
    ASSERT_EQ(snap.size(), 3u);
    EXPECT_EQ(snap[0]->val, 1);
    EXPECT_EQ(snap[1]->val, 2);
    EXPECT_EQ(snap[2]->val, 3);
}

// =============================================================================
// 2. Addr — parse, encode, decode
// =============================================================================
TEST(Addr, MakeNoSSID) {
    auto a = Addr::make("N0CALL");
    EXPECT_STREQ(a.call, "N0CALL");
    EXPECT_EQ(a.ssid, 0);
    EXPECT_EQ(a.str(), "N0CALL");
}

TEST(Addr, MakeWithSSID) {
    auto a = Addr::make("W1AW-7");
    EXPECT_STREQ(a.call, "W1AW");
    EXPECT_EQ(a.ssid, 7);
    EXPECT_EQ(a.str(), "W1AW-7");
}

TEST(Addr, MakeLowerCase) {
    auto a = Addr::make("py2xxx-3");
    EXPECT_STREQ(a.call, "PY2XXX");
    EXPECT_EQ(a.ssid, 3);
}

TEST(Addr, EncodeLengthIs7) {
    auto a = Addr::make("W1AW-5");
    EXPECT_EQ(a.encode(false).size(), 7u);
    EXPECT_EQ(a.encode(true).size(), 7u);
}

TEST(Addr, EncodeLastBit) {
    auto a = Addr::make("N0CALL");
    EXPECT_EQ(a.encode(false)[6] & 0x01, 0);
    EXPECT_EQ(a.encode(true)[6]  & 0x01, 1);
}

TEST(Addr, EncodeThenDecode) {
    auto orig = Addr::make("PY2XXX-9");
    auto raw  = orig.encode(false);
    auto back = Addr::decode(raw.data());
    EXPECT_EQ(orig, back);
    EXPECT_EQ(back.str(), "PY2XXX-9");
}

TEST(Addr, EncodeDecodeSsid0) {
    auto orig = Addr::make("APRS");
    auto raw  = orig.encode(true);
    auto back = Addr::decode(raw.data());
    EXPECT_EQ(orig, back);
    EXPECT_STREQ(back.call, "APRS");
    EXPECT_EQ(back.ssid, 0);
}

TEST(Addr, Equality) {
    EXPECT_EQ(Addr::make("W1AW-1"), Addr::make("W1AW-1"));
    EXPECT_NE(Addr::make("W1AW-1"), Addr::make("W1AW-2"));
    EXPECT_NE(Addr::make("W1AW"),   Addr::make("N0CALL"));
}

// =============================================================================
// 3. KISS codec
// =============================================================================
using KD = kiss::Decoder;

TEST(KissEncode, WrapWithFEND) {
    std::vector<uint8_t> d{0x01, 0x02, 0x03};
    auto f = kiss::encode(d);
    EXPECT_EQ(f.front(), kiss::FEND);
    EXPECT_EQ(f.back(),  kiss::FEND);
}

TEST(KissEncode, CommandByte) {
    std::vector<uint8_t> d{0xAA};
    auto f = kiss::encode(d, kiss::Cmd::Data, 0);
    EXPECT_EQ(f[1], 0x00);   // port=0, cmd=Data
    auto g = kiss::encode(d, kiss::Cmd::TxDelay, 2);
    EXPECT_EQ(g[1], 0x21);   // port=2, cmd=1
}

TEST(KissEncode, ByteStuffFEND) {
    // FEND in payload → FESC TFEND
    std::vector<uint8_t> d{kiss::FEND};
    auto f = kiss::encode(d);
    // f = [ FEND, cmd, FESC, TFEND, FEND ] = 5 bytes
    EXPECT_EQ(f.size(), 5u);
    EXPECT_EQ(f[2], kiss::FESC);
    EXPECT_EQ(f[3], kiss::TFEND);
}

TEST(KissEncode, ByteStuffFESC) {
    // FESC in payload → FESC TFESC
    std::vector<uint8_t> d{kiss::FESC};
    auto f = kiss::encode(d);
    EXPECT_EQ(f.size(), 5u);
    EXPECT_EQ(f[2], kiss::FESC);
    EXPECT_EQ(f[3], kiss::TFESC);
}

TEST(KissDecode, SimpleFrame) {
    std::vector<uint8_t> payload{0x11, 0x22, 0x33};
    auto frame = kiss::encode(payload);
    KD dec;
    auto out = dec.feed(frame.data(), frame.size());
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].command, kiss::Cmd::Data);
    EXPECT_EQ(out[0].data, payload);
}

TEST(KissDecode, ByteStuffRoundtrip) {
    std::vector<uint8_t> payload{kiss::FEND, 0x42, kiss::FESC, 0xFF};
    auto frame = kiss::encode(payload);
    KD dec;
    auto out = dec.feed(frame.data(), frame.size());
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].data, payload);
}

TEST(KissDecode, SplitByteByByte) {
    std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF};
    auto frame = kiss::encode(payload);
    KD dec;
    std::vector<kiss::Frame> out;
    for (uint8_t b : frame) {
        auto v = dec.feed(&b, 1);
        out.insert(out.end(), v.begin(), v.end());
    }
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].data, payload);
}

TEST(KissDecode, MultipleFrames) {
    std::vector<uint8_t> p1{0x01}, p2{0x02}, p3{0x03};
    auto f1 = kiss::encode(p1);
    auto f2 = kiss::encode(p2);
    auto f3 = kiss::encode(p3);
    std::vector<uint8_t> stream;
    stream.insert(stream.end(), f1.begin(), f1.end());
    stream.insert(stream.end(), f2.begin(), f2.end());
    stream.insert(stream.end(), f3.begin(), f3.end());

    KD dec;
    auto out = dec.feed(stream.data(), stream.size());
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].data, p1);
    EXPECT_EQ(out[1].data, p2);
    EXPECT_EQ(out[2].data, p3);
}

TEST(KissDecode, EmptyFrameSkipped) {
    // FEND FEND = empty frame, should be ignored
    std::vector<uint8_t> stream{kiss::FEND, kiss::FEND};
    std::vector<uint8_t> payload{0xAA};
    auto real = kiss::encode(payload);
    stream.insert(stream.end(), real.begin(), real.end());

    KD dec;
    auto out = dec.feed(stream.data(), stream.size());
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].data, payload);
}

// =============================================================================
// 4. AX.25 Frame — encode / decode / type detection
// =============================================================================
static Frame make_ui(const std::string& src, const std::string& dst,
                     const std::string& info)
{
    Frame f;
    f.dest    = Addr::make(dst);
    f.src     = Addr::make(src);
    f.ctrl    = 0x03;   // UI, P=0
    f.pid     = 0xF0;
    f.has_pid = true;
    f.info    = std::vector<uint8_t>(info.begin(), info.end());
    return f;
}

TEST(AX25Frame, UIRoundtrip) {
    auto orig = make_ui("W1AW-1", "APRS", "Hello APRS!");
    auto raw  = orig.encode();
    Frame back;
    ASSERT_TRUE(Frame::decode(raw, back));
    EXPECT_EQ(back.type(), Frame::Type::UI);
    EXPECT_EQ(back.src,  orig.src);
    EXPECT_EQ(back.dest, orig.dest);
    EXPECT_EQ(back.info, orig.info);
    EXPECT_EQ(back.pid,  0xF0u);
}

TEST(AX25Frame, UITypeDetection) {
    Frame f; f.ctrl = 0x03;   EXPECT_EQ(f.type(), Frame::Type::UI);
    Frame g; g.ctrl = 0x13;   EXPECT_EQ(g.type(), Frame::Type::UI);  // P=1
}

TEST(AX25Frame, SABMTypeDetection) {
    Frame f; f.ctrl = 0x2F;   EXPECT_EQ(f.type(), Frame::Type::SABM);
    Frame g; g.ctrl = 0x3F;   EXPECT_EQ(g.type(), Frame::Type::SABM);  // P=1
    EXPECT_TRUE(g.get_pf());
    EXPECT_FALSE(f.get_pf());
}

TEST(AX25Frame, UATypeDetection) {
    Frame f; f.ctrl = 0x63;   EXPECT_EQ(f.type(), Frame::Type::UA);
    Frame g; g.ctrl = 0x73;   EXPECT_EQ(g.type(), Frame::Type::UA);   // F=1
}

TEST(AX25Frame, DISCTypeDetection) {
    Frame f; f.ctrl = 0x43;   EXPECT_EQ(f.type(), Frame::Type::DISC);
    Frame g; g.ctrl = 0x53;   EXPECT_EQ(g.type(), Frame::Type::DISC); // P=1
}

TEST(AX25Frame, DMTypeDetection) {
    Frame f; f.ctrl = 0x0F;   EXPECT_EQ(f.type(), Frame::Type::DM);
}

TEST(AX25Frame, RRTypeDetection) {
    Frame f; f.ctrl = 0x01;   EXPECT_EQ(f.type(), Frame::Type::RR);
    Frame g; g.ctrl = static_cast<uint8_t>(0x01 | (3 << 5)); // N(R)=3
    EXPECT_EQ(g.get_nr(), 3);
}

TEST(AX25Frame, IFrameNS_NR) {
    Frame f;
    f.dest = Addr::make("W1AW"); f.src = Addr::make("N0CALL");
    // N(S)=5, N(R)=3, P=0
    f.ctrl    = static_cast<uint8_t>((5 << 1) | (3 << 5));  // 0x6A
    f.pid     = 0xF0;
    f.has_pid = true;
    f.info    = {0x55};

    EXPECT_EQ(f.type(), Frame::Type::IFrame);
    EXPECT_EQ(f.get_ns(), 5);
    EXPECT_EQ(f.get_nr(), 3);
    EXPECT_FALSE(f.get_pf());

    auto raw  = f.encode();
    Frame back;
    ASSERT_TRUE(Frame::decode(raw, back));
    EXPECT_EQ(back.get_ns(), 5);
    EXPECT_EQ(back.get_nr(), 3);
}

TEST(AX25Frame, WithDigipeaters) {
    Frame orig;
    orig.dest    = Addr::make("APRS");
    orig.src     = Addr::make("W1AW");
    orig.digis   = {Addr::make("WIDE1-1"), Addr::make("WIDE2-1")};
    orig.ctrl    = 0x03;
    orig.pid     = 0xF0;
    orig.has_pid = true;
    orig.info    = {'T','e','s','t'};

    auto raw  = orig.encode();
    Frame back;
    ASSERT_TRUE(Frame::decode(raw, back));
    ASSERT_EQ(back.digis.size(), 2u);
    EXPECT_EQ(back.digis[0], Addr::make("WIDE1-1"));
    EXPECT_EQ(back.digis[1], Addr::make("WIDE2-1"));
}

TEST(AX25Frame, TooShortReturnsNullopt) {
    std::vector<uint8_t> tiny(5, 0);
    Frame dummy;
    EXPECT_FALSE(Frame::decode(tiny, dummy));
}

// =============================================================================
// 5. Router + Connection state machine (virtual wire, no serial port)
// =============================================================================

// Build two routers wired together in memory via VirtualWire
struct VirtualNet {
    Kiss        kiss_a, kiss_b;
    Router      router_a, router_b;
    VirtualWire wire;

    VirtualNet()
        : router_a(kiss_a, make_cfg("W1AW"))
        , router_b(kiss_b, make_cfg("N0CALL"))
        , wire(kiss_a, kiss_b)
    {}
};

TEST(RouterConnection, ConnectAndDisconnect) {
    VirtualNet net;

    // NOTE: on_connect for the *initiating* side (conn_a) fires synchronously
    // inside connect() via the VirtualWire deferred queue, before the caller
    // can set the callback.  We therefore verify connection via state(), not a
    // callback flag.  The *accepting* side sets callbacks in on_accept before
    // handle_frame fires on_connect, so its flag works fine.
    bool b_connected = false, b_disconnected = false;
    bool a_disconnected = false;
    Connection* accepted = nullptr;

    net.router_b.listen([&](Connection* c) {
        accepted = c;
        c->on_connect    = [&]{ b_connected    = true; };
        c->on_disconnect = [&]{ b_disconnected = true; };
    });

    // Handshake happens synchronously through VirtualWire
    auto* conn_a = net.router_a.connect(Addr::make("N0CALL"));
    conn_a->on_disconnect = [&]{ a_disconnected = true; };

    EXPECT_EQ(conn_a->state(), Connection::State::CONNECTED);
    ASSERT_NE(accepted, nullptr);
    EXPECT_EQ(accepted->state(), Connection::State::CONNECTED);
    EXPECT_TRUE(b_connected);

    // A disconnects
    conn_a->disconnect();
    EXPECT_EQ(conn_a->state(), Connection::State::DISCONNECTED);
    EXPECT_TRUE(a_disconnected);
    EXPECT_TRUE(b_disconnected);

    delete conn_a;
    delete accepted;
}

TEST(RouterConnection, DataTransfer) {
    VirtualNet net;

    Connection* accepted = nullptr;
    std::string received_by_b;

    net.router_b.listen([&](Connection* c) {
        accepted = c;
        c->on_data = [&](const uint8_t* d, std::size_t n) {
            received_by_b.append((const char*)d, n);
        };
    });

    auto* conn_a = net.router_a.connect(Addr::make("N0CALL"));
    ASSERT_EQ(conn_a->state(), Connection::State::CONNECTED);

    // A sends data to B
    std::string msg = "Hello from W1AW!";
    conn_a->send(msg);

    // Data flows synchronously through the wire
    EXPECT_EQ(received_by_b, msg);

    delete conn_a;
    delete accepted;
}

TEST(RouterConnection, DataBidirectional) {
    VirtualNet net;

    Connection* accepted = nullptr;
    std::string recv_a, recv_b;

    net.router_b.listen([&](Connection* c) {
        accepted = c;
        c->on_data = [&](const uint8_t* d, std::size_t n) {
            recv_b.append((const char*)d, n);
        };
    });

    auto* conn_a = net.router_a.connect(Addr::make("N0CALL"));
    conn_a->on_data = [&](const uint8_t* d, std::size_t n) {
        recv_a.append((const char*)d, n);
    };

    conn_a->send("A→B");
    accepted->send("B→A");

    EXPECT_EQ(recv_b, "A→B");
    EXPECT_EQ(recv_a, "B→A");

    delete conn_a;
    delete accepted;
}

TEST(RouterConnection, LargeDataChunked) {
    VirtualNet net;
    net.router_a.config().mtu = 32;  // small MTU to force chunking

    Connection* accepted = nullptr;
    std::string recv_b;

    net.router_b.listen([&](Connection* c) {
        accepted = c;
        c->on_data = [&](const uint8_t* d, std::size_t n) {
            recv_b.append((const char*)d, n);
        };
    });

    auto* conn_a = net.router_a.connect(Addr::make("N0CALL"));

    // 200 bytes → 7 chunks of 32 bytes (last chunk = 200 - 6*32 = 8 bytes)
    std::string big(200, 'X');
    conn_a->send(big);

    EXPECT_EQ(recv_b, big);

    delete conn_a;
    delete accepted;
}

TEST(RouterConnection, RejectedWhenNotListening) {
    VirtualNet net;
    // B does NOT listen → should send DM back to A.
    // DM fires synchronously inside connect(), before on_disconnect can be set,
    // so we verify via state() only.
    auto* conn_a = net.router_a.connect(Addr::make("N0CALL"));
    EXPECT_EQ(conn_a->state(), Connection::State::DISCONNECTED);

    delete conn_a;
}

// Router receives a non-SABM U-frame (DISC) for a connection it knows nothing
// about.  It should respond with DM so the sender stops retrying.
// Regression test for the "orphan frame" fix (commit 1b8442e).
TEST(RouterConnection, DMForOrphanNonSABM) {
    Kiss kiss_b;
    Router router_b(kiss_b, make_cfg("N0CALL"));

    // test_inject / on_send_hook both work with raw AX.25 bytes (no KISS framing)
    std::vector<uint8_t> b_sent_ax25;
    kiss_b.on_send_hook = [&](const std::vector<uint8_t>& raw) {
        b_sent_ax25 = raw;
        return true;
    };

    // Build DISC frame: W1AW → N0CALL, ctrl=0x43
    Frame disc;
    disc.dest    = Addr::make("N0CALL");
    disc.src     = Addr::make("W1AW");
    disc.ctrl    = 0x43;   // DISC
    disc.has_pid = false;

    // Inject raw AX.25 bytes directly (test_inject bypasses KISS framing)
    kiss_b.test_inject(disc.encode());

    ASSERT_FALSE(b_sent_ax25.empty()) << "router_b must reply to orphan DISC";

    Frame resp;
    ASSERT_TRUE(Frame::decode(b_sent_ax25, resp)) << "response must be valid AX.25";
    EXPECT_EQ(resp.type(), Frame::Type::DM)
        << "router_b must send DM for orphan non-SABM, got ctrl=0x"
        << std::hex << (int)(resp.ctrl & ~0x10u);
    EXPECT_EQ(resp.dest, Addr::make("W1AW"))   << "DM dest must be the originator";
    EXPECT_EQ(resp.src,  Addr::make("N0CALL")) << "DM src must be our callsign";
}

TEST(RouterConnection, IncomingConnectionCorrectAddresses) {
    VirtualNet net;
    Connection* accepted = nullptr;

    net.router_b.listen([&](Connection* c) { accepted = c; });
    auto* conn_a = net.router_a.connect(Addr::make("N0CALL"));

    ASSERT_NE(accepted, nullptr);
    EXPECT_EQ(accepted->local(),  Addr::make("N0CALL"));
    EXPECT_EQ(accepted->remote(), Addr::make("W1AW"));

    delete conn_a;
    delete accepted;
}

// =============================================================================
// 6. Router UI / APRS send (no connection)
// =============================================================================
TEST(RouterUI, SendUI) {
    Kiss kiss_a, kiss_b;
    Router router_a(kiss_a, make_cfg("W1AW"));
    Router router_b(kiss_b, make_cfg("N0CALL"));
    VirtualWire vwire(kiss_a, kiss_b);

    Frame received;
    bool got_frame = false;
    router_b.on_ui = [&](const Frame& f) { received = f; got_frame = true; };

    router_a.send_ui(Addr::make("N0CALL"), 0xF0, std::string("UI test"));

    EXPECT_TRUE(got_frame);
    EXPECT_EQ(received.type(), Frame::Type::UI);
    std::string info(received.info.begin(), received.info.end());
    EXPECT_EQ(info, "UI test");
}

TEST(RouterUI, APRSMonitorIgnoresDest) {
    // APRS frames are broadcast — on_ui fires regardless of destination
    Kiss kiss_a, kiss_b;
    Router router_a(kiss_a, make_cfg("W1AW"));
    Router router_b(kiss_b, make_cfg("N0CALL"));
    VirtualWire vwire(kiss_a, kiss_b);

    int ui_count = 0;
    router_b.on_ui = [&](const Frame&) { ++ui_count; };

    // Send to APRS (not N0CALL), but B should still receive via on_ui
    router_a.send_aprs("!0000.00N/00000.00E>Test");
    EXPECT_EQ(ui_count, 1);
}

// =============================================================================
// 7. T1 retransmit timer (triggered manually via tick())
// =============================================================================
TEST(Timers, T1RetransmitReachesLinkFailed) {
    VirtualNet net;
    Connection* accepted = nullptr;
    bool b_disconnected  = false;

    net.router_b.listen([&](Connection* c) {
        accepted = c;
        c->on_disconnect = [&]{ b_disconnected = true; };
    });

    auto* conn_a = net.router_a.connect(Addr::make("N0CALL"));
    ASSERT_EQ(conn_a->state(), Connection::State::CONNECTED);

    // Sever the wire: override A's hook so outgoing frames are dropped silently
    net.kiss_a.on_send_hook = [](const std::vector<uint8_t>&){ return true; };

    // Send a chunk — it goes out but gets no ACK, T1 will expire
    conn_a->send("unacked data");

    // Simulate time passing by forcing T1 to expire via tick().
    // Each tick must advance time by > compute_t1() so that T1 re-arms and
    // re-fires on the next call.  retransmit_all resets T1 to now+T1, so
    // using a fixed timestamp would stop T1 from firing after the first tick.
    // cfg n2=3 → link_failed on tick 4 (retry_ 0→1→2→3 >= 3).
    bool a_disconnected = false;
    conn_a->on_disconnect = [&]{ a_disconnected = true; };

    int t1 = net.router_a.config().compute_t1();
    Millis base = now_ms() + 100000;
    for (int i = 0; i < 5; ++i) conn_a->tick(base + (Millis)i * (t1 + 100));

    EXPECT_EQ(conn_a->state(), Connection::State::DISCONNECTED);
    EXPECT_TRUE(a_disconnected);

    delete conn_a;
    delete accepted;
}

// =============================================================================
// 8. IniConfig
// =============================================================================
TEST(IniConfig, LoadFromString) {
    // Write a temp INI file
    const char* path = "/tmp/test_kissbbs.ini";
    {
        FILE* f = fopen(path, "w");
        ASSERT_NE(f, nullptr);
        fprintf(f, "[ax25]\ncallsign = W1AW-1\nmtu = 64\n[bbs]\nname = TestBBS\n");
        fclose(f);
    }
    IniConfig cfg;
    ASSERT_TRUE(cfg.load(path));
    EXPECT_EQ(cfg.get("ax25", "callsign"), "W1AW-1");
    EXPECT_EQ(cfg.get_int("ax25", "mtu"), 64);
    EXPECT_EQ(cfg.get("bbs", "name"), "TestBBS");
    EXPECT_EQ(cfg.get("bbs", "missing", "default"), "default");
}

TEST(IniConfig, MissingFile) {
    IniConfig cfg;
    EXPECT_FALSE(cfg.load("/tmp/nonexistent_kissbbs_12345.ini"));
}

TEST(IniConfig, Comments) {
    const char* path = "/tmp/test_kissbbs_comments.ini";
    {
        FILE* f = fopen(path, "w");
        ASSERT_NE(f, nullptr);
        fprintf(f, "[sec]\nkey = value  ; inline comment\nkey2 = other # hash comment\n");
        fclose(f);
    }
    IniConfig cfg;
    ASSERT_TRUE(cfg.load(path));
    EXPECT_EQ(cfg.get("sec", "key"), "value");
    EXPECT_EQ(cfg.get("sec", "key2"), "other");
}

TEST(IniConfig, BoolAndDouble) {
    const char* path = "/tmp/test_kissbbs_types.ini";
    {
        FILE* f = fopen(path, "w");
        ASSERT_NE(f, nullptr);
        fprintf(f, "[s]\nflag = true\npi = 3.14\n");
        fclose(f);
    }
    IniConfig cfg;
    ASSERT_TRUE(cfg.load(path));
    EXPECT_TRUE(cfg.get_bool("s", "flag"));
    EXPECT_NEAR(cfg.get_double("s", "pi"), 3.14, 0.001);
}

// =============================================================================
// 9. Basic interpreter
// =============================================================================
static std::string run_basic(const std::string& src) {
    Basic interp;
    std::string output;
    interp.on_send = [&](const std::string& s) { output += s; };
    interp.on_recv = [](int) -> std::string { return "test_input"; };
    interp.load_string(src);
    interp.run();
    return output;
}

TEST(BasicInterp, PrintString) {
    std::string out = run_basic("10 PRINT \"Hello, World!\"");
    EXPECT_NE(out.find("Hello, World!"), std::string::npos);
}

TEST(BasicInterp, Arithmetic) {
    std::string out = run_basic("10 PRINT 2 + 3 * 4");
    EXPECT_NE(out.find("14"), std::string::npos);
}

TEST(BasicInterp, StringConcat) {
    std::string out = run_basic("10 A$ = \"Hello\" : B$ = \" World\" : PRINT A$ + B$");
    EXPECT_NE(out.find("Hello World"), std::string::npos);
}

TEST(BasicInterp, IfThen) {
    std::string out = run_basic("10 X = 5\n20 IF X > 3 THEN PRINT \"big\"");
    EXPECT_NE(out.find("big"), std::string::npos);
}

TEST(BasicInterp, IfThenElse) {
    std::string out = run_basic("10 X = 1\n20 IF X > 3 THEN PRINT \"big\" ELSE PRINT \"small\"");
    EXPECT_NE(out.find("small"), std::string::npos);
    EXPECT_EQ(out.find("big"), std::string::npos);
}

TEST(BasicInterp, ForNext) {
    std::string out = run_basic("10 FOR I = 1 TO 3\n20 PRINT STR$(I)\n30 NEXT I");
    EXPECT_NE(out.find("1"), std::string::npos);
    EXPECT_NE(out.find("2"), std::string::npos);
    EXPECT_NE(out.find("3"), std::string::npos);
}

TEST(BasicInterp, GosubReturn) {
    std::string out = run_basic(
        "10 GOSUB 100\n"
        "20 PRINT \"back\"\n"
        "30 END\n"
        "100 PRINT \"sub\"\n"
        "110 RETURN\n"
    );
    EXPECT_NE(out.find("sub"), std::string::npos);
    EXPECT_NE(out.find("back"), std::string::npos);
}

TEST(BasicInterp, StringFunctions) {
    std::string out = run_basic(
        "10 A$ = \"Hello World\"\n"
        "20 PRINT UPPER$(A$)\n"
        "30 PRINT LEN(A$)\n"
        "40 PRINT LEFT$(A$, 5)\n"
    );
    EXPECT_NE(out.find("HELLO WORLD"), std::string::npos);
    EXPECT_NE(out.find("11"), std::string::npos);
    EXPECT_NE(out.find("Hello"), std::string::npos);
}

TEST(BasicInterp, ExecCommand) {
    Basic interp;
    std::string output;
    interp.on_send = [&](const std::string& s) { output += s; };
    interp.on_recv = [](int) -> std::string { return ""; };
    interp.load_string("10 EXEC \"echo hello_exec\", R$\n20 PRINT R$");
    interp.run();
    EXPECT_NE(output.find("hello_exec"), std::string::npos);
}

TEST(BasicInterp, ExecTimeout) {
    Basic interp;
    std::string output;
    interp.on_send = [&](const std::string& s) { output += s; };
    interp.on_recv = [](int) -> std::string { return ""; };
    // sleep 10 with 100ms timeout should return TIMEOUT
    interp.load_string("10 EXEC \"sleep 10\", R$, 100\n20 PRINT R$");
    interp.run();
    EXPECT_NE(output.find("TIMEOUT"), std::string::npos);
}

// =============================================================================
// 10. BASIC — new features: WHILE/WEND, SEND_APRS, SEND_UI, math, multi-stmt IF
// =============================================================================

// ── tokenize_args (replicated inline for testing) ─────────────────────────
static std::vector<std::string> tok_args(const std::string& line) {
    std::vector<std::string> args;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
        if (i >= line.size()) break;
        std::string token;
        if (line[i] == '"' || line[i] == '\'') {
            char d = line[i++];
            while (i < line.size() && line[i] != d) token += line[i++];
            if (i < line.size()) ++i;
        } else {
            while (i < line.size() && !std::isspace((unsigned char)line[i])) token += line[i++];
        }
        if (!token.empty()) args.push_back(token);
    }
    return args;
}

TEST(TokenizeArgs, Basic) {
    auto v = tok_args("FOO BAR BAZ");
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "FOO");
    EXPECT_EQ(v[1], "BAR");
    EXPECT_EQ(v[2], "BAZ");
}

TEST(TokenizeArgs, QuotedDoubleQuote) {
    auto v = tok_args("CMD ARG1 \"hello world\" ARG3");
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v[2], "hello world");
    EXPECT_EQ(v[3], "ARG3");
}

TEST(TokenizeArgs, QuotedSingleQuote) {
    auto v = tok_args("CMD 'two words' end");
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[1], "two words");
}

TEST(TokenizeArgs, EmptyString) {
    auto v = tok_args("   ");
    EXPECT_TRUE(v.empty());
}

TEST(BasicInterp, WhileWend) {
    std::string out = run_basic(
        "10 I = 1\n"
        "20 WHILE I <= 3\n"
        "30   PRINT STR$(I)\n"
        "40   I = I + 1\n"
        "50 WEND\n"
        "60 PRINT \"done\"\n"
    );
    EXPECT_NE(out.find("1"), std::string::npos);
    EXPECT_NE(out.find("2"), std::string::npos);
    EXPECT_NE(out.find("3"), std::string::npos);
    EXPECT_NE(out.find("done"), std::string::npos);
    // 4 must not appear
    EXPECT_EQ(out.find("4\r\n"), std::string::npos);
}

TEST(BasicInterp, WhileWendZeroIter) {
    // Condition false from start — body never executes
    std::string out = run_basic(
        "10 WHILE 0\n"
        "20 PRINT \"never\"\n"
        "30 WEND\n"
        "40 PRINT \"after\"\n"
    );
    EXPECT_EQ(out.find("never"), std::string::npos);
    EXPECT_NE(out.find("after"), std::string::npos);
}

TEST(BasicInterp, SendAprs) {
    Basic interp;
    std::string aprs_info;
    std::string output;
    interp.on_send = [&](const std::string& s) { output += s; };
    interp.on_recv = [](int) -> std::string { return ""; };
    interp.on_send_aprs = [&](const std::string& info) { aprs_info = info; };
    interp.load_string("10 SEND_APRS \"!1234.00N/00123.00W>test\"");
    interp.run();
    EXPECT_EQ(aprs_info, "!1234.00N/00123.00W>test");
}

TEST(BasicInterp, SendUi) {
    Basic interp;
    std::string ui_dest, ui_text;
    interp.on_send = [](const std::string&) {};
    interp.on_recv = [](int) -> std::string { return ""; };
    interp.on_send_ui = [&](const std::string& d, const std::string& t) {
        ui_dest = d; ui_text = t;
    };
    interp.load_string("10 SEND_UI \"APRS\", \"hello rf\"");
    interp.run();
    EXPECT_EQ(ui_dest, "APRS");
    EXPECT_EQ(ui_text, "hello rf");
}

TEST(BasicInterp, MathFunctions) {
    // ABS, SQR, INT
    std::string out = run_basic(
        "10 PRINT ABS(-5)\n"
        "20 PRINT SQR(9)\n"
        "30 PRINT INT(3.9)\n"
    );
    EXPECT_NE(out.find("5"), std::string::npos);
    EXPECT_NE(out.find("3"), std::string::npos);
}

TEST(BasicInterp, MultiStmtIfThen) {
    // Multiple colon-separated statements in THEN branch
    std::string out = run_basic(
        "10 X = 10\n"
        "20 IF X > 5 THEN A$ = \"big\" : PRINT A$\n"
        "30 IF X < 5 THEN PRINT \"small\"\n"
    );
    EXPECT_NE(out.find("big"), std::string::npos);
    EXPECT_EQ(out.find("small"), std::string::npos);
}

TEST(BasicInterp, MultiStmtIfElse) {
    std::string out = run_basic(
        "10 X = 2\n"
        "20 IF X > 5 THEN PRINT \"yes\" : PRINT \"big\" ELSE PRINT \"no\" : PRINT \"small\"\n"
    );
    EXPECT_EQ(out.find("yes"), std::string::npos);
    EXPECT_NE(out.find("no"), std::string::npos);
    EXPECT_NE(out.find("small"), std::string::npos);
}

// =============================================================================
// QBASIC dialect tests
// =============================================================================

// ── Labels and GOTO label ────────────────────────────────────────────────────
TEST(QBasic, LabelGoto) {
    std::string out = run_basic(
        "X = 1\n"
        "GOTO Done\n"
        "X = 99\n"
        "Done:\n"
        "PRINT STR$(X)\n"
    );
    EXPECT_NE(out.find("1"), std::string::npos);
    EXPECT_EQ(out.find("99"), std::string::npos);
}

// ── CONST ────────────────────────────────────────────────────────────────────
TEST(QBasic, Const) {
    std::string out = run_basic(
        "CONST PI = 3\n"
        "CONST GREETING$ = \"Hi\"\n"
        "PRINT STR$(PI)\n"
        "PRINT GREETING$\n"
    );
    EXPECT_NE(out.find("3"), std::string::npos);
    EXPECT_NE(out.find("Hi"), std::string::npos);
}

// ── Block IF / ELSEIF / ELSE / END IF ────────────────────────────────────────
TEST(QBasic, BlockIfElseIf) {
    std::string out = run_basic(
        "X = 5\n"
        "IF X > 10 THEN\n"
        "  PRINT \"big\"\n"
        "ELSEIF X > 3 THEN\n"
        "  PRINT \"medium\"\n"
        "ELSE\n"
        "  PRINT \"small\"\n"
        "END IF\n"
    );
    EXPECT_NE(out.find("medium"), std::string::npos);
    EXPECT_EQ(out.find("big"),   std::string::npos);
    EXPECT_EQ(out.find("small"), std::string::npos);
}

TEST(QBasic, BlockIfFalseElse) {
    std::string out = run_basic(
        "X = 1\n"
        "IF X > 10 THEN\n"
        "  PRINT \"big\"\n"
        "ELSE\n"
        "  PRINT \"small\"\n"
        "END IF\n"
    );
    EXPECT_NE(out.find("small"), std::string::npos);
    EXPECT_EQ(out.find("big"),   std::string::npos);
}

// ── DO / LOOP WHILE ──────────────────────────────────────────────────────────
TEST(QBasic, DoLoopWhile) {
    std::string out = run_basic(
        "I = 1\n"
        "DO\n"
        "  PRINT STR$(I)\n"
        "  I = I + 1\n"
        "LOOP WHILE I <= 3\n"
    );
    EXPECT_NE(out.find("1"), std::string::npos);
    EXPECT_NE(out.find("2"), std::string::npos);
    EXPECT_NE(out.find("3"), std::string::npos);
    EXPECT_EQ(out.find("4"), std::string::npos);
}

// ── DO WHILE / LOOP (pre-condition, zero iterations) ─────────────────────────
TEST(QBasic, DoWhileZeroIter) {
    std::string out = run_basic(
        "I = 10\n"
        "DO WHILE I < 3\n"
        "  PRINT \"body\"\n"
        "LOOP\n"
    );
    EXPECT_EQ(out.find("body"), std::string::npos);
}

// ── DO / LOOP UNTIL ───────────────────────────────────────────────────────────
TEST(QBasic, DoLoopUntil) {
    std::string out = run_basic(
        "I = 0\n"
        "DO\n"
        "  I = I + 1\n"
        "LOOP UNTIL I >= 3\n"
        "PRINT STR$(I)\n"
    );
    EXPECT_NE(out.find("3"), std::string::npos);
}

// ── EXIT DO ───────────────────────────────────────────────────────────────────
TEST(QBasic, ExitDo) {
    std::string out = run_basic(
        "I = 0\n"
        "DO\n"
        "  I = I + 1\n"
        "  IF I = 3 THEN EXIT DO\n"
        "LOOP WHILE I < 10\n"
        "PRINT STR$(I)\n"
    );
    EXPECT_NE(out.find("3"), std::string::npos);
    EXPECT_EQ(out.find("10"), std::string::npos);
}

// ── EXIT FOR ─────────────────────────────────────────────────────────────────
TEST(QBasic, ExitFor) {
    std::string out = run_basic(
        "FOR I = 1 TO 10\n"
        "  IF I = 4 THEN EXIT FOR\n"
        "  PRINT STR$(I)\n"
        "NEXT I\n"
        "PRINT \"done\"\n"
    );
    EXPECT_NE(out.find("3"), std::string::npos);
    EXPECT_EQ(out.find("4"), std::string::npos);
    EXPECT_NE(out.find("done"), std::string::npos);
}

// ── SELECT CASE ───────────────────────────────────────────────────────────────
TEST(QBasic, SelectCaseSimple) {
    std::string out = run_basic(
        "X = 2\n"
        "SELECT CASE X\n"
        "  CASE 1\n"
        "    PRINT \"one\"\n"
        "  CASE 2\n"
        "    PRINT \"two\"\n"
        "  CASE ELSE\n"
        "    PRINT \"other\"\n"
        "END SELECT\n"
    );
    EXPECT_NE(out.find("two"),   std::string::npos);
    EXPECT_EQ(out.find("one"),   std::string::npos);
    EXPECT_EQ(out.find("other"), std::string::npos);
}

TEST(QBasic, SelectCaseElse) {
    std::string out = run_basic(
        "X = 99\n"
        "SELECT CASE X\n"
        "  CASE 1\n"
        "    PRINT \"one\"\n"
        "  CASE ELSE\n"
        "    PRINT \"other\"\n"
        "END SELECT\n"
    );
    EXPECT_NE(out.find("other"), std::string::npos);
    EXPECT_EQ(out.find("one"),   std::string::npos);
}

TEST(QBasic, SelectCaseRange) {
    std::string out = run_basic(
        "X = 5\n"
        "SELECT CASE X\n"
        "  CASE 1 TO 3\n"
        "    PRINT \"low\"\n"
        "  CASE 4 TO 6\n"
        "    PRINT \"mid\"\n"
        "  CASE ELSE\n"
        "    PRINT \"high\"\n"
        "END SELECT\n"
    );
    EXPECT_NE(out.find("mid"),  std::string::npos);
    EXPECT_EQ(out.find("low"),  std::string::npos);
    EXPECT_EQ(out.find("high"), std::string::npos);
}

TEST(QBasic, SelectCaseIs) {
    std::string out = run_basic(
        "X = 15\n"
        "SELECT CASE X\n"
        "  CASE IS < 10\n"
        "    PRINT \"small\"\n"
        "  CASE IS >= 10\n"
        "    PRINT \"large\"\n"
        "END SELECT\n"
    );
    EXPECT_NE(out.find("large"), std::string::npos);
    EXPECT_EQ(out.find("small"), std::string::npos);
}

// ── SUB (no return value) ─────────────────────────────────────────────────────
TEST(QBasic, Sub) {
    std::string out = run_basic(
        "CALL Greet(\"World\")\n"
        "END\n"
        "\n"
        "SUB Greet(name$)\n"
        "  PRINT \"Hello, \" + name$\n"
        "END SUB\n"
    );
    EXPECT_NE(out.find("Hello, World"), std::string::npos);
}

TEST(QBasic, SubImplicitCall) {
    std::string out = run_basic(
        "Greet \"Alice\"\n"
        "END\n"
        "\n"
        "SUB Greet(name$)\n"
        "  PRINT \"Hi \" + name$\n"
        "END SUB\n"
    );
    EXPECT_NE(out.find("Hi Alice"), std::string::npos);
}

// ── FUNCTION (returns value) ──────────────────────────────────────────────────
TEST(QBasic, Function) {
    std::string out = run_basic(
        "PRINT STR$(Square(4))\n"
        "END\n"
        "\n"
        "FUNCTION Square(n)\n"
        "  Square = n * n\n"
        "END FUNCTION\n"
    );
    EXPECT_NE(out.find("16"), std::string::npos);
}

TEST(QBasic, FunctionString) {
    std::string out = run_basic(
        "PRINT Greet$(\"Bob\")\n"
        "END\n"
        "\n"
        "FUNCTION Greet$(name$)\n"
        "  Greet$ = \"Hello, \" + name$\n"
        "END FUNCTION\n"
    );
    EXPECT_NE(out.find("Hello, Bob"), std::string::npos);
}

// ── FUNCTION calling FUNCTION ─────────────────────────────────────────────────
TEST(QBasic, FunctionCallsFunction) {
    std::string out = run_basic(
        "PRINT STR$(Double(Triple(2)))\n"
        "END\n"
        "\n"
        "FUNCTION Double(n)\n"
        "  Double = n * 2\n"
        "END FUNCTION\n"
        "\n"
        "FUNCTION Triple(n)\n"
        "  Triple = n * 3\n"
        "END FUNCTION\n"
    );
    EXPECT_NE(out.find("12"), std::string::npos); // 2*3=6, 6*2=12
}

// ── EXIT SUB ─────────────────────────────────────────────────────────────────
TEST(QBasic, ExitSub) {
    std::string out = run_basic(
        "CALL Test\n"
        "END\n"
        "\n"
        "SUB Test\n"
        "  PRINT \"before\"\n"
        "  EXIT SUB\n"
        "  PRINT \"after\"\n"
        "END SUB\n"
    );
    EXPECT_NE(out.find("before"), std::string::npos);
    EXPECT_EQ(out.find("after"),  std::string::npos);
}

// ── TYPE (struct) ─────────────────────────────────────────────────────────────
TEST(QBasic, TypeStruct) {
    std::string out = run_basic(
        "TYPE Point\n"
        "  X AS DOUBLE\n"
        "  Y AS DOUBLE\n"
        "END TYPE\n"
        "\n"
        "DIM P AS Point\n"
        "P.X = 10\n"
        "P.Y = 20\n"
        "PRINT STR$(P.X) + \",\" + STR$(P.Y)\n"
    );
    EXPECT_NE(out.find("10"), std::string::npos);
    EXPECT_NE(out.find("20"), std::string::npos);
}

// ── DIM variable ─────────────────────────────────────────────────────────────
TEST(QBasic, DimVariable) {
    std::string out = run_basic(
        "DIM N AS INTEGER\n"
        "DIM S$ AS STRING\n"
        "N = 42\n"
        "S$ = \"hello\"\n"
        "PRINT STR$(N) + \" \" + S$\n"
    );
    EXPECT_NE(out.find("42"),    std::string::npos);
    EXPECT_NE(out.find("hello"), std::string::npos);
}

// ── No line numbers (pure QBASIC style) ──────────────────────────────────────
TEST(QBasic, NoLineNumbers) {
    std::string out = run_basic(
        "A = 10\n"
        "B = 20\n"
        "PRINT STR$(A + B)\n"
    );
    EXPECT_NE(out.find("30"), std::string::npos);
}

// ── GOSUB to label ───────────────────────────────────────────────────────────
TEST(QBasic, GosubLabel) {
    std::string out = run_basic(
        "GOSUB PrintHi\n"
        "GOTO Done\n"
        "PrintHi:\n"
        "  PRINT \"hi\"\n"
        "  RETURN\n"
        "Done:\n"
    );
    EXPECT_NE(out.find("hi"), std::string::npos);
}

// =============================================================================
// FOR IN MATCH — regex match iterator
// =============================================================================

TEST(QBasicExt, ForInMatchBasic) {
    // Iterates over every word-token in the sentence
    std::string out = run_basic(
        "FOR tok$ IN \"hello world foo\" MATCH \"[a-z]+\"\n"
        "  PRINT tok$\n"
        "NEXT tok$\n"
    );
    EXPECT_NE(out.find("hello"), std::string::npos);
    EXPECT_NE(out.find("world"), std::string::npos);
    EXPECT_NE(out.find("foo"),   std::string::npos);
}

TEST(QBasicExt, ForInMatchNumbers) {
    // Extract all integers from a mixed string
    std::string out = run_basic(
        "S$ = \"abc 10 def 20 ghi 30\"\n"
        "SUM = 0\n"
        "FOR n$ IN S$ MATCH \"[0-9]+\"\n"
        "  SUM = SUM + VAL(n$)\n"
        "NEXT n$\n"
        "PRINT STR$(SUM)\n"
    );
    EXPECT_NE(out.find("60"), std::string::npos);
}

TEST(QBasicExt, ForInMatchNoMatches) {
    // When no matches exist the body must not execute
    std::string out = run_basic(
        "FOR x$ IN \"hello\" MATCH \"[0-9]+\"\n"
        "  PRINT \"match\"\n"
        "NEXT x$\n"
        "PRINT \"done\"\n"
    );
    EXPECT_EQ(out.find("match"), std::string::npos);
    EXPECT_NE(out.find("done"),  std::string::npos);
}

TEST(QBasicExt, ForInMatchExitFor) {
    // EXIT FOR must work inside a match iterator loop
    std::string out = run_basic(
        "COUNT = 0\n"
        "FOR w$ IN \"a b c d e\" MATCH \"[a-z]\"\n"
        "  COUNT = COUNT + 1\n"
        "  IF COUNT = 2 THEN EXIT FOR\n"
        "NEXT w$\n"
        "PRINT STR$(COUNT)\n"
    );
    EXPECT_NE(out.find("2"), std::string::npos);
}

// =============================================================================
// REGEX functions
// =============================================================================

TEST(QBasicExt, ReMatch) {
    // REMATCH returns 1 when pattern is found, 0 otherwise
    std::string out = run_basic(
        "PRINT STR$(REMATCH(\"[0-9]+\", \"abc123\"))\n"
        "PRINT STR$(REMATCH(\"[0-9]+\", \"abcdef\"))\n"
    );
    EXPECT_NE(out.find("1"), std::string::npos);
    EXPECT_NE(out.find("0"), std::string::npos);
}

TEST(QBasicExt, ReFind) {
    // REFIND$ returns first match, or "" when no match
    std::string out = run_basic(
        "PRINT REFIND$(\"[0-9]+\", \"foo 42 bar 99\")\n"
        "PRINT REFIND$(\"[0-9]+\", \"no numbers here\")\n"
    );
    EXPECT_NE(out.find("42"), std::string::npos);
}

TEST(QBasicExt, ReAll) {
    // REALL$ joins all matches with default "," separator
    std::string out = run_basic(
        "PRINT REALL$(\"[0-9]+\", \"a1b2c3\")\n"
    );
    EXPECT_NE(out.find("1,2,3"), std::string::npos);
}

TEST(QBasicExt, ReAllCustomSep) {
    // Custom separator
    std::string out = run_basic(
        "PRINT REALL$(\"[a-z]+\", \"hello world\", \"-\")\n"
    );
    EXPECT_NE(out.find("hello-world"), std::string::npos);
}

TEST(QBasicExt, ReSub) {
    // RESUB$ replaces only the first match
    std::string out = run_basic(
        "PRINT RESUB$(\"[0-9]+\", \"NUM\", \"a1b2c3\")\n"
    );
    // First digit run replaced, second left intact
    EXPECT_NE(out.find("NUM"), std::string::npos);
    EXPECT_NE(out.find("2"),   std::string::npos);
}

TEST(QBasicExt, ReSubAll) {
    // RESUBALL$ replaces every match
    std::string out = run_basic(
        "PRINT RESUBALL$(\"[0-9]+\", \"X\", \"a1b2c3\")\n"
    );
    EXPECT_NE(out.find("aXbXcX"), std::string::npos);
}

TEST(QBasicExt, ReGroup) {
    // REGROUP$ extracts a capture group (0 = whole match, 1 = first group)
    std::string out = run_basic(
        "PRINT REGROUP$(\"([0-9]+)-([a-z]+)\", \"ID:42-foo\", 1)\n"
        "PRINT REGROUP$(\"([0-9]+)-([a-z]+)\", \"ID:42-foo\", 2)\n"
    );
    EXPECT_NE(out.find("42"),  std::string::npos);
    EXPECT_NE(out.find("foo"), std::string::npos);
}

TEST(QBasicExt, ReCount) {
    // RECOUNT returns number of non-overlapping matches
    std::string out = run_basic(
        "PRINT STR$(RECOUNT(\"[0-9]+\", \"1 22 333\"))\n"
    );
    EXPECT_NE(out.find("3"), std::string::npos);
}

// =============================================================================
// MAP — named associative array
// =============================================================================

TEST(QBasicExt, MapSetGet) {
    // Basic store and retrieve
    std::string out = run_basic(
        "MAP_SET \"cfg\", \"host\", \"localhost\"\n"
        "MAP_SET \"cfg\", \"port\", \"8080\"\n"
        "MAP_GET \"cfg\", \"host\", host$\n"
        "MAP_GET \"cfg\", \"port\", port$\n"
        "PRINT host$ + \":\" + port$\n"
    );
    EXPECT_NE(out.find("localhost:8080"), std::string::npos);
}

TEST(QBasicExt, MapHas) {
    // MAP_HAS returns 1 when key present, 0 when absent
    std::string out = run_basic(
        "MAP_SET \"m\", \"k\", \"v\"\n"
        "PRINT STR$(MAP_HAS(\"m\", \"k\"))\n"
        "PRINT STR$(MAP_HAS(\"m\", \"missing\"))\n"
    );
    EXPECT_NE(out.find("1"), std::string::npos);
    EXPECT_NE(out.find("0"), std::string::npos);
}

TEST(QBasicExt, MapDel) {
    // MAP_DEL removes an entry
    std::string out = run_basic(
        "MAP_SET \"m\", \"x\", \"10\"\n"
        "MAP_DEL \"m\", \"x\"\n"
        "PRINT STR$(MAP_HAS(\"m\", \"x\"))\n"
    );
    EXPECT_NE(out.find("0"), std::string::npos);
}

TEST(QBasicExt, MapKeys) {
    // MAP_KEYS produces comma-joined key list (keys are sorted by std::map)
    std::string out = run_basic(
        "MAP_SET \"m\", \"b\", \"2\"\n"
        "MAP_SET \"m\", \"a\", \"1\"\n"
        "MAP_KEYS \"m\", k$\n"
        "PRINT k$\n"
    );
    EXPECT_NE(out.find("a"), std::string::npos);
    EXPECT_NE(out.find("b"), std::string::npos);
}

TEST(QBasicExt, MapSize) {
    // MAP_SIZE returns entry count
    std::string out = run_basic(
        "MAP_SET \"m\", \"one\", \"1\"\n"
        "MAP_SET \"m\", \"two\", \"2\"\n"
        "PRINT STR$(MAP_SIZE(\"m\"))\n"
    );
    EXPECT_NE(out.find("2"), std::string::npos);
}

TEST(QBasicExt, MapClear) {
    // MAP_CLEAR removes all entries
    std::string out = run_basic(
        "MAP_SET \"m\", \"x\", \"1\"\n"
        "MAP_CLEAR \"m\"\n"
        "PRINT STR$(MAP_SIZE(\"m\"))\n"
    );
    EXPECT_NE(out.find("0"), std::string::npos);
}

// =============================================================================
// QUEUE — named FIFO
// =============================================================================

TEST(QBasicExt, QueuePushPop) {
    // Items dequeue in FIFO order
    std::string out = run_basic(
        "QUEUE_PUSH \"q\", \"first\"\n"
        "QUEUE_PUSH \"q\", \"second\"\n"
        "QUEUE_PUSH \"q\", \"third\"\n"
        "QUEUE_POP \"q\", v$\n"
        "PRINT v$\n"
        "QUEUE_POP \"q\", v$\n"
        "PRINT v$\n"
    );
    EXPECT_NE(out.find("first"),  std::string::npos);
    EXPECT_NE(out.find("second"), std::string::npos);
}

TEST(QBasicExt, QueuePeek) {
    // QUEUE_PEEK reads front without removing
    std::string out = run_basic(
        "QUEUE_PUSH \"q\", \"hello\"\n"
        "QUEUE_PEEK \"q\", v$\n"
        "PRINT v$\n"
        "PRINT STR$(QUEUE_SIZE(\"q\"))\n"  // must still be 1
    );
    EXPECT_NE(out.find("hello"), std::string::npos);
    EXPECT_NE(out.find("1"),     std::string::npos);
}

TEST(QBasicExt, QueueSize) {
    // QUEUE_SIZE tracks count correctly
    std::string out = run_basic(
        "QUEUE_PUSH \"q\", \"a\"\n"
        "QUEUE_PUSH \"q\", \"b\"\n"
        "PRINT STR$(QUEUE_SIZE(\"q\"))\n"
        "QUEUE_POP \"q\", v$\n"
        "PRINT STR$(QUEUE_SIZE(\"q\"))\n"
    );
    EXPECT_NE(out.find("2"), std::string::npos);
    EXPECT_NE(out.find("1"), std::string::npos);
}

TEST(QBasicExt, QueueEmpty) {
    // QUEUE_EMPTY returns 1 when empty, 0 otherwise
    std::string out = run_basic(
        "PRINT STR$(QUEUE_EMPTY(\"q\"))\n"
        "QUEUE_PUSH \"q\", \"x\"\n"
        "PRINT STR$(QUEUE_EMPTY(\"q\"))\n"
    );
    EXPECT_NE(out.find("1"), std::string::npos);
    EXPECT_NE(out.find("0"), std::string::npos);
}

TEST(QBasicExt, QueueClear) {
    // QUEUE_CLEAR empties the queue
    std::string out = run_basic(
        "QUEUE_PUSH \"q\", \"a\"\n"
        "QUEUE_PUSH \"q\", \"b\"\n"
        "QUEUE_CLEAR \"q\"\n"
        "PRINT STR$(QUEUE_EMPTY(\"q\"))\n"
    );
    EXPECT_NE(out.find("1"), std::string::npos);
}

TEST(QBasicExt, QueuePopEmpty) {
    // QUEUE_POP on an empty queue gives "" for string vars
    std::string out = run_basic(
        "QUEUE_POP \"q\", v$\n"
        "PRINT \"got:\" + v$\n"
    );
    EXPECT_NE(out.find("got:"), std::string::npos);
}

TEST(QBasicExt, QueueDoLoop) {
    // Real-world pattern: process a queue with DO/LOOP
    std::string out = run_basic(
        "QUEUE_PUSH \"jobs\", \"compile\"\n"
        "QUEUE_PUSH \"jobs\", \"link\"\n"
        "QUEUE_PUSH \"jobs\", \"test\"\n"
        "DO WHILE QUEUE_EMPTY(\"jobs\") = 0\n"
        "  QUEUE_POP \"jobs\", job$\n"
        "  PRINT job$\n"
        "LOOP\n"
    );
    EXPECT_NE(out.find("compile"), std::string::npos);
    EXPECT_NE(out.find("link"),    std::string::npos);
    EXPECT_NE(out.find("test"),    std::string::npos);
}

// =============================================================================
// ARRAY — DIM arr(n), arr(i) = val, x = arr(i), FOR x IN arr, ARRAY_SIZE
// =============================================================================

TEST(QBasicExt, ArrayDimRead) {
    // DIM arr(2) pre-initialises slots 0..2; read back returns default value
    std::string out = run_basic(
        "DIM nums(2)\n"
        "PRINT STR$(ARRAY_SIZE(\"NUMS\"))\n"  // 3 slots (0, 1, 2)
    );
    EXPECT_NE(out.find("3"), std::string::npos);
}

TEST(QBasicExt, ArrayWriteRead) {
    // arr(i) = value followed by x = arr(i)
    std::string out = run_basic(
        "DIM arr$(4)\n"
        "arr$(0) = \"alpha\"\n"
        "arr$(1) = \"beta\"\n"
        "arr$(2) = \"gamma\"\n"
        "PRINT arr$(0)\n"
        "PRINT arr$(1)\n"
        "PRINT arr$(2)\n"
    );
    EXPECT_NE(out.find("alpha"), std::string::npos);
    EXPECT_NE(out.find("beta"),  std::string::npos);
    EXPECT_NE(out.find("gamma"), std::string::npos);
}

TEST(QBasicExt, ArrayNumeric) {
    // Numeric array with DIM and arithmetic on elements
    std::string out = run_basic(
        "DIM vals(3)\n"
        "vals(0) = 10\n"
        "vals(1) = 20\n"
        "vals(2) = 30\n"
        "PRINT STR$(vals(0) + vals(1) + vals(2))\n"
    );
    EXPECT_NE(out.find("60"), std::string::npos);
}

TEST(QBasicExt, ArrayAutoDeclaration) {
    // Writing to arr(i) without DIM auto-registers it as an array
    std::string out = run_basic(
        "x$(0) = \"first\"\n"
        "x$(1) = \"second\"\n"
        "PRINT x$(0)\n"
        "PRINT x$(1)\n"
    );
    EXPECT_NE(out.find("first"),  std::string::npos);
    EXPECT_NE(out.find("second"), std::string::npos);
}

TEST(QBasicExt, ArrayAssocKey) {
    // String-keyed (associative) array access
    std::string out = run_basic(
        "DIM lookup$(0)\n"    // just registers the name
        "lookup$(\"alice\") = \"admin\"\n"
        "lookup$(\"bob\")   = \"user\"\n"
        "PRINT lookup$(\"alice\")\n"
        "PRINT lookup$(\"bob\")\n"
    );
    EXPECT_NE(out.find("admin"), std::string::npos);
    EXPECT_NE(out.find("user"),  std::string::npos);
}

TEST(QBasicExt, ArraySize) {
    // ARRAY_SIZE reflects number of stored elements
    std::string out = run_basic(
        "DIM v(4)\n"
        "v(0) = 1\n"
        "v(1) = 2\n"
        "PRINT STR$(ARRAY_SIZE(\"V\"))\n"  // DIM (4) pre-inits 5 slots
    );
    EXPECT_NE(out.find("5"), std::string::npos);
}

TEST(QBasicExt, ForInArray) {
    // FOR x IN arr iterates all array values in index order
    std::string out = run_basic(
        "DIM words$(2)\n"
        "words$(0) = \"cat\"\n"
        "words$(1) = \"dog\"\n"
        "words$(2) = \"bird\"\n"
        "FOR w$ IN WORDS$\n"
        "  PRINT w$\n"
        "NEXT\n"
    );
    EXPECT_NE(out.find("cat"),  std::string::npos);
    EXPECT_NE(out.find("dog"),  std::string::npos);
    EXPECT_NE(out.find("bird"), std::string::npos);
}

TEST(QBasicExt, ForInArrayNumericOrder) {
    // Numeric indices must iterate in ascending numeric (not lexicographic) order
    std::string out = run_basic(
        "DIM n(2)\n"
        "n(0) = 100\n"
        "n(1) = 200\n"
        "n(2) = 300\n"
        "DIM acc$ AS STRING\n"
        "FOR v IN N\n"
        "  acc$ = acc$ + STR$(v) + \"|\"\n"
        "NEXT\n"
        "PRINT acc$\n"
    );
    // Values must appear in order: 100, 200, 300
    auto p100  = out.find("100");
    auto p200  = out.find("200");
    auto p300  = out.find("300");
    EXPECT_NE(p100,  std::string::npos);
    EXPECT_NE(p200,  std::string::npos);
    EXPECT_NE(p300,  std::string::npos);
    EXPECT_LT(p100, p200);
    EXPECT_LT(p200, p300);
}

TEST(QBasicExt, ForInArrayEmpty) {
    // FOR IN over an empty array should execute zero iterations
    std::string out = run_basic(
        "DIM empty$(0)\n"
        // Remove the pre-inited slot so the array is truly empty
        "MAP_DEL \"EMPTY$\", \"0\"\n"
        "FOR v$ IN EMPTY$\n"
        "  PRINT \"SHOULD_NOT_PRINT\"\n"
        "NEXT\n"
        "PRINT \"done\"\n"
    );
    EXPECT_EQ(out.find("SHOULD_NOT_PRINT"), std::string::npos);
    EXPECT_NE(out.find("done"), std::string::npos);
}

TEST(QBasicExt, ArrayFunctionReturn) {
    // A FUNCTION can populate an array named after itself for multi-value return
    std::string out = run_basic(
        "FUNCTION Split$(sentence$)\n"
        "  DIM i AS INTEGER\n"
        "  i = 0\n"
        "  DIM word$ AS STRING\n"
        "  DIM c AS INTEGER\n"
        "  FOR c = 1 TO LEN(sentence$)\n"
        "    DIM ch$ AS STRING\n"
        "    ch$ = MID$(sentence$, c, 1)\n"
        "    IF ch$ = \" \" THEN\n"
        "      IF word$ <> \"\" THEN\n"
        "        Split$(i) = word$\n"
        "        i = i + 1\n"
        "        word$ = \"\"\n"
        "      END IF\n"
        "    ELSE\n"
        "      word$ = word$ + ch$\n"
        "    END IF\n"
        "  NEXT c\n"
        "  IF word$ <> \"\" THEN\n"
        "    Split$(i) = word$\n"
        "    i = i + 1\n"
        "  END IF\n"
        "  Split$ = STR$(i)\n"
        "END FUNCTION\n"
        "DIM count AS INTEGER\n"
        "count = VAL(Split$(\"hello world foo\"))\n"
        "PRINT STR$(count)\n"
        "PRINT Split$(0)\n"
        "PRINT Split$(1)\n"
        "PRINT Split$(2)\n"
    );
    EXPECT_NE(out.find("3"),     std::string::npos);
    EXPECT_NE(out.find("hello"), std::string::npos);
    EXPECT_NE(out.find("world"), std::string::npos);
    EXPECT_NE(out.find("foo"),   std::string::npos);
}

// =============================================================================
// Host-side MAP / QUEUE pre-population API
//
// Tests for the C++ methods:
//   map_set(name, key, string)  map_set(name, key, double)  map_clear(name)
//   queue_push(name, string)    queue_push(name, double)    queue_clear(name)
//
// All methods must be called AFTER load_string / load_file because those
// functions call clear() which wipes maps_ and queues_.
// =============================================================================

// Helper: load a BASIC program, run a pre-population lambda, then run().
static std::string run_basic_preload(const std::string& src,
                                      std::function<void(Basic&)> preload) {
    Basic interp;
    std::string output;
    interp.on_send = [&](const std::string& s) { output += s; };
    interp.on_recv = [](int) -> std::string { return ""; };
    interp.load_string(src);   // clear() happens here
    if (preload) preload(interp); // inject data after clear
    interp.run();
    return output;
}

// ── MAP tests ────────────────────────────────────────────────────────────────

TEST(HostMapQueue, MapSetStrReadBack) {
    // Host injects a string value; script reads it via MAP_GET.
    auto out = run_basic_preload(
        "10 MAP_GET \"cfg\", \"host\", host$\n"
        "20 PRINT host$",
        [](Basic& b) { b.map_set("cfg", "host", "localhost"); });
    EXPECT_NE(out.find("localhost"), std::string::npos);
}

TEST(HostMapQueue, MapSetNumReadBack) {
    // Host injects a numeric value; script reads it via MAP_GET.
    auto out = run_basic_preload(
        "10 MAP_GET \"cfg\", \"port\", p$\n"
        "20 PRINT p$",
        [](Basic& b) { b.map_set("cfg", "port", 8080.0); });
    EXPECT_NE(out.find("8080"), std::string::npos);
}

TEST(HostMapQueue, MapHasAfterHostSet) {
    // MAP_HAS() returns 1 for an injected key, 0 for a missing key.
    auto out = run_basic_preload(
        "10 PRINT STR$(MAP_HAS(\"m\", \"k\"))\n"
        "20 PRINT STR$(MAP_HAS(\"m\", \"missing\"))",
        [](Basic& b) { b.map_set("m", "k", "v"); });
    EXPECT_NE(out.find("1"), std::string::npos);
    EXPECT_NE(out.find("0"), std::string::npos);
}

TEST(HostMapQueue, MapSizeAfterHostSet) {
    // MAP_SIZE() reflects the number of host-injected entries.
    auto out = run_basic_preload(
        "10 PRINT STR$(MAP_SIZE(\"scores\"))",
        [](Basic& b) {
            b.map_set("scores", "W1ABC", 100.0);
            b.map_set("scores", "W2XYZ", 200.0);
            b.map_set("scores", "KD9FOO", 50.0);
        });
    EXPECT_NE(out.find("3"), std::string::npos);
}

TEST(HostMapQueue, MapOverwriteValue) {
    // Calling map_set twice on the same key overwrites the value.
    auto out = run_basic_preload(
        "10 MAP_GET \"m\", \"x\", v$\n"
        "20 PRINT v$",
        [](Basic& b) {
            b.map_set("m", "x", "first");
            b.map_set("m", "x", "second");
        });
    EXPECT_NE(out.find("second"), std::string::npos);
    EXPECT_EQ(out.find("first"),  std::string::npos);
}

TEST(HostMapQueue, MapClearHost) {
    // map_clear() removes all entries; MAP_SIZE() should return 0.
    auto out = run_basic_preload(
        "10 PRINT STR$(MAP_SIZE(\"m\"))",
        [](Basic& b) {
            b.map_set("m", "a", "1");
            b.map_set("m", "b", "2");
            b.map_clear("m");
        });
    EXPECT_NE(out.find("0"), std::string::npos);
}

TEST(HostMapQueue, MapKeysAfterHostSet) {
    // MAP_KEYS returns a comma-separated list of keys in sorted order.
    auto out = run_basic_preload(
        "10 MAP_KEYS \"m\", keys$\n"
        "20 PRINT keys$",
        [](Basic& b) {
            b.map_set("m", "beta",  "2");
            b.map_set("m", "alpha", "1");
        });
    // std::map keeps keys sorted; so "alpha" comes before "beta"
    EXPECT_NE(out.find("alpha"), std::string::npos);
    EXPECT_NE(out.find("beta"),  std::string::npos);
    auto pos_a = out.find("alpha");
    auto pos_b = out.find("beta");
    EXPECT_LT(pos_a, pos_b);
}

TEST(HostMapQueue, MapMixedTypesInSameMap) {
    // A single map can hold both string and numeric values (host-injected).
    auto out = run_basic_preload(
        "10 MAP_GET \"m\", \"name\", n$\n"
        "20 MAP_GET \"m\", \"score\", s$\n"
        "30 PRINT n$ + \"/\" + s$",
        [](Basic& b) {
            b.map_set("m", "name",  "Alice");
            b.map_set("m", "score", 42.0);
        });
    EXPECT_NE(out.find("Alice"), std::string::npos);
    EXPECT_NE(out.find("42"),    std::string::npos);
}

// ── QUEUE tests ───────────────────────────────────────────────────────────────

TEST(HostMapQueue, QueuePushStrPopFIFO) {
    // Host pushes two strings; script pops them in FIFO order.
    auto out = run_basic_preload(
        "10 QUEUE_POP \"q\", a$\n"
        "20 QUEUE_POP \"q\", b$\n"
        "30 PRINT a$ + \",\" + b$",
        [](Basic& b) {
            b.queue_push("q", "first");
            b.queue_push("q", "second");
        });
    // a$ should be "first", b$ should be "second"
    EXPECT_NE(out.find("first,second"), std::string::npos);
}

TEST(HostMapQueue, QueuePushNumPop) {
    // Host pushes a numeric value; script pops and prints it.
    auto out = run_basic_preload(
        "10 QUEUE_POP \"q\", v$\n"
        "20 PRINT v$",
        [](Basic& b) { b.queue_push("q", 3.14); });
    EXPECT_NE(out.find("3.14"), std::string::npos);
}

TEST(HostMapQueue, QueueSizeAfterHostPush) {
    // QUEUE_SIZE() reflects the number of host-pushed elements.
    auto out = run_basic_preload(
        "10 PRINT STR$(QUEUE_SIZE(\"jobs\"))",
        [](Basic& b) {
            b.queue_push("jobs", "compile");
            b.queue_push("jobs", "link");
            b.queue_push("jobs", "test");
        });
    EXPECT_NE(out.find("3"), std::string::npos);
}

TEST(HostMapQueue, QueueEmptyAfterClear) {
    // queue_clear() removes all elements; QUEUE_EMPTY() returns 1.
    auto out = run_basic_preload(
        "10 PRINT STR$(QUEUE_EMPTY(\"q\"))",
        [](Basic& b) {
            b.queue_push("q", "a");
            b.queue_push("q", "b");
            b.queue_clear("q");
        });
    EXPECT_NE(out.find("1"), std::string::npos);
}

TEST(HostMapQueue, QueueFIFOOrderMultiple) {
    // Five items pushed in order must pop in the same order.
    auto out = run_basic_preload(
        "10 QUEUE_POP \"q\", a$\n"
        "20 QUEUE_POP \"q\", b$\n"
        "30 QUEUE_POP \"q\", c$\n"
        "40 QUEUE_POP \"q\", d$\n"
        "50 QUEUE_POP \"q\", e$\n"
        "60 PRINT a$ + b$ + c$ + d$ + e$",
        [](Basic& b) {
            for (const char* s : {"A", "B", "C", "D", "E"})
                b.queue_push("q", s);
        });
    EXPECT_NE(out.find("ABCDE"), std::string::npos);
}

TEST(HostMapQueue, MapAndQueueTogether) {
    // A script that reads from both a host-injected map and a queue.
    auto out = run_basic_preload(
        "10 MAP_GET \"user\", \"name\", name$\n"
        "20 QUEUE_POP \"msgs\", msg$\n"
        "30 PRINT name$ + \": \" + msg$",
        [](Basic& b) {
            b.map_set("user", "name", "W1ABC");
            b.queue_push("msgs", "Hello from host!");
        });
    EXPECT_NE(out.find("W1ABC: Hello from host!"), std::string::npos);
}

// =============================================================================
// Line-terminator compatibility tests
//
// ax25tnc sends CR-only (\r) as the line terminator (packet radio
// convention).  The BBS on_data() must accept \r, \n, and \r\n equally so
// that any client — modern or legacy — works without special configuration.
//
// We exercise this directly on the Kiss/Router/Connection data path by
// routing raw bytes to a VirtualNet connection and verifying that lines are
// dispatched identically regardless of the terminator used.
// =============================================================================

// Helper: feed raw bytes as AX.25 I-frame data into a Connection via
// VirtualNet and collect the complete string received on the other side.
static std::string feed_data_via_connection(const std::vector<uint8_t>& data) {
    VirtualNet net;
    std::string received;
    net.router_b.listen([&](Connection* c) {
        c->on_data = [&](const uint8_t* d, std::size_t n) {
            received.append(reinterpret_cast<const char*>(d), n);
        };
        c->on_disconnect = [&] {};
    });
    auto* conn_a = net.router_a.connect(Addr::make("N0CALL"));
    EXPECT_EQ(conn_a->state(), Connection::State::CONNECTED);
    conn_a->send(data.data(), data.size());
    delete conn_a;
    return received;
}

TEST(LineTerminator, CROnlyDelivered) {
    // ax25tnc sends CR-only; receiver gets the CR byte verbatim.
    auto rx = feed_data_via_connection({'H','i','\r'});
    EXPECT_NE(rx.find("Hi"), std::string::npos);
    EXPECT_NE(rx.find('\r'), std::string::npos);
}

TEST(LineTerminator, LFOnlyDelivered) {
    auto rx = feed_data_via_connection({'H','i','\n'});
    EXPECT_NE(rx.find("Hi"), std::string::npos);
    EXPECT_NE(rx.find('\n'), std::string::npos);
}

TEST(LineTerminator, CRLFDelivered) {
    auto rx = feed_data_via_connection({'H','i','\r','\n'});
    EXPECT_NE(rx.find("Hi"), std::string::npos);
    // Both CR and LF must arrive intact
    EXPECT_NE(rx.find('\r'), std::string::npos);
    EXPECT_NE(rx.find('\n'), std::string::npos);
}

// =============================================================================
// ax25dump.hpp — hex_dump() and ctrl_detail()
// =============================================================================

// ── hex_dump ─────────────────────────────────────────────────────────────────

TEST(HexDump, EmptyReturnsEmpty) {
    EXPECT_EQ(hex_dump(nullptr, 0), "");
    EXPECT_EQ(hex_dump(nullptr, 0, ">>"), "");
}

TEST(HexDump, SinglePrintableByte) {
    uint8_t b = 0x41;  // 'A'
    std::string out = hex_dump(&b, 1);
    EXPECT_NE(out.find("00000000"), std::string::npos) << "offset missing";
    EXPECT_NE(out.find("41"),       std::string::npos) << "hex byte missing";
    EXPECT_NE(out.find("|A|"),      std::string::npos) << "ASCII column missing";
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1) << "must be one line";
}

TEST(HexDump, SingleNonPrintableByteShownAsDot) {
    uint8_t b = 0x01;
    std::string out = hex_dump(&b, 1);
    EXPECT_NE(out.find("01"), std::string::npos);
    EXPECT_NE(out.find("|.|"), std::string::npos) << "non-printable must be '.'";
}

TEST(HexDump, Exactly16BytesIsOneLine) {
    uint8_t data[16];
    for (int i = 0; i < 16; i++) data[i] = (uint8_t)('A' + i);
    std::string out = hex_dump(data, 16);
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1);
    EXPECT_NE(out.find("00000000"), std::string::npos);
    EXPECT_EQ(out.find("00000010"), std::string::npos) << "no second line offset";
}

TEST(HexDump, SeventeenBytesIsTwoLines) {
    uint8_t data[17];
    for (int i = 0; i < 17; i++) data[i] = (uint8_t)i;
    std::string out = hex_dump(data, 17);
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 2);
    EXPECT_NE(out.find("00000000"), std::string::npos) << "first line offset";
    EXPECT_NE(out.find("00000010"), std::string::npos) << "second line offset (16 = 0x10)";
}

TEST(HexDump, PrefixAppliedToEveryLine) {
    uint8_t data[17];
    for (int i = 0; i < 17; i++) data[i] = (uint8_t)i;
    std::string out = hex_dump(data, 17, "   ");
    // Every line must start with the prefix
    size_t pos = 0;
    int lines = 0;
    while ((pos = out.find('\n', pos)) != std::string::npos) {
        ++lines;
        ++pos;
    }
    EXPECT_EQ(lines, 2);
    // Find beginning of each line and verify prefix
    EXPECT_EQ(out.substr(0, 3), "   ") << "first line prefix";
    size_t nl = out.find('\n');
    EXPECT_EQ(out.substr(nl + 1, 3), "   ") << "second line prefix";
}

TEST(HexDump, MixedPrintableAndNonPrintable) {
    uint8_t data[] = {'H','e','l','l','o', 0x00, 0xFF};
    std::string out = hex_dump(data, sizeof(data));
    size_t bar = out.find('|');
    ASSERT_NE(bar, std::string::npos);
    std::string ascii_section = out.substr(bar);
    EXPECT_NE(ascii_section.find("Hello.."), std::string::npos)
        << "printable chars intact, non-printable as dot";
}

TEST(HexDump, TwoGroupsOfEightSeparatedByExtraSpace) {
    // 16-byte line: "xx xx xx xx xx xx xx xx  xx xx xx xx xx xx xx xx"
    //                 ^---- 8 bytes ----^  ^ extra space ^---- 8 bytes ----^
    uint8_t data[16];
    for (int i = 0; i < 16; i++) data[i] = (uint8_t)0xAA;
    std::string out = hex_dump(data, 16);
    // The pattern "aa aa " (8th byte space) followed by extra space then "aa"
    // Full hex section: "aa aa aa aa aa aa aa aa  aa aa aa aa aa aa aa aa "
    EXPECT_NE(out.find("aa aa aa aa aa aa aa aa  aa aa"), std::string::npos)
        << "double space between byte groups 1 and 2";
}

// ── ctrl_detail ──────────────────────────────────────────────────────────────

TEST(CtrlDetail, IFrameBasic) {
    // ctrl=0x00: NS=0, NR=0, P/F=0
    EXPECT_EQ(ctrl_detail(0x00, 10),
              "ctrl=0x00  I     N(S)=0 N(R)=0 P/F=0  (10 bytes)");
}

TEST(CtrlDetail, IFrameWithSequenceNumbers) {
    // ctrl=0x04: bit0=0 (I), NS=(4>>1)&7=2, NR=(4>>5)&7=0, P/F=0
    EXPECT_EQ(ctrl_detail(0x04, 20),
              "ctrl=0x04  I     N(S)=2 N(R)=0 P/F=0  (20 bytes)");
}

TEST(CtrlDetail, IFrameWithPF) {
    // ctrl=0x14: NS=2, NR=0, P/F=1  (bit4 set)
    EXPECT_EQ(ctrl_detail(0x14, 5),
              "ctrl=0x14  I     N(S)=2 N(R)=0 P/F=1  (5 bytes)");
}

TEST(CtrlDetail, IFrameNRInUpperThreeBits) {
    // ctrl=0xE0: bit0=0, NS=0, NR=(0xE0>>5)&7=7, P/F=0
    EXPECT_EQ(ctrl_detail(0xE0, 1),
              "ctrl=0xe0  I     N(S)=0 N(R)=7 P/F=0  (1 bytes)");
}

TEST(CtrlDetail, SFrameRR) {
    // ctrl=0x61: (ctrl&0x03)=0x01 (S), (ctrl>>2)&3=0 → RR, NR=(0x61>>5)&7=3, P/F=0
    EXPECT_EQ(ctrl_detail(0x61, 15),
              "ctrl=0x61  S/RR  N(R)=3 P/F=0  (15 bytes)");
}

TEST(CtrlDetail, SFrameRNR) {
    // ctrl=0x05: (ctrl&0x03)=0x01 (S), (ctrl>>2)&3=1 → RNR, NR=0, P/F=0
    EXPECT_EQ(ctrl_detail(0x05, 8),
              "ctrl=0x05  S/RNR  N(R)=0 P/F=0  (8 bytes)");
}

TEST(CtrlDetail, SFrameREJ) {
    // ctrl=0x09: (ctrl>>2)&3=2 → REJ, NR=0, P/F=0
    EXPECT_EQ(ctrl_detail(0x09, 8),
              "ctrl=0x09  S/REJ  N(R)=0 P/F=0  (8 bytes)");
}

TEST(CtrlDetail, UFrameSABM) {
    EXPECT_EQ(ctrl_detail(0x2F, 15),
              "ctrl=0x2f  U/SABM  P/F=0  (15 bytes)");
}

TEST(CtrlDetail, UFrameSABMWithPF) {
    // 0x3F = 0x2F | 0x10  (P/F bit set)
    EXPECT_EQ(ctrl_detail(0x3F, 15),
              "ctrl=0x3f  U/SABM  P/F=1  (15 bytes)");
}

TEST(CtrlDetail, UFrameUA) {
    EXPECT_EQ(ctrl_detail(0x63, 15),
              "ctrl=0x63  U/UA  P/F=0  (15 bytes)");
}

TEST(CtrlDetail, UFrameUAWithPF) {
    EXPECT_EQ(ctrl_detail(0x73, 15),
              "ctrl=0x73  U/UA  P/F=1  (15 bytes)");
}

TEST(CtrlDetail, UFrameDISC) {
    EXPECT_EQ(ctrl_detail(0x43, 15),
              "ctrl=0x43  U/DISC  P/F=0  (15 bytes)");
}

TEST(CtrlDetail, UFrameDM) {
    EXPECT_EQ(ctrl_detail(0x0F, 15),
              "ctrl=0x0f  U/DM  P/F=0  (15 bytes)");
}

TEST(CtrlDetail, UFrameUI) {
    EXPECT_EQ(ctrl_detail(0x03, 10),
              "ctrl=0x03  U/UI  P/F=0  (10 bytes)");
}

TEST(CtrlDetail, UFrameFRMR) {
    EXPECT_EQ(ctrl_detail(0x87, 10),
              "ctrl=0x87  U/FRMR  P/F=0  (10 bytes)");
}

TEST(CtrlDetail, UFrameUnknown) {
    // 0xFF is not a known U-frame — should show U/U?
    std::string out = ctrl_detail(0xFF, 5);
    EXPECT_NE(out.find("U/U?"), std::string::npos) << "unknown U-frame type";
    EXPECT_NE(out.find("(5 bytes)"), std::string::npos);
}

TEST(CtrlDetail, ByteCountInOutput) {
    // The frame size should always appear at the end
    EXPECT_NE(ctrl_detail(0x00,   1).find("(1 bytes)"),   std::string::npos);
    EXPECT_NE(ctrl_detail(0x00, 256).find("(256 bytes)"), std::string::npos);
}

// Main — GoogleTest entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
