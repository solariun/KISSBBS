// =============================================================================
// ax25sim.cpp — AX.25 / KISS TNC Simulator  (C++11, POSIX)
//
// Creates a PTY-based virtual serial port so other KISSBBS tools (bbs, ax25tnc)
// can connect to it for testing without real radio hardware.
//
// The simulator provides an interactive TNC-style terminal with:
//   - Colorful prompt showing connection state (connected to/from)
//   - Hex dump display of received frames
//   - Full AX.25 parameter tuning (TXDELAY, MTU, window, T1, T3, N2, persist)
//   - UI and APRS frame sending
//   - BASIC script execution with regex-based file listing
//
// Build:
//   g++ -std=c++11 -O2 -Wall -o ax25sim ax25sim.cpp ax25lib.o basic.o -lutil -lsqlite3
//
// Usage:
//   ax25sim [OPTIONS]
//
// Options:
//   -c CALL         Simulator callsign (default: N0SIM)
//   -l PATH         PTY symlink path (default: /tmp/kiss_sim)
//   -s DIR          Script directory for .bas files (default: .)
//   -w WIN          Window size 1-7 (default: 3)
//   -t T1_MS        T1 retransmit timer ms (default: 3000)
//   -k T3_MS        T3 keep-alive timer ms (default: 60000)
//   -n N2           Max retry count (default: 10)
//   --mtu BYTES     I-frame MTU bytes (default: 128)
//   --txdelay MS    KISS TX delay ms (default: 400)
//   -p PATH         Digipeater path, comma-separated
//   -h              Show this help
//
// Simulator commands (prefixed with //):
//   //c <call>      Connect to remote callsign
//   //d             Disconnect
//   //ui <dest> <text>  Send UI frame
//   //aprs <info>   Send APRS beacon
//   //myc <call>    Set/show callsign
//   //mon [on|off]  Toggle frame monitor
//   //hex [on|off]  Toggle hex dump display
//   //s             Show status + PTY path + stats
//   //txdelay <ms>  Set KISS TX delay
//   //mtu <bytes>   Set MTU
//   //win <n>       Set window 1-7
//   //t1 <ms>       Set T1 retransmit timer
//   //t3 <ms>       Set T3 keep-alive timer
//   //n2 <n>        Set max retries
//   //persist <val> Set KISS persistence 0-255
//   //path <d1,d2>  Set digipeater path
//   //b [file|pat]  Run BASIC script or list/select
//   //h             Show help
//   //q             Quit
//
// Text without // prefix is sent as I-frame data if connected.
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

#include <getopt.h>
#include <signal.h>
#include <sys/select.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

using namespace ax25;

// ─────────────────────────────────────────────────────────────────────────────
// Global state — signal handler sets these
// ─────────────────────────────────────────────────────────────────────────────
static volatile sig_atomic_t g_ctrl_c_count = 0;
static volatile sig_atomic_t g_quit         = 0;
static time_t                g_ctrl_c_time  = 0;

