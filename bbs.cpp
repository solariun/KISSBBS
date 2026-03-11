// =============================================================================
// bbs.cpp — AX.25 BBS  (multi-connection, shell access, APRS/UI, BASIC scripts)
//
// Compile:
//   Linux : g++ -std=c++11 -DHAVE_SQLITE3 -O2 -o bbs basic.cpp ax25lib.cpp bbs.cpp -lutil -lsqlite3
//   macOS : g++ -std=c++11 -DHAVE_SQLITE3 -O2 -o bbs basic.cpp ax25lib.cpp bbs.cpp -lsqlite3
//
// Config file  bbs.ini  (optional -- command-line flags take precedence):
//   [kiss]   device, baud
//   [ax25]   callsign, mtu, window, t1_ms, t3_ms, n2, txdelay, persist, digipeaters
//   [bbs]    name, beacon, beacon_interval, welcome_script
//   [basic]  script_dir, database
//
// Session commands:
//   H/?    Help          U      List users      M <CALL> <msg>  Message
//   UI <DEST> <text>     POS <lat> <lon> [sym] [comment]
//   AMSG <CALL> <msg>    I      BBS info        B     Beacon
//   SH     Remote shell  BYE/Q  Disconnect
// =============================================================================
#include "ax25lib.hpp"
#include "basic.hpp"
#include "ini.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
// Simple utilities
// ─────────────────────────────────────────────────────────────────────────────
static std::string timestamp() {
    time_t t = time(nullptr);
    struct tm tm_buf;
    memset(&tm_buf, 0, sizeof(tm_buf));
    localtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return buf;
}

static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

static std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Session -- one connected AX.25 station
// ─────────────────────────────────────────────────────────────────────────────
struct Session {
    Connection*  conn;
    std::string  callsign;
    std::string  recv_buf;
    bool         shell_mode;
    int          pty_master;
    pid_t        shell_pid;
    // Line receive queue for BASIC INPUT/RECV
    std::vector<std::string> line_queue;

    explicit Session(Connection* c)
        : conn(c), callsign(c->remote().str()),
          shell_mode(false), pty_master(-1), shell_pid(-1) {}
    ~Session() { stop_shell(); delete conn; }

    bool start_shell() {
        if (shell_mode) return true;
        struct winsize ws;
        ws.ws_row = 24; ws.ws_col = 80; ws.ws_xpixel = 0; ws.ws_ypixel = 0;
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
        if (shell_pid > 0) { kill(shell_pid, SIGHUP); waitpid(shell_pid, nullptr, WNOHANG); shell_pid = -1; }
        if (pty_master >= 0) { close(pty_master); pty_master = -1; }
        shell_mode = false;
    }

    void reply(const std::string& s) { if (conn && conn->connected()) conn->send(s); }
    void println(const std::string& s) { reply(s + "\r\n"); }
};

// ─────────────────────────────────────────────────────────────────────────────
// BBS
// ─────────────────────────────────────────────────────────────────────────────
class BBS {
public:
    BBS(const std::string& name, Router& router,
        const std::string& beacon_text, int beacon_interval_s,
        const std::string& welcome_script, const std::string& db_path)
        : name_(name), beacon_text_(beacon_text),
          welcome_script_(welcome_script), db_path_(db_path),
          router_(router), beacon_interval_s_(beacon_interval_s),
          next_beacon_(0), running_(false)
    {
        router_.listen([this](Connection* c) { on_accept(c); });
        router_.on_ui = [this](const Frame& f)  { on_ui_frame(f); };
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
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if (!s.empty()) s += ", ";
            s += it->first;
        }
        return s;
    }

    void send_ui_once(const Addr& dest, const std::string& text) {
        router_.send_ui(dest, 0xF0, text);
    }
    void send_aprs_once(const std::string& text) {
        router_.send_aprs(text);
    }

