// ============================================================================
// bt_kiss_bridge_sble.cpp — BLE KISS bridge using SimpleBLE
//
// Same purpose as bt_kiss_bridge but uses SimpleBLE instead of native
// CoreBluetooth/BlueZ.  Built to compare behaviour and isolate TX issues.
//
// Usage:
//   bt_kiss_bridge_sble --scan [--timeout <s>]
//   bt_kiss_bridge_sble --test    <DEVICE> [--call <CALL>] [--mtu <N>]
//   bt_kiss_bridge_sble --monitor <DEVICE> [--mtu <N>]
//   bt_kiss_bridge_sble --device  <DEVICE> [--link <PTY>] [--mtu <N>]
//
// Build:
//   make bt_kiss_bridge_sble
// ============================================================================

#include "simpleble/SimpleBLE.h"
#include "ax25dump.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// PTY
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#ifdef __APPLE__
#  include <util.h>
#else
#  include <pty.h>
#endif

// ── Globals ──────────────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static void sigint_handler(int) { g_running = false; }

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::string ts() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    struct tm tm_s{};
    localtime_r(&t, &tm_s);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tm_s.tm_hour, tm_s.tm_min, tm_s.tm_sec, (int)ms.count());
    return buf;
}

static void sble_hex_dump(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (i && i % 24 == 0) std::printf("\n               ");
        std::printf("%02x ", data[i]);
    }
    std::printf("\n");
}

static std::string hr(char c = '-', int n = 68) { return std::string(n, c); }

// ── KISS frame extractor ──────────────────────────────────────────────────────
// Accumulates raw bytes and calls cb(payload) for each complete KISS frame.
// Handles FESC/TFEND/TFESC escaping. Skips the KISS command byte.

class KissDecoder {
public:
    using Callback = std::function<void(uint8_t port, uint8_t cmd,
                                        const std::vector<uint8_t>&)>;

    explicit KissDecoder(Callback cb) : cb_(std::move(cb)) {}

    void feed(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            uint8_t b = data[i];
            if (b == 0xC0) {          // FEND
                if (in_frame_ && !buf_.empty()) flush();
                in_frame_ = true;
                buf_.clear();
                escaped_ = false;
            } else if (!in_frame_) {
                // ignore bytes outside frame
            } else if (b == 0xDB) {   // FESC
                escaped_ = true;
            } else if (escaped_) {
                escaped_ = false;
                buf_.push_back(b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b);
            } else {
                buf_.push_back(b);
            }
        }
    }

private:
    void flush() {
        if (buf_.empty()) return;
        uint8_t kiss_cmd = buf_[0];
        uint8_t port = (kiss_cmd >> 4) & 0x0F;
        uint8_t cmd  = kiss_cmd & 0x0F;
        std::vector<uint8_t> payload(buf_.begin() + 1, buf_.end());
        cb_(port, cmd, payload);
        buf_.clear();
    }

    Callback cb_;
    std::vector<uint8_t> buf_;
    bool in_frame_ = false;
    bool escaped_  = false;
};

// ── AX.25 frame decoder ───────────────────────────────────────────────────────

static std::string decode_ax25_addr(const uint8_t* b) {
    char call[7]{};
    for (int i = 0; i < 6; i++) call[i] = (char)(b[i] >> 1);
    // trim trailing spaces
    int len = 6;
    while (len > 0 && call[len-1] == ' ') len--;
    call[len] = '\0';
    int ssid = (b[6] >> 1) & 0x0F;
    std::string s = call;
    if (ssid) s += "-" + std::to_string(ssid);
    return s;
}

static std::string decode_pid(uint8_t pid) {
    switch (pid) {
        case 0xF0: return "no L3";
        case 0x08: return "NET/ROM";
        case 0xCF: return "TheNET";
        case 0x01: return "X.25";
        case 0x06: return "Compressed TCP/IP";
        case 0x07: return "Uncompressed TCP/IP";
        case 0xCC: return "IP";
        case 0xCD: return "ARP";
        default:   char b[8]; std::snprintf(b,sizeof(b),"0x%02X",pid); return b;
    }
}

