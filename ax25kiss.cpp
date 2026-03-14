// =============================================================================
// ax25kiss.cpp  —  AX.25 / KISS Terminal
//
// Implements the KISS TNC protocol over a serial port and decodes/encodes
// AX.25 frames.  No third-party libraries needed; only standard C++11 and
// POSIX (works on Linux and macOS).
//
// Compile:
//   g++ -std=c++11 -O2 -pthread -o ax25kiss ax25kiss.cpp
//
// Usage:
//   ./ax25kiss [OPTIONS] <serial_device>
//
// Options:
//   -b <baud>    Baud rate (default: 9600)
//   -c <call>    My callsign, e.g. PY2XXX-1 (default: N0CALL)
//   -d <dest>    Destination callsign (default: CQ)
//   -p <path>    Digipeater path, comma-separated (e.g. WIDE1-1,WIDE2-1)
//
// In-terminal commands (prefix with /):
//   /mycall <CALL[-SSID]>       Change my callsign
//   /dest   <CALL[-SSID]>       Change destination
//   /digi   <CALL>,<CALL>,...   Set digipeater path (empty = none)
//   /txdelay <ms>               Set TNC TX delay (0-2550 ms)
//   /persist <0-255>            Set persistence byte
//   /status                     Show current settings
//   /help                       Show this help
//   /quit  or  /exit            Quit
//   <anything else>             Sent as UI frame (AX.25)
// =============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// POSIX
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "ax25dump.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// ANSI colour helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace Colour {
    static bool enabled = true;
    static const char* reset()  { return enabled ? "\033[0m"  : ""; }
    static const char* red()    { return enabled ? "\033[31m" : ""; }
    static const char* green()  { return enabled ? "\033[32m" : ""; }
    static const char* yellow() { return enabled ? "\033[33m" : ""; }
    static const char* cyan()   { return enabled ? "\033[36m" : ""; }
    static const char* bold()   { return enabled ? "\033[1m"  : ""; }
    static const char* dim()    { return enabled ? "\033[2m"  : ""; }
}

// ─────────────────────────────────────────────────────────────────────────────
// KISS protocol constants & codec
// ─────────────────────────────────────────────────────────────────────────────
namespace KISS {

constexpr uint8_t FEND  = 0xC0;
constexpr uint8_t FESC  = 0xDB;
constexpr uint8_t TFEND = 0xDC;
constexpr uint8_t TFESC = 0xDD;

enum class Cmd : uint8_t {
    DataFrame   = 0x00,
    TxDelay     = 0x01,
    Persistence = 0x02,
    SlotTime    = 0x03,
    TxTail      = 0x04,
    FullDuplex  = 0x05,
    SetHardware = 0x06,
    Return      = 0xFF
};

struct Frame {
    Cmd     command = Cmd::DataFrame;
    int     port    = 0;
    std::vector<uint8_t> data;
};

// Encode: wrap AX.25 data in a KISS frame (byte-stuffed)
inline std::vector<uint8_t> encode(const std::vector<uint8_t>& payload,
                                   Cmd cmd = Cmd::DataFrame,
                                   int port = 0)
{
    std::vector<uint8_t> out;
    out.reserve(payload.size() + 4);

    out.push_back(FEND);
    out.push_back(static_cast<uint8_t>(((port & 0x0F) << 4) | (uint8_t(cmd) & 0x0F)));

    for (uint8_t b : payload) {
        if (b == FEND) { out.push_back(FESC); out.push_back(TFEND); }
        else if (b == FESC) { out.push_back(FESC); out.push_back(TFESC); }
        else { out.push_back(b); }
    }

    out.push_back(FEND);
    return out;
}

// Stateful decoder: feed bytes, get zero or more complete frames back
class Decoder {
public:
    std::vector<Frame> feed(const uint8_t* buf, std::size_t len) {
        std::vector<Frame> frames;
        for (std::size_t i = 0; i < len; ++i) {
            uint8_t b = buf[i];

            if (b == FEND) {
                if (!in_frame_) {
                    in_frame_ = true;
                    buf_.clear();
                    escaped_ = false;
                } else if (!buf_.empty()) {
                    Frame f;
                    f.port    = (buf_[0] >> 4) & 0x0F;
                    f.command = Cmd(buf_[0] & 0x0F);
                    f.data    = std::vector<uint8_t>(buf_.begin() + 1, buf_.end());
                    frames.push_back(std::move(f));
                    in_frame_ = false;
                    buf_.clear();
                    escaped_ = false;
                }
            } else if (in_frame_) {
                if (escaped_) {
                    if (b == TFEND) buf_.push_back(FEND);
                    else if (b == TFESC) buf_.push_back(FESC);
                    escaped_ = false;
                } else if (b == FESC) {
                    escaped_ = true;
                } else {
                    buf_.push_back(b);
                }
            }
        }
        return frames;
    }

private:
    bool in_frame_ = false;
    bool escaped_  = false;
    std::vector<uint8_t> buf_;
};

} // namespace KISS

