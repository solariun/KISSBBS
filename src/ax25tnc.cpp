// =============================================================================
// ax25tnc.cpp — Complete AX.25 / KISS TNC client  (C++11, POSIX)
//
// Acts as a classic packet-radio terminal with four operating modes:
//
//   tnc      — Interactive TNC terminal (DEFAULT).  Two sub-modes:
//              • Command mode: type TNC commands (C, D, MYC, MON, …)
//              • Data mode:    typed text sent as I-frames; ~ escapes available.
//              Incoming connections are always accepted (router.listen active).
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
//   g++ -std=c++11 -O2 -Wall -o ax25tnc ax25tnc.cpp ax25lib.cpp basic.o
//
// Usage:
//   ax25tnc [OPTIONS] <serial_device>
//
// Options:
//   -c CALL         My callsign (default: N0CALL)
//   -r REMOTE       Remote station callsign  (auto-connect in TNC mode)
//   -m MODE         Operating mode: tnc | connect | monitor | unproto (default: tnc)
//   -d DEST         Destination for unproto / APRS (default: CQ)
//   -b BAUD         Baud rate (default: 9600)
//   -p PATH         Digipeater path, comma-separated (e.g. WIDE1-1,WIDE2-1)
//   -M              Enable frame monitor
//   -w WIN          Window size 1-7 (default: 3)
//   -t T1_MS        T1 retransmit timer ms (default: 3000)
//   -k T3_MS        T3 keep-alive timer ms (default: 60000)
//   -n N2           Max retry count (default: 10)
//   --mtu BYTES     I-frame MTU bytes (default: 128)
//   --txdelay MS    KISS TX delay ms (default: 400)
//   --pid HEX       PID for UI frames in hex (default: F0)
//   -s FILE         Run BASIC script after connect (connect mode only)
//                   Pre-set vars: remote$, local$, callsign$
//   --ka SECS       App-level keep-alive: send CR every N seconds while idle (default: 60, 0=off)
//   -h              Show this help
//
// TNC commands (command mode):
//   C <call>        Connect to callsign
//   D               Disconnect current connection
//   L               Show listen status
//   MYC <call>      Set my callsign
//   MON [ON|OFF]    Toggle frame monitor
//   UNPROTO [ON|OFF] Toggle UI frame display
//   STATUS          Show link status and stats
//   WIN <n>         Set window size 1-7
//   T1 <ms>         Set T1 retransmit timer
//   T3 <ms>         Set T3 keep-alive timer
//   MTU <bytes>     Set MTU
//   TXDELAY <ms>    Set KISS TX delay
//   SCRIPT <file>   Run BASIC script (only when connected)
//   HELP            Show command help
//   QUIT            Exit
//
// Tilde-escape commands (data mode, entered at the start of a line):
//   ~.        disconnect, return to command mode
//   ~d        same as ~.
//   ~s        show connection status / statistics
//   ~x FILE   run BASIC script while staying connected; return to data mode after
//   ~?        show tilde-escape help
//   ~~        send literal ~
// =============================================================================

#include "ax25lib.hpp"
#include "ax25dump.hpp"
#include "basic.hpp"
#include "script_finder.hpp"

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
// Global state — signal handler sets these
// ─────────────────────────────────────────────────────────────────────────────
static volatile sig_atomic_t g_ctrl_c_count = 0;
static volatile sig_atomic_t g_quit         = 0;
static time_t                g_ctrl_c_time  = 0;

static void handle_signal(int sig) {
    if (sig == SIGTERM) { g_quit = 1; return; }
    // SIGINT
    time_t now = time(nullptr);
    if (now - g_ctrl_c_time > 5) g_ctrl_c_count = 0;
    g_ctrl_c_time = now;
    if (++g_ctrl_c_count >= 2) g_quit = 1;
}

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
enum class Mode { Tnc, Connect, Monitor, Unproto, Test };