static void print_ax25(const std::vector<uint8_t>& payload) {
    // Minimum AX.25: 14 bytes (dest+src addr) + 1 ctrl
    if (payload.size() < 15) {
        std::printf("  [too short: %zu bytes]\n", payload.size());
        return;
    }

    std::string dst = decode_ax25_addr(payload.data());
    std::string src = decode_ax25_addr(payload.data() + 7);

    // Digipeaters
    std::string digi;
    size_t addr_end = 14;
    while (addr_end > 7 && !(payload[addr_end - 1] & 0x01)) {
        if (payload.size() >= addr_end + 7) {
            digi += "," + decode_ax25_addr(payload.data() + addr_end);
            addr_end += 7;
        } else break;
    }

    if (payload.size() <= addr_end) {
        std::printf("  [truncated at ctrl]\n"); return;
    }

    uint8_t ctrl = payload[addr_end];
    bool is_ui   = ((ctrl & ~0x10) == 0x03);

    std::string info_str;
    if (is_ui && payload.size() > addr_end + 2) {
        uint8_t pid = payload[addr_end + 1];
        const uint8_t* info = payload.data() + addr_end + 2;
        size_t info_len = payload.size() - addr_end - 2;

        // Check if printable
        bool printable = true;
        for (size_t i = 0; i < info_len; i++)
            if (info[i] < 0x20 || info[i] > 0x7E) { printable = false; break; }

        if (printable)
            info_str = "\"" + std::string((const char*)info, info_len) + "\"";
        else {
            std::ostringstream os;
            os << "[" << info_len << " bytes hex: ";
            for (size_t i = 0; i < std::min(info_len, (size_t)16); i++)
                os << std::hex << std::setw(2) << std::setfill('0') << (int)info[i] << " ";
            if (info_len > 16) os << "...";
            os << "]";
            info_str = os.str();
        }

        std::printf("  %s>%s%s [UI] PID:%s  %s\n",
                    src.c_str(), dst.c_str(), digi.c_str(),
                    decode_pid(pid).c_str(), info_str.c_str());
    } else {
        std::printf("  %s>%s%s  %s\n",
                    src.c_str(), dst.c_str(), digi.c_str(),
                    ctrl_detail(ctrl, payload.size()).c_str());
    }

    // Hex dump
    std::printf("%s", hex_dump(payload.data(), payload.size(), "    ").c_str());
}

// ── Auto-detect KISS service ──────────────────────────────────────────────────

struct KissUUIDs {
    std::string service, write, read;
    bool valid() const { return !service.empty() && !write.empty() && !read.empty(); }
};

static KissUUIDs auto_detect(SimpleBLE::Peripheral& p) {
    int best_score = -1;
    KissUUIDs best{};

    for (auto& svc : p.services()) {
        std::string w, r;
        int score = 0;

        for (auto& chr : svc.characteristics()) {
            bool can_notify = chr.can_notify() || chr.can_indicate();
            bool can_write  = chr.can_write_request() || chr.can_write_command();
            bool wwr        = chr.can_write_command();

            if (can_notify && r.empty()) { r = chr.uuid(); score += 1; }
            if (can_write  && w.empty()) { w = chr.uuid(); score += (wwr ? 2 : 1); }
        }

        if (!w.empty() && !r.empty() && score > best_score) {
            best_score = score;
            best = { svc.uuid(), w, r };
        }
    }
    return best;
}

// ── BLE chunked write ─────────────────────────────────────────────────────────
// Splits data into MTU-sized chunks and writes each as a BLE command.

static void write_ble(SimpleBLE::Peripheral& p, const KissUUIDs& uuids,
                      const uint8_t* data, size_t len, size_t mtu) {
    if (mtu == 0) mtu = 512;  // no limit
    for (size_t off = 0; off < len; off += mtu) {
        size_t chunk = std::min(mtu, len - off);
        SimpleBLE::ByteArray ba((const char*)(data + off), chunk);
        p.write_command(uuids.service, uuids.write, ba);
    }
}

