// =============================================================================
// ax25client.cpp — Complete AX.25 / KISS TNC client  (C++11, POSIX)
//
// Acts as a classic packet-radio terminal with three operating modes:
//
//   connect  — AX.25 connected session with Go-Back-N ARQ.
//              Typed text is sent as I-frames; received data prints to stdout.
//              Tilde-escape commands control the session interactively.
//
//   monitor  — Passive receive-only mode.  All decoded AX.25 frames (UI,
//              connected-mode, APRS) are printed; nothing is transmitted.
//
//   unproto  — Connectionless (UI frame) mode.  Each input line is sent
//              as a UI frame to the chosen destination.  Received UI frames
//              matching the source callsign are displayed.
//
// Build:
//   g++ -std=c++11 -O2 -Wall -o ax25client ax25client.cpp ax25lib.cpp
//
// Usage:
//   ax25client [OPTIONS] <serial_device>
//
// Options:
//   -c CALL         My callsign (required)
//   -r REMOTE       Remote station callsign  (connect mode default)
//   -m MODE         Operating mode: connect | monitor | unproto (default: connect)
//   -d DEST         Destination for unproto / APRS (default: CQ)
//   -b BAUD         Baud rate (default: 9600)
//   -p PATH         Digipeater path, comma-separated (e.g. WIDE1-1,WIDE2-1)
//   -M              Enable frame monitor even in connect/unproto mode
//   -w WIN          Window size 1-7 (default: 3)
//   -t T1_MS        T1 retransmit timer ms (default: 3000)
//   -k T3_MS        T3 keep-alive timer ms (default: 60000)
//   -n N2           Max retry count (default: 10)
//   --mtu BYTES     I-frame MTU bytes (default: 128)
//   --txdelay MS    KISS TX delay ms (default: 300)
//   --pid HEX       PID for UI frames in hex (default: F0)
//   -s FILE         Run BASIC script after connect (connect mode only)
//                   Pre-set vars: remote$, local$, callsign$
//   --ka SECS       App-level keep-alive: send CR every N seconds while idle (default: 60, 0=off)
//   -h              Show this help
//
// Tilde-escape commands (connect mode only, entered at the start of a line):
//   ~.        disconnect and exit
//   ~s        show connection status / statistics
//   ~m        toggle monitor mode on/off
//   ~r        redisplay prompt
//   ~x FILE   run BASIC script while staying connected; return to interactive after
//   ~?        show tilde-escape help
// =============================================================================

#include "ax25lib.hpp"
#include "basic.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace ax25;

// ─────────────────────────────────────────────────────────────────────────────
// Global state — signal handler sets this
// ─────────────────────────────────────────────────────────────────────────────
static volatile sig_atomic_t g_quit = 0;

static void handle_signal(int) { g_quit = 1; }

// ─────────────────────────────────────────────────────────────────────────────
// ANSI colour helpers (disabled when stdout is not a tty)
// ─────────────────────────────────────────────────────────────────────────────
static bool g_colour = false;

static const char* BOLD()  { return g_colour ? "\033[1m"    : ""; }
static const char* DIM()   { return g_colour ? "\033[2m"    : ""; }
static const char* GREEN() { return g_colour ? "\033[32m"   : ""; }
static const char* CYAN()  { return g_colour ? "\033[36m"   : ""; }
static const char* RED()   { return g_colour ? "\033[31m"   : ""; }
static const char* YELLOW(){ return g_colour ? "\033[33m"   : ""; }
static const char* RESET() { return g_colour ? "\033[0m"    : ""; }

// ─────────────────────────────────────────────────────────────────────────────
// Statistics counter
// ─────────────────────────────────────────────────────────────────────────────
struct Stats {
    uint64_t frames_rx  = 0;   // total decoded AX.25 frames received
    uint64_t frames_tx  = 0;   // total frames transmitted
    uint64_t bytes_rx   = 0;   // data bytes received (I-frame info or UI info)
    uint64_t bytes_tx   = 0;   // data bytes sent
    uint64_t ui_rx      = 0;   // UI frames received
    uint64_t connect_t  = 0;   // UNIX time when session was connected
};

