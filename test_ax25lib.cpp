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
    c.mycall = Addr::make(call);
    c.t1_ms  = 500;
    c.t3_ms  = 5000;
    c.n2     = 3;
    return c;
}

// =============================================================================
// 1. Intrusive List — Node<T> / List<T>
// =============================================================================
struct Item : Node<Item> {
    int val;
    explicit Item(int v) : val(v) {}
};

TEST(IntrusiveList, PushBackAndSize) {
    List<Item> lst;
    EXPECT_TRUE(lst.empty());
    EXPECT_EQ(lst.size(), 0u);

    Item a(1), b(2), c(3);
    lst.push_back(&a);
    lst.push_back(&b);
    lst.push_back(&c);

    EXPECT_EQ(lst.size(), 3u);
    EXPECT_FALSE(lst.empty());
}

TEST(IntrusiveList, IterationOrder) {
    List<Item> lst;
    Item a(10), b(20), c(30);
    lst.push_back(&a);
    lst.push_back(&b);
    lst.push_back(&c);

    std::vector<int> vals;
    for (auto& x : lst) vals.push_back(x.val);
    EXPECT_EQ(vals, (std::vector<int>{10, 20, 30}));
}

TEST(IntrusiveList, RemoveMiddle) {
    List<Item> lst;
    Item a(1), b(2), c(3);
    lst.push_back(&a);
    lst.push_back(&b);
    lst.push_back(&c);

    lst.remove(&b);
    EXPECT_EQ(lst.size(), 2u);

    std::vector<int> vals;
    for (auto& x : lst) vals.push_back(x.val);
    EXPECT_EQ(vals, (std::vector<int>{1, 3}));
}

TEST(IntrusiveList, RemoveHead) {
    List<Item> lst;
    Item a(1), b(2);
    lst.push_back(&a);
    lst.push_back(&b);
    lst.remove(&a);

    std::vector<int> vals;
    for (auto& x : lst) vals.push_back(x.val);
    EXPECT_EQ(vals, (std::vector<int>{2}));
}

TEST(IntrusiveList, RemoveTail) {
    List<Item> lst;
    Item a(1), b(2);
    lst.push_back(&a);
    lst.push_back(&b);
    lst.remove(&b);

    std::vector<int> vals;
    for (auto& x : lst) vals.push_back(x.val);
    EXPECT_EQ(vals, (std::vector<int>{1}));
}

TEST(IntrusiveList, AutoInsertRemovePattern) {
    // Mimics Connection: object auto-inserts on ctor, auto-removes on dtor
    struct AutoItem : Node<AutoItem> {
        List<AutoItem>& list_;
        explicit AutoItem(List<AutoItem>& l) : list_(l) { l.push_back(this); }
        ~AutoItem() { list_.remove(this); }
    };

    List<AutoItem> lst;
    {
        AutoItem x(lst), y(lst), z(lst);
        EXPECT_EQ(lst.size(), 3u);
        {
            AutoItem w(lst);
            EXPECT_EQ(lst.size(), 4u);
        }                           // w destroyed → auto-remove
        EXPECT_EQ(lst.size(), 3u);
    }                               // x, y, z destroyed
    EXPECT_EQ(lst.size(), 0u);
    EXPECT_TRUE(lst.empty());
}

TEST(IntrusiveList, Snapshot) {
    List<Item> lst;
    Item a(1), b(2), c(3);
    lst.push_back(&a); lst.push_back(&b); lst.push_back(&c);

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
    // Each tick must advance time by > t1_ms (500 ms) so that T1 re-arms and
    // re-fires on the next call.  retransmit_all resets T1 to now+t1_ms, so
    // using a fixed timestamp would stop T1 from firing after the first tick.
    // cfg n2=3 → link_failed on tick 4 (retry_ 0→1→2→3 >= 3).
    bool a_disconnected = false;
    conn_a->on_disconnect = [&]{ a_disconnected = true; };

    Millis base = now_ms() + 100000;
    for (int i = 0; i < 5; ++i) conn_a->tick(base + (Millis)i * 1000);

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

// Main — GoogleTest entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