// ─────────────────────────────────────────────────────────────────────────────
// AX.25 address
// ─────────────────────────────────────────────────────────────────────────────
struct Addr {
    std::string callsign;
    int  ssid            = 0;
    bool has_been_repeated = false;
    bool is_last_addr    = false;  // address-list end bit (H bit in last addr)

    // Decode from 7 raw AX.25 bytes
    static Addr decode(const uint8_t* p) {
        Addr a;
        char cs[7]{};
        for (int i = 0; i < 6; ++i) cs[i] = (p[i] >> 1) & 0x7F;
        a.callsign = cs;
        while (!a.callsign.empty() && a.callsign.back() == ' ')
            a.callsign.pop_back();
        uint8_t ssid_byte       = p[6];
        a.ssid                  = (ssid_byte >> 1) & 0x0F;
        a.has_been_repeated     = (ssid_byte & 0x80) != 0;
        a.is_last_addr          = (ssid_byte & 0x01) != 0;
        return a;
    }

    // Encode to 7 bytes
    std::vector<uint8_t> encode(bool last) const {
        std::vector<uint8_t> out(7, (' ' << 1));
        for (int i = 0; i < 6 && i < (int)callsign.size(); ++i)
            out[i] = (callsign[i] & 0x7F) << 1;
        uint8_t sb = 0x60;  // reserved bits set per spec
        sb |= (ssid & 0x0F) << 1;
        if (has_been_repeated) sb |= 0x80;
        if (last) sb |= 0x01;
        out[6] = sb;
        return out;
    }

    std::string str() const {
        std::string s = callsign;
        if (ssid > 0) s += '-' + std::to_string(ssid);
        if (has_been_repeated) s += '*';
        return s;
    }