// ── AX.25 / KISS frame builder ────────────────────────────────────────────────

static uint8_t ax25_char(char c) { return (uint8_t)(c << 1); }

static std::vector<uint8_t> build_kiss_ui(const std::string& src,
                                           const std::string& dst,
                                           const std::string& info) {
    std::string d = dst; d.resize(6, ' ');
    std::string s = src; s.resize(6, ' ');

    std::vector<uint8_t> frame;
    frame.push_back(0xC0);
    frame.push_back(0x00);  // KISS cmd: data port 0

    for (int i = 0; i < 6; i++) frame.push_back(ax25_char(d[i]));
    frame.push_back(0x60);  // dest SSID, C=0, RR=11, E=0

    for (int i = 0; i < 6; i++) frame.push_back(ax25_char(s[i]));
    frame.push_back(0x61);  // src SSID, C=0, RR=11, E=1 (last addr)

    frame.push_back(0x03);  // UI ctrl
    frame.push_back(0xF0);  // PID: no L3
    for (char c : info) frame.push_back((uint8_t)c);
    frame.push_back(0xC0);
    return frame;
}

static const uint8_t KISS_NULL_BYTES[] = {0xC0, 0xC0};

// ── TNC / KISS initialisation ─────────────────────────────────────────────────
//
// Replicates the tnc_kiss_init() sequence from ax25lib:
//   1. Text wake-up commands (same as --tnc in ax25tnc), 100 ms apart
//   2. 2 s settle for the TNC to enter KISS mode
//   3. KISS parameter frames: TXDELAY, PERSISTENCE, SLOTTIME, one per 50 ms

static std::vector<uint8_t> kiss_param_frame(uint8_t cmd, uint8_t val) {
    // C0 <cmd> <val> C0  — single-byte parameter frame
    return {0xC0, cmd, val, 0xC0};
}

