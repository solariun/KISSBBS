// =============================================================================
// ax25lib.hpp — AX.25 / KISS library  (C++11, POSIX, Linux + macOS)
//
// Layer stack:
//   Serial → Kiss → Router → Connection
//
// Intrusive containers:
//   ObjNode<T>  — base; stores next/prev pointers + list reference.
//                 Default constructor is DELETED.
//                 The sole protected constructor takes an ObjList<T>& and
//                 automatically inserts the derived object into that list.
//                 The protected destructor automatically removes it.
//                 Developers never call push_back / remove manually.
//   ObjList<T>  — doubly-linked list over ObjNode<T>-derived objects.
//                 Insertion/removal is driven entirely by ObjNode ctor/dtor.
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
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
template<typename T> class ObjList;
template<typename T> class ObjNode;

// ─────────────────────────────────────────────────────────────────────────────
// ObjNode<T> — intrusive doubly-linked node with automatic list management.
//
// Usage:
//   struct MyObj : ObjNode<MyObj> {
//       MyObj(ObjList<MyObj>& list, int val)
//           : ObjNode<MyObj>(list), val_(val) {}
//       int val_;
//   };
//
//   ObjList<MyObj> list;
//   { MyObj a(list, 1), b(list, 2); }  // auto-insert on ctor, auto-remove on dtor
//
// Rules:
//   • T must inherit from ObjNode<T>.
//   • Default constructor is deleted; you must pass an ObjList<T>&.
//   • Copy and move are deleted (nodes are owned by the list).
//   • Destruction automatically removes the node from its list.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class ObjNode {
    friend class ObjList<T>;
public:
    ObjNode(const ObjNode&)            = delete;
    ObjNode& operator=(const ObjNode&) = delete;

protected:
    ObjNode() = delete; // must bind to an ObjList

    /// Sole constructor: registers this node in \p list immediately.
    explicit ObjNode(ObjList<T>& list) : list_(&list), next_(nullptr), prev_(nullptr) {
        list_->insert_back(static_cast<T*>(this));
    }

    /// Destructor: unregisters this node from its list automatically.
    ~ObjNode() {
        list_->erase(static_cast<T*>(this));
    }

private:
    ObjList<T>* list_;   // non-owning; always valid
    T*          next_;
    T*          prev_;
};

// ─────────────────────────────────────────────────────────────────────────────
// ObjList<T> — intrusive doubly-linked list.
//
// Items join the list through their ObjNode<T> constructor and leave through
// their ObjNode<T> destructor.  No explicit push/remove calls are needed.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class ObjList {
    friend class ObjNode<T>;
public:
    ObjList() = default;
    ObjList(const ObjList&)            = delete;
    ObjList& operator=(const ObjList&) = delete;

    bool        empty() const { return size_ == 0; }
    std::size_t size()  const { return size_; }

    struct iterator {
        T* cur;
        T* operator->() { return cur; }
        T& operator*()  { return *cur; }
        iterator& operator++() {
            cur = static_cast<ObjNode<T>*>(cur)->next_; return *this;
        }
        bool operator!=(const iterator& o) const { return cur != o.cur; }
    };
    iterator begin() { return {head_}; }
    iterator end()   { return {nullptr}; }

    /// Snapshot: safe to iterate even if callbacks modify the list mid-loop.
    std::vector<T*> snapshot() {
        std::vector<T*> v; v.reserve(size_);
        for (auto& x : *this) v.push_back(&x);
        return v;
    }