    static Addr from_str(std::string s) {
        // uppercase
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        Addr a;
        auto dash = s.find('-');
        if (dash != std::string::npos) {
            a.callsign = s.substr(0, dash);
            try { a.ssid = std::stoi(s.substr(dash + 1)); }
            catch (...) { a.ssid = 0; }
        } else {
            a.callsign = s;
        }
        return a;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// AX.25 frame
// ─────────────────────────────────────────────────────────────────────────────
namespace AX25 {

// Well-known control bytes (P/F bit cleared for matching)
constexpr uint8_t CTRL_UI   = 0x03;
constexpr uint8_t CTRL_SABM = 0x2F;
constexpr uint8_t CTRL_DISC = 0x43;
constexpr uint8_t CTRL_UA   = 0x63;
constexpr uint8_t CTRL_DM   = 0x0F;
constexpr uint8_t CTRL_FRMR = 0x87;
constexpr uint8_t CTRL_PF   = 0x10;  // Poll/Final bit mask

// PID values
constexpr uint8_t PID_NO_L3 = 0xF0;
constexpr uint8_t PID_NETROM= 0xCF;
constexpr uint8_t PID_IP    = 0xCC;
constexpr uint8_t PID_ARP   = 0xCD;

enum class FrameType { Unknown, IFrame, SFrame, UI, SABM, DISC, UA, DM, FRMR };

struct Frame {
    Addr dest;
    Addr src;
    std::vector<Addr> digis;
    uint8_t ctrl  = CTRL_UI;
    uint8_t pid   = PID_NO_L3;
    bool has_pid  = true;
    std::vector<uint8_t> info;

    FrameType type() const {
        if ((ctrl & 0x01) == 0x00) return FrameType::IFrame;
        if ((ctrl & 0x03) == 0x01) return FrameType::SFrame;
        // U-frame: mask P/F bit for matching
        switch (ctrl & ~CTRL_PF) {
            case CTRL_UI:   return FrameType::UI;
            case CTRL_SABM: return FrameType::SABM;
            case CTRL_DISC: return FrameType::DISC;
            case CTRL_UA:   return FrameType::UA;
            case CTRL_DM:   return FrameType::DM;
            case CTRL_FRMR: return FrameType::FRMR;
            default:        return FrameType::Unknown;
        }
    }

    const char* type_str() const {
        switch (type()) {
            case FrameType::IFrame: return "I";
            case FrameType::SFrame: return "S";
            case FrameType::UI:     return "UI";
            case FrameType::SABM:   return "SABM";
            case FrameType::DISC:   return "DISC";
            case FrameType::UA:     return "UA";
            case FrameType::DM:     return "DM";
            case FrameType::FRMR:   return "FRMR";
            default:                return "?";
        }
    }

    std::string pid_str() const {
        switch (pid) {
            case PID_NO_L3:  return "NL3";
            case PID_NETROM: return "NETROM";
            case PID_IP:     return "IP";
            case PID_ARP:    return "ARP";
            default: {
                std::ostringstream os;
                os << "0x" << std::hex << std::uppercase << std::setw(2)
                   << std::setfill('0') << (int)pid;
                return os.str();
            }
        }
    }

    // Decode from raw KISS payload (no flags, no FCS — TNC strips those).
    // Returns true and fills 'f' on success; returns false if malformed.
    static bool decode(const std::vector<uint8_t>& d, Frame& f) {
        if (d.size() < 14) return false;

        std::size_t pos = 0;

        // Destination (7 bytes)
        f.dest = Addr::decode(&d[pos]); pos += 7;

        // Source (7 bytes)
        f.src  = Addr::decode(&d[pos]); pos += 7;
        bool last = f.src.is_last_addr;

        // Digipeaters
        while (!last && pos + 7 <= d.size()) {
            Addr digi = Addr::decode(&d[pos]); pos += 7;
            last = digi.is_last_addr;
            f.digis.push_back(digi);
        }

        if (pos >= d.size()) return false;

        f.ctrl = d[pos++];
        FrameType t = f.type();

        // PID present only for I-frames and UI frames
        if (t == FrameType::IFrame || t == FrameType::UI) {
            if (pos >= d.size()) return false;
            f.pid     = d[pos++];
            f.has_pid = true;
        } else {
            f.has_pid = false;
        }

        f.info = std::vector<uint8_t>(d.begin() + pos, d.end());
        return true;
    }

    // Encode to raw AX.25 bytes (no flags, no FCS)
    std::vector<uint8_t> encode() const {
        std::vector<uint8_t> out;

        auto append = [&](const std::vector<uint8_t>& v) {
            out.insert(out.end(), v.begin(), v.end());
        };

        append(dest.encode(false));
        append(src.encode(digis.empty()));
        for (std::size_t i = 0; i < digis.size(); ++i)
            append(digis[i].encode(i == digis.size() - 1));

        out.push_back(ctrl);
        if (has_pid) out.push_back(pid);
        out.insert(out.end(), info.begin(), info.end());
        return out;
    }

    // Pretty-print for the terminal
    std::string format(bool colour = true) const {
        std::ostringstream os;

        if (colour) os << Colour::bold();
        os << src.str() << '>' << dest.str();
        for (const auto& d : digis) os << ',' << d.str();
        if (colour) os << Colour::reset();

        os << " [" << type_str() << "]";

        auto t = type();
        if (t == FrameType::UI || t == FrameType::IFrame) {
            os << " <" << pid_str() << "> ";

            // Show info: text if printable, else hex
            bool printable = !info.empty();
            for (uint8_t b : info)
                if (b < 0x20 && b != '\r' && b != '\n' && b != '\t')
                    { printable = false; break; }

            if (printable) {
                if (colour) os << Colour::green();
                std::string txt(info.begin(), info.end());
                // strip trailing CR/LF for display
                while (!txt.empty() && (txt.back() == '\r' || txt.back() == '\n'))
                    txt.pop_back();
                os << txt;
                if (colour) os << Colour::reset();
            } else {
                if (colour) os << Colour::cyan();
                os << "[HEX:";
                for (uint8_t b : info)
                    os << ' ' << std::hex << std::uppercase
                       << std::setw(2) << std::setfill('0') << (int)b;
                os << std::dec << ']';
                if (colour) os << Colour::reset();
            }
        }
        return os.str();
    }
};

} // namespace AX25

// ─────────────────────────────────────────────────────────────────────────────
// Serial port (POSIX termios)
// ─────────────────────────────────────────────────────────────────────────────
class SerialPort {
public:
    ~SerialPort() { close(); }

    bool open(const std::string& dev, int baud) {
        fd_ = ::open(dev.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) return false;

        struct termios tty{};
        if (tcgetattr(fd_, &tty) < 0) { ::close(fd_); fd_ = -1; return false; }
        orig_ = tty;

        speed_t sp = baud_to_speed(baud);
        cfsetospeed(&tty, sp);
        cfsetispeed(&tty, sp);
        cfmakeraw(&tty);

        // 8N1, enable receiver, ignore modem lines
        tty.c_cflag &= (tcflag_t)~(PARENB | CSTOPB | CSIZE);
        tty.c_cflag |= CS8 | CREAD | CLOCAL;
        tty.c_cc[VMIN]  = 0;
        tty.c_cc[VTIME] = 1;  // 100 ms read timeout

        if (tcsetattr(fd_, TCSANOW, &tty) < 0) { ::close(fd_); fd_ = -1; return false; }
        return true;
    }

    void close() {
        if (fd_ >= 0) {
            tcsetattr(fd_, TCSANOW, &orig_);
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool     is_open() const { return fd_ >= 0; }
    int      fd()      const { return fd_; }

    ssize_t write(const std::vector<uint8_t>& v) {
        return ::write(fd_, v.data(), v.size());
    }
    ssize_t read(uint8_t* buf, std::size_t len) {
        return ::read(fd_, buf, len);
    }

private:
    int fd_ = -1;
    struct termios orig_{};

    static speed_t baud_to_speed(int b) {
        switch (b) {
            case 1200:   return B1200;
            case 2400:   return B2400;
            case 4800:   return B4800;
            case 9600:   return B9600;
            case 19200:  return B19200;
            case 38400:  return B38400;
            case 57600:  return B57600;
            case 115200: return B115200;
#ifdef B230400
            case 230400: return B230400;
#endif
#ifdef B460800
            case 460800: return B460800;
#endif
            default:     return B9600;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Global quit flag (set by signal handler)
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_quit{false};

static void sig_handler(int) { g_quit = true; }

// ─────────────────────────────────────────────────────────────────────────────
// Terminal application
// ─────────────────────────────────────────────────────────────────────────────
class Terminal {
public:
    explicit Terminal(const std::string& device, int baud)
        : device_(device), baud_(baud) {}

    void set_mycall(const std::string& s)  { mycall_ = Addr::from_str(s); }
    void set_dest(const std::string& s)    { dest_   = Addr::from_str(s); }
    void set_digis(const std::string& csv) { parse_digi_path(csv); }

    int run() {
        if (!serial_.open(device_, baud_)) {
            std::cerr << Colour::red() << "Cannot open " << device_
                      << ": " << strerror(errno) << Colour::reset() << '\n';
            return 1;
        }

        Colour::enabled = isatty(STDOUT_FILENO);

        print_banner();
        print_help();

        running_ = true;
        std::thread rx_thread(&Terminal::rx_loop, this);

        input_loop();

        running_ = false;
        if (rx_thread.joinable()) rx_thread.join();
        return 0;
    }

private:
    // ── state ──────────────────────────────────────────────────────────────
    std::string device_;
    int baud_;

    SerialPort    serial_;
    KISS::Decoder kiss_dec_;

    std::atomic<bool> running_{false};
    std::mutex        print_mtx_;

    Addr mycall_ = Addr::from_str("N0CALL");
    Addr dest_   = Addr::from_str("CQ");
    std::vector<Addr> digis_;

    // ── helpers ────────────────────────────────────────────────────────────
    static std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm{};
        localtime_r(&t, &tm);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        return buf;
    }

    void println(const std::string& s) {
        std::lock_guard<std::mutex> lk(print_mtx_);
        std::cout << s << '\n';
    }

    void reprint_prompt() {
        // Called while print_mtx_ is already held by caller
        std::cout << Colour::bold() << "\n> " << Colour::reset() << std::flush;
    }

    // ── RX thread ──────────────────────────────────────────────────────────
    void rx_loop() {
        uint8_t buf[512];
        while (running_ && !g_quit) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(serial_.fd(), &rfds);

            struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 100000;  // 100 ms
            int rc = select(serial_.fd() + 1, &rfds, nullptr, nullptr, &tv);

            if (rc > 0 && FD_ISSET(serial_.fd(), &rfds)) {
                ssize_t n = serial_.read(buf, sizeof(buf));
                if (n > 0) {
                    auto frames = kiss_dec_.feed(buf, (std::size_t)n);
                    for (const auto& kf : frames)
                        handle_rx_frame(kf);
                }
            }
        }
    }

    void handle_rx_frame(const KISS::Frame& kf) {
        std::lock_guard<std::mutex> lk(print_mtx_);
        std::cout << '\n';

        if (kf.command == KISS::Cmd::DataFrame) {
            AX25::Frame ax;
            if (AX25::Frame::decode(kf.data, ax)) {
                std::cout << Colour::cyan() << '[' << timestamp() << "] RX "
                          << Colour::reset() << ax.format() << '\n';
                std::cout << Colour::dim()
                          << "           " << ctrl_detail(ax.ctrl, kf.data.size()) << '\n'
                          << hex_dump(kf.data.data(), kf.data.size(), "           ")
                          << Colour::reset();
            } else {
                std::cout << Colour::red()
                          << '[' << timestamp() << "] RX MALFORMED ("
                          << kf.data.size() << " bytes)" << Colour::reset() << '\n';
                std::cout << Colour::dim()
                          << hex_dump(kf.data.data(), kf.data.size(), "           ")
                          << Colour::reset();
            }
        } else {
            // Non-data KISS command received from TNC
            std::cout << Colour::yellow()
                      << '[' << timestamp() << "] KISS cmd=0x"
                      << std::hex << std::uppercase
                      << (int)uint8_t(kf.command)
                      << " port=" << kf.port
                      << std::dec << Colour::reset() << '\n';
        }

        reprint_prompt();
    }

    // ── input loop (runs in main thread) ───────────────────────────────────
    void input_loop() {
        std::string line;
        {
            std::lock_guard<std::mutex> lk(print_mtx_);
            std::cout << Colour::bold() << "> " << Colour::reset() << std::flush;
        }

        while (!g_quit) {
            // Poll stdin so we can also watch g_quit
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;
            int rc = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);

            if (rc > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
                if (!std::getline(std::cin, line)) break;  // EOF

                process_line(line);

                if (!running_) break;

                std::lock_guard<std::mutex> lk(print_mtx_);
                std::cout << Colour::bold() << "> " << Colour::reset() << std::flush;
            }
        }
        running_ = false;
    }

    // ── command dispatch ───────────────────────────────────────────────────
    void process_line(const std::string& raw) {
        if (raw.empty()) return;

        if (raw[0] == '/') {
            auto sp = raw.find(' ');
            std::string cmd  = raw.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
            std::string args = sp == std::string::npos ? "" : raw.substr(sp + 1);
            for (auto& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            dispatch_command(cmd, args);
        } else {
            cmd_send(raw);
        }
    }

    void dispatch_command(const std::string& cmd, const std::string& args) {
        if (cmd == "quit" || cmd == "exit") {
            println("Goodbye!");
            running_ = false;
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "status") {
            print_status();
        } else if (cmd == "mycall") {
            if (args.empty()) { println("Usage: /mycall <CALL[-SSID]>"); return; }
            mycall_ = Addr::from_str(args);
            println("My callsign → " + mycall_.str());
        } else if (cmd == "dest") {
            if (args.empty()) { println("Usage: /dest <CALL[-SSID]>"); return; }
            dest_ = Addr::from_str(args);
            println("Destination → " + dest_.str());
        } else if (cmd == "digi") {
            parse_digi_path(args);
            if (digis_.empty()) {
                println("Digipeater path cleared.");
            } else {
                std::string p;
                for (std::size_t i = 0; i < digis_.size(); ++i) {
                    if (i) p += ',';
                    p += digis_[i].str();
                }
                println("Digipeater path → " + p);
            }
        } else if (cmd == "txdelay") {
            cmd_txdelay(args);
        } else if (cmd == "persist") {
            cmd_persist(args);
        } else if (cmd == "send") {
            cmd_send(args);
        } else {
            println(std::string(Colour::red()) + "Unknown command: /" + cmd +
                    ".  Try /help" + Colour::reset());
        }
    }

    // ── commands ───────────────────────────────────────────────────────────
    void cmd_send(const std::string& text) {
        AX25::Frame f;
        f.dest    = dest_;
        f.src     = mycall_;
        f.digis   = digis_;
        f.ctrl    = AX25::CTRL_UI;
        f.pid     = AX25::PID_NO_L3;
        f.has_pid = true;
        f.info    = std::vector<uint8_t>(text.begin(), text.end());

        auto ax25_bytes = f.encode();
        auto kiss_bytes = KISS::encode(ax25_bytes);

        ssize_t wr = serial_.write(kiss_bytes);

        std::lock_guard<std::mutex> lk(print_mtx_);
        if (wr < 0) {
            std::cout << Colour::red()
                      << '[' << timestamp() << "] TX ERROR: " << strerror(errno)
                      << Colour::reset() << '\n';
        } else {
            std::cout << Colour::yellow()
                      << '[' << timestamp() << "] TX "
                      << Colour::reset() << f.format() << '\n';
            std::cout << Colour::dim()
                      << "           " << ctrl_detail(f.ctrl, ax25_bytes.size()) << '\n'
                      << hex_dump(ax25_bytes.data(), ax25_bytes.size(), "           ")
                      << Colour::reset();
        }
    }

    void cmd_txdelay(const std::string& args) {
        try {
            int ms = std::stoi(args);
            if (ms < 0 || ms > 2550) { println("Value must be 0–2550 ms"); return; }
            std::vector<uint8_t> d = {static_cast<uint8_t>(ms / 10)};
            serial_.write(KISS::encode(d, KISS::Cmd::TxDelay));
            println("TX delay set to " + std::to_string(ms) + " ms");
        } catch (...) {
            println("Usage: /txdelay <ms>  (0–2550)");
        }
    }

    void cmd_persist(const std::string& args) {
        try {
            int v = std::stoi(args);
            if (v < 0 || v > 255) { println("Value must be 0–255"); return; }
            std::vector<uint8_t> d = {static_cast<uint8_t>(v)};
            serial_.write(KISS::encode(d, KISS::Cmd::Persistence));
            println("Persistence set to " + std::to_string(v));
        } catch (...) {
            println("Usage: /persist <0-255>");
        }
    }

    // ── utilities ──────────────────────────────────────────────────────────
    void parse_digi_path(const std::string& csv) {
        digis_.clear();
        if (csv.empty()) return;
        std::istringstream ss(csv);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            // trim
            auto b = tok.find_first_not_of(" \t");
            auto e = tok.find_last_not_of(" \t");
            if (b != std::string::npos)
                digis_.push_back(Addr::from_str(tok.substr(b, e - b + 1)));
        }
    }

    // ── help / banner / status ─────────────────────────────────────────────
    void print_banner() {
        std::lock_guard<std::mutex> lk(print_mtx_);
        std::cout
            << Colour::bold() << Colour::cyan()
            << "\n╔══════════════════════════════════════════╗\n"
            <<   "║        AX.25 / KISS  Terminal  v1.0     ║\n"
            <<   "╚══════════════════════════════════════════╝\n"
            << Colour::reset()
            << "  Device : " << device_ << "  @" << baud_ << " baud\n"
            << "  Callsign: " << mycall_.str() << "\n\n";
    }

    void print_help() {
        std::lock_guard<std::mutex> lk(print_mtx_);
        std::cout <<
            Colour::bold() << "Commands:\n" << Colour::reset() <<
            "  /mycall <CALL[-SSID]>      Set my callsign\n"
            "  /dest   <CALL[-SSID]>      Set destination callsign\n"
            "  /digi   <CALL,...>          Set digipeater path (empty = none)\n"
            "  /txdelay <ms>              Set TNC TX delay (0-2550 ms)\n"
            "  /persist <0-255>           Set persistence parameter\n"
            "  /status                    Show current settings\n"
            "  /help                      Show this help\n"
            "  /quit | /exit              Exit terminal\n"
            "  <text>                     Send as UI frame\n"
            "\n";
    }

    void print_status() {
        std::lock_guard<std::mutex> lk(print_mtx_);
        std::string digi_str = "(none)";
        if (!digis_.empty()) {
            digi_str.clear();
            for (std::size_t i = 0; i < digis_.size(); ++i) {
                if (i) digi_str += ',';
                digi_str += digis_[i].str();
            }
        }
        std::cout
            << Colour::bold() << "\n── Status ──────────────────────────\n" << Colour::reset()
            << "  My callsign : " << mycall_.str() << '\n'
            << "  Destination : " << dest_.str()   << '\n'
            << "  Digi path   : " << digi_str       << '\n'
            << "  Serial port : " << device_ << " @ " << baud_ << " baud  ["
            << (serial_.is_open() ? "OPEN" : "CLOSED") << "]\n"
            << Colour::bold() << "────────────────────────────────────\n" << Colour::reset()
            << '\n';
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
static void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [OPTIONS] <serial_device>\n\n"
        << "Options:\n"
        << "  -b <baud>    Baud rate        (default: 9600)\n"
        << "  -c <call>    My callsign      (default: N0CALL)\n"
        << "  -d <dest>    Destination call (default: CQ)\n"
        << "  -p <path>    Digi path, comma-separated (e.g. WIDE1-1,WIDE2-1)\n"
        << "  -h           Show this help\n\n"
        << "Examples:\n"
        << "  " << prog << " -c PY2XXX-9 -b 9600 /dev/ttyUSB0\n"
        << "  " << prog << " -c W1AW -d APRS -p WIDE1-1 /dev/tty.usbserial-1\n";
}

int main(int argc, char* argv[]) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    std::string device, mycall = "N0CALL", dest = "CQ", digi_path;
    int baud = 9600;

    int opt;
    while ((opt = getopt(argc, argv, "b:c:d:p:h")) != -1) {
        switch (opt) {
            case 'b': baud      = std::atoi(optarg); break;
            case 'c': mycall    = optarg; break;
            case 'd': dest      = optarg; break;
            case 'p': digi_path = optarg; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        std::cerr << "Error: serial device required.\n";
        usage(argv[0]);
        return 1;
    }
    device = argv[optind];

    Terminal term(device, baud);
    term.set_mycall(mycall);
    term.set_dest(dest);
    if (!digi_path.empty()) term.set_digis(digi_path);

    return term.run();
}