private:
    std::string  name_, beacon_text_, welcome_script_, db_path_;
    Router&      router_;
    int          beacon_interval_s_;
    time_t       next_beacon_;
    bool         running_;

    std::map<std::string, std::unique_ptr<Session>> sessions_;
    std::vector<std::string> dead_;

    // ── Accept ────────────────────────────────────────────────────────────
    void on_accept(Connection* c) {
        std::string call = c->remote().str();
        std::cerr << "[" << timestamp() << "] CONNECT  " << call << "\n";

        std::unique_ptr<Session> ses(new Session(c));
        c->on_connect    = [this, call]()                               { on_connected(call);    };
        c->on_disconnect = [this, call]()                               { on_disconnected(call); };
        c->on_data       = [this, call](const uint8_t* d, std::size_t n){ on_data(call, d, n);   };

        sessions_[call] = std::move(ses);
        on_connected(call);
    }

    void on_connected(const std::string& call) {
        auto it = sessions_.find(call);
        if (it == sessions_.end()) return;
        Session& ses = *it->second;

        if (!welcome_script_.empty()) {
            run_script(ses, welcome_script_);
        } else {
            ses.println("*** " + name_ + " AX.25 BBS ***");
            ses.println("Welcome " + call + "!  Type H for help.");
            ses.println("Users online: " + list_users());
            ses.println("");
        }
    }

    void on_disconnected(const std::string& call) {
        std::cerr << "[" << timestamp() << "] DISCONN  " << call << "\n";
        dead_.push_back(call);
    }

    // ── Run a BASIC script for a session ──────────────────────────────────
    void run_script(Session& ses, const std::string& path) {
        Basic interp;
        interp.on_send = [&ses](const std::string& s) { ses.reply(s); };
        interp.on_recv = [&ses](int /*ms*/) -> std::string {
            if (!ses.line_queue.empty()) {
                std::string line = ses.line_queue.front();
                ses.line_queue.erase(ses.line_queue.begin());
                return line;
            }
            return "";
        };
        interp.on_log = [](const std::string& msg) {
            std::cerr << "[BASIC] " << msg << "\n";
        };
        interp.set_str("callsign$", ses.callsign);
        interp.set_str("bbs_name$", name_);
        if (!db_path_.empty()) interp.set_str("db_path$", db_path_);
        if (!interp.load_file(path)) {
            ses.println("*** " + name_ + " AX.25 BBS ***");
            ses.println("Welcome " + ses.callsign + "!  Type H for help.");
        } else {
            interp.run();
        }
    }

    // ── Receive data ──────────────────────────────────────────────────────
    void on_data(const std::string& call,
                 const uint8_t* data, std::size_t len)
    {
        auto it = sessions_.find(call);
        if (it == sessions_.end()) return;
        Session& ses = *it->second;

        if (ses.shell_mode) {
            if (ses.pty_master >= 0) {
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
                        ::write(ses.pty_master, ses.recv_buf.c_str(), ses.recv_buf.size());
                        ses.recv_buf.clear();
                    }
                }
            }
            return;
        }

        for (std::size_t i = 0; i < len; ++i) {
            char ch = (char)data[i];
            if (ch == '\r' || ch == '\n') {
                std::string line = trim(ses.recv_buf);
                ses.recv_buf.clear();
                if (!line.empty()) {
                    ses.line_queue.push_back(line);
                    handle_command(call, line, ses);
                }
            } else if (ch == '\x08' || ch == '\x7F') {
                if (!ses.recv_buf.empty()) ses.recv_buf.pop_back();
            } else {
                ses.recv_buf += ch;
            }
        }
    }

    // ── UI/APRS frames ────────────────────────────────────────────────────
    void on_ui_frame(const Frame& f) {
        std::string from = f.src.str();
        std::string info = aprs::info_str(f);
        std::string pid_s = (f.pid == 0xF0) ? "APRS" : ("PID=" + std::to_string(f.pid));
        std::cerr << "[" << timestamp() << "] UI  " << from
                  << ">" << f.dest.str() << " [" << pid_s << "] " << info << "\n";

        if (f.pid == 0xF0 && !f.info.empty()) {
            std::string infostr(f.info.begin(), f.info.end());
            aprs::Msg msg;
            if (aprs::parse_msg(infostr, msg)) {
                route_aprs_msg(from, msg);
            } else if (aprs::is_pos(infostr)) {
                std::string notice = "\r\n[APRS POS] " + from + ": " + trim(infostr) + "\r\n";
                for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
                    if (!it->second->shell_mode) it->second->reply(notice);
            }
        }
    }

    void route_aprs_msg(const std::string& from, const aprs::Msg& msg) {
        std::string to_u = upper(msg.to);
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            const std::string& call = it->first;
            std::string cu = upper(call);
            bool match = (cu == to_u)
                      || (cu.substr(0, cu.find('-'))   == to_u)
                      || (to_u.substr(0, to_u.find('-')) == cu.substr(0, cu.find('-')));
            if (match && !it->second->shell_mode) {
                it->second->println("\r\n[APRS MSG from " + from + "] " + msg.text);
                std::cerr << "[" << timestamp() << "] APRS->SES "
                          << from << " -> " << call << ": " << msg.text << "\n";
            }
        }
    }

    // ── Command dispatcher ────────────────────────────────────────────────
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
            ses.println("  H/?  Help          U     Users        M <CALL> <msg>  Message");
            ses.println("  UI <DEST> <text>   Send UI frame");
            ses.println("  POS <lat> <lon> [sym] [comment]   APRS position");
            ses.println("  AMSG <CALL> <msg>  APRS message    I  Info   B  Beacon");
            ses.println("  SH  Remote shell   BYE/Q  Disconnect");

        } else if (cmd == "U" || cmd == "USERS") {
            ses.println("Online: " + list_users());

        } else if (cmd == "M" || cmd == "MSG") {
            std::string dest, msg_text;
            ss >> dest;
            std::getline(ss, msg_text); msg_text = trim(msg_text);
            if (dest.empty()) { ses.println("Usage: M <CALL> <message>"); return; }
            auto it2 = sessions_.find(dest);
            if (it2 == sessions_.end()) ses.println("User " + dest + " not connected.");
            else { it2->second->println("\r\n[MSG from " + call + "]: " + msg_text); ses.println("Sent."); }

        } else if (cmd == "UI") {
            std::string dest_str, text;
            ss >> dest_str;
            std::getline(ss, text); text = trim(text);
            if (dest_str.empty() || text.empty()) { ses.println("Usage: UI <DEST> <text>"); return; }
            router_.send_ui(Addr::make(dest_str), 0xF0, text);
            ses.println("UI sent -> " + dest_str + ": " + text);
            std::cerr << "[" << timestamp() << "] UI-TX " << call << " -> " << dest_str << ": " << text << "\n";

        } else if (cmd == "POS") {
            double lat = 0, lon = 0; char sym = '>'; std::string rest;
            ss >> lat >> lon;
            std::string sym_str;
            if (ss >> sym_str && !sym_str.empty()) {
                if (sym_str.size()==1 && !std::isdigit((unsigned char)sym_str[0])) {
                    sym = sym_str[0]; std::getline(ss, rest); rest = trim(rest);
                } else { rest = sym_str; std::string more; std::getline(ss, more); rest += more; rest = trim(rest); }
            }
            std::string info = aprs::make_pos(lat, lon, sym, rest);
            router_.send_aprs(info);
            ses.println("APRS position sent: " + info);
            std::cerr << "[" << timestamp() << "] APRS-POS " << call << ": " << info << "\n";

        } else if (cmd == "AMSG") {
            std::string dest_call, text;
            ss >> dest_call;
            std::getline(ss, text); text = trim(text);
            if (dest_call.empty() || text.empty()) { ses.println("Usage: AMSG <CALL> <message>"); return; }
            std::string info = aprs::make_msg(dest_call, text);
            router_.send_aprs(info);
            ses.println("APRS MSG sent -> " + dest_call + ": " + text);
            std::cerr << "[" << timestamp() << "] APRS-MSG " << call << " -> " << dest_call << ": " << text << "\n";

        } else if (cmd == "I" || cmd == "INFO") {
            ses.println("BBS: " + name_);
            ses.println("Call: " + router_.config().mycall.str());
            ses.println("Sessions: " + std::to_string(sessions_.size()));
            ses.println("MTU: " + std::to_string(router_.config().mtu) +
                        "  Window: " + std::to_string(router_.config().window));

        } else if (cmd == "B" || cmd == "BEACON") {
            if (!beacon_text_.empty()) {
                router_.send_aprs(beacon_text_);
                ses.println("Beacon sent: " + beacon_text_);
            } else {
                ses.println("No beacon configured.");
            }

        } else if (cmd == "SH" || cmd == "SHELL") {
            ses.println("Starting shell -- type ~. on a new line to exit.");
            if (!ses.start_shell()) ses.println("[ERROR] Could not start shell.");

        } else if (cmd == "BYE" || cmd == "QUIT" || cmd == "Q") {
            ses.println("73! Goodbye.");
            ses.conn->disconnect();

        } else {
            ses.println("Unknown: " + cmd + "  (type H for help)");
        }
    }

    // ── Poll PTY output ───────────────────────────────────────────────────
    void poll_shells() {
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            Session& ses = *it->second;
            if (!ses.shell_mode || ses.pty_master < 0) continue;
            char buf[256];
            ssize_t n;
            while ((n = ::read(ses.pty_master, buf, sizeof(buf))) > 0)
                if (ses.conn && ses.conn->connected())
                    ses.conn->send((const uint8_t*)buf, (std::size_t)n);
            int st;
            if (ses.shell_pid > 0 && waitpid(ses.shell_pid, &st, WNOHANG) == ses.shell_pid) {
                ses.shell_pid = -1; ses.stop_shell(); ses.println("\r\n[Shell exited]");
            }
        }
    }

    // ── Cleanup ───────────────────────────────────────────────────────────
    void reap_dead() {
        for (std::size_t i = 0; i < dead_.size(); ++i) sessions_.erase(dead_[i]);
        dead_.clear();
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if (it->second->conn->state() == Connection::State::DISCONNECTED)
                it = sessions_.erase(it);
            else ++it;
        }
    }

    // ── Beacon ────────────────────────────────────────────────────────────
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
BBS options:
  -n <name>               BBS name (default: AX25BBS)
  -u <text>               APRS beacon info string
  -B <secs>               APRS beacon interval (0=off, default 0)
  -C <bbs.ini>            Load config from INI file