static void handle_signal(int sig) {
    if (sig == SIGTERM) { g_quit = 1; return; }
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
    uint64_t frames_rx  = 0;
    uint64_t frames_tx  = 0;
    uint64_t bytes_rx   = 0;
    uint64_t bytes_tx   = 0;
    uint64_t ui_rx      = 0;
    uint64_t connect_t  = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────
struct SimCfg {
    std::string link_path  = "/tmp/kiss_sim";
    ScriptFinder scripts;
    int         txdelay    = 400;
    Config      ax25;
    bool        monitor    = true;
    bool        hex_on     = true;

    SimCfg() {
        ax25.mycall = Addr::make("N0SIM");
        ax25.mtu    = 128;
        ax25.window = 3;
        ax25.t1_ms  = 3000;
        ax25.t3_ms  = 60000;
        ax25.n2     = 10;
        ax25.baud   = 9600;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// PTY setup (adapted from bt_kiss_bridge.cpp)
// ─────────────────────────────────────────────────────────────────────────────
static bool open_pty(int& master, int& slave, std::string& path) {
    char name[256]{};
    if (openpty(&master, &slave, name, nullptr, nullptr) < 0) {
        std::cerr << "openpty: " << strerror(errno) << "\n";
        return false;
    }
    struct termios t{};
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);

    int fl = fcntl(master, F_GETFL);
    fcntl(master, F_SETFL, fl | O_NONBLOCK);
    path = name;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::string timestamp() {
    time_t t = time(nullptr);
    char buf[24];
    struct tm* tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    return buf;
}

static void print_frame(const Frame& f, const char* direction, bool show_hex) {
    std::cout << DIM() << "[" << timestamp() << "]" << RESET()
              << " " << CYAN() << direction << RESET()
              << " " << f.format() << "\n";
    if (show_hex) {
        auto raw = f.encode();
        std::cout << DIM()
                  << "           " << ctrl_detail(f.ctrl, raw.size()) << "\n"
                  << hex_dump(raw.data(), raw.size(), "           ")
                  << RESET() << std::flush;
    }
}

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
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    return true;
}

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
// Prompt — shows connection state with colors and arrows
// ─────────────────────────────────────────────────────────────────────────────
static void print_prompt(const SimCfg& cfg, const Connection* conn,
                          bool outgoing, std::size_t conn_count) {
    if (!conn || conn->state() != Connection::State::CONNECTED) {
        std::cout << "[" << cfg.ax25.mycall.str() << "]> " << std::flush;
        return;
    }
    const char* color = outgoing ? GREEN() : CYAN();
    std::string arrow = outgoing
        ? (cfg.ax25.mycall.str() + "\xe2\x86\x92" + conn->remote().str())
        : (conn->remote().str() + "\xe2\x86\x92" + cfg.ax25.mycall.str());
    std::cout << color << "[" << arrow;
    if (conn_count > 1)
        std::cout << " +" << (conn_count - 1);
    std::cout << "]> " << RESET() << std::flush;
}

// ─────────────────────────────────────────────────────────────────────────────
// Status display
// ─────────────────────────────────────────────────────────────────────────────
static void show_status(const SimCfg& cfg, const Connection* conn,
                        const Stats& st, const std::string& pty_path) {
    std::cout << "\n" << BOLD() << "=== Simulator Status ===" << RESET() << "\n"
              << "  Callsign : " << cfg.ax25.mycall.str() << "\n"
              << "  PTY      : " << pty_path << "\n"
              << "  State    : ";
    if (conn) {
        switch (conn->state()) {
        case Connection::State::CONNECTED:     std::cout << GREEN() << "CONNECTED to " << conn->remote().str() << RESET(); break;
        case Connection::State::CONNECTING:    std::cout << YELLOW() << "CONNECTING" << RESET(); break;
        case Connection::State::DISCONNECTING: std::cout << YELLOW() << "DISCONNECTING" << RESET(); break;
        default:                               std::cout << RED() << "DISCONNECTED" << RESET(); break;
        }
    } else {
        std::cout << "IDLE (listening)";
    }
    std::cout << "\n"
              << "  Frames RX : " << st.frames_rx << "  (" << st.bytes_rx << " data bytes)\n"
              << "  Frames TX : " << st.frames_tx << "  (" << st.bytes_tx << " data bytes)\n"
              << "  UI RX     : " << st.ui_rx << "\n"
              << "  Window    : " << cfg.ax25.window
              << "  MTU=" << cfg.ax25.mtu
              << "  T1=" << cfg.ax25.t1_ms << "ms"
              << "  T3=" << cfg.ax25.t3_ms << "ms"
              << "  N2=" << cfg.ax25.n2 << "\n"
              << "  TXDelay=" << cfg.txdelay << "ms"
              << "  Persist=" << cfg.ax25.persist
              << "  Monitor=" << (cfg.monitor ? "ON" : "OFF")
              << "  HexDump=" << (cfg.hex_on ? "ON" : "OFF")
              << "\n\n" << std::flush;
}

// (Script discovery is handled by ScriptFinder in lib/script_finder.hpp)

// ─────────────────────────────────────────────────────────────────────────────
// Run a BASIC script (adapted from ax25tnc.cpp)
// ─────────────────────────────────────────────────────────────────────────────
static void run_basic_script(
    const std::string& fname,
    Connection* conn,
    int pty_fd,
    SimCfg& cfg,
    Router& router,
    Stats& st)
{
    using Clock = std::chrono::steady_clock;
    std::deque<std::string> rx_lines;
    std::string recv_buf_local;

    std::cout << DIM() << "[Running script: " << fname << "]" << RESET() << "\n" << std::flush;

    Basic interp;

    // SEND → connection if connected, else stdout
    interp.on_send = [&](const std::string& s) {
        if (conn && conn->connected()) {
            if (conn->send(s)) {
                st.bytes_tx += s.size();
                ++st.frames_tx;
            }
        } else {
            std::cout << s << std::flush;
        }
    };

    // RECV → from connection data queue or stdin
    interp.on_recv = [&](int tmo) -> std::string {
        if (!rx_lines.empty()) {
            std::string l = rx_lines.front(); rx_lines.pop_front();
            return l;
        }
        if (!conn || !conn->connected()) {
            // No connection — read from stdin
            std::string line;
            if (stdin_readline(line, tmo)) return line;
            return "";
        }
        auto deadline = Clock::now() + std::chrono::milliseconds(tmo);
        while (!g_quit) {
            struct timeval tv{ 0, 20000 };
            fd_set fds; FD_ZERO(&fds); FD_SET(pty_fd, &fds);
            select(pty_fd + 1, &fds, nullptr, nullptr, &tv);
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

    interp.on_send_aprs = [&](const std::string& info) {
        router.send_aprs(info);
        ++st.frames_tx;
    };

    interp.on_send_ui = [&](const std::string& dest, const std::string& text) {
        router.send_ui(Addr::make(dest), 0xF0, text);
        ++st.frames_tx;
    };

    // Override on_data temporarily if connected
    std::function<void(const uint8_t*, std::size_t)> old_on_data;
    if (conn) {
        old_on_data = conn->on_data;
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
    }

    // Set standard BBS-compatible variables
    interp.set_str("REMOTE$",   conn ? conn->remote().str() : "");
    interp.set_str("LOCAL$",    cfg.ax25.mycall.str());
    interp.set_str("CALLSIGN$", conn ? conn->remote().str() : "");
    interp.set_str("BBS_NAME$", cfg.ax25.mycall.str());
    interp.set_str("DB_PATH$",  "");
    interp.set_num("ARGC",      0);

    if (!interp.load_file(fname)) {
        std::cout << RED() << "[Error: cannot open script: " << fname << "]"
                  << RESET() << "\n" << std::flush;
    } else {
        interp.run();
        std::cout << DIM() << "[Script finished]" << RESET() << "\n" << std::flush;
    }

    if (conn) conn->on_data = old_on_data;
}

// ─────────────────────────────────────────────────────────────────────────────
// Handle //basic command: run script or list/select
// ─────────────────────────────────────────────────────────────────────────────
static void handle_basic_cmd(const std::string& arg,
                              Connection* conn, int pty_fd,
                              SimCfg& cfg, Router& router, Stats& st) {
    auto out_fn = [](const std::string& s) {
        std::cout << s << "\n" << std::flush;
    };
    auto readline_fn = [](const std::string& prompt, int tmo) -> std::string {
        std::cout << prompt << std::flush;
        std::string line;
        stdin_readline(line, tmo);
        return line;
    };

    std::string path = cfg.scripts.resolve_interactive(arg, out_fn, readline_fn);
    if (!path.empty())
        run_basic_script(path, conn, pty_fd, cfg, router, st);
}

// ─────────────────────────────────────────────────────────────────────────────
// Toggle helper for on/off commands
// ─────────────────────────────────────────────────────────────────────────────
static bool parse_toggle(const std::string& rest, bool& flag, const char* name) {
    if (rest.empty()) {
        flag = !flag;
    } else {
        std::string upper = rest;
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (upper == "ON" || upper == "1") flag = true;
        else if (upper == "OFF" || upper == "0") flag = false;
        else {
            std::cout << RED() << "Usage: //" << name << " [ON|OFF]" << RESET() << "\n" << std::flush;
            return false;
        }
    }
    std::cout << DIM() << "[" << name << " " << (flag ? "ON" : "OFF") << "]"
              << RESET() << "\n" << std::flush;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parse and set numeric parameter
// ─────────────────────────────────────────────────────────────────────────────
static void param_cmd(const std::string& rest, int& field,
                       const char* label, const char* unit,
                       int min_val = 0, int max_val = 0) {
    if (rest.empty()) {
        std::cout << label << ": " << field << " " << unit << "\n" << std::flush;
    } else {
        int n = std::atoi(rest.c_str());
        if (max_val > 0 && (n < min_val || n > max_val)) {
            std::cout << RED() << label << " must be " << min_val << "-" << max_val << "."
                      << RESET() << "\n" << std::flush;
        } else {
            field = n;
            std::cout << DIM() << "[" << label << " set to " << n << " " << unit << "]"
                      << RESET() << "\n" << std::flush;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Command parser — returns true if user requested QUIT
// ─────────────────────────────────────────────────────────────────────────────
static bool parse_sim_command(
    const std::string& raw,
    SimCfg& cfg,
    Router& router,
    Connection*& conn,
    bool& outgoing,
    bool& mon_on,
    Stats& st,
    int pty_fd,
    const std::string& pty_path)
{
    std::string line = raw;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.erase(line.begin());
    if (line.empty()) return false;

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

    // ── QUIT / Q ──────────────────────────────────────────────────────────────
    if (cmd_match(verb, "QUIT") || (verb.size() == 1 && (verb[0]=='Q'||verb[0]=='q'))) {
        return true;
    }

    // ── HELP / H / ? ──────────────────────────────────────────────────────────
    if (cmd_match(verb, "HELP") ||
        (verb.size() == 1 && (verb[0]=='H'||verb[0]=='h'||verb[0]=='?'))) {
        std::cout << "\n"
                  << BOLD() << "Simulator Commands (prefix with //):" << RESET() << "\n"
                  << "  //c <call>         Connect to remote callsign\n"
                  << "  //d                Disconnect\n"
                  << "  //ui <dest> <text> Send UI frame\n"
                  << "  //aprs <info>      Send APRS beacon\n"
                  << "  //myc <call>       Set/show callsign\n"
                  << "  //mon [on|off]     Toggle frame monitor\n"
                  << "  //hex [on|off]     Toggle hex dump display\n"
                  << "  //s                Show status + PTY path\n"
                  << "  //txdelay <ms>     Set KISS TX delay\n"
                  << "  //mtu <bytes>      Set I-frame MTU\n"
                  << "  //win <n>          Set window size 1-7\n"
                  << "  //t1 <ms>          Set T1 retransmit timer\n"
                  << "  //t3 <ms>          Set T3 keep-alive timer\n"
                  << "  //n2 <n>           Set max retries\n"
                  << "  //persist <val>    Set KISS persistence 0-255\n"
                  << "  //path <d1,d2>     Set digipeater path\n"
                  << "  //b [file|pattern] Run BASIC script or list/select\n"
                  << "  //q                Quit\n"
                  << "\n"
                  << "Text without // prefix is sent as I-frame data (when connected).\n"
                  << "Double Ctrl+C (within 5s) to exit.\n"
                  << "\n" << std::flush;
        return false;
    }

    // ── CONNECT / C ───────────────────────────────────────────────────────────
    if (cmd_match(verb, "CONNECT") || (verb.size() == 1 && (verb[0]=='C'||verb[0]=='c'))) {
        if (rest.empty()) {
            std::cout << RED() << "Usage: //c <callsign>" << RESET() << "\n" << std::flush;
            return false;
        }
        if (conn && conn->state() != Connection::State::DISCONNECTED) {
            std::cout << YELLOW() << "Already connected. Use //d to disconnect first."
                      << RESET() << "\n" << std::flush;
            return false;
        }
        Addr remote = Addr::make(rest);
        if (remote == cfg.ax25.mycall) {
            std::cout << RED() << "Cannot connect to yourself." << RESET() << "\n" << std::flush;
            return false;
        }
        std::cout << YELLOW() << "Connecting to " << BOLD() << remote.str() << RESET()
                  << YELLOW() << " from " << cfg.ax25.mycall.str() << "..."
                  << RESET() << "\n" << std::flush;
        conn = router.connect(remote);
        outgoing = true;
        ++st.frames_tx;
        return false;
    }

    // ── DISCONNECT / D ────────────────────────────────────────────────────────
    if (cmd_match(verb, "DISCONNECT") || (verb.size() == 1 && (verb[0]=='D'||verb[0]=='d'))) {
        if (!conn || conn->state() == Connection::State::DISCONNECTED) {
            std::cout << YELLOW() << "Not connected." << RESET() << "\n" << std::flush;
        } else {
            std::cout << YELLOW() << "Disconnecting..." << RESET() << "\n" << std::flush;
            conn->disconnect();
        }
        return false;
    }

    // ── UI <dest> <text> ──────────────────────────────────────────────────────
    if (cmd_match(verb, "UI")) {
        if (rest.empty()) {
            std::cout << RED() << "Usage: //ui <dest> <text>" << RESET() << "\n" << std::flush;
            return false;
        }
        std::string dest_str, text;
        std::size_t sp = rest.find_first_of(" \t");
        if (sp == std::string::npos) {
            dest_str = rest;
        } else {
            dest_str = rest.substr(0, sp);
            text = rest.substr(sp + 1);
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.erase(text.begin());
        }
        router.send_ui(Addr::make(dest_str), 0xF0, text);
        ++st.frames_tx;
        st.bytes_tx += text.size();
        std::cout << DIM() << "[UI sent to " << dest_str << "]" << RESET() << "\n" << std::flush;
        return false;
    }

    // ── APRS <info> ───────────────────────────────────────────────────────────
    if (cmd_match(verb, "APRS")) {
        if (rest.empty()) {
            std::cout << RED() << "Usage: //aprs <info string>" << RESET() << "\n" << std::flush;
            return false;
        }
        router.send_aprs(rest);
        ++st.frames_tx;
        st.bytes_tx += rest.size();
        std::cout << DIM() << "[APRS beacon sent]" << RESET() << "\n" << std::flush;
        return false;
    }

    // ── MSG <text> ────────────────────────────────────────────────────────────
    if (cmd_match(verb, "MSG")) {
        if (!conn || !conn->connected()) {
            std::cout << YELLOW() << "Not connected." << RESET() << "\n" << std::flush;
            return false;
        }
        if (rest.empty()) {
            std::cout << RED() << "Usage: //msg <text>" << RESET() << "\n" << std::flush;
            return false;
        }
        std::string payload = rest + "\r";
        if (conn->send(payload)) {
            st.bytes_tx += payload.size();
            ++st.frames_tx;
        }
        return false;
    }

    // ── MYCALL / MYC ──────────────────────────────────────────────────────────
    if (cmd_match(verb, "MYCALL")) {
        if (rest.empty()) {
            std::cout << "Callsign: " << cfg.ax25.mycall.str() << "\n" << std::flush;
        } else {
            Addr newcall = Addr::make(rest);
            cfg.ax25.mycall = newcall;
            router.config().mycall = newcall;
            std::cout << GREEN() << "Callsign set to " << cfg.ax25.mycall.str()
                      << RESET() << "\n" << std::flush;
        }
        return false;
    }

    // ── MONITOR / MON ─────────────────────────────────────────────────────────
    if (cmd_match(verb, "MONITOR") || cmd_match(verb, "MON")) {
        parse_toggle(rest, cfg.monitor, "Monitor");
        mon_on = cfg.monitor;
        return false;
    }

    // ── HEX ───────────────────────────────────────────────────────────────────
    if (cmd_match(verb, "HEX")) {
        parse_toggle(rest, cfg.hex_on, "HexDump");
        return false;
    }

    // ── STATUS / S ────────────────────────────────────────────────────────────
    if (cmd_match(verb, "STATUS") || (verb.size() == 1 && (verb[0]=='S'||verb[0]=='s'))) {
        show_status(cfg, conn, st, pty_path);
        return false;
    }

    // ── Parameter commands ────────────────────────────────────────────────────
    if (cmd_match(verb, "TXDELAY")) { param_cmd(rest, cfg.txdelay, "TXDelay", "ms"); return false; }
    if (cmd_match(verb, "MTU"))     { param_cmd(rest, cfg.ax25.mtu, "MTU", "bytes"); return false; }
    if (cmd_match(verb, "WIN") || cmd_match(verb, "WINDOW")) {
        param_cmd(rest, cfg.ax25.window, "Window", "", 1, 7); return false;
    }
    if (cmd_match(verb, "T1"))      { param_cmd(rest, cfg.ax25.t1_ms, "T1", "ms"); return false; }
    if (cmd_match(verb, "T3"))      { param_cmd(rest, cfg.ax25.t3_ms, "T3", "ms"); return false; }
    if (cmd_match(verb, "N2"))      { param_cmd(rest, cfg.ax25.n2, "N2", "retries"); return false; }
    if (cmd_match(verb, "PERSIST") || cmd_match(verb, "PERSISTENCE")) {
        param_cmd(rest, cfg.ax25.persist, "Persistence", "", 0, 255); return false;
    }

    // ── PATH ──────────────────────────────────────────────────────────────────
    if (cmd_match(verb, "PATH")) {
        if (rest.empty()) {
            if (cfg.ax25.digis.empty()) {
                std::cout << "Digipeater path: (none)\n" << std::flush;
            } else {
                std::cout << "Digipeater path: ";
                for (std::size_t i = 0; i < cfg.ax25.digis.size(); ++i) {
                    if (i) std::cout << ",";
                    std::cout << cfg.ax25.digis[i].str();
                }
                std::cout << "\n" << std::flush;
            }
        } else {
            cfg.ax25.digis.clear();
            std::istringstream ss(rest);
            std::string hop;
            while (std::getline(ss, hop, ',')) {
                while (!hop.empty() && hop.front() == ' ') hop.erase(hop.begin());
                while (!hop.empty() && hop.back() == ' ') hop.pop_back();
                if (!hop.empty()) cfg.ax25.digis.push_back(Addr::make(hop));
            }
            std::cout << DIM() << "[Path set: " << rest << "]" << RESET() << "\n" << std::flush;
        }
        return false;
    }

    // ── BASIC / B ─────────────────────────────────────────────────────────────
    if (cmd_match(verb, "BASIC") || (verb.size() == 1 && (verb[0]=='B'||verb[0]=='b'))) {
        handle_basic_cmd(rest, conn, pty_fd, cfg, router, st);
        return false;
    }

    // Unknown command
    std::cout << RED() << "Unknown command: //" << verb
              << RESET() << "  (type //h for help)\n" << std::flush;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument parser
// ─────────────────────────────────────────────────────────────────────────────
static bool parse_args(int argc, char* argv[], SimCfg& cfg) {
    static const struct option long_opts[] = {
        {"mtu",      required_argument, nullptr, 'M'},
        {"txdelay",  required_argument, nullptr, 'T'},
        {"bas-path", required_argument, nullptr, 'S'},
        {"help",     no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int ch;
    while ((ch = getopt_long(argc, argv, "c:l:s:w:t:k:n:p:h", long_opts, nullptr)) != -1) {
        switch (ch) {
        case 'c': cfg.ax25.mycall = Addr::make(optarg); break;
        case 'l': cfg.link_path = optarg; break;
        case 's': cfg.scripts.set_default_dir(optarg); break;
        case 'S': cfg.scripts.add_search_path(optarg); break;
        case 'w': cfg.ax25.window = std::atoi(optarg); break;
        case 't': cfg.ax25.t1_ms = std::atoi(optarg); break;
        case 'k': cfg.ax25.t3_ms = std::atoi(optarg); break;
        case 'n': cfg.ax25.n2 = std::atoi(optarg); break;
        case 'p': {
            std::istringstream ss(optarg);
            std::string hop;
            while (std::getline(ss, hop, ','))
                cfg.ax25.digis.push_back(Addr::make(hop));
            break;
        }
        case 'M': cfg.ax25.mtu = std::atoi(optarg); break;
        case 'T': cfg.txdelay = std::atoi(optarg); break;
        case 'h':
            std::cout << "Usage: ax25sim [OPTIONS]\n\n"
                      << "Options:\n"
                      << "  -c CALL         Callsign (default: N0SIM)\n"
                      << "  -l PATH         PTY symlink path (default: /tmp/kiss_sim)\n"
                      << "  -s DIR          Script directory (default: .)\n"
                      << "  --bas-path DIR  Extra script search path (repeatable, highest priority)\n"
                      << "  -w N            Window size 1-7 (default: 3)\n"
                      << "  -t MS           T1 retransmit timer ms (default: 3000)\n"
                      << "  -k MS           T3 keep-alive timer ms (default: 60000)\n"
                      << "  -n N            Max retry count (default: 10)\n"
                      << "  --mtu N         MTU bytes (default: 128)\n"
                      << "  --txdelay N     TX delay ms (default: 400)\n"
                      << "  -p PATH         Digipeater path (comma-separated)\n"
                      << "  -h              Show this help\n\n"
                      << "Environment:\n"
                      << "  KISSBBS_BASIC_PATH   Default script search directory\n";
            return false;
        default:
            return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main simulator loop
// ─────────────────────────────────────────────────────────────────────────────
static int run_sim(Kiss& kiss, Router& router, SimCfg& cfg,
                    const std::string& pty_path) {
    int         pty_fd   = kiss.fd();
    Stats       st;
    Connection* conn     = nullptr;
    bool        outgoing = false;
    bool        mon_on   = cfg.monitor;
    std::string recv_buf;
    std::vector<Connection*> dead_conns;

    // ── Monitor hook (on by default) ──────────────────────────────────────────
    if (mon_on)
        router.on_monitor = [&](const Frame& f){ print_frame(f, ">>", cfg.hex_on); };

    // ── Attach callbacks to a connection ──────────────────────────────────────
    auto attach_callbacks = [&](Connection* c) {
        c->on_connect = [&, c]() {
            st.connect_t = static_cast<uint64_t>(time(nullptr));
            const char* dir = outgoing ? "to" : "from";
            const char* color = outgoing ? GREEN() : CYAN();
            std::cout << "\n" << color << BOLD()
                      << "*** Connected " << dir << " "
                      << c->remote().str() << " ***" << RESET() << "\n" << std::flush;
            print_prompt(cfg, c, outgoing, router.connections().size());
        };
        c->on_disconnect = [&]() {
            std::cout << "\n" << YELLOW() << "*** Disconnected ***" << RESET() << "\n" << std::flush;
            dead_conns.push_back(conn);
            conn = nullptr;
            outgoing = false;
            print_prompt(cfg, nullptr, false, 0);
        };
        c->on_data = [&](const uint8_t* d, std::size_t n) {
            st.bytes_rx += n; ++st.frames_rx;
            std::string text(reinterpret_cast<const char*>(d), n);
            // Strip trailing CR/LF for clean display
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
                text.pop_back();
            if (!text.empty())
                std::cout << CYAN() << text << RESET() << "\n" << std::flush;
            if (cfg.hex_on) {
                std::cout << DIM()
                          << hex_dump(d, n, "  ")
                          << RESET() << std::flush;
            }
        };
    };

    // ── Listen for incoming connections ────────────────────────────────────────
    router.listen([&](Connection* inc) {
        if (conn && conn->state() != Connection::State::DISCONNECTED) {
            // Already connected — reject
            return;
        }
        conn = inc;
        outgoing = false;
        attach_callbacks(inc);
    });

    // ── UI frame callback ─────────────────────────────────────────────────────
    router.on_ui = [&](const Frame& f) {
        ++st.ui_rx;
        if (mon_on)
            print_frame(f, "UI", cfg.hex_on);
    };

    // ── Banner ────────────────────────────────────────────────────────────────
    std::cout << BOLD() << "AX.25 TNC Simulator" << RESET() << "\n"
              << "  PTY      : " << pty_path << "\n"
              << "  Callsign : " << cfg.ax25.mycall.str() << "\n"
              << "  Scripts  : " << cfg.scripts.search_dirs().front() << "/\n"
              << "Type " << BOLD() << "//h" << RESET() << " for help. Listening for connections.\n\n"
              << std::flush;
    print_prompt(cfg, conn, outgoing, 0);

    // ── Main loop ─────────────────────────────────────────────────────────────
    while (!g_quit) {
        // Poll the KISS/Router
        router.poll();

        // Deferred cleanup
        for (auto* dc : dead_conns) delete dc;
        dead_conns.clear();

        // Handle Ctrl+C warning
        if (g_ctrl_c_count == 1) {
            std::cout << "\n" << YELLOW()
                      << "[Press Ctrl+C again within 5s to exit]"
                      << RESET() << "\n" << std::flush;
            g_ctrl_c_count = 0;
            if (conn && conn->connected()) {
                std::cout << YELLOW() << "Disconnecting..." << RESET() << "\n" << std::flush;
                conn->disconnect();
            }
            print_prompt(cfg, conn, outgoing, router.connections().size());
        }

        // Read stdin (non-blocking)
        std::string line;
        if (!stdin_readline(line, 20)) continue;  // 20ms poll

        // Command parsing
        if (line.size() >= 2 && line[0] == '/' && line[1] == '/') {
            std::string cmd_line = line.substr(2);
            if (parse_sim_command(cmd_line, cfg, router, conn, outgoing,
                                   mon_on, st, pty_fd, pty_path)) {
                break; // QUIT
            }
            // After connect command, attach callbacks if new conn exists
            if (conn && !conn->on_connect) {
                attach_callbacks(conn);
            }
            // Update monitor hook based on current state
            if (mon_on)
                router.on_monitor = [&](const Frame& f){ print_frame(f, ">>", cfg.hex_on); };
            else
                router.on_monitor = nullptr;
        } else if (!line.empty()) {
            // Bare text → send as I-frame if connected
            if (conn && conn->connected()) {
                std::string payload = line + "\r";
                if (conn->send(payload)) {
                    st.bytes_tx += payload.size();
                    ++st.frames_tx;
                }
            } else {
                std::cout << YELLOW() << "Not connected. Use //c <call> to connect, "
                          << "or //ui to send UI frames." << RESET() << "\n" << std::flush;
            }
        }

        print_prompt(cfg, conn, outgoing, router.connections().size());
    }

    // ── Graceful shutdown ─────────────────────────────────────────────────────
    if (conn && conn->state() != Connection::State::DISCONNECTED) {
        conn->disconnect();
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < end) {
            router.poll();
            usleep(10000);
            if (!conn || conn->state() == Connection::State::DISCONNECTED) break;
        }
    }
    for (auto* dc : dead_conns) delete dc;

    // Summary
    std::cout << "\n" << DIM()
              << "Session: TX " << st.frames_tx << " frames (" << st.bytes_tx << " bytes)"
              << "  RX " << st.frames_rx << " frames (" << st.bytes_rx << " bytes)"
              << "  UI " << st.ui_rx
              << RESET() << "\n" << std::flush;

    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    SimCfg cfg;
    if (!parse_args(argc, argv, cfg)) return 1;

    g_colour = isatty(STDOUT_FILENO) != 0;

    // Signal handling
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Create PTY pair
    int master_fd = -1, slave_fd = -1;
    std::string slave_path;
    if (!open_pty(master_fd, slave_fd, slave_path)) {
        std::cerr << "Failed to create PTY.\n";
        return 1;
    }

    // Symlink to stable path
    std::string display_path = slave_path;
    if (!cfg.link_path.empty()) {
        ::unlink(cfg.link_path.c_str());
        if (::symlink(slave_path.c_str(), cfg.link_path.c_str()) == 0) {
            display_path = cfg.link_path + " -> " + slave_path;
        } else {
            std::cerr << "Warning: symlink " << cfg.link_path
                      << ": " << strerror(errno) << "\n";
        }
    }

    // Create KISS layer on PTY master fd
    Kiss kiss;
    if (!kiss.open_fd(master_fd)) {
        std::cerr << "Failed to initialize KISS on PTY.\n";
        return 1;
    }

    // Set KISS TX parameters
    cfg.ax25.txdelay = cfg.txdelay / 10;  // convert ms to KISS units (×10ms)

    // Create Router
    Router router(kiss, cfg.ax25);

    // Run the simulator
    int rc = run_sim(kiss, router, cfg, display_path);

    // Cleanup
    if (!cfg.link_path.empty()) ::unlink(cfg.link_path.c_str());
    ::close(slave_fd);
    kiss.close();

    return rc;
}