static void send_tnc_init(SimpleBLE::Peripheral& p, const KissUUIDs& uuids,
                          size_t mtu,
                          int txdelay_ms = 400,
                          int persist    = 63,
                          int slottime_ms= 100) {
    std::cout << "  [TNC-INIT] Sending text wake-up commands...\n";

    // Text commands — sent as raw bytes (no KISS framing)
    const char* text_cmds[] = {
        "KISS ON\r",
        "RESTART\r",
        "INTERFACE KISS\r",
        "RESET\r",
    };
    for (const char* cmd : text_cmds) {
        std::cout << "  [TNC-INIT]   " << std::string(cmd, ::strlen(cmd) - 2) << "\n";
        SimpleBLE::ByteArray ba(cmd, ::strlen(cmd));
        p.write_command(uuids.service, uuids.write, ba);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "  [TNC-INIT] Waiting 2s for KISS mode...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // KISS parameter frames
    struct { const char* name; uint8_t cmd; uint8_t val; } params[] = {
        { "TXDELAY",     0x01, (uint8_t)(txdelay_ms  / 10) },
        { "PERSISTENCE", 0x02, (uint8_t) persist           },
        { "SLOTTIME",    0x03, (uint8_t)(slottime_ms / 10) },
    };
    std::cout << "  [TNC-INIT] Sending KISS parameters...\n";
    for (auto& p_param : params) {
        std::cout << "  [TNC-INIT]   " << p_param.name
                  << " = 0x" << std::hex << std::setw(2) << std::setfill('0')
                  << (int)p_param.val << std::dec << "\n";
        auto frame = kiss_param_frame(p_param.cmd, p_param.val);
        write_ble(p, uuids, frame.data(), frame.size(), mtu);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "  [TNC-INIT] Done.\n";
}

// ── Connect helpers ───────────────────────────────────────────────────────────

static std::optional<SimpleBLE::Peripheral> find_peripheral(
        SimpleBLE::Adapter& adapter, const std::string& target, double timeout_s) {

    std::string tlo = lower(target);
    std::optional<SimpleBLE::Peripheral> found;
    std::mutex mtx;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        std::string name = lower(p.identifier());
        std::string addr = lower(p.address());
        if (name.find(tlo) != std::string::npos || addr == tlo) {
            std::lock_guard<std::mutex> lk(mtx);
            if (!found.has_value()) {
                std::cout << "  Found: " << p.identifier()
                          << "  [" << p.address() << "]"
                          << "  RSSI=" << p.rssi() << " dBm\n";
                found = p;
            }
        }
    });

    adapter.scan_start();
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds((long long)(timeout_s * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        { std::lock_guard<std::mutex> lk(mtx); if (found.has_value()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    adapter.scan_stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::lock_guard<std::mutex> lk(mtx);
    return found;
}

static bool get_adapter(SimpleBLE::Adapter& out) {
    if (!SimpleBLE::Adapter::bluetooth_enabled()) {
        std::cerr << "Bluetooth not enabled.\n"; return false;
    }
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) { std::cerr << "No BLE adapter found.\n"; return false; }
    out = adapters[0];
    return true;
}

// ── do_scan ───────────────────────────────────────────────────────────────────

static void do_scan(double timeout_s) {
    SimpleBLE::Adapter adapter;
    if (!get_adapter(adapter)) return;

    std::cout << "Scanning for BLE devices (" << (int)timeout_s << "s)...\n\n";
    std::vector<SimpleBLE::Peripheral> found;
    std::mutex mtx;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& x : found)
            if (lower(x.address()) == lower(p.address())) return;
        found.push_back(p);
        std::cout << hr() << "\n"
                  << "  Name : " << (p.identifier().empty() ? "(no name)" : p.identifier()) << "\n"
                  << "  Addr : " << p.address() << "\n"
                  << "  RSSI : " << p.rssi() << " dBm\n";
        std::cout.flush();
    });

    adapter.scan_for((size_t)(timeout_s * 1000));
    std::lock_guard<std::mutex> lk(mtx);
    std::cout << hr('=') << "\nFound " << found.size() << " BLE device(s).\n";
}

// ── do_inspect ────────────────────────────────────────────────────────────────

static std::string desc_name(const std::string& uuid) {
    std::string u = lower(uuid);
    if (u.find("2900") == 0 || u == "00002900-0000-1000-8000-00805f9b34fb") return "Characteristic Extended Properties";
    if (u.find("2901") == 0 || u == "00002901-0000-1000-8000-00805f9b34fb") return "User Description";
    if (u.find("2902") == 0 || u == "00002902-0000-1000-8000-00805f9b34fb") return "CCCD (notify/indicate config)";
    if (u.find("2903") == 0 || u == "00002903-0000-1000-8000-00805f9b34fb") return "Server Characteristic Configuration";
    if (u.find("2904") == 0 || u == "00002904-0000-1000-8000-00805f9b34fb") return "Presentation Format";
    return "";
}

static std::string bytes_to_str(const SimpleBLE::ByteArray& ba) {
    // Try UTF-8 printable string first
    bool printable = !ba.empty();
    for (char c : ba)
        if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) { printable = false; break; }
    if (printable) return "\"" + std::string(ba.begin(), ba.end()) + "\"";

    // Hex fallback
    std::ostringstream os;
    os << "0x";
    for (unsigned char c : ba)
        os << std::hex << std::setw(2) << std::setfill('0') << (int)c;

    // CCCD special case
    if (ba.size() == 2) {
        uint16_t v = (uint8_t)ba[0] | ((uint8_t)ba[1] << 8);
        if      (v == 0x0000) os << "  [notifications OFF]";
        else if (v == 0x0001) os << "  [notifications ON]";
        else if (v == 0x0002) os << "  [indications ON]";
        else if (v == 0x0003) os << "  [notify+indicate ON]";
    }
    return os.str();
}

