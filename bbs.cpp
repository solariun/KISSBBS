// =============================================================================
// bbs.cpp — AX.25 BBS  (multi-connection, shell access, APRS/UI)
//
// Compile:
//   Linux : g++ -std=c++11 -O2 -o bbs ax25lib.cpp bbs.cpp -lutil
//   macOS : g++ -std=c++11 -O2 -o bbs ax25lib.cpp bbs.cpp
//
// Usage:
//   ./bbs -c KD9XXX-1 -b 9600 /dev/ttyUSB0
//
// Extra flags (in addition to standard ax25lib flags):
//   -n <name>           BBS name (default: AX25BBS)
//   -u <text>           APRS beacon info string
//   -B <secs>           APRS beacon interval seconds (0 = off)
//   --ui <DEST> <text>  Send one UI frame and exit
//   --aprs <text>       Send one APRS frame and exit
//
// Session commands (typed by the remote user over AX.25):
//   H / ?                  Help
//   U                      List connected users
//   M  <CALL> <msg>        Send in-BBS message to connected user
//   UI <DEST> <text>       Send a raw UI frame over the air
//   POS <lat> <lon> [sym] [comment]
//                          Send APRS position (lat/lon in decimal degrees)
//                          sym: one-char APRS symbol (default '>'); e.g. '-' house
//   AMSG <CALL> <msg>      Send APRS message to any callsign
//   I                      BBS info
//   B                      Send APRS beacon now
//   SH                     Open remote shell
//   BYE / Q                Disconnect
// =============================================================================
#include "ax25lib.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#  include <util.h>
#else
#  include <pty.h>
#endif