static Mode parse_mode(const char* s) {
    std::string m(s);
    for (auto& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (m == "monitor" || m == "mon") return Mode::Monitor;
    if (m == "unproto" || m == "ui")  return Mode::Unproto;
    if (m == "connect" || m == "con") return Mode::Connect;
    return Mode::Tnc;
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────
struct AppCfg {
    std::string device;
    std::string remote;                   // connect mode: remote callsign
    std::string dest     = "CQ";          // unproto destination
    Mode        mode     = Mode::Tnc;
    bool        tnc_mode = true;          // true when -m was NOT explicitly given
    bool        kiss_tnc = false;         // --tnc: send KISS ON/RESTART/INTERFACE KISS/RESET
    bool        monitor  = false;         // extra monitor in connect/unproto
    uint8_t     pid      = 0xF0;          // UI frame PID
    int         txdelay  = 400;           // KISS TX delay ms
    Config      ax25;                     // ax25lib Config (mycall, mtu, etc.)
    int         baud     = 9600;
    std::string script;                   // BASIC script name/path/pattern (-s); empty = interactive
    ScriptFinder scripts;                 // script search paths (--bas-path, AX25TK_BASIC_PATH)
    int         ka_ms    = 60000;         // app-level keep-alive interval ms (0=off)
    bool        debug    = false;         // verbose routing debug output (-D)
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
        << "Default mode: TNC interactive terminal (type H for help once running)\n\n"
        << "Modes  (-m):\n"
        << "  tnc        Interactive TNC terminal with command + data modes  (DEFAULT)\n"
        << "  connect    AX.25 connected session with ARQ\n"
        << "  monitor    Passive frame monitor (no TX)\n"
        << "  unproto    Connectionless UI frames\n\n"
        << "Options:\n"
        << "  -c CALL      My callsign (default: N0CALL)\n"
        << "  -r REMOTE    Remote station — auto-connects in TNC/connect mode\n"
        << "  -m MODE      tnc | connect | monitor | unproto\n"
        << "  -d DEST      Destination callsign for unproto (default: CQ)\n"
        << "  -b BAUD      Baud rate for serial (default: 9600; ignored for TCP)\n"
        << "  -p PATH      Digipeater path, comma-separated\n"
        << "  -M           Enable monitor output\n"
        << "  -D           Enable debug output (verbose routing info)\n"
        << "  -w WIN       Window size 1-7 (default: 3)\n"
        << "  -t T1        T1 retransmit timer ms (default: 3000)\n"
        << "  -k T3        T3 keep-alive timer ms (default: 60000)\n"
        << "  -n N2        Max retry count (default: 10)\n"
        << "  --mtu N      I-frame MTU bytes (default: 128)\n"
        << "  --txdelay N  KISS TX delay ms (default: 400)\n"
        << "  --tnc        Send KISS ON/RESTART/INTERFACE KISS/RESET before KISS (for legacy TNCs)\n"
        << "  --pid HEX    PID for UI frames (default: F0)\n"
        << "  -s FILE      BASIC script to run after connect (connect mode only)\n"
        << "  --ka SECS    App-level keep-alive: send CR every N seconds when idle (default: 60, 0=off)\n"
        << "  -h           Show this help\n\n"
        << "TNC commands (type H inside TNC mode for the full list):\n"
        << "  C <call>     Connect to callsign\n"
        << "  D            Disconnect\n"
        << "  MYC <call>   Set my callsign\n"
        << "  MON [ON|OFF] Toggle frame monitor\n"
        << "  STATUS       Show link status\n"
        << "  QUIT         Exit\n\n"
        << "Tilde escapes (data mode, when connected):\n"
        << "  ~.  ~d    disconnect, return to command mode\n"
        << "  ~s        show status / statistics\n"
        << "  ~x FILE   run BASIC script (stay connected after)\n"
        << "  ~~        send literal ~\n"
        << "  ~?        show this help\n\n"
        << "Double Ctrl+C within 5 seconds to disconnect and exit.\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument parser
// ─────────────────────────────────────────────────────────────────────────────
static bool parse_args(int argc, char* argv[], AppCfg& cfg) {
    // Default callsign
    cfg.ax25.mycall = Addr::make("N0CALL");

    // Long options
    static struct option longopts[] = {
        {"mtu",      required_argument, nullptr, 1001},
        {"txdelay",  required_argument, nullptr, 1002},
        {"pid",      required_argument, nullptr, 1003},
        {"script",   required_argument, nullptr, 's'},
        {"bas-path", required_argument, nullptr, 1005},
        {"ka",       required_argument, nullptr, 1004},
        {"test",     no_argument,       nullptr, 1006},
        {"tnc",      no_argument,       nullptr, 1007},
        {nullptr, 0, nullptr, 0}
    };

    bool mode_explicit = false;

    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "c:r:m:d:b:p:Mw:t:k:n:s:Dh", longopts, &idx)) != -1) {
        switch (opt) {
        case 'c': cfg.ax25.mycall = Addr::make(optarg); break;
        case 'r': cfg.remote      = optarg; break;
        case 'm':
            cfg.mode        = parse_mode(optarg);
            mode_explicit   = true;
            cfg.tnc_mode    = (cfg.mode == Mode::Tnc);
            break;
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
        case 'D': cfg.debug       = true; break;
        case 'w': cfg.ax25.window = std::max(1, std::min(7, std::atoi(optarg))); break;
        case 't': cfg.ax25.t1_ms  = std::atoi(optarg); break;
        case 'k': cfg.ax25.t3_ms  = std::atoi(optarg); break;
        case 'n': cfg.ax25.n2     = std::atoi(optarg); break;
        case 1001: cfg.ax25.mtu   = std::atoi(optarg); break;
        case 1002: cfg.txdelay    = std::atoi(optarg); break;
        case 1003: cfg.pid        = static_cast<uint8_t>(std::strtoul(optarg, nullptr, 16)); break;
        case 's':  cfg.script     = optarg; break;
        case 1005: cfg.scripts.add_search_path(optarg); break;
        case 1004: cfg.ka_ms     = std::atoi(optarg) * 1000; break;
        case 1006: cfg.mode = Mode::Test; mode_explicit = true; cfg.tnc_mode = false; break;
        case 1007: cfg.kiss_tnc = true; break;
        case 'h':  print_usage(argv[0]); return false;
        default:   print_usage(argv[0]); return false;
        }
    }

    // If -m was not given, we stay in TNC mode (default)
    if (!mode_explicit) {
        cfg.mode     = Mode::Tnc;
        cfg.tnc_mode = true;
        // Backward compat: if -r was given but no -m, auto-connect in TNC mode
    }

    if (optind < argc) cfg.device = argv[optind];

    if (cfg.device.empty()) {
        std::cerr << "Error: device or host:port required.\n";
        print_usage(argv[0]); return false;
    }

    // In TNC mode, -c and -r are optional (N0CALL default, no auto-connect needed).
    // In connect mode (explicitly set), validate as before.
    if (!cfg.tnc_mode) {
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
    // header line
    std::cout << DIM() << "[" << timestamp() << "]" << RESET()
              << " " << CYAN() << direction << RESET()
              << " " << f.format() << "\n";
    // ctrl detail + hexdump (dimmed — useful for debug, quiet on the eye)
    auto raw = f.encode();
    std::cout << DIM()
              << "           " << ctrl_detail(f.ctrl, raw.size()) << "\n"
              << hex_dump(raw.data(), raw.size(), "           ")
              << RESET() << std::flush;
}

// ─────────────────────────────────────────────────────────────────────────────
// Show status (connect/tnc mode)
// ─────────────────────────────────────────────────────────────────────────────
static void show_status(const AppCfg& cfg, const Connection* conn, const Stats& st) {
    std::cout << "\n" << BOLD() << "=== Status ===" << RESET() << "\n"
              << "  Local  : " << cfg.ax25.mycall.str() << "\n"
              << "  Remote : " << (cfg.remote.empty() ? "(none)" : cfg.remote) << "\n"
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

        // ── Resolve and load script ───────────────────────────────────────────
        std::string script_path = cfg.scripts.resolve(cfg.script);
        if (script_path.empty()) script_path = cfg.script;  // fallback to literal
        if (!interp.load_file(script_path)) {
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
                    std::string spath = cfg.scripts.resolve(fname);
                    if (spath.empty()) spath = fname;
                    if (!interp.load_file(spath)) {
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

// ─────────────────────────────────────────────────────────────────────────────
// Helper: print TNC command-mode prompt
// ─────────────────────────────────────────────────────────────────────────────
static void print_prompt(const AppCfg& cfg) {
    std::cout << "[" << cfg.ax25.mycall.str() << " cmd]> " << std::flush;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: case-insensitive prefix match for TNC commands
// Returns true if 'input' is a case-insensitive prefix of 'cmd' (min 1 char).
// ─────────────────────────────────────────────────────────────────────────────
static bool cmd_match(const std::string& input, const std::string& cmd) {
    if (input.empty()) return false;
    if (input.size() > cmd.size()) return false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(input[i])) !=
            std::tolower(static_cast<unsigned char>(cmd[i])))
            return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ── TNC MODE ─────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

// Forward declaration for parse_tnc_command (it needs run_basic_script which is
// defined after it, so we forward-declare the BASIC helper here).
static void run_basic_script(
    const std::string& fname,
    Connection* conn,
    int ser_fd,
    AppCfg& cfg,
    Stats& st,
    bool& done_flag);

// parse_tnc_command — parse one line from command mode.
// Returns true if the user requested QUIT.
static bool parse_tnc_command(
    const std::string& raw,
    AppCfg& cfg,
    Router& router,
    Connection*& conn,
    bool& data_mode,
    bool& mon_on,
    Stats& st,
    int ser_fd)
{
    // Trim leading whitespace
    std::string line = raw;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.erase(line.begin());
    if (line.empty()) return false;

    // Split into verb + rest
    std::string verb, rest;
    {
        std::size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) {
            verb = line;
        } else {
            verb = line.substr(0, sp);
            rest = line.substr(sp + 1);
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
                rest.erase(rest.begin());
        }
    }

    // ── QUIT / BYE / EXIT / Q ────────────────────────────────────────────────
    if (cmd_match(verb, "QUIT") || cmd_match(verb, "BYE") || cmd_match(verb, "EXIT")) {
        return true;
    }
    if (verb.size() == 1 &&
        (verb[0] == 'Q' || verb[0] == 'q')) {
        return true;
    }

    // ── CONNECT / C ──────────────────────────────────────────────────────────
    if (cmd_match(verb, "CONNECT") || (verb.size() == 1 && (verb[0]=='C'||verb[0]=='c'))) {
        if (rest.empty()) {
            std::cout << RED() << "Usage: C <callsign>" << RESET() << "\n" << std::flush;
            return false;
        }
        if (conn && conn->state() != Connection::State::DISCONNECTED) {
            std::cout << YELLOW() << "Already connected. Use D to disconnect first."
                      << RESET() << "\n" << std::flush;
            return false;
        }
        Addr remote = Addr::make(rest);
        if (remote == cfg.ax25.mycall) {
            std::cout << RED() << "Cannot connect to yourself." << RESET() << "\n" << std::flush;
            return false;
        }
        cfg.remote = rest;
        std::cout << YELLOW() << "Connecting to " << BOLD() << remote.str() << RESET()
                  << YELLOW() << " from " << cfg.ax25.mycall.str() << "..."
                  << RESET() << "\n" << std::flush;
        conn = router.connect(remote);
        ++st.frames_tx;
        // callbacks will be attached by run_tnc after this call returns
        // (via the attach_conn_callbacks lambda which is called after connect)
        // We signal data_mode here once on_connect fires; set it now optimistically
        // so the loop knows a conn pointer exists.
        (void)data_mode; // will be set by on_connect callback
        return false;
    }

    // ── DISCONNECT / D ───────────────────────────────────────────────────────
    if (cmd_match(verb, "DISCONNECT") || (verb.size() == 1 && (verb[0]=='D'||verb[0]=='d'))) {
        if (!conn || conn->state() == Connection::State::DISCONNECTED) {
            std::cout << YELLOW() << "Not connected." << RESET() << "\n" << std::flush;
        } else {
            std::cout << YELLOW() << "Disconnecting..." << RESET() << "\n" << std::flush;
            conn->disconnect();
        }
        return false;
    }

    // ── LISTEN / L ───────────────────────────────────────────────────────────
    if (cmd_match(verb, "LISTEN") || (verb.size() == 1 && (verb[0]=='L'||verb[0]=='l'))) {
        std::cout << GREEN() << "Listening for incoming connections on "
                  << cfg.ax25.mycall.str() << " (always active)." << RESET() << "\n" << std::flush;
        return false;
    }

    // ── MYCALL / MYC ─────────────────────────────────────────────────────────
    if (cmd_match(verb, "MYCALL")) {
        if (rest.empty()) {
            std::cout << "My callsign: " << cfg.ax25.mycall.str() << "\n" << std::flush;
        } else {
            Addr newcall = Addr::make(rest);
            cfg.ax25.mycall = newcall;
            router.config().mycall = newcall;   // update router so incoming frames match
            std::cout << GREEN() << "Callsign set to " << cfg.ax25.mycall.str()
                      << RESET() << "\n" << std::flush;
        }
        return false;
    }

    // ── MONITOR / MON ────────────────────────────────────────────────────────
    if (cmd_match(verb, "MONITOR")) {
        if (rest.empty()) {
            mon_on = !mon_on;
        } else {
            std::string upper = rest;
            for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (upper == "ON" || upper == "1") mon_on = true;
            else if (upper == "OFF" || upper == "0") mon_on = false;
            else {
                std::cout << RED() << "Usage: MON [ON|OFF]" << RESET() << "\n" << std::flush;
                return false;
            }
        }
        router.on_monitor = mon_on
            ? std::function<void(const Frame&)>([](const Frame& f){ print_frame(f, ">>"); })
            : nullptr;
        std::cout << DIM() << "[Monitor " << (mon_on ? "ON" : "OFF") << "]"
                  << RESET() << "\n" << std::flush;
        return false;
    }

    // ── UNPROTO ──────────────────────────────────────────────────────────────
    if (cmd_match(verb, "UNPROTO")) {
        // Toggle UI frame display (reuse mon_on for UI display in TNC mode for simplicity)
        bool& ui_on = mon_on; // display is bundled with monitor in TNC mode
        if (rest.empty()) {
            ui_on = !ui_on;
        } else {
            std::string upper = rest;
            for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (upper == "ON" || upper == "1") ui_on = true;
            else if (upper == "OFF" || upper == "0") ui_on = false;
            else {
                std::cout << RED() << "Usage: UNPROTO [ON|OFF]" << RESET() << "\n" << std::flush;
                return false;
            }
        }
        std::cout << DIM() << "[UI frame display " << (ui_on ? "ON" : "OFF") << "]"
                  << RESET() << "\n" << std::flush;
        return false;
    }

    // ── STATUS / STAT / S ────────────────────────────────────────────────────
    if (cmd_match(verb, "STATUS") || cmd_match(verb, "STAT") ||
        (verb.size() == 1 && (verb[0]=='S'||verb[0]=='s'))) {
        show_status(cfg, conn, st);
        return false;
    }

    // ── WIN ──────────────────────────────────────────────────────────────────
    if (cmd_match(verb, "WIN")) {
        if (rest.empty()) {
            std::cout << "Window size: " << cfg.ax25.window << "\n" << std::flush;
        } else {
            int n = std::atoi(rest.c_str());
            if (n < 1 || n > 7) {
                std::cout << RED() << "Window size must be 1-7." << RESET() << "\n" << std::flush;
            } else {
                cfg.ax25.window = n;
                std::cout << DIM() << "[Window set to " << n << "]" << RESET() << "\n" << std::flush;
            }
        }
        return false;
    }

    // ── T1 ───────────────────────────────────────────────────────────────────
    if (cmd_match(verb, "T1")) {
        if (rest.empty()) {
            std::cout << "T1 (retransmit): " << cfg.ax25.t1_ms << " ms\n" << std::flush;
        } else {
            cfg.ax25.t1_ms = std::atoi(rest.c_str());
            std::cout << DIM() << "[T1 set to " << cfg.ax25.t1_ms << " ms]"
                      << RESET() << "\n" << std::flush;
        }
        return false;
    }

    // ── T3 ───────────────────────────────────────────────────────────────────
    if (cmd_match(verb, "T3")) {
        if (rest.empty()) {
            std::cout << "T3 (keep-alive): " << cfg.ax25.t3_ms << " ms\n" << std::flush;
        } else {
            cfg.ax25.t3_ms = std::atoi(rest.c_str());
            std::cout << DIM() << "[T3 set to " << cfg.ax25.t3_ms << " ms]"
                      << RESET() << "\n" << std::flush;
        }
        return false;
    }

    // ── MTU ──────────────────────────────────────────────────────────────────
    if (cmd_match(verb, "MTU")) {
        if (rest.empty()) {
            std::cout << "MTU: " << cfg.ax25.mtu << " bytes\n" << std::flush;
        } else {
            cfg.ax25.mtu = std::atoi(rest.c_str());
            std::cout << DIM() << "[MTU set to " << cfg.ax25.mtu << " bytes]"
                      << RESET() << "\n" << std::flush;
        }
        return false;
    }

    // ── TXDELAY ──────────────────────────────────────────────────────────────
    if (cmd_match(verb, "TXDELAY")) {
        if (rest.empty()) {
            std::cout << "TX delay: " << cfg.txdelay << " ms\n" << std::flush;
        } else {
            cfg.txdelay = std::atoi(rest.c_str());
            std::cout << DIM() << "[TX delay set to " << cfg.txdelay << " ms]"
                      << RESET() << "\n" << std::flush;
        }
        return false;
    }

    // ── SCRIPT ───────────────────────────────────────────────────────────────
    if (cmd_match(verb, "SCRIPT")) {
        if (rest.empty()) {
            std::cout << RED() << "Usage: SCRIPT <file>" << RESET() << "\n" << std::flush;
            return false;
        }
        if (!conn || !conn->connected()) {
            std::cout << RED() << "Not connected — cannot run script." << RESET() << "\n" << std::flush;
            return false;
        }
        bool done_flag = false;
        run_basic_script(rest, conn, ser_fd, cfg, st, done_flag);
        return false;
    }

    // ── HELP / H / ? ─────────────────────────────────────────────────────────
    if (cmd_match(verb, "HELP") ||
        (verb.size() == 1 && (verb[0]=='H'||verb[0]=='h'||verb[0]=='?'))) {
        std::cout << "\n"
                  << BOLD() << "TNC Commands:" << RESET() << "\n"
                  << "  C <call>         Connect to callsign\n"
                  << "  D                Disconnect\n"
                  << "  L                Show listen status\n"
                  << "  MYC <call>       Set my callsign\n"
                  << "  MON [ON|OFF]     Toggle frame monitor\n"
                  << "  UNPROTO [ON|OFF] Toggle UI frame display\n"
                  << "  STATUS           Show link status and statistics\n"
                  << "  WIN <n>          Set window size 1-7\n"
                  << "  T1 <ms>          Set T1 retransmit timer\n"
                  << "  T3 <ms>          Set T3 keep-alive timer\n"
                  << "  MTU <bytes>      Set I-frame MTU\n"
                  << "  TXDELAY <ms>     Set KISS TX delay\n"
                  << "  SCRIPT <file>    Run BASIC script (connected only)\n"
                  << "  HELP             This help\n"
                  << "  QUIT             Exit\n"
                  << "\n"
                  << BOLD() << "Data mode tilde escapes (when connected):" << RESET() << "\n"
                  << "  ~.  ~d           Disconnect, return to command mode\n"
                  << "  ~s               Show status\n"
                  << "  ~x <file>        Run BASIC script, stay connected\n"
                  << "  ~~               Send literal ~\n"
                  << "  ~?               Show this tilde help\n"
                  << "\n"
                  << "Double Ctrl+C (within 5s) to disconnect and exit.\n"
                  << "\n" << std::flush;
        return false;
    }

    // Unknown command
    std::cout << RED() << "Unknown command: " << verb
              << RESET() << "  (type H for help)\n" << std::flush;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// run_basic_script — run a BASIC script while connected; used in both TNC
// command mode (SCRIPT command) and data mode (~x escape).
// ─────────────────────────────────────────────────────────────────────────────
static void run_basic_script(
    const std::string& fname,
    Connection* conn,
    int ser_fd,
    AppCfg& cfg,
    Stats& st,
    bool& done_flag)
{
    using Clock = std::chrono::steady_clock;
    std::deque<std::string> rx_lines;
    std::string recv_buf_local;

    std::cout << DIM() << "[Running script: " << fname << "]" << RESET() << "\n" << std::flush;

    Basic interp;
    interp.on_send = [&](const std::string& s) {
        if (!conn) return;
        if (conn->send(s)) {
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
        while (!g_quit && !done_flag) {
            struct timeval tv{ 0, 20000 };
            fd_set fds; FD_ZERO(&fds); FD_SET(ser_fd, &fds);
            select(ser_fd + 1, &fds, nullptr, nullptr, &tv);
            // We must poll the router here; but we don't have it in scope.
            // The connection's on_data may still fire via the router that
            // run_tnc controls. Incoming data that was buffered before the
            // script started is already in the outer recv_buf. For the
            // script's RECV calls we rely on a temporary on_data override.
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

    // Override on_data temporarily to collect into our local queue
    auto old_on_data = conn->on_data;
    conn->on_data = [&](const uint8_t* d, std::size_t n) {
        st.bytes_rx += n; ++st.frames_rx;
        recv_buf_local.append(reinterpret_cast<const char*>(d), n);
        std::size_t pos;
        while ((pos = recv_buf_local.find_first_of("\r\n")) != std::string::npos) {
            std::string li = recv_buf_local.substr(0, pos);
            recv_buf_local.erase(0, pos + 1);
            if (!li.empty()) rx_lines.push_back(li);
        }
    };

    // Set all standard BBS-compatible variables so scripts work identically
    // whether run from the BBS server or from ax25tnc
    interp.set_str("REMOTE$",    cfg.remote);
    interp.set_str("LOCAL$",     cfg.ax25.mycall.str());
    interp.set_str("CALLSIGN$",  cfg.remote);
    interp.set_str("BBS_NAME$",  cfg.ax25.mycall.str());   // use mycall as BBS name
    interp.set_str("DB_PATH$",   "");                      // no database in client mode
    interp.set_num("ARGC",       0);

    if (!interp.load_file(fname)) {
        std::cout << RED() << "[Error: cannot open script: " << fname << "]"
                  << RESET() << "\n" << std::flush;
    } else {
        interp.run();
        std::cout << DIM() << "[Script finished]" << RESET() << "\n" << std::flush;
    }

    // Restore the original on_data callback
    conn->on_data = old_on_data;
}

// ─────────────────────────────────────────────────────────────────────────────
// run_tnc — new default TNC interactive mode
// ─────────────────────────────────────────────────────────────────────────────
static int run_tnc(Kiss& kiss, Router& router, AppCfg& cfg) {
    int         ser_fd    = kiss.fd();
    Stats       st;
    Connection* conn      = nullptr;
    bool        data_mode = false;
    bool        mon_on    = cfg.monitor;
    std::string recv_buf;
    std::deque<std::string> rx_lines;
    std::vector<Connection*> dead_conns;   // deferred delete list

    using Clock = std::chrono::steady_clock;
    auto last_tx = Clock::now();

    // ── Monitor hook ─────────────────────────────────────────────────────────
    if (mon_on)
        router.on_monitor = [](const Frame& f){ print_frame(f, ">>"); };

    // ── Debug hook: log routing decisions for addressed frames ─────────────
    if (cfg.debug) {
        auto prev_mon = router.on_monitor;
        router.on_monitor = [&, prev_mon](const Frame& f) {
            if (prev_mon) prev_mon(f);
            if (f.type() != Frame::Type::UI) {
                bool match = (f.dest == router.config().mycall);
                std::cerr << DIM() << "[DEBUG] " << f.format()
                          << " dest_match=" << (match ? "YES" : "NO")
                          << " mycall=" << router.config().mycall.str()
                          << RESET() << "\n" << std::flush;
            }
        };
    }

    // ── Connection callbacks (defined as a lambda so we can call recursively
    //    after disconnect to re-arm listen) ───────────────────────────────────
    // We need a std::function to allow self-reference; forward-declare it.
    std::function<void(Connection*)> attach_conn_callbacks;

    attach_conn_callbacks = [&](Connection* c) {
        c->on_connect = [&, c] {
            data_mode = true;
            st.connect_t = static_cast<uint64_t>(time(nullptr));
            std::cout << "\n" << GREEN() << BOLD() << "*** Connected to "
                      << c->remote().str() << " ***" << RESET() << "\n\n" << std::flush;
        };
        c->on_disconnect = [&] {
            data_mode = false;
            // Schedule old connection for deferred deletion (can't delete inside
            // its own callback).  The main loop cleans up dead_conns each tick.
            if (conn) dead_conns.push_back(conn);
            conn = nullptr;
            std::cout << "\n" << RED() << BOLD() << "*** Disconnected ***"
                      << RESET() << "\n\n" << std::flush;
            // Re-arm listen after disconnect
            router.listen([&](Connection* inc) {
                conn = inc;
                attach_conn_callbacks(inc);
            });
            print_prompt(cfg);
        };
        c->on_data = [&](const uint8_t* d, std::size_t n) {
            st.bytes_rx += n; ++st.frames_rx;
            recv_buf.append(reinterpret_cast<const char*>(d), n);
            std::size_t pos;
            while ((pos = recv_buf.find_first_of("\r\n")) != std::string::npos) {
                std::string li = recv_buf.substr(0, pos);
                recv_buf.erase(0, pos + 1);
                if (li.empty()) continue;
                rx_lines.push_back(li);
                std::cout << li << "\n" << std::flush;
            }
        };
    };

    // ── Always listen for incoming connections ────────────────────────────────
    router.listen([&](Connection* inc) {
        if (conn && conn->state() != Connection::State::DISCONNECTED) {
            // Already connected — library will auto-reject with DM
            return;
        }
        conn = inc;
        attach_conn_callbacks(inc);
        cfg.remote = inc->remote().str();
        std::cout << "\n" << YELLOW() << BOLD()
                  << "*** Incoming connection from " << inc->remote().str()
                  << " — type D to disconnect ***"
                  << RESET() << "\n\n" << std::flush;
        data_mode = true;
    });

    // ── Auto-connect if -r was given ─────────────────────────────────────────
    if (!cfg.remote.empty()) {
        Addr remote = Addr::make(cfg.remote);
        if (remote == cfg.ax25.mycall) {
            std::cout << RED() << "Error: remote callsign cannot equal local callsign."
                      << RESET() << "\n" << std::flush;
        } else {
            std::cout << YELLOW() << "Connecting to "
                      << BOLD() << remote.str() << RESET()
                      << YELLOW() << " from " << cfg.ax25.mycall.str()
                      << "..." << RESET() << "\n" << std::flush;
            conn = router.connect(remote);
            attach_conn_callbacks(conn);
            ++st.frames_tx;
        }
    }

    // ── Banner ────────────────────────────────────────────────────────────────
    std::cout << "Type H for help, C <callsign> to connect, QUIT to exit.\n"
              << "Listening on " << cfg.ax25.mycall.str()
              << " for incoming connections.\n" << std::flush;
    if (!data_mode) print_prompt(cfg);

    // ═════════════════════════════════════════════════════════════════════════
    // Main loop
    // ═════════════════════════════════════════════════════════════════════════
    static sig_atomic_t last_reported_ctrl_c = 0; // track if we already warned

    while (!g_quit) {
        // ── Poll serial (20 ms) ──────────────────────────────────────────────
        {
            struct timeval tv{ 0, 20000 };
            fd_set fds; FD_ZERO(&fds); FD_SET(ser_fd, &fds);
            select(ser_fd + 1, &fds, nullptr, nullptr, &tv);
        }
        router.poll();

        // ── Deferred cleanup of disconnected connections ─────────────────────
        for (auto* dc : dead_conns) delete dc;
        dead_conns.clear();

        // ── Double Ctrl+C warning ────────────────────────────────────────────
        if (g_ctrl_c_count == 1 && last_reported_ctrl_c != g_ctrl_c_count) {
            last_reported_ctrl_c = g_ctrl_c_count;
            if (conn && conn->state() != Connection::State::DISCONNECTED) {
                std::cout << "\a\n" << YELLOW() << BOLD()
                          << "Press Ctrl+C again within 5s to disconnect and exit."
                          << RESET() << "\n" << std::flush;
            } else {
                std::cout << "\n" << YELLOW()
                          << "Use QUIT or press Ctrl+C again to exit."
                          << RESET() << "\n" << std::flush;
            }
            if (!data_mode) print_prompt(cfg);
        }
        // Reset warning tracker if the counter was reset by the handler
        if (g_ctrl_c_count == 0) last_reported_ctrl_c = 0;

        // ── App-level keep-alive ─────────────────────────────────────────────
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

        // ── Read stdin (non-blocking) ────────────────────────────────────────
        std::string line;
        if (!stdin_readline(line, 0)) continue;

        // ════════════════════════════════════════════════════════════════════
        // DATA MODE
        // ════════════════════════════════════════════════════════════════════
        if (data_mode) {
            // Tilde escapes
            if (!line.empty() && line[0] == '~') {
                if (line.size() == 1) {
                    // bare ~ — just ignore or treat as literal send
                    // fall through to send
                } else {
                    char esc = line[1];

                    if (esc == '~') {
                        // ~~ → send literal ~
                        if (conn && conn->connected()) {
                            std::string payload = "~\r";
                            if (conn->send(payload)) {
                                last_tx = Clock::now();
                                st.bytes_tx += payload.size();
                                ++st.frames_tx;
                            }
                        }
                        continue;
                    }
                    if (esc == '.' || esc == 'd' || esc == 'D') {
                        std::cout << YELLOW() << "Disconnecting..." << RESET() << "\n" << std::flush;
                        if (conn && conn->connected()) conn->disconnect();
                        // data_mode will be cleared by on_disconnect callback
                        continue;
                    }
                    if (esc == 's' || esc == 'S') {
                        show_status(cfg, conn, st);
                        continue;
                    }
                    if (esc == 'x' || esc == 'X') {
                        std::string fname = (line.size() > 2) ? line.substr(2) : "";
                        while (!fname.empty() && (fname.front() == ' ' || fname.front() == '\t'))
                            fname.erase(fname.begin());
                        if (fname.empty()) {
                            std::cout << RED() << "[Usage: ~x <script.bas>]"
                                      << RESET() << "\n" << std::flush;
                        } else if (!conn || !conn->connected()) {
                            std::cout << RED() << "[Not connected]"
                                      << RESET() << "\n" << std::flush;
                        } else {
                            bool done_flag = false;
                            run_basic_script(fname, conn, ser_fd, cfg, st, done_flag);
                            last_tx = Clock::now();
                        }
                        continue;
                    }
                    if (esc == '?') {
                        std::cout << "\nTilde escapes (data mode):\n"
                                  << "  ~.  ~d    disconnect, return to command mode\n"
                                  << "  ~s        show status / statistics\n"
                                  << "  ~x FILE   run BASIC script (stay connected after)\n"
                                  << "  ~~        send literal ~\n"
                                  << "  ~?        this help\n\n" << std::flush;
                        continue;
                    }
                    // Unknown tilde escape
                    std::cout << DIM() << "[Unknown escape: ~" << esc
                              << " — type ~? for help]" << RESET() << "\n" << std::flush;
                    continue;
                }
            }

            // Send line as I-frame
            if (!conn || !conn->connected()) {
                std::cout << RED() << "[Not connected]" << RESET() << "\n" << std::flush;
                data_mode = false;
                print_prompt(cfg);
                continue;
            }
            std::string payload = line + "\r";
            if (conn->send(payload)) {
                last_tx = Clock::now();
                st.bytes_tx += payload.size();
                ++st.frames_tx;
            } else {
                std::cout << RED() << "[Send failed — connection may have dropped]"
                          << RESET() << "\n" << std::flush;
            }

        // ════════════════════════════════════════════════════════════════════
        // COMMAND MODE
        // ════════════════════════════════════════════════════════════════════
        } else {
            // If the user connected via C command, conn may now exist but
            // callbacks haven't been attached yet (parse_tnc_command does the
            // router.connect call; we attach here afterwards).
            // We handle that below after parse_tnc_command returns.

            bool quit = parse_tnc_command(line, cfg, router, conn, data_mode,
                                          mon_on, st, ser_fd);
            if (quit) {
                g_quit = 1;
                break;
            }

            // If parse_tnc_command called router.connect() (C command), conn
            // is now non-null but has no callbacks yet.  Attach them.
            // The on_connect callback will set data_mode = true when the
            // connection completes, so we do NOT force data_mode here.
            if (conn) {
                // Check if callbacks need attaching (on_connect not set → fresh conn)
                if (!conn->on_connect) {
                    attach_conn_callbacks(conn);
                }
            }

            if (!data_mode) print_prompt(cfg);
        }
    }

    // ── Graceful shutdown ────────────────────────────────────────────────────
    if (conn && conn->state() != Connection::State::DISCONNECTED) {
        std::cout << "\n" << YELLOW() << "Disconnecting..." << RESET() << "\n" << std::flush;
        conn->disconnect();
        auto t0 = Clock::now();
        while (conn && conn->state() != Connection::State::DISCONNECTED) {
            struct timeval tv{ 0, 20000 };
            fd_set fds; FD_ZERO(&fds); FD_SET(ser_fd, &fds);
            select(ser_fd + 1, &fds, nullptr, nullptr, &tv);
            router.poll();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          Clock::now() - t0).count();
            if (ms >= 1000) break;
        }
    }

    std::cout << "\nSession summary:\n"
              << "  TX: " << st.frames_tx << " frames / " << st.bytes_tx << " bytes\n"
              << "  RX: " << st.frames_rx << " frames / " << st.bytes_rx << " bytes\n";

    if (conn) delete conn;
    for (auto* dc : dead_conns) delete dc;
    dead_conns.clear();
    return 0;
}

// =============================================================================
// TEST MODE -- send periodic UI test frames, show RX
// =============================================================================
static int run_test(Kiss& kiss, Router& router, const AppCfg& cfg) {
    int ser_fd = kiss.fd();
    Addr src  = cfg.ax25.mycall;
    Addr dest = Addr::make(cfg.dest.empty() ? "CQ" : cfg.dest);

    // Override with NOC4LL if callsign is the default N0CALL
    if (src.str() == "N0CALL") src = Addr::make("NOC4LL");

    router.on_monitor = [](const Frame& f) { print_frame(f, "RX"); };

    std::cout << GREEN() << "Test mode." << RESET()
              << "  Sending " << src.str() << ">CQ every 2s.  Ctrl-C to exit.\n\n"
              << std::flush;

    int tx_count = 0, rx_count = 0;
    using Clock = std::chrono::steady_clock;
    auto next_tx = Clock::now();

    while (!g_quit) {
        auto now = Clock::now();
        if (now >= next_tx) {
            ++tx_count;
            char info[32];
            std::snprintf(info, sizeof(info), "Test %03d", tx_count);
            router.send_ui(dest, cfg.pid, std::string(info), cfg.ax25.digis);
            std::cout << "[" << timestamp() << "]  TX >> " << src.str() << ">CQ [UI] \""
                      << info << "\"\n" << std::flush;
            ++tx_count; --tx_count;  // suppress unused warn
            next_tx = now + std::chrono::seconds(2);
        }

        struct timeval tv{ 0, 20000 };
        fd_set fds; FD_ZERO(&fds); FD_SET(ser_fd, &fds);
        select(ser_fd + 1, &fds, nullptr, nullptr, &tv);
        router.poll();
    }

    std::cout << "\nTest ended.  TX: " << tx_count << "  RX: " << rx_count << "\n";
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
            if (cfg.kiss_tnc) tnc_kiss_init(kiss.fd());
        }
    }
    kiss.set_txdelay(cfg.txdelay);  // cfg.txdelay is already in ms, set_txdelay expects ms
    kiss.set_persistence(cfg.ax25.persist);

    // ── Create Router ────────────────────────────────────────────────────────
    Router router(kiss, cfg.ax25);

    // ── Banner ───────────────────────────────────────────────────────────────
    {
        std::string tcp_host, tcp_port;
        bool is_tcp = is_tcp_address(cfg.device, tcp_host, tcp_port);
        std::cout << BOLD() << "ax25tnc" << RESET()
                  << "  " << cfg.ax25.mycall.str()
                  << "  " << cfg.device
                  << (is_tcp ? "  TCP" : ("  @" + std::to_string(cfg.baud) + " baud"))
                  << "\n";
    }

    switch (cfg.mode) {
    case Mode::Monitor: return run_monitor(kiss, router);
    case Mode::Unproto: return run_unproto(kiss, router, cfg);
    case Mode::Connect: return run_connect(kiss, router, cfg);
    case Mode::Test:    return run_test(kiss, router, cfg);
    case Mode::Tnc:     return run_tnc(kiss, router, cfg);
    }
    return 0;
}