static void do_inspect(const std::string& target, double scan_timeout_s) {
    std::cout << hr('=') << "\n"
              << "  BLE Inspect (SimpleBLE) — with descriptor values\n"
              << hr('=') << "\n"
              << "  Device : " << target << "\n"
              << hr() << "\n"
              << "  Scanning...\n";
    std::cout.flush();

    SimpleBLE::Adapter adapter;
    if (!get_adapter(adapter)) return;

    auto periph = find_peripheral(adapter, target, scan_timeout_s);
    if (!periph.has_value()) { std::cerr << "  Device not found.\n"; return; }

    auto& p = periph.value();
    std::cout << "  Connecting...\n"; std::cout.flush();
    p.connect();

    std::cout << "\n"
              << "  Name : " << p.identifier() << "\n"
              << "  Addr : " << p.address()    << "\n"
              << "  MTU  : " << p.mtu()        << " bytes  (payload: " << (p.mtu()-3) << ")\n\n";

    for (auto& svc : p.services()) {
        std::cout << "SERVICE " << svc.uuid() << "\n";

        for (auto& chr : svc.characteristics()) {
            // Build capability string
            std::string caps;
            auto add = [&](bool b, const char* s){ if (b) { if(!caps.empty()) caps+=", "; caps+=s; } };
            add(chr.can_read(),          "read");
            add(chr.can_notify(),        "notify");
            add(chr.can_indicate(),      "indicate");
            add(chr.can_write_request(), "write");
            add(chr.can_write_command(), "write-without-response");

            std::cout << "  CHR " << chr.uuid()
                      << "  [" << caps << "]\n";

            // Read value if readable
            if (chr.can_read()) {
                try {
                    auto val = p.read(svc.uuid(), chr.uuid());
                    if (!val.empty())
                        std::cout << "       Value : " << bytes_to_str(val) << "\n";
                } catch (...) {}
            }

            // Read each descriptor
            for (auto& desc : chr.descriptors()) {
                std::string dname = desc_name(desc.uuid());
                std::string dval;
                try {
                    auto val = p.read(svc.uuid(), chr.uuid(), desc.uuid());
                    dval = bytes_to_str(val);
                } catch (...) {
                    dval = "(read failed)";
                }
                std::cout << "       DESC " << desc.uuid();
                if (!dname.empty()) std::cout << "  " << dname;
                std::cout << "\n"
                          << "            Value : " << dval << "\n";
            }
        }
        std::cout << "\n";
    }

    std::cout << hr('=') << "\n";
    p.disconnect();
}

// ── do_test ───────────────────────────────────────────────────────────────────

