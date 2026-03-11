// =============================================================================
// ax25lib.hpp — AX.25 / KISS library  (C++11, POSIX, Linux + macOS)
//
// Layer stack:
//   Serial → Kiss → Router → Connection
//
// Intrusive containers:
//   Node<T>    — base; adds next/prev to T
//   List<T>    — doubly-linked list over Node<T>
//   Connection extends Node<Connection> and auto-inserts/removes itself from
//   the List<Connection> that is passed to its constructor.
//
// Compile:
//   g++ -std=c++11 -O2 -pthread -o prog ax25lib.cpp prog.cpp
//   (Linux: add -lutil for shell/pty support in your application)
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

namespace ax25 {

// ─────────────────────────────────────────────────────────────────────────────
// Time (monotonic milliseconds)
// ─────────────────────────────────────────────────────────────────────────────
using Millis = uint64_t;
inline Millis now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<Millis>(ts.tv_sec) * 1000u + ts.tv_nsec / 1000000u;
}

// ─────────────────────────────────────────────────────────────────────────────
// Intrusive doubly-linked list
//
// T must publicly inherit Node<T>.
// Pushing an already-inserted node is undefined behaviour.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
struct Node {
    T* next = nullptr;
    T* prev = nullptr;
};

template<typename T>
class List {
    T*          head_ = nullptr;
    T*          tail_ = nullptr;
    std::size_t size_ = 0;
public:
    List() = default;
    List(const List&)            = delete;
    List& operator=(const List&) = delete;

    void push_back(T* item) {
        auto* n  = static_cast<Node<T>*>(item);
        n->prev  = tail_;
        n->next  = nullptr;
        if (tail_) static_cast<Node<T>*>(tail_)->next = item;
        else       head_ = item;
        tail_ = item;
        ++size_;
    }

    void remove(T* item) {
        auto* n = static_cast<Node<T>*>(item);
        if (n->prev) static_cast<Node<T>*>(n->prev)->next = n->next;
        else         head_ = n->next;
        if (n->next) static_cast<Node<T>*>(n->next)->prev = n->prev;
        else         tail_ = n->prev;
        n->next = n->prev = nullptr;
        if (size_) --size_;
    }

    bool        empty() const { return size_ == 0; }
    std::size_t size()  const { return size_; }

    struct iterator {
        T* cur;
        T* operator->() { return cur; }
        T& operator*()  { return *cur; }
        iterator& operator++() {
            cur = static_cast<Node<T>*>(cur)->next; return *this;
        }
        bool operator!=(const iterator& o) const { return cur != o.cur; }
    };
    iterator begin() { return {head_}; }
    iterator end()   { return {nullptr}; }