using namespace ax25;

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────
static std::string timestamp() {
    time_t t = time(nullptr);
    struct tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

// Upper-case a string
static std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// APRS helpers
// ─────────────────────────────────────────────────────────────────────────────

// Build APRS position string (!DDMM.mmN/DDDMM.mmWS[comment])
//   lat, lon  : decimal degrees (negative = S / W)
//   sym       : APRS symbol char ('>'=>car, '-'=>house, 'K'=>school, etc.)
static std::string make_aprs_pos(double lat, double lon,
                                  char sym = '>',
                                  const std::string& comment = "")
{
    char latch = lat >= 0 ? 'N' : 'S';
    char lonch = lon >= 0 ? 'E' : 'W';
    double alat = std::abs(lat), alon = std::abs(lon);
    int latd = (int)alat;  double latm = (alat - latd) * 60.0;
    int lond = (int)alon;  double lonm = (alon - lond) * 60.0;
    char buf[80];
    snprintf(buf, sizeof(buf), "!%02d%05.2f%c/%03d%05.2f%c%c%s",
             latd, latm, latch, lond, lonm, lonch, sym, comment.c_str());
    return buf;
}

// Build APRS message frame  (:ADDRESSEE :text{seq})
static int g_aprs_seq = 0;
static std::string make_aprs_msg(const std::string& dest, const std::string& text) {
    // Addressee padded to 9 chars
    char addr[10];
    snprintf(addr, sizeof(addr), "%-9s", dest.c_str());
    ++g_aprs_seq;
    char buf[256];
    snprintf(buf, sizeof(buf), ":%s:%s{%03d}", addr, text.c_str(), g_aprs_seq);
    return buf;
}

// Parse an APRS message info field.  Returns true and fills 'out' on success.
struct APRSMsg { std::string to, text, seq; };
static bool parse_aprs_msg(const std::string& info, APRSMsg& m) {
    // Format: :ADDRESSEE:text{seq}
    if (info.size() < 11 || info[0] != ':') return false;
    std::string::size_type sep = info.find(':', 1);
    if (sep == std::string::npos || sep > 10) return false;
    m.to   = trim(info.substr(1, sep - 1));
    m.text = info.substr(sep + 1);
    // strip {seq}
    std::string::size_type brace = m.text.rfind('{');
    if (brace != std::string::npos) {
        m.seq  = m.text.substr(brace + 1);
        std::string::size_type cb = m.seq.find('}');
        if (cb != std::string::npos) m.seq.resize(cb);
        m.text.resize(brace);
    }
    m.text = trim(m.text);
    return true;
}

// Guess if an APRS info field is a position (starts with ! @ = / ')
static bool is_aprs_pos(const std::string& info) {
    if (info.empty()) return false;
    return (info[0]=='!' || info[0]=='=' || info[0]=='@' ||
            info[0]=='/' || info[0]=='\'');
}

// Pretty-print a raw Frame info field as a string (printable only)
static std::string info_str(const Frame& f) {
    std::string s(f.info.begin(), f.info.end());
    for (auto& c : s) if ((unsigned char)c < 0x20 && c!='\r' && c!='\n') c = '.';
    while (!s.empty() && (s.back()=='\r'||s.back()=='\n')) s.pop_back();
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Session — one connected AX.25 station
// ─────────────────────────────────────────────────────────────────────────────
struct Session {
    Connection* conn;
    std::string callsign;
    std::string recv_buf;
    bool shell_mode = false;
    int  pty_master = -1;
    pid_t shell_pid = -1;

    explicit Session(Connection* c) : conn(c), callsign(c->remote().str()) {}

    ~Session() { stop_shell(); delete conn; }

    bool start_shell() {
        if (shell_mode) return true;
        struct winsize ws{24, 80, 0, 0};
        pid_t pid = forkpty(&pty_master, nullptr, nullptr, &ws);
        if (pid < 0) return false;
        if (pid == 0) {
            const char* sh = getenv("SHELL");
            if (!sh) sh = "/bin/sh";
            execl(sh, sh, nullptr);
            _exit(127);
        }
        shell_pid  = pid;
        shell_mode = true;
        int fl = fcntl(pty_master, F_GETFL, 0);
        fcntl(pty_master, F_SETFL, fl | O_NONBLOCK);
        return true;
    }

    void stop_shell() {
        if (shell_pid > 0) { kill(shell_pid, SIGHUP); waitpid(shell_pid, nullptr, WNOHANG); shell_pid=-1; }
        if (pty_master >= 0) { close(pty_master); pty_master=-1; }
        shell_mode = false;
    }

    void reply(const std::string& s) {
        if (conn && conn->connected()) conn->send(s);
    }
    void println(const std::string& s) { reply(s + "\r\n"); }
};

// ─────────────────────────────────────────────────────────────────────────────
// BBS
// ─────────────────────────────────────────────────────────────────────────────
class BBS {
public:
    BBS(const std::string& name,
        Router& router,
        const std::string& beacon_text,
        int beacon_interval_s)
        : name_(name), router_(router),
          beacon_text_(beacon_text),
          beacon_interval_s_(beacon_interval_s),
          next_beacon_(0)
    {
        // Accept incoming AX.25 connections
        router_.listen([this](Connection* c){ on_accept(c); });

        // Receive all UI / APRS frames from the air
        router_.on_ui = [this](const Frame& f){ on_ui_frame(f); };
    }

    void run() {
        running_ = true;
        while (running_) {
            router_.poll();
            poll_shells();
            reap_dead();
            send_beacon_if_due();
            usleep(5000);
        }
    }

    void stop() { running_ = false; }

    // Broadcast a text to all connected (non-shell) sessions
    void broadcast(const std::string& msg, const std::string& from = "") {
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if (it->second->shell_mode) continue;
            std::string m = "\r\n[BROADCAST";
            if (!from.empty()) m += " from " + from;
            m += "] " + msg + "\r\n";
            it->second->reply(m);
        }
    }

    std::string list_users() const {
        if (sessions_.empty()) return "(none)";
        std::string s;
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) { if (!s.empty()) s += ", "; s += it->first; }
        return s;
    }

    void send_ui_once(const Addr& dest, const std::string& text) {
        router_.send_ui(dest, 0xF0, text);
    }
    void send_aprs_once(const std::string& text) {
        router_.send_aprs(text);
    }

private:
    std::string  name_;
    Router&      router_;
    std::string  beacon_text_;
    int          beacon_interval_s_;
    time_t       next_beacon_;
    bool         running_ = false;

    std::map<std::string, std::unique_ptr<Session>> sessions_;
    std::vector<std::string> dead_;

    // ── Accept incoming connection ────────────────────────────────────────
    void on_accept(Connection* c) {
        std::string call = c->remote().str();
        std::cerr << "[" << timestamp() << "] CONNECT  " << call << "\n";

        std::unique_ptr<Session> ses(new Session(c));

        c->on_connect    = [this, call]()                            { on_connected(call);    };
        c->on_disconnect = [this, call]()                            { on_disconnected(call); };
        c->on_data       = [this, call](const uint8_t* d, std::size_t n){ on_data(call, d, n); };

        sessions_[call] = std::move(ses);
        on_connected(call);   // already CONNECTED when listen cb fires
    }

    void on_connected(const std::string& call) {
        auto it = sessions_.find(call);
        if (it == sessions_.end()) return;
        auto& ses = *it->second;
        ses.println("*** " + name_ + " AX.25 BBS ***");
        ses.println("Welcome " + call + "!  Type H for help.");
        ses.println("Users online: " + list_users());
        ses.println("");
    }

    void on_disconnected(const std::string& call) {
        std::cerr << "[" << timestamp() << "] DISCONN  " << call << "\n";
        dead_.push_back(call);
    }

    // ── Receive data from a connected session ─────────────────────────────
    void on_data(const std::string& call,
                 const uint8_t* data, std::size_t len)
    {
        auto it = sessions_.find(call);
        if (it == sessions_.end()) return;
        auto& ses = *it->second;

        // Shell mode: forward raw bytes to PTY
        if (ses.shell_mode) {
            if (ses.pty_master >= 0) {
                // Check for escape sequence ~. on a new line → exit shell
                for (std::size_t i = 0; i < len; ++i) {
                    char ch = (char)data[i];
                    ses.recv_buf += ch;
                    if (ch == '\n' || ch == '\r') {
                        if (trim(ses.recv_buf) == "~.") {
                            ses.stop_shell();
                            ses.recv_buf.clear();
                            ses.println("\r\n[Shell closed]");
                            return;
                        }
                        // Forward line to pty
                        ::write(ses.pty_master, ses.recv_buf.c_str(), ses.recv_buf.size());
                        ses.recv_buf.clear();
                    }
                }
            }
            return;
        }

        // Normal mode: accumulate lines
        for (std::size_t i = 0; i < len; ++i) {
            char ch = (char)data[i];
            if (ch == '\r' || ch == '\n') {
                std::string line = trim(ses.recv_buf);
                ses.recv_buf.clear();
                if (!line.empty()) handle_command(call, line, ses);
            } else if (ch == '\x08' || ch == '\x7F') {
                if (!ses.recv_buf.empty()) ses.recv_buf.pop_back();
            } else {
                ses.recv_buf += ch;
            }
        }
    }

    // ── Receive any UI / APRS frame from the air ──────────────────────────
    void on_ui_frame(const Frame& f) {
        std::string from = f.src.str();
        std::string info = info_str(f);

        // Log to stderr
        std::string pid_s = (f.pid == 0xF0) ? "APRS" : ("PID=" + std::to_string(f.pid));
        std::cerr << "[" << timestamp() << "] UI  " << from
                  << ">" << f.dest.str()
                  << " [" << pid_s << "] " << info << "\n";

        // Try to parse as APRS
        if (f.pid == 0xF0 && !f.info.empty()) {
            std::string infostr(f.info.begin(), f.info.end());

            APRSMsg parsed_msg;
            if (parse_aprs_msg(infostr, parsed_msg)) {
                // It's an APRS message — find addressee among connected users
                route_aprs_msg(from, parsed_msg);
            } else if (is_aprs_pos(infostr)) {
                // Position report: notify connected users (non-shell)
                std::string notice = "\r\n[APRS POS] " + from + ": " + trim(infostr) + "\r\n";
                for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
                    if (!it->second->shell_mode) it->second->reply(notice);
            }
        }
    }

    // Route an APRS message to the matching connected session
    void route_aprs_msg(const std::string& from, const APRSMsg& msg) {
        // Strip SSID suffix for matching (APRS messages often omit it)
        std::string to_upper = upper(msg.to);

        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            const std::string& call = it->first;
            std::unique_ptr<Session>& ses = it->second;
            // Match if to_upper is a prefix of (or equal to) the connected callsign
            std::string cu = upper(call);
            bool match = (cu == to_upper) ||
                         (cu.substr(0, cu.find('-')) == to_upper) ||
                         (to_upper.substr(0, to_upper.find('-')) == cu.substr(0, cu.find('-')));
            if (match && !ses->shell_mode) {
                ses->println("\r\n[APRS MSG from " + from + "] " + msg.text);
                std::cerr << "[" << timestamp() << "] APRS→SES "
                          << from << " → " << call << ": " << msg.text << "\n";
            }
        }
    }

    // ── BBS command dispatcher ────────────────────────────────────────────
    void handle_command(const std::string& call,
                        const std::string& line,
                        Session& ses)
    {
        std::cerr << "[" << timestamp() << "] CMD " << call << " > " << line << "\n";
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;
        cmd = upper(cmd);

        if (cmd == "H" || cmd == "HELP" || cmd == "?") {
            ses.println("Commands:");
            ses.println("  H / ?                   This help");
            ses.println("  U                        List connected users");
            ses.println("  M  <CALL> <msg>          Send in-BBS message to connected user");
            ses.println("  UI <DEST> <text>         Send UI frame (any callsign/DEST)");
            ses.println("  POS <lat> <lon> [sym] [comment]");
            ses.println("                           Send APRS position");
            ses.println("                           sym: 1-char APRS symbol (default '>')");
            ses.println("  AMSG <CALL> <msg>        Send APRS message to any callsign");
            ses.println("  I                        BBS info");
            ses.println("  B                        Send APRS beacon now");
            ses.println("  SH                       Open remote shell");
            ses.println("  BYE / Q                  Disconnect");

        } else if (cmd == "U" || cmd == "USERS") {
            ses.println("Online: " + list_users());

        } else if (cmd == "M" || cmd == "MSG") {
            // In-BBS message to connected user
            std::string dest_call, msg;
            ss >> dest_call;
            std::getline(ss, msg); msg = trim(msg);
            if (dest_call.empty()) { ses.println("Usage: M <CALL> <message>"); return; }
            auto it2 = sessions_.find(dest_call);
            if (it2 == sessions_.end()) {
                ses.println("User " + dest_call + " not connected.");
            } else {
                it2->second->println("\r\n[MSG from " + call + "]: " + msg);
                ses.println("Message sent.");
            }

        } else if (cmd == "UI") {
            // Send raw UI frame
            std::string dest_str, text;
            ss >> dest_str;
            std::getline(ss, text); text = trim(text);
            if (dest_str.empty() || text.empty()) {
                ses.println("Usage: UI <DEST> <text>");
                return;
            }
            Addr dest = Addr::make(dest_str);
            router_.send_ui(dest, 0xF0, text);
            ses.println("UI sent → " + dest.str() + ": " + text);
            std::cerr << "[" << timestamp() << "] UI-TX  " << call
                      << " → " << dest.str() << ": " << text << "\n";

        } else if (cmd == "POS") {
            // Send APRS position
            double lat = 0, lon = 0;
            char sym = '>';
            std::string rest;
            ss >> lat >> lon;
            // Optional symbol char
            std::string sym_str;
            if (ss >> sym_str && !sym_str.empty()) {
                if (sym_str.size() == 1 && !std::isdigit((unsigned char)sym_str[0])) {
                    sym = sym_str[0];
                    std::getline(ss, rest); rest = trim(rest);
                } else {
                    // It's the start of the comment, not a symbol
                    rest = sym_str;
                    std::string more; std::getline(ss, more);
                    rest += more;
                    rest = trim(rest);
                }
            }
            if (lat == 0 && lon == 0 && rest.empty()) {
                ses.println("Usage: POS <lat> <lon> [sym] [comment]");
                ses.println("  lat/lon in decimal degrees (-23.5 = 23.5 S)");
                ses.println("  sym: APRS symbol char (default '>', see APRS spec)");
                return;
            }
            std::string aprs_info = make_aprs_pos(lat, lon, sym, rest);
            router_.send_aprs(aprs_info);
            ses.println("APRS position sent: " + aprs_info);
            std::cerr << "[" << timestamp() << "] APRS-POS " << call
                      << ": " << aprs_info << "\n";

        } else if (cmd == "AMSG") {
            // Send APRS message to any callsign
            std::string dest_call, text;
            ss >> dest_call;
            std::getline(ss, text); text = trim(text);
            if (dest_call.empty() || text.empty()) {
                ses.println("Usage: AMSG <CALL> <message>");
                return;
            }
            std::string aprs_info = make_aprs_msg(dest_call, text);
            router_.send_aprs(aprs_info);
            ses.println("APRS MSG sent → " + dest_call + ": " + text);
            std::cerr << "[" << timestamp() << "] APRS-MSG " << call
                      << " → " << dest_call << ": " << text << "\n";

        } else if (cmd == "I" || cmd == "INFO") {
            ses.println("BBS: " + name_);
            ses.println("Callsign: " + router_.config().mycall.str());
            ses.println("Sessions: " + std::to_string(sessions_.size()));
            ses.println("MTU: "    + std::to_string(router_.config().mtu));
            ses.println("Window: " + std::to_string(router_.config().window));
            std::string digi;
            for (auto& d : router_.config().digis) { if (!digi.empty()) digi+=","; digi+=d.str(); }
            ses.println("Digis: " + (digi.empty() ? "(none)" : digi));

        } else if (cmd == "B" || cmd == "BEACON") {
            if (!beacon_text_.empty()) {
                router_.send_aprs(beacon_text_);
                ses.println("Beacon sent: " + beacon_text_);
            } else {
                ses.println("No beacon text configured  (use -u flag).");
            }

        } else if (cmd == "SH" || cmd == "SHELL") {
            ses.println("Starting shell — type ~. on a new line to exit.");
            if (!ses.start_shell()) ses.println("[ERROR] Could not start shell.");

        } else if (cmd == "BYE" || cmd == "QUIT" || cmd == "Q") {
            ses.println("73! Goodbye.");
            ses.conn->disconnect();

        } else {
            ses.println("Unknown: " + cmd + "  (type H for help)");
        }
    }

    // ── PTY output → AX.25 ───────────────────────────────────────────────
    void poll_shells() {
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            std::unique_ptr<Session>& ses = it->second;
            if (!ses->shell_mode || ses->pty_master < 0) continue;

            char buf[256];
            ssize_t n;
            while ((n = ::read(ses->pty_master, buf, sizeof(buf))) > 0)
                if (ses->conn && ses->conn->connected())
                    ses->conn->send((const uint8_t*)buf, (std::size_t)n);

            // Check if shell exited
            int st;
            if (ses->shell_pid > 0 &&
                waitpid(ses->shell_pid, &st, WNOHANG) == ses->shell_pid)
            {
                ses->shell_pid = -1;
                ses->stop_shell();
                ses->println("\r\n[Shell exited]");
            }
        }
    }

    // ── Cleanup disconnected sessions ─────────────────────────────────────
    void reap_dead() {
        for (const auto& call : dead_) sessions_.erase(call);
        dead_.clear();

        // Also reap sessions that silently died (keep-alive timeout etc.)
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if (it->second->conn->state() == Connection::State::DISCONNECTED)
                it = sessions_.erase(it);
            else
                ++it;
        }
    }

    // ── Periodic APRS beacon ──────────────────────────────────────────────
    void send_beacon_if_due() {
        if (beacon_interval_s_ <= 0 || beacon_text_.empty()) return;
        time_t now = time(nullptr);
        if (now >= next_beacon_) {
            router_.send_aprs(beacon_text_);
            next_beacon_ = now + beacon_interval_s_;
            std::cerr << "[" << timestamp() << "] BEACON   " << beacon_text_ << "\n";
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Signal handler
// ─────────────────────────────────────────────────────────────────────────────
static BBS* g_bbs = nullptr;
static void sig_handler(int) { if (g_bbs) g_bbs->stop(); }

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
static const char* EXTRA_USAGE = R"(
BBS-specific options:
  -n <name>               BBS name (default: AX25BBS)
  -u <text>               APRS beacon info string
  -B <secs>               APRS beacon interval seconds (0 = off, default 0)

One-shot modes (send frame and exit, no BBS):
  --ui   <DEST> <text>    Send one UI frame and exit
  --aprs <text>           Send one APRS frame and exit

Examples:
  bbs -c PY2XXX-1 -b 9600 /dev/ttyUSB0
  bbs -c W1AW -n MYNODE -u '!2330.00S/04636.00W#W1AW BBS' -B 600 /dev/ttyUSB0
  bbs -c PY2XXX --aprs '=2330.00S/04636.00W>Test station' /dev/ttyUSB0
  bbs -c PY2XXX --ui CQ 'Hello from PY2XXX' /dev/ttyUSB0
)";

int main(int argc, char* argv[]) {
    std::string bbs_name   = "AX25BBS";
    std::string beacon_text;
    int         beacon_int = 0;
    std::string ui_dest, ui_text, aprs_text;
    bool one_shot_ui = false, one_shot_aprs = false;

    // Pre-scan for BBS-specific long options and short flags not in CLIParams
    std::vector<char*> remaining;
    remaining.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ui" && i + 2 < argc) {
            one_shot_ui = true;
            ui_dest     = argv[++i];
            ui_text     = argv[++i];
        } else if (a == "--aprs" && i + 1 < argc) {
            one_shot_aprs = true;
            aprs_text     = argv[++i];
        } else if (a == "-n" && i + 1 < argc) {
            bbs_name = argv[++i];
        } else if (a == "-u" && i + 1 < argc) {
            beacon_text = argv[++i];
        } else if (a == "-B" && i + 1 < argc) {
            beacon_int = std::atoi(argv[++i]);
        } else {
            remaining.push_back(argv[i]);
        }
    }

    // Parse standard ax25lib flags
    CLIParams p;
    int rargc = (int)remaining.size();
    if (!p.parse(rargc, remaining.data(), EXTRA_USAGE)) return 1;

    // Open KISS serial port
    Kiss kiss;
    if (!kiss.open(p.device, p.baud)) {
        std::cerr << "Cannot open serial: " << p.device
                  << " — " << strerror(errno) << "\n";
        return 1;
    }
    kiss.set_txdelay(p.cfg.txdelay * 10);
    kiss.set_persistence(p.cfg.persist);

    Router router(kiss, p.cfg);

    // ── One-shot modes ────────────────────────────────────────────────────
    if (one_shot_ui) {
        router.send_ui(Addr::make(ui_dest), 0xF0, ui_text);
        std::cerr << "UI → " << ui_dest << ": " << ui_text << "\n";
        return 0;
    }
    if (one_shot_aprs) {
        router.send_aprs(aprs_text);
        std::cerr << "APRS: " << aprs_text << "\n";
        return 0;
    }

    // ── BBS mode ──────────────────────────────────────────────────────────
    std::string digi_str;
    for (auto& d : p.cfg.digis) { if (!digi_str.empty()) digi_str+=","; digi_str+=d.str(); }

    std::cerr
        << "====================================\n"
        << " " << bbs_name << " — AX.25 BBS\n"
        << " Callsign  : " << p.cfg.mycall.str()     << "\n"
        << " Device    : " << p.device << " @ " << p.baud << " baud\n"
        << " Digipeaters: " << (digi_str.empty() ? "(none)" : digi_str) << "\n"
        << " MTU       : " << p.cfg.mtu    << "\n"
        << " Window    : " << p.cfg.window << "\n"
        << " T1        : " << p.cfg.t1_ms  << " ms\n"
        << " T3        : " << p.cfg.t3_ms  << " ms\n"
        << " Beacon    : " << (beacon_int > 0 ? std::to_string(beacon_int)+"s" : "off") << "\n"
        << " Monitor   : UI/APRS frames logged to stderr\n"
        << "====================================\n";

    BBS bbs(bbs_name, router, beacon_text, beacon_int);
    g_bbs = &bbs;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, SIG_DFL);

    bbs.run();
    std::cerr << "BBS shutdown.\n";
    return 0;
}
