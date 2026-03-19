// bt_sniffer.cpp — KISS proxy tap for BLE/BT bridge debugging  (C++11, POSIX)
//
// Sits transparently between bt_kiss_bridge and ax25tnc.
// Forwards all traffic in both directions while decoding and displaying
// every KISS frame + AX.25 header in real-time.
//
// Does NOT connect to Bluetooth directly — it taps the PTY or TCP side
// that bt_kiss_bridge exposes, so both tools can run simultaneously.
//
// Build:
//   make bt_sniffer
//
// Usage:
//   bt_sniffer <upstream> [--link <path>]
//
//   <upstream>  PTY/serial path (e.g. /tmp/kiss) or host:port (TCP)
//   --link      Downstream PTY symlink  (default: /tmp/kiss-tap)
//
// Typical 3-terminal workflow:
//
//   Terminal 1 (bridge):
//     bt_kiss_bridge --ble --device "VR-N76" --link /tmp/kiss
//
//   Terminal 2 (sniffer):
//     bt_sniffer /tmp/kiss --link /tmp/kiss-tap
//
//   Terminal 3 (client):
//     ax25tnc -c N0CALL /tmp/kiss-tap
//
//   Or with TCP upstream:
//     bt_kiss_bridge --ble --device "VR-N76" --server-port 8001
//     bt_sniffer localhost:8001 --link /tmp/kiss-tap
//     ax25tnc -c N0CALL /tmp/kiss-tap
//
// Output format:
//   [HH:MM:SS.mmm]  ← BRIDGE   23 bytes        (from bridge to client)
//       00000000  c0 00 82 a4 64 98 ...  |....d..|
//     └─ [KISS←#1 port=0]  AX.25: N0CALL -> CQ  [UI]
//            ctrl=0x03  UI P/F=0  (21 bytes)
//            ...hexdump-C...
//
//   [HH:MM:SS.mmm]  → BRIDGE    8 bytes        (from client to bridge)
//       00000000  c0 00 c0  ...
//     └─ [KISS→#2 port=0]  ctrl: TXDELAY val=40 (0x28)
// =============================================================================

#include "ax25dump.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#ifdef __APPLE__
#  include <util.h>
#else
#  include <pty.h>
#endif
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────────────────────
// Signal handling
// ─────────────────────────────────────────────────────────────────────────────
static volatile int g_quit = 0;
static void sigint_handler(int) { g_quit = 1; }

// ─────────────────────────────────────────────────────────────────────────────
// Timestamp  HH:MM:SS.mmm
// ─────────────────────────────────────────────────────────────────────────────
static std::string ts() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto tt  = system_clock::to_time_t(now);
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    struct tm tm_info{};
    localtime_r(&tt, &tm_info);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, (int)ms.count());
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// KISS decoder
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint8_t FEND  = 0xC0;
static constexpr uint8_t FESC  = 0xDB;
static constexpr uint8_t TFEND = 0xDC;
static constexpr uint8_t TFESC = 0xDD;

struct KissFrame { int port, type; std::vector<uint8_t> payload; };