    // Snapshot: safe to iterate even if callbacks modify the list
    std::vector<T*> snapshot() {
        std::vector<T*> v; v.reserve(size_);
        for (auto& x : *this) v.push_back(&x);
        return v;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Serial port (POSIX termios)
// ─────────────────────────────────────────────────────────────────────────────
class Serial {
public:
    ~Serial() { close(); }
    bool    open(const std::string& dev, int baud);
    void    close();
    bool    is_open() const { return fd_ >= 0; }
    int     fd()      const { return fd_; }
    ssize_t write(const uint8_t* buf, std::size_t len);
    ssize_t read (uint8_t* buf, std::size_t len);
private:
    int            fd_   = -1;
    struct termios orig_{};
    static speed_t to_speed(int baud);
};

// ─────────────────────────────────────────────────────────────────────────────
// KISS protocol
// ─────────────────────────────────────────────────────────────────────────────
namespace kiss {
constexpr uint8_t FEND  = 0xC0;
constexpr uint8_t FESC  = 0xDB;
constexpr uint8_t TFEND = 0xDC;
constexpr uint8_t TFESC = 0xDD;

enum class Cmd : uint8_t {
    Data        = 0x00, TxDelay   = 0x01, Persistence = 0x02,
    SlotTime    = 0x03, TxTail    = 0x04, FullDuplex  = 0x05,
    SetHardware = 0x06, Return    = 0xFF
};

struct Frame { Cmd command; int port; std::vector<uint8_t> data; };

std::vector<uint8_t> encode(const std::vector<uint8_t>& payload,
                             Cmd cmd = Cmd::Data, int port = 0);

class Decoder {
public:
    std::vector<Frame> feed(const uint8_t* buf, std::size_t len);
private:
    bool in_frame_ = false, escaped_ = false;
    std::vector<uint8_t> buf_;
};
} // namespace kiss

// ─────────────────────────────────────────────────────────────────────────────
// AX.25 address  (max 6-char callsign + 0-15 SSID)
// ─────────────────────────────────────────────────────────────────────────────
struct Addr {
    char call[7]{};      // NUL-terminated, uppercase
    int  ssid     = 0;
    bool repeated = false;  // H bit (digipeater "has repeated")

    static Addr         make(const std::string& callssid);
    static Addr         decode(const uint8_t* p);      // 7 raw bytes
    std::vector<uint8_t> encode(bool last_addr) const; // 7 raw bytes

    std::string str() const;
    bool operator==(const Addr& o) const;
    bool operator!=(const Addr& o) const { return !(*this == o); }
    bool empty()                   const { return call[0] == '\0'; }
};

// ─────────────────────────────────────────────────────────────────────────────
// AX.25 raw frame
// ─────────────────────────────────────────────────────────────────────────────
struct Frame {
    Addr  dest, src;
    std::vector<Addr>    digis;
    uint8_t              ctrl    = 0x03;
    uint8_t              pid     = 0xF0;
    bool                 has_pid = true;
    std::vector<uint8_t> info;

    enum class Type { Unknown, IFrame, SFrame, UI, SABM, DISC, UA, DM, FRMR, RR, RNR, REJ };

    Type type()     const;
    int  get_ns()   const { return (ctrl >> 1) & 7; }
    int  get_nr()   const { return (ctrl >> 5) & 7; }
    bool get_pf()   const { return (ctrl & 0x10) != 0; }

    // Returns true and fills 'out' on success; returns false if raw is malformed.
    static bool decode(const std::vector<uint8_t>& raw, Frame& out);
    std::vector<uint8_t>        encode() const;
    std::string                 format() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Protocol configuration
// ─────────────────────────────────────────────────────────────────────────────
struct Config {
    Addr              mycall;
    std::vector<Addr> digis;          // digipeater path (default: empty)
    int mtu     = 128;                // max info bytes per I-frame
    int window  = 3;                  // max outstanding I-frames (K, 1-7)
    int t1_ms   = 3000;               // retransmit timer ms
    int t2_ms   = 200;                // delayed-ack timer ms (not used in send path)
    int t3_ms   = 60000;              // keep-alive / inactivity timer ms
    int n2      = 10;                 // max retransmissions before link-fail
    int txdelay = 30;                 // KISS TX delay (×10 ms units)
    int persist = 63;                 // KISS persistence (0-255)
};

// ─────────────────────────────────────────────────────────────────────────────
// Kiss — serial port + KISS framing layer
// ─────────────────────────────────────────────────────────────────────────────
class Kiss {
public:
    // Open the serial device.  Call set_on_frame() before poll().
    bool open(const std::string& device, int baud);
    void close();
    bool is_open() const { return serial_.is_open(); }
    int  serial_fd() const { return serial_.fd(); }

    // Register callback (called from poll() for each received AX.25 payload)
    void set_on_frame(std::function<void(std::vector<uint8_t>)> cb) {
        on_frame_ = std::move(cb);
    }

    // Send raw AX.25 bytes as a KISS data frame (TNC adds flags + FCS)
    bool send_frame(const std::vector<uint8_t>& ax25);

    // KISS parameter commands
    void set_txdelay(int ms);
    void set_persistence(int val);
    void set_slottime(int ms);

    // Read from serial; fires on_frame for each complete AX.25 payload
    void poll();

    // ── Test / simulation hooks ───────────────────────────────────────────
    // Simulate receiving an AX.25 payload (as if read from serial).
    // Useful in unit tests — no real serial port needed.
    void test_inject(const std::vector<uint8_t>& ax25) {
        if (on_frame_) on_frame_(ax25);
    }

    // If set, send_frame() calls this hook instead of the real serial write.
    // Useful for loopback / simulation tests.
    std::function<bool(const std::vector<uint8_t>&)> on_send_hook;

private:
    Serial       serial_;
    kiss::Decoder decoder_;
    std::function<void(std::vector<uint8_t>)> on_frame_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Connection — AX.25 connected-mode session (intrusive list node)
//
// Extends Node<Connection> so it lives in a List<Connection>.
// Constructor: auto-inserts into the supplied container.
// Destructor:  auto-removes from the container.
// Created only by Router.
// ─────────────────────────────────────────────────────────────────────────────
class Router;

class Connection : public Node<Connection> {
    friend class Router;
public:
    enum class State { DISCONNECTED, CONNECTING, CONNECTED, DISCONNECTING };

    // ── Callbacks (set before connection becomes active) ─────────────────
    std::function<void()>                             on_connect;
    std::function<void()>                             on_disconnect;
    std::function<void(const uint8_t*, std::size_t)>  on_data;

    // ── API ───────────────────────────────────────────────────────────────
    bool send(const uint8_t* data, std::size_t len);
    bool send(const std::string& s) {
        return send(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    void disconnect();

    State       state()     const { return state_; }
    bool        connected() const { return state_ == State::CONNECTED; }
    const Addr& remote()    const { return remote_; }
    const Addr& local()     const { return local_; }

    // Tick timers — called by Router::poll()
    void tick(Millis now);

    ~Connection();

private:
    Connection(Router* router, List<Connection>& lst,
               const Addr& local, const Addr& remote,
               const Config& cfg, bool outgoing);

    void start_connect(Millis now);
    void handle_frame(const Frame& f, Millis now);

    // Frame builders
    void tx_sabm();
    void tx_disc();
    void tx_ua(bool pf);
    void tx_dm(bool pf);
    void tx_rr(bool pf);
    void tx_iframe(int ns, int nr, bool pf, const uint8_t* d, std::size_t len);

    // Window management
    void flush_window(Millis now);
    void process_nr(int nr, Millis now);
    void retransmit_all(Millis now);
    void link_failed();

    // Timer helpers
    void start_t1(Millis now) { t1_exp_=now+cfg_.t1_ms; t1_run_=true; }
    void stop_t1()            { t1_run_=false; }
    void start_t3(Millis now) { t3_exp_=now+cfg_.t3_ms; t3_run_=true; }
    void stop_t3()            { t3_run_=false; }
    void reset_t3(Millis now) { t3_exp_=now+cfg_.t3_ms; t3_run_=true; }

    Router*           router_;
    List<Connection>& list_;
    Addr              local_, remote_;
    Config            cfg_;
    State             state_    = State::DISCONNECTED;
    bool outgoing_;  // true = we initiated; false = peer initiated

    // AX.25 state variables (mod 8)
    int vs_=0, vr_=0, va_=0, retry_=0;

    // Timers
    bool   t1_run_=false; Millis t1_exp_=0;
    bool   t3_run_=false; Millis t3_exp_=0;
    bool   poll_pending_=false;

    // Data queues
    std::deque<std::vector<uint8_t>> send_buf_;  // MTU-chunked, waiting to send

    struct UnackedFrame { int ns; std::vector<uint8_t> data; };
    std::deque<UnackedFrame> unacked_;           // sent, not yet acked
};

// ─────────────────────────────────────────────────────────────────────────────
// Router — routes incoming frames; manages Connection list
// ─────────────────────────────────────────────────────────────────────────────
class Router {
    friend class Connection;
public:
    // Kiss must already be open.  Router registers itself as Kiss's on_frame.
    Router(Kiss& kiss, Config cfg);

    // Initiate outgoing connection (returns heap-allocated Connection; caller owns)
    Connection* connect(const Addr& remote);

    // Accept incoming connections (callback fires with heap-allocated Connection)
    void listen(std::function<void(Connection*)> on_accept);

    // Connectionless (UI) send
    void send_ui(const Addr& dest, uint8_t pid,
                 const void* data, std::size_t len,
                 const std::vector<Addr>& digis = {});
    void send_ui(const Addr& dest, uint8_t pid, const std::string& text,
                 const std::vector<Addr>& digis = {}) {
        send_ui(dest, pid, text.data(), text.size(), digis);
    }

    // APRS (UI with PID 0xF0 to APRS/BEACON)
    void send_aprs(const std::string& info,
                   const Addr& dest        = Addr::make("APRS"),
                   const std::vector<Addr>& digis = {});

    // Callback fired for every received UI frame (regardless of destination).
    // Use this to monitor APRS traffic and receive UI messages.
    std::function<void(const Frame&)> on_ui;

    // Callback fired for every received frame (monitor / logging).
    // Fires for ALL frames including connected-mode ones.
    std::function<void(const Frame&)> on_monitor;

    // Main-loop integration: read serial + tick all connection timers
    void poll();

    List<Connection>&  connections()       { return conns_; }
    const Config&      config()      const { return cfg_; }
    Config&            config()            { return cfg_; }

private:
    Kiss&            kiss_;
    Config           cfg_;
    List<Connection> conns_;
    std::function<void(Connection*)> on_accept_;

    void route(std::vector<uint8_t> raw, Millis now);
    Connection* find(const Addr& local, const Addr& remote);
    bool        tx(const Frame& f);
    bool        send_frame(const Frame& f) { return tx(f); }  // for Connection
};

// ─────────────────────────────────────────────────────────────────────────────
// CLI parameter helper
// ─────────────────────────────────────────────────────────────────────────────
struct CLIParams {
    std::string device;
    int         baud = 9600;
    Config      cfg;

    // Parses standard AX.25 flags from argv.
    // Returns false (and prints usage) if required args are missing.
    bool parse(int argc, char* argv[], const char* extra_usage = "");
    static void print_help(const char* prog, const char* extra = "");
};

} // namespace ax25