static void do_test(const std::string& target, const std::string& call,
                    double interval_s, double scan_timeout_s, size_t mtu,
                    bool monitor, bool tnc_init) {
    std::cout << hr('=') << "\n"
              << "  BLE KISS Test Mode (SimpleBLE)\n"
              << hr('=') << "\n"
              << "  Device : " << target << "\n"
              << hr() << "\n"
              << "  Scanning...\n";
    std::cout.flush();

    SimpleBLE::Adapter adapter;
    if (!get_adapter(adapter)) return;

    auto periph = find_peripheral(adapter, target, scan_timeout_s);
    if (!periph.has_value()) { std::cerr << "  Device not found.\n"; return; }

    auto& p = periph.value();
    std::cout << "  Connecting...\n"; std::cout.flush();
    p.connect();
    std::cout << "  Connected.  MTU=" << p.mtu() << "\n";

    auto uuids = auto_detect(p);
    if (!uuids.valid()) {
        std::cerr << "  No KISS service found.\n"; p.disconnect(); return;
    }
    std::cout << "  Service : " << uuids.service << "\n"
              << "  Write   : " << uuids.write   << "\n"
              << "  Read    : " << uuids.read     << "\n";

    if (mtu == 0) mtu = (size_t)p.mtu();
    std::cout << "  Chunk   : " << mtu << " bytes\n";

    // Subscribe — blocks until CCCD confirmed
    std::cout << "  Subscribing to notify...\n"; std::cout.flush();
    std::mutex rx_mtx;
    int rx_count = 0;

    // Decoder used when --monitor is active
    KissDecoder dec([&](uint8_t port, uint8_t cmd, const std::vector<uint8_t>& payload) {
        ++rx_count;
        if (cmd == 0x00) {
            std::printf("\n[%s]  RX #%d  port=%d  (%zuB)\n",
                        ts().c_str(), rx_count, port, payload.size());
            print_ax25(payload);
        } else {
            uint8_t val = payload.empty() ? 0 : payload[0];
            std::printf("\n[%s]  KISS CMD  cmd=0x%02X  val=0x%02X\n",
                        ts().c_str(), cmd, val);
        }
        std::fflush(stdout);
    });

    // Raw RX buffer used when --monitor is NOT active
    std::vector<uint8_t> rx_raw;

    p.notify(uuids.service, uuids.read,
             [&](SimpleBLE::ByteArray bytes) {
                 std::lock_guard<std::mutex> lk(rx_mtx);
                 if (monitor)
                     dec.feed((const uint8_t*)bytes.data(), bytes.size());
                 else
                     for (auto b : bytes) rx_raw.push_back((uint8_t)b);
             });
    std::cout << "  Notify confirmed (CCCD written and ACKed).\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (tnc_init)
        send_tnc_init(p, uuids, mtu);

    std::cout << hr() << "\n"
              << "  Sending UI frames every " << (int)interval_s
              << "s.  Ctrl-C to stop.\n"
              << hr() << "\n\n";
    std::cout.flush();

    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    int tx_count = 0;
    auto next_tx = std::chrono::steady_clock::now();
    auto next_ka = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (g_running && p.is_connected()) {
        auto now = std::chrono::steady_clock::now();

        if (now >= next_tx) {
            ++tx_count;
            char info[32];
            std::snprintf(info, sizeof(info), "Test %03d", tx_count);
            auto frame = build_kiss_ui(call, "CQ", info);

            std::printf("[%s]  TX >> #%d  %s>CQ [UI] \"%s\"  (%zuB)\n",
                        ts().c_str(), tx_count, call.c_str(), info, frame.size());
            std::printf("               ");
            sble_hex_dump(frame.data(), frame.size());
            std::fflush(stdout);

            write_ble(p, uuids, frame.data(), frame.size(), mtu);
            next_tx = now + std::chrono::milliseconds((long long)(interval_s * 1000));
        }

        if (now >= next_ka) {
            write_ble(p, uuids, KISS_NULL_BYTES, 2, mtu);
            next_ka = now + std::chrono::seconds(5);
        }

        // Plain hex RX dump (when --monitor not set)
        if (!monitor) {
            std::lock_guard<std::mutex> lk(rx_mtx);
            if (!rx_raw.empty()) {
                ++rx_count;
                std::printf("[%s]  RX << (%zuB):\n               ",
                            ts().c_str(), rx_raw.size());
                sble_hex_dump(rx_raw.data(), rx_raw.size());
                std::fflush(stdout);
                rx_raw.clear();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\n" << hr() << "\n"
              << "  Test ended.  TX: " << tx_count << "  RX: " << rx_count << "\n"
              << hr() << "\n";

    p.unsubscribe(uuids.service, uuids.read);
    p.disconnect();
}

// ── do_bridge ─────────────────────────────────────────────────────────────────

static void do_bridge(const std::string& target, const std::string& link_path,
                      double scan_timeout_s, size_t mtu, bool monitor, bool tnc_init) {
    SimpleBLE::Adapter adapter;
    if (!get_adapter(adapter)) return;

    std::cout << "  Scanning for " << target << "...\n"; std::cout.flush();
    auto periph = find_peripheral(adapter, target, scan_timeout_s);
    if (!periph.has_value()) { std::cerr << "  Device not found.\n"; return; }

    auto& p = periph.value();
    std::cout << "  Connecting...\n"; std::cout.flush();
    p.connect();
    std::cout << "  Connected.  MTU=" << p.mtu() << "\n";

    auto uuids = auto_detect(p);
    if (!uuids.valid()) {
        std::cerr << "  No KISS service found.\n"; p.disconnect(); return;
    }
    std::cout << "  Service : " << uuids.service << "\n"
              << "  Write   : " << uuids.write   << "\n"
              << "  Read    : " << uuids.read     << "\n";

    if (mtu == 0) mtu = (size_t)p.mtu();
    std::cout << "  Chunk   : " << mtu << " bytes\n";

    // Create PTY
    int master = -1, slave = -1;
    char pty_name[64]{};
    if (openpty(&master, &slave, pty_name, nullptr, nullptr) < 0) {
        std::cerr << "openpty failed: " << strerror(errno) << "\n";
        p.disconnect(); return;
    }
    struct termios t{};
    tcgetattr(master, &t);
    cfmakeraw(&t);
    tcsetattr(master, TCSANOW, &t);
    close(slave);

    if (!link_path.empty()) {
        unlink(link_path.c_str());
        if (symlink(pty_name, link_path.c_str()) == 0)
            std::cout << "  PTY: " << pty_name << "  →  " << link_path << "\n";
        else
            std::cout << "  PTY: " << pty_name << "  (symlink failed)\n";
    } else {
        std::cout << "  PTY: " << pty_name << "\n";
    }

    // Subscribe — blocks until CCCD ACKed
    std::cout << "  Subscribing to notify...\n"; std::cout.flush();
    if (monitor)
        std::cout << "  Monitor: decoded RX frames will be shown below.\n";

    std::mutex mon_mtx;
    int mon_rx = 0, mon_tx = 0;

    KissDecoder rx_dec([&](uint8_t port, uint8_t cmd, const std::vector<uint8_t>& payload) {
        ++mon_rx;
        if (cmd == 0x00) {
            std::printf("\n[%s]  RX #%d  port=%d  (%zuB)\n",
                        ts().c_str(), mon_rx, port, payload.size());
            print_ax25(payload);
        } else {
            uint8_t val = payload.empty() ? 0 : payload[0];
            std::printf("\n[%s]  KISS CMD  cmd=0x%02X  val=0x%02X\n",
                        ts().c_str(), cmd, val);
        }
        std::fflush(stdout);
    });

    KissDecoder tx_dec([&](uint8_t port, uint8_t cmd, const std::vector<uint8_t>& payload) {
        ++mon_tx;
        if (cmd == 0x00) {
            std::printf("[%s]  TX #%d  port=%d  (%zuB)\n",
                        ts().c_str(), mon_tx, port, payload.size());
            print_ax25(payload);
        }
        std::fflush(stdout);
    });

    p.notify(uuids.service, uuids.read,
             [&](SimpleBLE::ByteArray bytes) {
                 // Always forward to PTY
                 ::write(master, bytes.data(), bytes.size());
                 // Decode if monitor active
                 if (monitor) {
                     std::lock_guard<std::mutex> lk(mon_mtx);
                     rx_dec.feed((const uint8_t*)bytes.data(), bytes.size());
                 }
             });
    std::cout << "  Notify confirmed.  Bridge active.\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (tnc_init)
        send_tnc_init(p, uuids, mtu);

    std::cout << "\n";

    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    uint8_t buf[512];
    auto next_ka = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (g_running && p.is_connected()) {
        auto now = std::chrono::steady_clock::now();

        fd_set rfds; FD_ZERO(&rfds); FD_SET(master, &rfds);
        struct timeval tv{0, 50000};
        if (::select(master + 1, &rfds, nullptr, nullptr, &tv) > 0) {
            ssize_t n = ::read(master, buf, sizeof(buf));
            if (n > 0) {
                write_ble(p, uuids, buf, (size_t)n, mtu);
                if (monitor) {
                    std::lock_guard<std::mutex> lk(mon_mtx);
                    tx_dec.feed(buf, (size_t)n);
                }
            }
        }

        if (now >= next_ka) {
            write_ble(p, uuids, KISS_NULL_BYTES, 2, mtu);
            next_ka = now + std::chrono::seconds(5);
        }
    }

    std::cout << "\nDisconnecting...\n";
    p.unsubscribe(uuids.service, uuids.read);
    p.disconnect();
    close(master);
    if (!link_path.empty()) unlink(link_path.c_str());
}

// ── main ──────────────────────────────────────────────────────────────────────

static void usage(const char* prog) {
    std::cout <<
        "Usage:\n"
        "  " << prog << " --scan [--timeout <s>]\n"
        "  " << prog << " --inspect <DEVICE> [--timeout <s>]\n"
        "  " << prog << " --test    <DEVICE> [--call <CALL>] [--mtu <N>] [--monitor] [--timeout <s>]\n"
        "  " << prog << " --device  <DEVICE> [--link <PTY>] [--mtu <N>] [--monitor] [--timeout <s>]\n"
        "\n"
        "Options:\n"
        "  --scan              Scan and list BLE devices\n"
        "  --inspect <DEV>     Connect and dump all services/chars/descriptors with values\n"
        "  --test <DEV>        Send AX.25 UI test frames and show RX\n"
        "  --device <DEV>      Bridge BLE KISS ↔ PTY\n"
        "  --monitor           Decode and print AX.25 frames in real-time (combines with --test/--device)\n"
        "  --link <PATH>       PTY symlink path (default: /tmp/kiss_sble)\n"
        "  --call <CALL>       Source callsign for --test (default: NOC4LL)\n"
        "  --mtu <N>           BLE write chunk size in bytes (default: auto from MTU)\n"
        "  --timeout <s>       Scan timeout in seconds (default: 15)\n";
}

int main(int argc, char* argv[]) {
    std::string mode;
    std::string device;
    std::string link_path  = "/tmp/kiss_sble";
    std::string call       = "NOC4LL";
    double      timeout_s  = 15.0;
    double      interval_s = 2.0;
    size_t      mtu        = 0;  // 0 = auto
    bool        monitor    = false;
    bool        tnc_init   = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--scan")                      mode = "scan";
        else if (a == "--inspect" && i+1 < argc)    { mode = "inspect"; device = argv[++i]; }
        else if (a == "--test"    && i+1 < argc)    { mode = "test";    device = argv[++i]; }
        else if (a == "--device"  && i+1 < argc)    { mode = "bridge";  device = argv[++i]; }
        else if (a == "--monitor")                   monitor   = true;
        else if (a == "--tnc-init")                  tnc_init  = true;
        else if (a == "--link"    && i+1 < argc)     link_path = argv[++i];
        else if (a == "--call"    && i+1 < argc)     call      = argv[++i];
        else if (a == "--mtu"     && i+1 < argc)     mtu       = (size_t)std::atoi(argv[++i]);
        else if (a == "--timeout" && i+1 < argc)     timeout_s = std::atof(argv[++i]);
        else if (a == "--help" || a == "-h")        { usage(argv[0]); return 0; }
        else { std::cerr << "Unknown option: " << a << "\n"; usage(argv[0]); return 1; }
    }

    if (mode.empty()) { usage(argv[0]); return 1; }

    if (mode == "scan")    { do_scan(timeout_s); return 0; }
    if (mode == "inspect") { do_inspect(device, timeout_s); return 0; }
    if (mode == "test")    { do_test(device, call, interval_s, timeout_s, mtu, monitor, tnc_init); return 0; }
    if (mode == "bridge")  { do_bridge(device, link_path, timeout_s, mtu, monitor, tnc_init); return 0; }

    return 0;
}