// ─────────────────────────────────────────────────────────────────────────────
// Operating modes
// ─────────────────────────────────────────────────────────────────────────────
enum class Mode { Connect, Monitor, Unproto };

static Mode parse_mode(const char* s) {
    std::string m(s);
    for (auto& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (m == "monitor" || m == "mon") return Mode::Monitor;
    if (m == "unproto" || m == "ui")  return Mode::Unproto;
    return Mode::Connect;
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────
struct AppCfg {
    std::string device;
    std::string remote;                   // connect mode: remote callsign
    std::string dest     = "CQ";          // unproto destination
    Mode        mode     = Mode::Connect;
    bool        monitor  = false;         // extra monitor in connect/unproto
    uint8_t     pid      = 0xF0;          // UI frame PID
    int         txdelay  = 300;           // KISS TX delay ms
    Config      ax25;                     // ax25lib Config (mycall, mtu, etc.)
    int         baud     = 9600;
    std::string script;                   // path to BASIC script (-s); empty = interactive
    int         ka_ms    = 60000;         // app-level keep-alive interval ms (0=off)
};

// ─────────────────────────────────────────────────────────────────────────────
// TCP connect helper (returns socket fd, or -1 on error)
// ─────────────────────────────────────────────────────────────────────────────
static int tcp_connect_fd(const std::string& host, const std::string& port_str) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
        return -1;
    int s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { ::freeaddrinfo(res); return -1; }
    if (::connect(s, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(s); ::freeaddrinfo(res); return -1;
    }
    ::freeaddrinfo(res);
    int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return s;
}

// Detect "host:port" format — true if the string does NOT start with '/'
// and the substring after the last ':' is all digits.
static bool is_tcp_address(const std::string& s, std::string& host, std::string& port) {
    if (s.empty() || s[0] == '/') return false;   // unix path
    auto p = s.rfind(':');
    if (p == std::string::npos) return false;
    std::string pt = s.substr(p + 1);
    if (pt.empty()) return false;
    for (char c : pt) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    host = s.substr(0, p);
    port = pt;
    return true;
}

static void print_usage(const char* prog) {
    std::cerr
        << "AX.25 / KISS TNC client — " << BOLD() << prog << RESET() << "\n\n"
        << "Usage:\n"
        << "  " << prog << " [OPTIONS] <device|host:port>\n\n"
        << "Modes  (-m):\n"
        << "  connect    AX.25 connected session with ARQ  (default when -r given)\n"
        << "  monitor    Passive frame monitor (no TX)\n"
        << "  unproto    Connectionless UI frames\n\n"
        << "Options:\n"
        << "  -c CALL      My callsign (required)\n"
        << "  -r REMOTE    Remote station (connect mode)\n"
        << "  -m MODE      connect | monitor | unproto\n"
        << "  -d DEST      Destination callsign for unproto (default: CQ)\n"
        << "  -b BAUD      Baud rate for serial (default: 9600; ignored for TCP)\n"
        << "  -p PATH      Digipeater path, comma-separated\n"
        << "  -M           Enable monitor output in connect/unproto mode\n"
        << "  -w WIN       Window size 1-7 (default: 3)\n"
        << "  -t T1        T1 retransmit timer ms (default: 3000)\n"
        << "  -k T3        T3 keep-alive timer ms (default: 60000)\n"
        << "  -n N2        Max retry count (default: 10)\n"
        << "  --mtu N      I-frame MTU bytes (default: 128)\n"
        << "  --txdelay N  KISS TX delay ms (default: 300)\n"
        << "  --pid HEX    PID for UI frames (default: F0)\n"
        << "  -s FILE      BASIC script to run after connect\n"
        << "  --ka SECS    App-level keep-alive: send CR every N seconds when idle (default: 60, 0=off)\n"
        << "  -h           Show this help\n\n"
        << "Tilde escapes (connect mode):\n"
        << "  ~.        disconnect and exit\n"
        << "  ~s        show status / statistics\n"
        << "  ~m        toggle frame monitor\n"
        << "  ~x FILE   run BASIC script (stay connected after)\n"
        << "  ~?        show this help\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument parser
// ─────────────────────────────────────────────────────────────────────────────
static bool parse_args(int argc, char* argv[], AppCfg& cfg) {
    // Long options
    static struct option longopts[] = {
        {"mtu",     required_argument, nullptr, 1001},
        {"txdelay", required_argument, nullptr, 1002},
        {"pid",     required_argument, nullptr, 1003},
        {"script",  required_argument, nullptr, 's'},
        {"ka",      required_argument, nullptr, 1004},
        {nullptr, 0, nullptr, 0}
    };

    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "c:r:m:d:b:p:Mw:t:k:n:s:h", longopts, &idx)) != -1) {
        switch (opt) {
        case 'c': cfg.ax25.mycall = Addr::make(optarg); break;
        case 'r': cfg.remote      = optarg; break;
        case 'm': cfg.mode        = parse_mode(optarg); break;
        case 'd': cfg.dest        = optarg; break;
        case 'b': cfg.baud        = std::atoi(optarg); break;
        case 'p': {
            cfg.ax25.digis.clear();
            std::istringstream ss(optarg); std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) cfg.ax25.digis.push_back(Addr::make(tok));
            break;
        }
        case 'M': cfg.monitor     = true; break;
        case 'w': cfg.ax25.window = std::max(1, std::min(7, std::atoi(optarg))); break;
        case 't': cfg.ax25.t1_ms  = std::atoi(optarg); break;
        case 'k': cfg.ax25.t3_ms  = std::atoi(optarg); break;
        case 'n': cfg.ax25.n2     = std::atoi(optarg); break;
        case 1001: cfg.ax25.mtu   = std::atoi(optarg); break;
        case 1002: cfg.txdelay    = std::atoi(optarg); break;
        case 1003: cfg.pid        = static_cast<uint8_t>(std::strtoul(optarg, nullptr, 16)); break;
        case 's':  cfg.script     = optarg; break;
        case 1004: cfg.ka_ms     = std::atoi(optarg) * 1000; break;
        case 'h':  print_usage(argv[0]); return false;
        default:   print_usage(argv[0]); return false;
        }
    }

    if (optind < argc) cfg.device = argv[optind];

    if (cfg.device.empty()) {
        std::cerr << "Error: device or host:port required.\n";
        print_usage(argv[0]); return false;
    }
    if (cfg.ax25.mycall.empty()) {
        std::cerr << "Error: callsign required  (-c CALL[-SSID])\n";
        return false;
    }
    if (cfg.mode == Mode::Connect && cfg.remote.empty()) {
        std::cerr << "Error: connect mode requires a remote callsign  (-r REMOTE)\n";
        return false;
    }
    if (cfg.mode == Mode::Connect && !cfg.remote.empty() &&
        Addr::make(cfg.remote) == cfg.ax25.mycall) {
        std::cerr << "Error: remote callsign (-r " << cfg.remote
                  << ") cannot be the same as your own callsign (-c "
                  << cfg.ax25.mycall.str() << ").\n"
                  << "       A SABM addressed to yourself will never receive a UA reply.\n";
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame formatter for monitor output
// ─────────────────────────────────────────────────────────────────────────────
static std::string timestamp() {
    time_t t = time(nullptr);
    char buf[24];
    struct tm* tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    return buf;
}

static void print_frame(const Frame& f, const char* direction = "") {
    std::cout << DIM() << "[" << timestamp() << "]" << RESET()
              << " " << CYAN() << direction << RESET()
              << " " << f.format() << "\n" << std::flush;
}

// ─────────────────────────────────────────────────────────────────────────────
// Show status (connect mode)
// ─────────────────────────────────────────────────────────────────────────────
static void show_status(const AppCfg& cfg, const Connection* conn, const Stats& st) {
    std::cout << "\n" << BOLD() << "=== Status ===" << RESET() << "\n"
              << "  Local  : " << cfg.ax25.mycall.str() << "\n"
              << "  Remote : " << cfg.remote << "\n"
              << "  Device : " << cfg.device << "\n"
              << "  State  : ";
    if (conn) {
        switch (conn->state()) {
        case Connection::State::CONNECTED:     std::cout << GREEN() << "CONNECTED" << RESET(); break;
        case Connection::State::CONNECTING:    std::cout << YELLOW() << "CONNECTING" << RESET(); break;
        case Connection::State::DISCONNECTING: std::cout << YELLOW() << "DISCONNECTING" << RESET(); break;
        default:                               std::cout << RED() << "DISCONNECTED" << RESET(); break;
        }
    } else {
        std::cout << RED() << "DISCONNECTED" << RESET();
    }
    std::cout << "\n"
              << "  Frames RX : " << st.frames_rx << "  ("
                 << st.bytes_rx  << " data bytes)\n"
              << "  Frames TX : " << st.frames_tx << "  ("
                 << st.bytes_tx  << " data bytes)\n"
              << "  UI RX     : " << st.ui_rx << "\n"
              << "  Window    : " << cfg.ax25.window
              << "  MTU=" << cfg.ax25.mtu
              << "  T1=" << cfg.ax25.t1_ms << "ms"
              << "  T3=" << cfg.ax25.t3_ms << "ms"
              << "  KA=" << (cfg.ka_ms > 0 ? std::to_string(cfg.ka_ms / 1000) + "s" : "off")
              << "\n\n"
              << std::flush;
}

// ─────────────────────────────────────────────────────────────────────────────
// Read one line from stdin, non-blocking via select()
// Returns false if EOF or error
// ─────────────────────────────────────────────────────────────────────────────
static bool stdin_readline(std::string& line, int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0) return false;
    if (!std::getline(std::cin, line)) { g_quit = 1; return false; }
    // getline strips \n but leaves \r if the terminal sends \r\n
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── MONITOR MODE ─────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
static int run_monitor(Kiss& kiss, Router& router) {
    router.on_monitor = [](const Frame& f) { print_frame(f, "RX"); };

    std::cout << GREEN() << "Monitor mode active." << RESET()
              << "  Press Ctrl-C to exit.\n\n" << std::flush;

    while (!g_quit) {
        router.poll();
        struct timeval tv{ 0, 20000 }; // 20 ms
        fd_set fds; FD_ZERO(&fds);
        FD_SET(kiss.fd(), &fds);
        select(kiss.fd() + 1, &fds, nullptr, nullptr, &tv);
    }

    std::cout << "\nMonitor stopped.\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── UNPROTO MODE ─────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
static int run_unproto(Kiss& kiss, Router& router, const AppCfg& cfg) {
    Stats st;
    Addr  dest   = Addr::make(cfg.dest);
    int   ser_fd = kiss.fd();

    // Show all received UI frames
    router.on_ui = [&](const Frame& f) {
        ++st.ui_rx; ++st.frames_rx;
        st.bytes_rx += f.info.size();
        if (cfg.monitor) print_frame(f, "RX");
        else {
            std::string info(f.info.begin(), f.info.end());
            while (!info.empty() && (info.back() == '\r' || info.back() == '\n'))
                info.pop_back();
            std::cout << CYAN() << f.src.str() << RESET()
                      << " > " << f.dest.str() << " : "
                      << info << "\n" << std::flush;
        }
    };

    if (cfg.monitor) router.on_monitor = [](const Frame& f) { print_frame(f, "RX"); };

    std::cout << GREEN() << "Unproto mode." << RESET()
              << "  Sending to " << BOLD() << cfg.dest << RESET()
              << " from " << cfg.ax25.mycall.str()
              << "  (Ctrl-C to exit)\n\n" << std::flush;

    while (!g_quit) {
        // Poll serial port — wait up to 20 ms before checking stdin
        {
            struct timeval tv{ 0, 20000 };
            fd_set fds; FD_ZERO(&fds); FD_SET(ser_fd, &fds);
            select(ser_fd + 1, &fds, nullptr, nullptr, &tv);
        }
        router.poll();

        // Non-blocking stdin
        std::string line;
        if (stdin_readline(line, 0)) {
            if (line == "/quit" || line == "/exit") break;
            if (line.empty()) continue;
            std::string payload = line + "\r";   // CR only — packet radio convention
            router.send_ui(dest, cfg.pid, payload, cfg.ax25.digis);
            ++st.frames_tx;
            st.bytes_tx += payload.size();
        }
    }

    std::cout << "\nUnproto session ended.  TX=" << st.frames_tx
              << " RX=" << st.frames_rx << "\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── CONNECT MODE ─────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
static int run_connect(Kiss& kiss, Router& router, const AppCfg& cfg) {
    int         ser_fd    = kiss.fd();
    Stats       st;
    Connection* conn      = nullptr;
    bool        done      = false;
    bool        mon_on    = cfg.monitor;
    Addr        remote    = Addr::make(cfg.remote);
    std::string recv_buf;                    // partial-line buffer for interactive mode
    std::deque<std::string> rx_lines;        // complete lines queued for BASIC on_recv

    // ── Monitor hook (optional) ──────────────────────────────────────────────
    if (mon_on) {
        router.on_monitor = [](const Frame& f){ print_frame(f, ">>"); };
    }

    // ── Shared connection-event callbacks ────────────────────────────────────
    auto attach_callbacks = [&](Connection* c) {
        c->on_connect = [&, c]{
            std::cout << "\n" << GREEN() << BOLD()
                      << "*** Connected to " << c->remote().str()
                      << " ***" << RESET() << "\n\n" << std::flush;
            st.connect_t = static_cast<uint64_t>(time(nullptr));
        };
        c->on_disconnect = [&]{
            std::cout << "\n" << RED() << BOLD()
                      << "*** Disconnected ***" << RESET() << "\n\n" << std::flush;
            done = true;
        };
        c->on_data = [&](const uint8_t* d, std::size_t n) {
            st.bytes_rx += n;
            ++st.frames_rx;
            // Split incoming bytes on CR/LF and push complete lines
            recv_buf.append(reinterpret_cast<const char*>(d), n);
            std::size_t pos;
            while ((pos = recv_buf.find_first_of("\r\n")) != std::string::npos) {
                std::string line_in = recv_buf.substr(0, pos);
                recv_buf.erase(0, pos + 1);
                if (line_in.empty()) continue;  // swallow bare CR/LF of CRLF pairs
                rx_lines.push_back(line_in);
                // In interactive (non-script) mode also print to stdout
                if (cfg.script.empty())
                    std::cout << line_in << "\n" << std::flush;
            }
        };
    };

    // ── Establish connection ─────────────────────────────────────────────────
    if (cfg.remote == "*" || cfg.remote == "ANY") {
        std::cout << YELLOW() << "Waiting for incoming connection on "
                  << cfg.ax25.mycall.str() << "..." << RESET() << "\n" << std::flush;
        router.listen([&](Connection* c) {
            conn = c;
            attach_callbacks(c);
        });
    } else {
        std::cout << YELLOW() << "Connecting to "
                  << BOLD() << remote.str() << RESET()
                  << YELLOW() << " from " << cfg.ax25.mycall.str()
                  << "..." << RESET() << "\n" << std::flush;
        conn = router.connect(remote);
        attach_callbacks(conn);
        ++st.frames_tx;   // SABM
    }

    // ── Wait until connected (needed before BASIC can run) ───────────────────
    if (!cfg.script.empty()) {
        std::cout << DIM() << "[Waiting for connection before running script…]"
                  << RESET() << "\n" << std::flush;
        while (!g_quit && !done && conn &&
               conn->state() != Connection::State::CONNECTED) {
            struct timeval tv{ 0, 20000 };
            fd_set fds; FD_ZERO(&fds); FD_SET(ser_fd, &fds);
            select(ser_fd + 1, &fds, nullptr, nullptr, &tv);
            router.poll();
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // ── BASIC script mode ────────────────────────────────────────────────────
    // ═════════════════════════════════════════════════════════════════════════
    if (!cfg.script.empty() && !g_quit && !done) {
        Basic interp;

        // ── Wire output: PRINT / SEND → AX.25 send ──────────────────────────
        interp.on_send = [&](const std::string& s) {
            if (!conn) return;
            if (conn->send(s)) {
                st.bytes_tx += s.size();
                ++st.frames_tx;
            }
        };

        // ── Wire input: INPUT / RECV ← poll serial + drain rx_lines ─────────
        interp.on_recv = [&](int timeout_ms) -> std::string {
            // Already have a queued line?
            if (!rx_lines.empty()) {
                std::string l = rx_lines.front(); rx_lines.pop_front();
                return l;
            }
            // Poll until we get one or time out
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
            while (!g_quit && !done) {
                struct timeval tv{ 0, 20000 };
                fd_set fds; FD_ZERO(&fds); FD_SET(ser_fd, &fds);
                select(ser_fd + 1, &fds, nullptr, nullptr, &tv);
                router.poll();
                if (!rx_lines.empty()) {
                    std::string l = rx_lines.front(); rx_lines.pop_front();
                    return l;
                }
                if (std::chrono::steady_clock::now() >= deadline) break;
            }
            return "";
        };

        // ── Wire log ─────────────────────────────────────────────────────────
        interp.on_log = [](const std::string& msg) {
            std::cerr << "[BASIC] " << msg << "\n";
        };

        // ── Pre-set session variables ─────────────────────────────────────────
        interp.set_str("REMOTE$",   cfg.remote);
        interp.set_str("LOCAL$",    cfg.ax25.mycall.str());
        interp.set_str("CALLSIGN$", cfg.remote);   // alias expected by BBS scripts

        // ── Load and run ─────────────────────────────────────────────────────
        if (!interp.load_file(cfg.script)) {
            std::cerr << "Error: cannot open script: " << cfg.script << "\n";
        } else {
            interp.run();
        }

        done = true;  // script finished → disconnect
    }

    // ═════════════════════════════════════════════════════════════════════════
    // ── Interactive mode ─────────────────────────────────────────────────────
    // ═════════════════════════════════════════════════════════════════════════
    using Clock   = std::chrono::steady_clock;
    auto last_tx  = Clock::now();   // tracks time of last user/keep-alive send

    while (!g_quit && !done) {
        // Poll serial port (20 ms) then tick timers
        {
            struct timeval tv{ 0, 20000 };
            fd_set fds; FD_ZERO(&fds); FD_SET(ser_fd, &fds);
            select(ser_fd + 1, &fds, nullptr, nullptr, &tv);
        }
        router.poll();

        // ── App-level keep-alive ─────────────────────────────────────────────
        // Only inject a CR when the AX.25 window is empty: if there are
        // unacked frames the retransmit machinery already keeps the link
        // alive, and adding new I-frames would worsen a REJ situation.
        if (cfg.ka_ms > 0 && conn && conn->connected() && !conn->has_unacked()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               Clock::now() - last_tx).count();
            if (elapsed >= cfg.ka_ms) {
                if (conn->send("\r")) {
                    last_tx = Clock::now();
                    std::cout << DIM() << "[keep-alive]" << RESET() << "\n" << std::flush;
                    ++st.frames_tx;
                }
            }
        }

        // Non-blocking stdin read
        std::string line;
        if (!stdin_readline(line, 0)) continue;

        // ── Tilde-escape processing ──────────────────────────────────────────
        if (line.size() >= 2 && line[0] == '~') {
            char esc = line[1];
            switch (esc) {
            case '.':
                std::cout << YELLOW() << "Disconnecting..." << RESET() << "\n" << std::flush;
                if (conn && conn->connected()) conn->disconnect();
                done = true;
                break;
            case 's':
                show_status(cfg, conn, st);
                break;
            case 'm':
                mon_on = !mon_on;
                router.on_monitor = mon_on
                    ? std::function<void(const Frame&)>([](const Frame& f){ print_frame(f,">>"); })
                    : nullptr;
                std::cout << DIM() << "[Monitor " << (mon_on ? "ON" : "OFF") << "]"
                          << RESET() << "\n" << std::flush;
                break;
            case 'r':
                // Redraw prompt (useful after unsolicited data scrolled the line)
                std::cout << std::flush;
                break;
            case 'x': {
                // Run a BASIC script without disconnecting; return to interactive after
                std::string fname = (line.size() > 2) ? line.substr(2) : "";
                while (!fname.empty() && (fname.front() == ' ' || fname.front() == '\t'))
                    fname.erase(fname.begin());
                if (fname.empty()) {
                    std::cout << RED() << "[Usage: ~x <script.bas>]"
                              << RESET() << "\n" << std::flush;
                    break;
                }
                if (!conn || !conn->connected()) {
                    std::cout << RED() << "[Not connected — cannot run script]"
                              << RESET() << "\n" << std::flush;
                    break;
                }
                std::cout << DIM() << "[Running script: " << fname << "]"
                          << RESET() << "\n" << std::flush;
                {
                    Basic interp;
                    interp.on_send = [&](const std::string& s) {
                        if (!conn) return;
                        if (conn->send(s)) {
                            last_tx = Clock::now();
                            st.bytes_tx += s.size();
                            ++st.frames_tx;
                        }
                    };
                    interp.on_recv = [&](int tmo) -> std::string {
                        if (!rx_lines.empty()) {
                            std::string l = rx_lines.front(); rx_lines.pop_front();
                            return l;
                        }
                        auto deadline = Clock::now() + std::chrono::milliseconds(tmo);
                        while (!g_quit && !done) {
                            struct timeval tv{ 0, 20000 };
                            fd_set fds; FD_ZERO(&fds); FD_SET(ser_fd, &fds);
                            select(ser_fd + 1, &fds, nullptr, nullptr, &tv);
                            router.poll();
                            if (!rx_lines.empty()) {
                                std::string l = rx_lines.front(); rx_lines.pop_front();
                                return l;
                            }
                            if (Clock::now() >= deadline) break;
                        }
                        return "";
                    };
                    interp.on_log = [](const std::string& msg) {
                        std::cerr << "[BASIC] " << msg << "\n";
                    };
                    interp.set_str("REMOTE$",   cfg.remote);
                    interp.set_str("LOCAL$",    cfg.ax25.mycall.str());
                    interp.set_str("CALLSIGN$", cfg.remote);
                    if (!interp.load_file(fname)) {
                        std::cout << RED() << "[Error: cannot open: " << fname << "]"
                                  << RESET() << "\n" << std::flush;
                    } else {
                        interp.run();
                        std::cout << DIM() << "[Script finished]"
                                  << RESET() << "\n" << std::flush;
                    }
                }
                last_tx = Clock::now();   // reset keep-alive clock after script
                break;
            }
            case '?':
                std::cout << "\nTilde escapes:\n"
                          << "  ~.        disconnect and exit\n"
                          << "  ~s        show status / statistics\n"
                          << "  ~m        toggle monitor\n"
                          << "  ~r        redraw\n"
                          << "  ~x FILE   run BASIC script (stay connected after)\n"
                          << "  ~?        this help\n\n" << std::flush;
                break;
            default:
                std::cout << DIM() << "[Unknown escape: ~" << esc
                          << " — type ~? for help]" << RESET() << "\n" << std::flush;
            }
            continue;
        }

        // ── Send data ────────────────────────────────────────────────────────
        if (!conn) {
            std::cout << RED() << "[Not connected]" << RESET() << "\n" << std::flush;
            continue;
        }
        if (!conn->connected()) {
            std::cout << YELLOW() << "[Not yet connected — please wait]"
                      << RESET() << "\n" << std::flush;
            continue;
        }

        std::string payload = line + "\r";   // CR only — packet radio convention
        if (conn->send(payload)) {
            last_tx = Clock::now();
            st.bytes_tx += payload.size();
            ++st.frames_tx;
        } else {
            std::cout << RED() << "[Send failed — connection may have dropped]"
                      << RESET() << "\n" << std::flush;
        }
    }

    // ── Cleanup ──────────────────────────────────────────────────────────────
    if (conn && conn->connected()) {
        if (g_quit)
            std::cout << "\n" << YELLOW() << "Signal received — disconnecting…"
                      << RESET() << "\n" << std::flush;
        conn->disconnect();
        // Poll until DISC is acknowledged (UA received) or 1 s elapses.
        // A single poll() is not enough: serial I/O only happens inside poll().
        auto t0 = std::chrono::steady_clock::now();
        while (conn->state() != Connection::State::DISCONNECTED) {
            struct timeval tv{ 0, 20000 };
            fd_set fds; FD_ZERO(&fds); FD_SET(ser_fd, &fds);
            select(ser_fd + 1, &fds, nullptr, nullptr, &tv);
            router.poll();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            if (ms >= 1000) break;   // give up after 1 s
        }
    }

    std::cout << "\nSession summary:\n"
              << "  TX: " << st.frames_tx << " frames / " << st.bytes_tx << " bytes\n"
              << "  RX: " << st.frames_rx << " frames / " << st.bytes_rx << " bytes\n";

    delete conn;
    return 0;
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char* argv[]) {
    // ── Parse arguments ──────────────────────────────────────────────────────
    AppCfg cfg;
    if (!parse_args(argc, argv, cfg)) return 1;

    // Detect if stdout is a terminal (enable colour)
    g_colour = isatty(STDOUT_FILENO) != 0;

    // ── Signal handling ──────────────────────────────────────────────────────
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // ── Open KISS transport (serial or TCP) ─────────────────────────────────
    Kiss kiss;
    {
        std::string tcp_host, tcp_port;
        if (is_tcp_address(cfg.device, tcp_host, tcp_port)) {
            // TCP mode: connect socket, hand fd to Kiss
            int tcp_fd = tcp_connect_fd(tcp_host, tcp_port);
            if (tcp_fd < 0) {
                std::cerr << "Error: cannot connect to " << cfg.device << ": "
                          << strerror(errno) << "\n";
                return 1;
            }
            kiss.open_fd(tcp_fd);
        } else {
            // Serial mode
            if (!kiss.open(cfg.device, cfg.baud)) {
                std::cerr << "Error: cannot open " << cfg.device << ": "
                          << strerror(errno) << "\n";
                return 1;
            }
        }
    }
    kiss.set_txdelay(cfg.txdelay);
    kiss.set_persistence(cfg.ax25.persist);

    // ── Create Router ────────────────────────────────────────────────────────
    Router router(kiss, cfg.ax25);

    // ── Banner ───────────────────────────────────────────────────────────────
    {
        std::string tcp_host, tcp_port;
        bool is_tcp = is_tcp_address(cfg.device, tcp_host, tcp_port);
        std::cout << BOLD() << "ax25client" << RESET()
                  << "  " << cfg.ax25.mycall.str()
                  << "  " << cfg.device
                  << (is_tcp ? "  TCP" : ("  @" + std::to_string(cfg.baud) + " baud"))
                  << "\n";
    }

    switch (cfg.mode) {
    case Mode::Monitor: return run_monitor(kiss, router);
    case Mode::Unproto: return run_unproto(kiss, router, cfg);
    case Mode::Connect: return run_connect(kiss, router, cfg);
    }
    return 0;
}