class KissDecoder {
    std::vector<uint8_t> buf_;
    bool in_ = false, esc_ = false;
public:
    std::vector<KissFrame> feed(const uint8_t* data, size_t len) {
        std::vector<KissFrame> frames;
        for (size_t i = 0; i < len; i++) {
            uint8_t b = data[i];
            if (b == FEND) {
                if (in_ && buf_.size() > 1) {
                    int cmd = buf_[0];
                    frames.push_back({(cmd >> 4) & 0xF, cmd & 0xF,
                                      {buf_.begin() + 1, buf_.end()}});
                }
                buf_.clear(); in_ = true; esc_ = false;
            } else if (!in_) {
                // outside frame — ignore (TNC command text etc.)
            } else if (b == FESC) {
                esc_ = true;
            } else if (esc_) {
                esc_ = false;
                buf_.push_back(b == TFEND ? FEND : b == TFESC ? FESC : b);
            } else {
                buf_.push_back(b);
            }
        }
        return frames;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Minimal AX.25 decode (display only)
// ─────────────────────────────────────────────────────────────────────────────
struct Ax25Info {
    std::string summary;
    uint8_t ctrl_byte = 0;
};

static std::pair<std::string, bool> decode_addr(const uint8_t* d, int off) {
    std::string call;
    for (int i = 0; i < 6; i++) {
        char c = (char)(d[off + i] >> 1);
        if (c != ' ') call += c;
    }
    uint8_t sb = d[off + 6];
    int ssid = (sb >> 1) & 0xF;
    if (ssid) call += "-" + std::to_string(ssid);
    return {call, (sb & 0x01) != 0};
}

static Ax25Info decode_ax25(const uint8_t* d, size_t n) {
    Ax25Info r;
    if (n < 15) { r.summary = "[too short: " + std::to_string(n) + " bytes]"; return r; }

    auto [dest, _d]     = decode_addr(d, 0);
    auto [src, end_src] = decode_addr(d, 7);
    int  off = 14;
    std::string via;
    while (!end_src && (size_t)(off + 7) <= n) {
        auto [rep, e] = decode_addr(d, off);
        via += " via " + rep;
        end_src = e; off += 7;
    }
    if ((size_t)off >= n) { r.summary = src + " -> " + dest + "  [no ctrl]"; return r; }

    uint8_t ctrl = d[off];
    r.ctrl_byte  = ctrl;
    bool pf      = (ctrl & 0x10) != 0;
    std::string ft;

    if ((ctrl & 0x01) == 0) {                              // I-frame
        int ns = (ctrl >> 1) & 7, nr = (ctrl >> 5) & 7;
        ft = "I(NS=" + std::to_string(ns) + ",NR=" + std::to_string(nr) + (pf ? "P" : "") + ")";
    } else if ((ctrl & 0x03) == 0x01) {                    // S-frame
        int nr = (ctrl >> 5) & 7;
        static constexpr std::pair<uint8_t, const char*> st[] =
            {{0x01,"RR"},{0x05,"RNR"},{0x09,"REJ"},{0x0D,"SREJ"}};
        const char* stype = "S?";
        for (auto& [v, nm] : st) if ((ctrl & 0xF) == v) { stype = nm; break; }
        ft = std::string(stype) + "(NR=" + std::to_string(nr) + (pf ? "/F" : "") + ")";
    } else {                                                // U-frame
        uint8_t base = ctrl & ~0x10u;
        static constexpr std::pair<uint8_t, const char*> ut[] =
            {{0x2F,"SABM"},{0x43,"DISC"},{0x63,"UA"},{0x0F,"DM"},{0x87,"FRMR"},{0x03,"UI"}};
        const char* utype = nullptr;
        for (auto& [v, nm] : ut)
            if (ctrl == v || ctrl == (uint8_t)(v | 0x10u) || base == v) { utype = nm; break; }
        if (utype) ft = std::string(utype) + (pf ? "(P/F)" : "");
        else { char s[12]; snprintf(s, sizeof(s), "U?0x%02x", ctrl); ft = s; }
    }

    r.summary = src + " -> " + dest + via + "  [" + ft + "]";
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Display one decoded KISS frame
// ─────────────────────────────────────────────────────────────────────────────
static void print_kiss_frame(int num, const KissFrame& kf, const char* arrow) {
    if (kf.type == 0) {
        auto ax = decode_ax25(kf.payload.data(), kf.payload.size());
        printf("    └─ [KISS%s#%d port=%d]  AX.25: %s\n",
               arrow, num, kf.port, ax.summary.c_str());
        // ctrl + hexdump-C via ax25dump.hpp
        if (!kf.payload.empty()) {
            printf("%s", ctrl_detail(ax.ctrl_byte, kf.payload.size()).c_str());
            printf("%s", hex_dump(kf.payload.data(), kf.payload.size(), "           ").c_str());
        }
    } else {
        static constexpr std::pair<int, const char*> knames[] = {
            {1,"TXDELAY"},{2,"PERSIST"},{3,"SLOTTIME"},
            {4,"TXTAIL"},{5,"FULLDUPLEX"},{6,"SETHW"},{255,"RETURN"}};
        const char* kname = "CMD?";
        for (auto& [v, nm] : knames) if (kf.type == v) { kname = nm; break; }
        uint8_t val = kf.payload.empty() ? 0 : kf.payload[0];
        printf("    └─ [KISS%s#%d port=%d]  ctrl: %s  val=%u (0x%02x)\n",
               arrow, num, kf.port, kname, val, val);
    }
    printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Open a serial/PTY device in raw non-blocking mode
// ─────────────────────────────────────────────────────────────────────────────
static int open_serial(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    // Back to blocking — we use select()
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    struct termios t{};
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    tcsetattr(fd, TCSANOW, &t);
    return fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// TCP connect helper  "host:port" → fd, or -1
// ─────────────────────────────────────────────────────────────────────────────
static int tcp_connect(const std::string& hostport) {
    auto colon = hostport.rfind(':');
    if (colon == std::string::npos) return -1;
    std::string host = hostport.substr(0, colon);
    std::string port = hostport.substr(colon + 1);

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return -1;

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// Create downstream PTY pair, set raw mode on slave, optionally symlink
// ─────────────────────────────────────────────────────────────────────────────
static bool create_pty(int& master, int& slave, std::string& slave_path,
                        const std::string& link_path) {
    char name[256]{};
    if (openpty(&master, &slave, name, nullptr, nullptr) < 0) return false;
    slave_path = name;
    struct termios t{};
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    if (!link_path.empty()) {
        ::unlink(link_path.c_str());
        ::symlink(name, link_path.c_str());
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Usage
// ─────────────────────────────────────────────────────────────────────────────
static void usage(const char* prog) {
    fprintf(stderr,
        "KISS proxy tap — transparent bridge sniffer\n\n"
        "Usage:\n"
        "  %s <upstream> [--link <path>] [-h]\n\n"
        "  <upstream>     PTY/serial path OR host:port (TCP)\n"
        "  --link <path>  Downstream PTY symlink  (default: /tmp/kiss-tap)\n\n"
        "Workflow:\n"
        "  Terminal 1: bt_kiss_bridge --ble --device \"VR-N76\" --link /tmp/kiss\n"
        "  Terminal 2: %s /tmp/kiss --link /tmp/kiss-tap\n"
        "  Terminal 3: ax25tnc -c N0CALL /tmp/kiss-tap\n\n"
        "  Or TCP upstream:\n"
        "  Terminal 1: bt_kiss_bridge --ble --device \"VR-N76\" --server-port 8001\n"
        "  Terminal 2: %s localhost:8001 --link /tmp/kiss-tap\n"
        "  Terminal 3: ax25tnc -c N0CALL /tmp/kiss-tap\n",
        prog, prog, prog);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string upstream;
    std::string link_path = "/tmp/kiss-tap";

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "--link" || a == "-l") && i + 1 < argc) {
            link_path = argv[++i];
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]); return 0;
        } else if (a[0] != '-') {
            upstream = a;
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", a.c_str());
            usage(argv[0]); return 1;
        }
    }

    if (upstream.empty()) { usage(argv[0]); return 1; }

    // ── Connect upstream ────────────────────────────────────────────────────
    int  up_fd  = -1;
    bool is_tcp = (upstream.find(':') != std::string::npos && upstream[0] != '/');

    if (is_tcp) {
        printf("Connecting TCP → %s ...\n", upstream.c_str());
        fflush(stdout);
        up_fd = tcp_connect(upstream);
    } else {
        printf("Opening %s ...\n", upstream.c_str());
        fflush(stdout);
        up_fd = open_serial(upstream);
    }

    if (up_fd < 0) {
        fprintf(stderr, "Error: cannot open upstream %s: %s\n",
                upstream.c_str(), strerror(errno));
        return 1;
    }
    printf("Upstream OK.\n");

    // ── Create downstream PTY ───────────────────────────────────────────────
    int  master_fd = -1, slave_fd = -1;
    std::string slave_path;
    if (!create_pty(master_fd, slave_fd, slave_path, link_path)) {
        fprintf(stderr, "Error: cannot create PTY: %s\n", strerror(errno));
        ::close(up_fd); return 1;
    }

    printf("Downstream PTY : %s\n", slave_path.c_str());
    if (!link_path.empty())
        printf("Symlink        : %s\n", link_path.c_str());
    printf("\nConnect your AX.25 client to: %s\n",
           link_path.empty() ? slave_path.c_str() : link_path.c_str());
    printf("Ctrl-C to stop.\n");
    printf("\n%s\n\n", std::string(68, '-').c_str());
    fflush(stdout);

    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    // ── Main proxy loop ─────────────────────────────────────────────────────
    KissDecoder up_dec;   // upstream  (bridge  → client)
    KissDecoder dn_dec;   // downstream (client → bridge)
    int    frame_count = 0;
    size_t up_bytes = 0, dn_bytes = 0;
    uint8_t buf[4096];

    while (!g_quit) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(up_fd,     &rfds);
        FD_SET(master_fd, &rfds);
        int maxfd = std::max(up_fd, master_fd) + 1;

        struct timeval tv{0, 100000};  // 100 ms
        int r = ::select(maxfd, &rfds, nullptr, nullptr, &tv);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;

        // ── Data from upstream (bridge → client) ───────────────────────────
        if (FD_ISSET(up_fd, &rfds)) {
            ssize_t n = ::read(up_fd, buf, sizeof(buf));
            if (n <= 0) { printf("\n[Upstream disconnected]\n"); break; }

            up_bytes += (size_t)n;
            printf("[%s]  \033[32m← BRIDGE\033[0m  %zd bytes\n", ts().c_str(), n);
            printf("%s", hex_dump(buf, (size_t)n, "    ").c_str());

            auto frames = up_dec.feed(buf, (size_t)n);
            for (auto& kf : frames)
                print_kiss_frame(++frame_count, kf, "←");

            fflush(stdout);

            // Forward to downstream client
            ssize_t sent = 0;
            while (sent < n)  {
                ssize_t w = ::write(master_fd, buf + sent, (size_t)(n - sent));
                if (w <= 0) break;
                sent += w;
            }
        }

        // ── Data from downstream (client → bridge) ─────────────────────────
        if (FD_ISSET(master_fd, &rfds)) {
            ssize_t n = ::read(master_fd, buf, sizeof(buf));
            if (n <= 0) continue;  // client disconnected — keep waiting

            dn_bytes += (size_t)n;
            printf("[%s]  \033[33m→ BRIDGE\033[0m  %zd bytes\n", ts().c_str(), n);
            printf("%s", hex_dump(buf, (size_t)n, "    ").c_str());

            auto frames = dn_dec.feed(buf, (size_t)n);
            for (auto& kf : frames)
                print_kiss_frame(++frame_count, kf, "→");

            fflush(stdout);

            // Forward to upstream bridge
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t w = ::write(up_fd, buf + sent, (size_t)(n - sent));
                if (w <= 0) break;
                sent += w;
            }
        }
    }

    printf("\n%s\n", std::string(68, '-').c_str());
    printf("Sniffer stopped.  ← %zu bytes   → %zu bytes   %d KISS frames decoded\n",
           up_bytes, dn_bytes, frame_count);

    if (!link_path.empty()) ::unlink(link_path.c_str());
    ::close(master_fd);
    ::close(slave_fd);
    ::close(up_fd);
    return 0;
}