private:
    T*          head_ = nullptr;
    T*          tail_ = nullptr;
    std::size_t size_ = 0;

    /// Called by ObjNode constructor.
    void insert_back(T* item) {
        auto* n  = static_cast<ObjNode<T>*>(item);
        n->prev_ = tail_;
        n->next_ = nullptr;
        if (tail_) static_cast<ObjNode<T>*>(tail_)->next_ = item;
        else       head_ = item;
        tail_ = item;
        ++size_;
    }

    /// Called by ObjNode destructor.
    void erase(T* item) {
        auto* n = static_cast<ObjNode<T>*>(item);
        if (n->prev_) static_cast<ObjNode<T>*>(n->prev_)->next_ = n->next_;
        else          head_ = n->next_;
        if (n->next_) static_cast<ObjNode<T>*>(n->next_)->prev_ = n->prev_;
        else          tail_ = n->prev_;
        n->next_ = n->prev_ = nullptr;
        if (size_) --size_;
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

    // Called for bytes that arrive outside KISS frame boundaries (e.g. TNC command text)
    void set_on_raw(std::function<void(uint8_t)> cb) { on_raw_ = std::move(cb); }

private:
    bool in_frame_ = false, escaped_ = false;
    std::vector<uint8_t> buf_;
    std::function<void(uint8_t)> on_raw_;
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
    int t1_ms   = 15000;              // retransmit timer ms (min, see compute_t1)
    int t3_ms   = 60000;              // keep-alive / inactivity timer ms
    int n2      = 10;                 // max retransmissions before link-fail
    int baud    = 9600;               // link speed (for dynamic T1 computation)
    int txdelay = 40;                 // KISS TX delay (×10 ms units, default 400ms)
    int persist = 63;                 // KISS persistence (0-255)

    // KISSet-style dynamic T1: max(t1_ms, window × mtu × 40000 / baud).
    // Ensures T1 is long enough for the full window to transit the link.
    int compute_t1() const {
        int link_t1 = (window * mtu * 40000) / std::max(baud, 1);
        return std::max(t1_ms, link_t1);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Kiss — KISS framing layer (transport-agnostic)
//
// Two ways to open:
//   kiss.open(device, baud)   — opens a serial port internally
//   kiss.open_fd(fd)          — uses any already-open fd (TCP socket,
//                               PTY master, pipe, …)
// ─────────────────────────────────────────────────────────────────────────────
class Kiss {
public:
    // Open a serial device (classic KISS TNC).
    bool open(const std::string& device, int baud);

    // Open with any pre-existing file descriptor (TCP socket, PTY master, …).
    // The fd is set non-blocking and is owned by Kiss (closed on close()).
    bool open_fd(int fd);

    void close();

    bool is_open()    const { return serial_.is_open() || ext_fd_ >= 0; }

    // Returns the active I/O file descriptor (ext_fd if set, else serial fd).
    int  fd()         const { return ext_fd_ >= 0 ? ext_fd_ : serial_.fd(); }
    int  serial_fd()  const { return fd(); }   // backward-compat alias

    // Register callback (called from poll() for each received AX.25 payload)
    void set_on_frame(std::function<void(std::vector<uint8_t>)> cb) {
        on_frame_ = std::move(cb);
    }

    // Called for each byte received outside KISS frame boundaries (e.g. TNC command text)
    void set_on_raw(std::function<void(uint8_t)> cb) {
        decoder_.set_on_raw(std::move(cb));
    }

    // Send raw AX.25 bytes as a KISS data frame (TNC adds flags + FCS)
    bool send_frame(const std::vector<uint8_t>& ax25);

    // KISS parameter commands
    void set_txdelay(int ms);
    void set_persistence(int val);
    void set_slottime(int ms);

    // Read from transport; fires on_frame for each complete AX.25 payload
    void poll();

    // ── Test / simulation hooks ───────────────────────────────────────────
    // Simulate receiving an AX.25 payload (as if read from the transport).
    void test_inject(const std::vector<uint8_t>& ax25) {
        if (on_frame_) on_frame_(ax25);
    }

    // If set, send_frame() calls this hook instead of the real I/O write.
    std::function<bool(const std::vector<uint8_t>&)> on_send_hook;

private:
    // Write helper: writes to ext_fd_ if set, otherwise to serial_.
    bool raw_write(const uint8_t* data, std::size_t len);

    Serial        serial_;
    int           ext_fd_ = -1;   // externally-provided fd (TCP, PTY, …)
    kiss::Decoder decoder_;
    std::function<void(std::vector<uint8_t>)> on_frame_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Connection — AX.25 connected-mode session (intrusive list node)
//
// Extends ObjNode<Connection>: the list is passed to the constructor, and
// the node auto-inserts on construction / auto-removes on destruction.
// Created only by Router; deleted by the application (delete conn).
// ─────────────────────────────────────────────────────────────────────────────
class Router;

class Connection : public ObjNode<Connection> {
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

    State       state()       const { return state_; }
    bool        connected()   const { return state_ == State::CONNECTED; }
    const Addr& remote()      const { return remote_; }
    const Addr& local()       const { return local_; }

    // True when there are frames in-flight (sent but not yet acked) or
    // queued to send.  Keep-alive should NOT inject data while this is true
    // — the AX.25 retransmit logic already takes care of the link.
    bool has_unacked() const { return !unacked_.empty() || !send_buf_.empty(); }

    // Tick timers — called by Router::poll()
    void tick(Millis now);

    ~Connection();

private:
    Connection(Router* router, ObjList<Connection>& lst,
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

    // Timer helpers — simple fixed timers like KISSet
    void start_t1(Millis now) { t1_exp_ = now + cfg_.compute_t1(); t1_run_ = true; }
    void stop_t1()            { t1_run_=false; }
    void start_t3(Millis now) { t3_exp_=now+cfg_.t3_ms; t3_run_=true; }
    void stop_t3()            { t3_run_=false; }
    void reset_t3(Millis now) { t3_exp_=now+cfg_.t3_ms; t3_run_=true; }

    Router* router_;
    Addr    local_, remote_;
    Config  cfg_;
    State   state_    = State::DISCONNECTED;
    bool    outgoing_;

    // AX.25 state variables (mod 8)
    int vs_=0, vr_=0, va_=0, retry_=0;

    // Timers
    bool   t1_run_=false; Millis t1_exp_=0;
    bool   t3_run_=false; Millis t3_exp_=0;
    bool   poll_pending_=false;

    // Flow control
    bool   peer_busy_=false;            // remote sent RNR — stop transmitting
    int    rx_since_ack_=0;             // I-frames received since last RR sent
    int    poll_sent_=0;               // count of outstanding P=1 polls awaiting F=1

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

    ObjList<Connection>&  connections()       { return conns_; }
    const Config&         config()      const { return cfg_; }
    Config&               config()            { return cfg_; }

private:
    Kiss&               kiss_;
    Config              cfg_;
    ObjList<Connection> conns_;
    std::function<void(Connection*)> on_accept_;

    // TX pacing — one frame per TXDELAY interval
    std::deque<std::vector<uint8_t>> tx_queue_;   // encoded frames waiting to go out
    Millis                           tx_next_ = 0; // earliest time next frame may be sent

    void route(std::vector<uint8_t> raw, Millis now);
    Connection* find(const Addr& local, const Addr& remote);
    bool        tx(const Frame& f);
    bool        send_frame(const Frame& f) { return tx(f); }  // for Connection
    void        drain_tx(Millis now);   // send queued frames respecting TXDELAY pacing
};

// ─────────────────────────────────────────────────────────────────────────────
// TNC KISS initialization
// Sends "KISS ON / RESTART / INTERFACE KISS / RESET" to a traditional TNC
// in command mode, waits 2 s, then drains any response bytes.
// Call with kiss.fd() right after opening the port, before set_txdelay().
// ─────────────────────────────────────────────────────────────────────────────
void tnc_kiss_init(int fd);

// ─────────────────────────────────────────────────────────────────────────────
// CLI parameter helper
// ─────────────────────────────────────────────────────────────────────────────
struct CLIParams {
    std::string device;
    int         baud     = 9600;
    bool        tnc_init = false;   // --tnc: send KISS ON / RESTART / INTERFACE KISS / RESET
    Config      cfg;

    // Parses standard AX.25 flags from argv.
    // Returns false (and prints usage) if required args are missing.
    bool parse(int argc, char* argv[], const char* extra_usage = "");
    static void print_help(const char* prog, const char* extra = "");
};

// ─────────────────────────────────────────────────────────────────────────────
// APRS helpers (ax25::aprs namespace)
// ─────────────────────────────────────────────────────────────────────────────
namespace aprs {

// Parsed APRS message
struct Msg { std::string to, text, seq; };

// Build APRS position info string  (!DDMM.mmN<table>DDDMM.mmW<sym>[comment])
// lat/lon: decimal degrees (negative = S / W)
// sym: APRS symbol code char (default '>' = car)
// table: symbol table ID ('/' = primary, '\\' = alternate, overlay A-Z/0-9)
std::string make_pos(double lat, double lon, char sym = '>', char table = '/',
                     const std::string& comment = "");

// Build APRS message info string  (:ADDRESSEE :text{seq})
// Sequence number auto-incremented globally.
std::string make_msg(const std::string& dest, const std::string& text);

// Parse APRS message info field. Returns true and fills 'out' on success.
bool parse_msg(const std::string& info, Msg& out);

// Returns true if info field looks like a position report (starts with !/@=/`)
bool is_pos(const std::string& info);

// Extract printable text from a Frame's info field (replaces non-printable with '.')
std::string info_str(const Frame& f);

} // namespace aprs

} // namespace ax25