One-shot modes:
  --ui   <DEST> <text>    Send one UI frame and exit
  --aprs <text>           Send one APRS frame and exit

Examples:
  bbs -c PY2XXX-1 -b 9600 /dev/ttyUSB0
  bbs -C bbs.ini
)";

int main(int argc, char* argv[]) {
    std::string bbs_name = "AX25BBS";
    std::string beacon_text;
    int         beacon_int    = 0;
    std::string welcome_script;
    std::string db_path       = "bbs.db";
    std::string ini_file;
    std::string ui_dest, ui_text, aprs_text;
    bool one_shot_ui = false, one_shot_aprs = false;

    // Pre-scan for BBS-specific long options
    std::vector<char*> remaining;
    remaining.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ui" && i+2 < argc)  { one_shot_ui=true; ui_dest=argv[++i]; ui_text=argv[++i]; }
        else if (a == "--aprs" && i+1 < argc) { one_shot_aprs=true; aprs_text=argv[++i]; }
        else if (a == "-n" && i+1 < argc) bbs_name = argv[++i];
        else if (a == "-u" && i+1 < argc) beacon_text = argv[++i];
        else if (a == "-B" && i+1 < argc) beacon_int = std::atoi(argv[++i]);
        else if (a == "-C" && i+1 < argc) ini_file = argv[++i];
        else remaining.push_back(argv[i]);
    }

    // Load INI config (values overridden by command-line flags below)
    IniConfig cfg_file;
    if (!ini_file.empty()) cfg_file.load(ini_file);
    else cfg_file.load("bbs.ini"); // try default

    if (bbs_name    == "AX25BBS"  && cfg_file.has("bbs","name"))            bbs_name    = cfg_file.get("bbs","name");
    if (beacon_text.empty()       && cfg_file.has("bbs","beacon"))          beacon_text = cfg_file.get("bbs","beacon");
    if (beacon_int  == 0          && cfg_file.has("bbs","beacon_interval")) beacon_int  = cfg_file.get_int("bbs","beacon_interval");
    if (welcome_script.empty()    && cfg_file.has("bbs","welcome_script"))  welcome_script = cfg_file.get("bbs","welcome_script");
    if (db_path     == "bbs.db"   && cfg_file.has("basic","database"))      db_path     = cfg_file.get("basic","database");

    // Parse standard ax25lib flags (device, baud, callsign, etc.)
    CLIParams p;
    // Pre-fill from INI if not provided via command-line
    if (remaining.size() == 1 && cfg_file.has("kiss","device")) {
        // Only INI config -- synthesise argv
        std::string dev  = cfg_file.get("kiss","device");
        std::string call = cfg_file.get("ax25","callsign","N0CALL");
        std::string baud = cfg_file.get("kiss","baud","9600");
        // Build minimal argv for CLIParams
        std::vector<std::string> args_s;
        args_s.push_back(std::string(argv[0]));
        args_s.push_back("-c"); args_s.push_back(call);
        args_s.push_back("-b"); args_s.push_back(baud);
        if (cfg_file.has("ax25","mtu"))     { args_s.push_back("-m"); args_s.push_back(cfg_file.get("ax25","mtu")); }
        if (cfg_file.has("ax25","window"))  { args_s.push_back("-w"); args_s.push_back(cfg_file.get("ax25","window")); }
        if (cfg_file.has("ax25","t1_ms"))   { args_s.push_back("-t"); args_s.push_back(cfg_file.get("ax25","t1_ms")); }
        if (cfg_file.has("ax25","t3_ms"))   { args_s.push_back("-k"); args_s.push_back(cfg_file.get("ax25","t3_ms")); }
        args_s.push_back(dev);
        std::vector<char*> av;
        for (std::size_t i = 0; i < args_s.size(); ++i) av.push_back(const_cast<char*>(args_s[i].c_str()));
        int ac = (int)av.size();
        if (!p.parse(ac, av.data(), EXTRA_USAGE)) return 1;
    } else {
        int rargc = (int)remaining.size();
        if (!p.parse(rargc, remaining.data(), EXTRA_USAGE)) return 1;
    }

    Kiss kiss;
    if (!kiss.open(p.device, p.baud)) {
        std::cerr << "Cannot open serial: " << p.device << " -- " << strerror(errno) << "\n";
        return 1;
    }
    kiss.set_txdelay(p.cfg.txdelay * 10);
    kiss.set_persistence(p.cfg.persist);
    Router router(kiss, p.cfg);

    if (one_shot_ui)   { router.send_ui(Addr::make(ui_dest), 0xF0, ui_text);  return 0; }
    if (one_shot_aprs) { router.send_aprs(aprs_text);                          return 0; }

    std::string digi_str;
    for (std::size_t i = 0; i < p.cfg.digis.size(); ++i) {
        if (!digi_str.empty()) digi_str += ",";
        digi_str += p.cfg.digis[i].str();
    }
    std::cerr
        << "====================================\n"
        << " " << bbs_name << " -- AX.25 BBS\n"
        << " Callsign   : " << p.cfg.mycall.str() << "\n"
        << " Device     : " << p.device << " @ " << p.baud << " baud\n"
        << " Digipeaters: " << (digi_str.empty() ? "(none)" : digi_str) << "\n"
        << " MTU/Window : " << p.cfg.mtu << "/" << p.cfg.window << "\n"
        << " Beacon     : " << (beacon_int>0 ? std::to_string(beacon_int)+"s" : "off") << "\n"
        << " Script     : " << (welcome_script.empty() ? "(none)" : welcome_script) << "\n"
        << "====================================\n";

    BBS bbs(bbs_name, router, beacon_text, beacon_int, welcome_script, db_path);
    g_bbs = &bbs;
    signal(SIGINT, sig_handler); signal(SIGTERM, sig_handler); signal(SIGCHLD, SIG_DFL);
    bbs.run();
    std::cerr << "BBS shutdown.\n";
    return 0;
}
