// bt_kiss_bridge.cpp — Bluetooth KISS TNC serial bridge + AX.25 monitor (C++17)
//
// Supports two radio transports:
//   BLE   — Bluetooth Low Energy (SimpleBLE, all platforms)
//   BT    — Classic Bluetooth RFCOMM (BlueZ sockets, Linux only)
//
// Requires: SimpleBLE  ->  make ble-deps  (builds vendor/simpleble via cmake)
//           libbluetooth-dev (Linux, for Classic BT)
//
// -- Modes ---------------------------------------------------------------
//   --scan                       Scan for nearby devices
//   --inspect <ADDR>             List services (GATT for BLE, SDP for BT)
//   --device  <ADDR>             Bridge mode (PTY or TCP)
//                  --ble                        Force BLE transport
//                  --bt                         Force Classic BT transport
//                  --service / --write / --read GATT UUIDs (BLE)
//                  --channel <N>                RFCOMM channel (BT, 0=auto)
//                  [--mtu <N>]                  Max chunk cap (default 517)
//                  [--write-with-response]      Force write-with-response
//                  [--monitor]                  Enable rich frame monitor
//
// -- Transport modes (mutually exclusive) --------------------------------
//   PTY  (default)  A virtual serial port is created.  Symlink -> /tmp/kiss.
//                   Connect: ax25tnc -c W1AW -r W1BBS-1 /tmp/kiss
//   TCP             --server-port N  opens a KISS-over-TCP listener.
//                   Connect: ax25tnc -c W1AW -r W1BBS-1 localhost:N
//
// -- Rich frame monitor (--monitor) --------------------------------------
//   Each AX.25 frame is shown as:
//
//     [HH:MM:SS.mmm]  <- BLE  W1AW -> W1BBS-1  [SABM]        <- normal
//                ctrl=0x2f  U/SABM  P/F=1  (15 bytes)         <- dim
//                00000000  c0 1e 9a 76 40 ...  |...v@...|      <- dim
//
//   Direction tags:
//     "<- BLE"  BLE TNC   -> PTY / TCP clients  (receive path)
//     "<- BT"   BT TNC    -> PTY / TCP clients  (receive path)
//     "-> PTY"  PTY        -> TNC               (transmit path, PTY mode)
//     "-> TCP"  TCP client -> TNC               (transmit path, TCP mode)
//
//   KISS control frames (TXDELAY, P, SLOTTIME ...) are shown in dim with a
//   plain hexdump.  Fragmented packets are labelled "(buffering)" until a
//   complete KISS frame is assembled.
//
//   The ctrl detail and hexdump-C output come from ax25dump.hpp, the same
//   header used by ax25kiss and ax25tnc, keeping the format identical
//   across all three tools.
//
// -- Build ---------------------------------------------------------------
//   make ble-deps          # build SimpleBLE (one-time, needs cmake)
//   make bt_kiss_bridge

#ifdef __APPLE__
#  include <util.h>
#else
#  include <pty.h>
#  include <dbus/dbus.h>   // BlueZ SetDiscoveryFilter (Linux only)
#endif

#include <simpleble/SimpleBLE.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <signal.h>
#include <sstream>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>

#ifdef __linux__
#  include <bluetooth/bluetooth.h>
#  include <bluetooth/rfcomm.h>
#  include <bluetooth/hci.h>
#  include <bluetooth/hci_lib.h>
#  include <bluetooth/sdp.h>
#  include <bluetooth/sdp_lib.h>
#endif

#include "ax25dump.hpp"

// -------------------------------------------------------------------------
// Global state
// -------------------------------------------------------------------------
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_transport_disc{false}; // set by transport disconnect callback
static void sigint_handler(int) { g_running = false; }

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------
static std::string ts() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::ostringstream ss;
    struct tm tm_info{};
    localtime_r(&t, &tm_info);
    ss << std::put_time(&tm_info, "%H:%M:%S") << "."
       << std::setw(3) << std::setfill('0') << ms.count();
    return ss.str();
}

static std::string hexdump(const uint8_t* d, size_t n) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < n; i++) ss << std::setw(2) << (int)d[i];
    return ss.str();
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static std::string hr(char c = '-', int n = 68) {
    return std::string(n, c);
}

static const char* DIM()   { static bool t = ::isatty(STDOUT_FILENO); return t ? "\033[2m" : ""; }
static const char* RESET() { static bool t = ::isatty(STDOUT_FILENO); return t ? "\033[0m" : ""; }

// -------------------------------------------------------------------------
// KISS decoder
// -------------------------------------------------------------------------
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

// -------------------------------------------------------------------------
// AX.25 decoder (display only)
// -------------------------------------------------------------------------
struct Ax25Info {
    std::string dest, src, type, ctrl_hex, summary;
    std::vector<std::string> via;
    std::vector<uint8_t> info;
    uint8_t ctrl_byte = 0;   // raw control byte for ctrl_detail()
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
    r.type = "?";
    if (n < 15) {
        r.type = "TRUNCATED";
        r.summary = "[too short: " + std::to_string(n) + " bytes]";
        return r;
    }
    auto [dest, _d]      = decode_addr(d, 0);
    auto [src, end_src]  = decode_addr(d, 7);
    r.dest = dest; r.src = src;

    int off = 14;
    while (!end_src && (size_t)(off + 7) <= n) {
        auto [rep, e] = decode_addr(d, off);
        r.via.push_back(rep);
        end_src = e;
        off += 7;
    }
    if ((size_t)off >= n) {
        r.type = "NO-CTRL";
        r.summary = src + " -> " + dest + "  [no control byte]";
        return r;
    }

    uint8_t ctrl = d[off];
    r.ctrl_byte = ctrl;
    {
        std::ostringstream ss;
        ss << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)ctrl;
        r.ctrl_hex = ss.str();
    }
    bool pf = (ctrl & 0x10) != 0;

    // Build via string for use in all frame type summaries
    std::string via_pfx;
    for (auto& v : r.via) via_pfx += " via " + v;

    if ((ctrl & 0x01) == 0) {                               // I-frame
        int ns = (ctrl >> 1) & 7, nr = (ctrl >> 5) & 7;
        r.type = "I";
        if ((size_t)(off + 2) < n) r.info = {d + off + 2, d + n};
        r.summary = src + " -> " + dest + via_pfx + "  [I(NS=" + std::to_string(ns)
                  + ",NR=" + std::to_string(nr) + (pf ? "P" : "") + ")]";
    } else if ((ctrl & 0x03) == 0x01) {                    // S-frame
        int nr = (ctrl >> 5) & 7;
        static constexpr std::pair<uint8_t, const char*> st[] =
            {{0x01,"RR"},{0x05,"RNR"},{0x09,"REJ"},{0x0D,"SREJ"}};
        const char* stype = "S?";
        for (auto& [v, n] : st) if ((ctrl & 0xF) == v) { stype = n; break; }
        r.type = stype;
        r.summary = src + " -> " + dest + via_pfx + "  [" + stype
                  + "(NR=" + std::to_string(nr) + (pf ? "P/F" : "") + ")]";
    } else {                                                 // U-frame
        uint8_t base = ctrl & ~0x10u;
        static constexpr std::pair<uint8_t, const char*> ut[] =
            {{0x2F,"SABM"},{0x43,"DISC"},{0x63,"UA"},{0x0F,"DM"},
             {0x87,"FRMR"},{0x03,"UI"}};
        const char* utype = nullptr;
        for (auto& [v, n] : ut)
            if (ctrl == v || ctrl == (uint8_t)(v | 0x10u) || base == v)
                { utype = n; break; }
        std::string ft;
        if (utype) { ft = utype; if (pf) ft += "(P/F)"; }
        else { std::ostringstream ss; ss << "U?0x" << std::hex << (int)ctrl; ft = ss.str(); }
        r.type = ft;
        std::string via_str;
        for (auto& v : r.via) via_str += " via " + v;
        r.summary = src + " -> " + dest + via_str + "  [" + ft + "]";
    }
    return r;
}

// Print the rich frame detail block (ctrl + hexdump-C) in dim.
// payload is the raw AX.25 frame bytes (after KISS unwrapping).
static void print_frame_detail(const Ax25Info& ax,
                                const uint8_t* payload, size_t payload_len)
{
    std::cout << DIM()
              << "           " << ctrl_detail(ax.ctrl_byte, payload_len) << "\n"
              << hex_dump(payload, payload_len, "           ")
              << RESET();
}

// -------------------------------------------------------------------------
// PTY setup
// -------------------------------------------------------------------------
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

// -------------------------------------------------------------------------
// UUID name lookup
//
// Bluetooth SIG assigns 16-bit numbers; in 128-bit UUIDs they appear as
// the first 4 bytes:  0000XXXX-????-????-????-????????????
// We extract XXXX regardless of the base UUID suffix, which is why vendor
// UUIDs like 0000XXXX-ba2a-... still resolve to the standard SIG names.
// -------------------------------------------------------------------------
static std::string uuid_name(const std::string& uuid) {
    // Well-known full 128-bit UUIDs (vendor / profile-specific)
    static const std::pair<const char*, const char*> full128[] = {
        // Nordic UART Service
        {"6e400001-b5a3-f393-e0a9-e50e24dcca9e", "Nordic UART Service"},
        {"6e400002-b5a3-f393-e0a9-e50e24dcca9e", "Nordic UART RX (write)"},
        {"6e400003-b5a3-f393-e0a9-e50e24dcca9e", "Nordic UART TX (notify)"},
    };
    std::string lo = lower(uuid);
    for (auto& [k, v] : full128)
        if (lo == k) return v;

    // Extract 16-bit SIG number from 0000XXXX-*
    // UUID format: 8-4-4-4-12  ->  positions 4-7 are the 16-bit number
    if (lo.size() >= 8) {
        unsigned val = 0;
        try { val = (unsigned)std::stoul(lo.substr(0, 8), nullptr, 16); }
        catch (...) { return "Unknown"; }
        // Only consider the lower 16 bits (upper 16 should be 0000 for SIG UUIDs)
        uint16_t sig = (uint16_t)(val & 0xFFFF);

        // Bluetooth SIG 16-bit UUIDs -- protocols
        static const std::pair<uint16_t, const char*> sig_uuids[] = {
            // Protocols
            {0x0001, "SDP"},
            {0x0003, "RFCOMM"},
            {0x0005, "TCS-BIN"},
            {0x0007, "ATT"},
            {0x0008, "OBEX"},
            {0x000F, "BNEP"},
            {0x0010, "UPNP"},
            {0x0011, "HIDP"},
            {0x0017, "AVCTP"},
            {0x0019, "AVDTP"},
            {0x001E, "MCAP Control Channel"},
            {0x001F, "MCAP Data Channel"},
            {0x0100, "L2CAP"},
            // Service classes (Classic BT)
            {0x1101, "Serial Port"},
            {0x1102, "LAN Access Using PPP"},
            {0x1103, "Dialup Networking"},
            {0x1104, "IrMC Sync"},
            {0x1105, "OBEX Object Push"},
            {0x1106, "OBEX File Transfer"},
            {0x1107, "IrMC Sync Command"},
            {0x1108, "Headset"},
            {0x1109, "Cordless Telephony"},
            {0x110A, "Audio Source"},
            {0x110B, "Audio Sink"},
            {0x110C, "A/V Remote Control Target"},
            {0x110D, "Advanced Audio Distribution"},
            {0x110E, "A/V Remote Control"},
            {0x110F, "A/V Remote Control Controller"},
            {0x1110, "Intercom"},
            {0x1111, "Fax"},
            {0x1112, "Headset Audio Gateway"},
            {0x1115, "PANU"},
            {0x1116, "NAP"},
            {0x1117, "GN"},
            {0x111E, "Handsfree"},
            {0x111F, "Handsfree Audio Gateway"},
            {0x1124, "HID"},
            {0x112D, "SIM Access"},
            {0x1131, "Headset HS"},
            {0x1132, "Message Access Server"},
            {0x1200, "PnP Information"},
            {0x1201, "Generic Networking"},
            {0x1202, "Generic File Transfer"},
            {0x1203, "Generic Audio"},
            {0x1204, "Generic Telephony"},
            // BLE GATT Services
            {0x1800, "Generic Access"},
            {0x1801, "Generic Attribute"},
            {0x1802, "Immediate Alert"},
            {0x1803, "Link Loss"},
            {0x1804, "Tx Power"},
            {0x1805, "Current Time Service"},
            {0x1807, "Next DST Change Service"},
            {0x1808, "Glucose"},
            {0x1809, "Health Thermometer"},
            {0x180A, "Device Information"},
            {0x180D, "Heart Rate"},
            {0x180F, "Battery Service"},
            {0x1810, "Blood Pressure"},
            {0x1812, "Human Interface Device"},
            {0x1816, "Cycling Speed and Cadence"},
            {0x1818, "Cycling Power"},
            {0x1819, "Location and Navigation"},
            {0x181A, "Environmental Sensing"},
            {0x181C, "User Data"},
            {0x181D, "Weight Scale"},
            {0x1826, "Fitness Machine"},
            {0x1827, "Mesh Provisioning Service"},
            {0x1828, "Mesh Proxy Service"},
            // BLE GATT Characteristics
            {0x2A00, "Device Name"},
            {0x2A01, "Appearance"},
            {0x2A04, "Peripheral Preferred Connection Params"},
            {0x2A05, "Service Changed"},
            {0x2A06, "Alert Level"},
            {0x2A07, "Tx Power Level"},
            {0x2A19, "Battery Level"},
            {0x2A1C, "Temperature Measurement"},
            {0x2A1D, "Temperature Type"},
            {0x2A23, "System ID"},
            {0x2A24, "Model Number String"},
            {0x2A25, "Serial Number String"},
            {0x2A26, "Firmware Revision String"},
            {0x2A27, "Hardware Revision String"},
            {0x2A28, "Software Revision String"},
            {0x2A29, "Manufacturer Name String"},
            {0x2A2A, "IEEE 11073-20601 Regulatory Cert"},
            {0x2A37, "Heart Rate Measurement"},
            {0x2A38, "Body Sensor Location"},
            {0x2A3F, "Alert Status"},
            {0x2A46, "New Alert"},
            {0x2A4D, "Report"},
            {0x2A50, "PnP ID"},
            {0x2A6D, "Pressure"},
            {0x2A6E, "Temperature"},
            {0x2A6F, "Humidity"},
            // GATT Descriptors
            {0x2900, "Characteristic Extended Properties"},
            {0x2901, "Characteristic User Description"},
            {0x2902, "Client Characteristic Configuration"},
            {0x2903, "Server Characteristic Configuration"},
            {0x2904, "Characteristic Presentation Format"},
            {0x2905, "Characteristic Aggregate Format"},
            {0x2906, "Valid Range"},
            {0x2908, "Report Reference"},
        };
        for (auto& [k, v] : sig_uuids)
            if (sig == k) return v;
    }
    return "Unknown";
}

// =========================================================================
// RadioTransport -- abstract interface for BLE and Classic BT
// =========================================================================
struct RadioTransport {
    virtual ~RadioTransport() = default;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    virtual void write(const uint8_t* data, size_t len) = 0;
    virtual int  read_fd() const { return -1; }          // -1 = callback-based (BLE)
    virtual void set_on_receive(std::function<void(const uint8_t*, size_t)>) {}
    virtual void set_on_disconnect(std::function<void()>) {}
    virtual const char* label() const = 0;               // "BLE" or "BT"
};

// =========================================================================
// BridgeConfig
// =========================================================================
struct BridgeConfig {
    std::string address, service_uuid, write_uuid, read_uuid;
    std::string link_path   = "/tmp/kiss"; // symlink -> PTY slave (empty = no link)
    int    mtu              = 517;
    double timeout          = 10.0;
    int    ble_ka_ms        = 5000;        // BLE keep-alive interval ms (0=off)
    int    server_port      = 0;           // TCP server port (0 = disabled)
    std::string server_host;               // bind address (empty = all interfaces)
    bool   monitor          = false;       // print per-frame hex + AX.25 decode
    bool   show_keepalive   = false;       // show BLE keep-alive in monitor output
    std::optional<bool> force_response;    // nullopt = auto-detect

    // Transport selection
    enum Transport { AUTO, BLE, BT };
    Transport transport = AUTO;

    // Classic BT specific
    int bt_channel = 0;  // RFCOMM channel (0 = auto-detect via SDP)
};

// =========================================================================
// BLE adapter helpers
// =========================================================================
static std::optional<SimpleBLE::Adapter> get_adapter() {
    if (!SimpleBLE::Adapter::bluetooth_enabled()) {
        std::cerr << "Bluetooth not enabled.\n";
        return {};
    }
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        std::cerr << "No Bluetooth adapters found.\n";
        return {};
    }
    return adapters[0];
}

// -------------------------------------------------------------------------
// Linux-only: tell BlueZ to do an active LE scan filtered to a service UUID.
// -------------------------------------------------------------------------
#ifdef __linux__
static void bluez_set_discovery_filter(const std::string& adapter_id,
                                       const std::string& service_uuid)
{
    if (service_uuid.empty()) return;

    DBusError err;
    dbus_error_init(&err);

    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return; // not fatal -- scan will proceed without the filter
    }

    std::string obj_path = "/org/bluez/" + adapter_id;

    DBusMessage* msg = dbus_message_new_method_call(
        "org.bluez", obj_path.c_str(), "org.bluez.Adapter1", "SetDiscoveryFilter");
    if (!msg) { dbus_connection_unref(conn); return; }

    // Build argument: a{sv}  ->  {"UUIDs": as["uuid"], "Transport": s"le"}
    DBusMessageIter args, dict, entry, variant, arr;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);

    // "UUIDs" -> ["service_uuid"]
    {
        const char* key = "UUIDs";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &arr);
        const char* uuid_c = service_uuid.c_str();
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &uuid_c);
        dbus_message_iter_close_container(&variant, &arr);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    // "Transport" -> "le"
    {
        const char* key = "Transport";
        const char* val = "le";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }

    dbus_message_iter_close_container(&args, &dict);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (reply) dbus_message_unref(reply);
    if (dbus_error_is_set(&err)) dbus_error_free(&err); // non-fatal
    dbus_connection_unref(conn);
}
#endif // __linux__

static std::optional<SimpleBLE::Peripheral>
find_peripheral(SimpleBLE::Adapter& adapter,
                const std::string& address,
                int timeout_ms,
                [[maybe_unused]] const std::string& service_uuid = "")
{
    std::string target = lower(address);
    std::optional<SimpleBLE::Peripheral> found;
    std::mutex mx;
    std::atomic<bool> done{false};

    // Match by address OR by name (identifier) -- case-insensitive
    auto check = [&](SimpleBLE::Peripheral p) {
        if (lower(p.address())    == target ||
            lower(p.identifier()) == target) {
            std::lock_guard<std::mutex> lk(mx);
            if (!found) { found = p; done = true; }
        }
    };

    adapter.set_callback_on_scan_found  ([&](SimpleBLE::Peripheral p) { check(p); });
    adapter.set_callback_on_scan_updated([&](SimpleBLE::Peripheral p) { check(p); });

#ifdef __linux__
    bluez_set_discovery_filter(adapter.identifier(), service_uuid);
#endif

    adapter.scan_start();
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (!done && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    adapter.scan_stop();
    return found;
}

// =========================================================================
// BleTransport -- wraps all SimpleBLE interactions
// =========================================================================
class BleTransport : public RadioTransport {
    const BridgeConfig& cfg_;
    std::optional<SimpleBLE::Adapter>     adapter_;
    std::optional<SimpleBLE::Peripheral>  peripheral_;

    // Async write queue + writer thread
    std::mutex                        tx_mx_;
    std::condition_variable           tx_cv_;
    std::deque<std::vector<uint8_t>>  tx_queue_;
    std::atomic<bool>                 tx_stop_{false};
    std::thread                       writer_thread_;

    // BLE write parameters (detected at connect time)
    int  chunk_size_  = 20;
    bool use_response_= false;

    // Callbacks
    std::function<void(const uint8_t*, size_t)> on_receive_;
    std::function<void()>                       on_disconnect_;

    // Disconnect flag
    std::atomic<bool> disconnected_{false};

    // Keep-alive timer
    using SteadyClock = std::chrono::steady_clock;
    std::atomic<SteadyClock::time_point> last_write_{SteadyClock::now()};

public:
    explicit BleTransport(const BridgeConfig& cfg) : cfg_(cfg) {}
    ~BleTransport() override { disconnect(); }

    const char* label() const override { return "BLE"; }
    int  read_fd() const override { return -1; }  // callback-based

    void set_on_receive(std::function<void(const uint8_t*, size_t)> cb) override {
        on_receive_ = std::move(cb);
    }
    void set_on_disconnect(std::function<void()> cb) override {
        on_disconnect_ = std::move(cb);
    }

    bool is_connected() const override {
        if (disconnected_) return false;
        if (!peripheral_) return false;
        // SimpleBLE::Peripheral::is_connected() is non-const, cast away const
        try { return const_cast<SimpleBLE::Peripheral&>(*peripheral_).is_connected(); }
        catch (...) { return false; }
    }

    // Keep-alive: returns true if a keep-alive was sent
    bool maybe_keepalive() {
        if (cfg_.ble_ka_ms <= 0) return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           SteadyClock::now() - last_write_.load()).count();
        if (elapsed >= cfg_.ble_ka_ms) {
            static const uint8_t kiss_null[] = {0xC0, 0xC0};
            write(kiss_null, 2);
            return true;
        }
        return false;
    }

    bool connect() override {
        disconnected_ = false;
        tx_stop_ = false;

        // Find adapter
        adapter_ = get_adapter();
        if (!adapter_) {
            std::cerr << "  No Bluetooth adapter available.\n";
            return false;
        }

        // Find peripheral
        peripheral_ = find_peripheral(*adapter_, cfg_.address,
                                      (int)(cfg_.timeout * 1000), cfg_.service_uuid);
        if (!peripheral_) {
            std::cerr << "[" << ts() << "]  Device " << cfg_.address << " not found.\n";
            return false;
        }

        try { peripheral_->connect(); }
        catch (const std::exception& e) {
            std::cerr << "[" << ts() << "]  Connect failed: " << e.what() << "\n";
            return false;
        }

        // Capability check
        bool can_wwr = false, can_wr = false;
        for (auto& svc : peripheral_->services()) {
            if (lower(svc.uuid()) != lower(cfg_.service_uuid)) continue;
            for (auto& chr : svc.characteristics()) {
                if (lower(chr.uuid()) != lower(cfg_.write_uuid)) continue;
                can_wwr = chr.can_write_command();
                can_wr  = chr.can_write_request();
            }
        }
        use_response_ = cfg_.force_response.has_value()
                       ? *cfg_.force_response
                       : (!can_wwr && can_wr);

        uint16_t mtu_val = 23;
        try { mtu_val = peripheral_->mtu(); } catch (...) {}
        if (mtu_val < 23) mtu_val = 23;
        chunk_size_ = std::max(1, std::min(cfg_.mtu, (int)mtu_val) - 3);

        std::cout << "  Connected.  MTU=" << mtu_val
                  << "  chunk=" << chunk_size_ << "b"
                  << "  wwr=" << (can_wwr ? "yes" : "no")
                  << "  response=" << (use_response_ ? "yes" : "no") << "\n";
        if (cfg_.ble_ka_ms > 0)
            std::cout << "  BLE keep-alive: " << cfg_.ble_ka_ms / 1000
                      << "s  (KISS null writes)\n";
        std::cout.flush();

        // Disconnect callback
        peripheral_->set_callback_on_disconnected([this]() {
            disconnected_ = true;
            if (on_disconnect_) on_disconnect_();
        });

        // Notify subscription
        peripheral_->notify(cfg_.service_uuid, cfg_.read_uuid,
            [this](SimpleBLE::ByteArray raw) {
                if (on_receive_) {
                    on_receive_(raw.data(), raw.size());
                }
            });

        // Start writer thread
        last_write_.store(SteadyClock::now());
        writer_thread_ = std::thread([this]() {
            while (!tx_stop_) {
                std::vector<uint8_t> pkt;
                {
                    std::unique_lock<std::mutex> lk(tx_mx_);
                    tx_cv_.wait_for(lk, std::chrono::milliseconds(50),
                        [this]{ return !tx_queue_.empty() || tx_stop_.load(); });
                    if (tx_queue_.empty()) continue;
                    pkt = std::move(tx_queue_.front());
                    tx_queue_.pop_front();
                }
                try {
                    for (int i = 0; i < (int)pkt.size(); i += chunk_size_) {
                        int clen = std::min(chunk_size_, (int)pkt.size() - i);
                        SimpleBLE::ByteArray chunk(pkt.data() + i, pkt.data() + i + clen);
                        if (use_response_)
                            peripheral_->write_request(cfg_.service_uuid, cfg_.write_uuid, chunk);
                        else
                            peripheral_->write_command(cfg_.service_uuid, cfg_.write_uuid, chunk);
                    }
                    last_write_.store(SteadyClock::now());
                } catch (const std::exception& e) {
                    std::cerr << "  BLE write error: " << e.what() << "\n";
                }
            }
        });

        return true;
    }

    void write(const uint8_t* data, size_t len) override {
        std::lock_guard<std::mutex> lk(tx_mx_);
        tx_queue_.emplace_back(data, data + len);
        tx_cv_.notify_one();
    }

    void disconnect() override {
        // Stop writer thread
        tx_stop_ = true;
        tx_cv_.notify_one();
        if (writer_thread_.joinable()) writer_thread_.join();

        // BLE cleanup
        if (peripheral_) {
            try { peripheral_->unsubscribe(cfg_.service_uuid, cfg_.read_uuid); } catch (...) {}
            try { peripheral_->disconnect(); } catch (...) {}
        }
        peripheral_.reset();
        adapter_.reset();
    }
};

// =========================================================================
// BtTransport -- Classic Bluetooth RFCOMM (Linux only)
// =========================================================================
#ifdef __linux__

// SDP lookup for SPP UUID 0x1101 -- returns RFCOMM channel, or -1
static int sdp_find_rfcomm_channel(const std::string& address) {
    bdaddr_t target;
    str2ba(address.c_str(), &target);

    bdaddr_t any_addr;
    bacpy(&any_addr, BDADDR_ANY);

    sdp_session_t* session = sdp_connect(&any_addr, &target, SDP_RETRY_IF_BUSY);
    if (!session) {
        std::cerr << "  SDP connect failed: " << strerror(errno) << "\n";
        return -1;
    }

    // Search for SPP UUID 0x1101
    uuid_t svc_uuid;
    sdp_uuid16_create(&svc_uuid, SERIAL_PORT_SVCLASS_ID);

    sdp_list_t* search_list = sdp_list_append(nullptr, &svc_uuid);
    uint32_t range = 0x0000FFFF;
    sdp_list_t* attrid_list = sdp_list_append(nullptr, &range);

    sdp_list_t* response_list = nullptr;
    int err = sdp_service_search_attr_req(session, search_list,
                                           SDP_ATTR_REQ_RANGE, attrid_list,
                                           &response_list);
    sdp_list_free(search_list, nullptr);
    sdp_list_free(attrid_list, nullptr);

    int channel = -1;
    if (err == 0) {
        for (sdp_list_t* r = response_list; r && channel < 0; r = r->next) {
            sdp_record_t* rec = (sdp_record_t*)r->data;
            sdp_list_t* proto_list = nullptr;
            if (sdp_get_access_protos(rec, &proto_list) == 0) {
                for (sdp_list_t* p = proto_list; p && channel < 0; p = p->next) {
                    sdp_list_t* pds = (sdp_list_t*)p->data;
                    for (sdp_list_t* d = pds; d && channel < 0; d = d->next) {
                        sdp_data_t* pd = (sdp_data_t*)d->data;
                        // Walk the protocol descriptor
                        for (; pd; pd = pd->next) {
                            if (pd->dtd == SDP_UUID16 &&
                                sdp_uuid_to_proto(&pd->val.uuid) == RFCOMM_UUID) {
                                // Next element is the channel number
                                pd = pd->next;
                                if (pd) channel = pd->val.uint8;
                                break;
                            }
                        }
                    }
                }
                sdp_list_free(proto_list, nullptr);
            }
            sdp_record_free(rec);
        }
        sdp_list_free(response_list, nullptr);
    }

    sdp_close(session);
    return channel;
}

class BtTransport : public RadioTransport {
    std::string address_;
    int channel_;
    int fd_ = -1;

    std::function<void()> on_disconnect_;

public:
    BtTransport(const std::string& address, int channel)
        : address_(address), channel_(channel) {}
    ~BtTransport() override { disconnect(); }

    const char* label() const override { return "BT"; }
    int  read_fd() const override { return fd_; }

    void set_on_disconnect(std::function<void()> cb) override {
        on_disconnect_ = std::move(cb);
    }

    bool is_connected() const override { return fd_ >= 0; }

    bool connect() override {
        int ch = channel_;

        // Auto-detect via SDP if channel is 0
        if (ch == 0) {
            std::cout << "  SDP lookup for SPP on " << address_ << "...\n";
            std::cout.flush();
            ch = sdp_find_rfcomm_channel(address_);
            if (ch < 0) {
                std::cerr << "  SDP: no SPP service found.  Use --channel to specify manually.\n";
                return false;
            }
            std::cout << "  SDP: found SPP on RFCOMM channel " << ch << "\n";
        }

        fd_ = ::socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
        if (fd_ < 0) {
            std::cerr << "  RFCOMM socket: " << strerror(errno) << "\n";
            return false;
        }

        struct sockaddr_rc addr{};
        addr.rc_family  = AF_BLUETOOTH;
        addr.rc_channel = (uint8_t)ch;
        str2ba(address_.c_str(), &addr.rc_bdaddr);

        std::cout << "  Connecting RFCOMM channel " << ch << " on " << address_ << "...\n";
        std::cout.flush();

        if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "  RFCOMM connect: " << strerror(errno) << "\n";
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Set non-blocking after connect
        int fl = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);

        std::cout << "  Connected.  RFCOMM channel=" << ch << "\n";
        std::cout.flush();
        return true;
    }

    void write(const uint8_t* data, size_t len) override {
        if (fd_ < 0) return;
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::write(fd_, data + sent, len - sent);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Brief wait for buffer space
                    fd_set wfds;
                    FD_ZERO(&wfds);
                    FD_SET(fd_, &wfds);
                    struct timeval tv{0, 50000}; // 50ms
                    if (::select(fd_ + 1, nullptr, &wfds, nullptr, &tv) <= 0) break;
                    continue;
                }
                std::cerr << "  BT write error: " << strerror(errno) << "\n";
                ::close(fd_);
                fd_ = -1;
                if (on_disconnect_) on_disconnect_();
                return;
            }
            sent += (size_t)n;
        }
    }

    void disconnect() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
};

#elif defined(__APPLE__)

#include "bt_rfcomm_macos.h"

class BtTransport : public RadioTransport {
    std::string address_;
    int channel_;
    bt_macos_handle_t handle_ = nullptr;
    std::function<void()> on_disconnect_;

public:
    BtTransport(const std::string& addr, int ch)
        : address_(addr), channel_(ch) {}
    ~BtTransport() override { disconnect(); }

    const char* label() const override { return "BT"; }
    int  read_fd() const override { return handle_ ? bt_macos_read_fd(handle_) : -1; }

    void set_on_disconnect(std::function<void()> cb) override {
        on_disconnect_ = std::move(cb);
    }

    bool is_connected() const override {
        return handle_ && bt_macos_is_connected(handle_);
    }

    bool connect() override {
        handle_ = bt_macos_connect(address_.c_str(), channel_);
        return handle_ != nullptr;
    }

    void write(const uint8_t* data, size_t len) override {
        if (handle_) bt_macos_write(handle_, data, len);
    }

    void disconnect() override {
        if (handle_) {
            bt_macos_disconnect(handle_);
            handle_ = nullptr;
        }
    }
};

#else
// Unsupported platform stub
class BtTransport : public RadioTransport {
public:
    BtTransport(const std::string&, int) {}
    ~BtTransport() override = default;
    const char* label() const override { return "BT"; }
    bool connect() override {
        std::cerr << "Classic Bluetooth (RFCOMM) is not supported on this platform.\n";
        return false;
    }
    void disconnect() override {}
    bool is_connected() const override { return false; }
    void write(const uint8_t*, size_t) override {}
};

#endif // __linux__

// =========================================================================
// SCAN modes
// =========================================================================

// -- BLE scan (all platforms) --
static void do_ble_scan(double timeout_s) {
    auto opt = get_adapter();
    if (!opt) return;
    auto& adapter = *opt;

    struct Entry { SimpleBLE::Peripheral p; int rssi; };
    std::vector<Entry> found;
    std::mutex mx;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        std::lock_guard<std::mutex> lk(mx);
        std::string addr = lower(p.address());
        for (auto& e : found)
            if (lower(e.p.address()) == addr) return;
        found.push_back({p, p.rssi()});
    });

    std::cout << "Scanning for BLE devices (" << (int)timeout_s << "s)...\n\n";
    adapter.scan_for((int)(timeout_s * 1000));

    std::sort(found.begin(), found.end(),
              [](const Entry& a, const Entry& b){ return a.rssi > b.rssi; });

    for (auto& [p, rssi] : found) {
        std::cout << hr() << "\n";
        std::cout << "  Name   : " << (p.identifier().empty() ? "(no name)" : p.identifier()) << "\n";
        std::cout << "  Address: " << p.address() << "\n";
        std::cout << "  RSSI   : " << rssi << " dBm\n";
        auto svcs = p.services();
        if (!svcs.empty()) {
            std::cout << "  Services advertised:\n";
            for (auto& s : svcs)
                std::cout << "    " << s.uuid()
                          << "  (" << uuid_name(s.uuid()) << ")\n";
        }
        std::cout << "\n";
    }
    std::cout << hr('=') << "\n";
    std::cout << "Found " << found.size() << " BLE device(s).\n";
    std::cout << "\nNext step:\n  bt_kiss_bridge --ble --inspect <ADDRESS>\n";
}

// -- Classic BT scan (Linux only) --
#ifdef __linux__
static void do_bt_scan(double timeout_s) {
    int dev_id = hci_get_route(nullptr);
    if (dev_id < 0) {
        std::cerr << "No Bluetooth adapter found (hci_get_route).\n";
        return;
    }
    int sock = hci_open_dev(dev_id);
    if (sock < 0) {
        std::cerr << "Cannot open HCI device: " << strerror(errno) << "\n";
        return;
    }

    // Duration in 1.28s units
    int duration = std::max(1, (int)(timeout_s / 1.28));
    int max_rsp = 255;
    inquiry_info* ii = nullptr;

    std::cout << "Scanning for Classic BT devices (" << (int)timeout_s << "s)...\n\n";

    int num_rsp = hci_inquiry(dev_id, duration, max_rsp, nullptr, &ii, IREQ_CACHE_FLUSH);
    if (num_rsp < 0) {
        std::cerr << "HCI inquiry failed: " << strerror(errno) << "\n";
        close(sock);
        return;
    }

    for (int i = 0; i < num_rsp; i++) {
        char addr_str[19]{};
        ba2str(&ii[i].bdaddr, addr_str);

        char name[248]{};
        if (hci_read_remote_name(sock, &ii[i].bdaddr, sizeof(name), name, 0) < 0)
            std::strcpy(name, "(unknown)");

        uint32_t cod = (ii[i].dev_class[2] << 16) |
                       (ii[i].dev_class[1] << 8)  |
                        ii[i].dev_class[0];

        std::cout << hr() << "\n";
        std::cout << "  Name   : " << name << "\n";
        std::cout << "  Address: " << addr_str << "\n";
        std::cout << "  CoD    : 0x" << std::hex << std::setw(6) << std::setfill('0')
                  << cod << std::dec << "\n";
        std::cout << "\n";
    }

    std::cout << hr('=') << "\n";
    std::cout << "Found " << num_rsp << " Classic BT device(s).\n";
    std::cout << "\nNext step:\n  bt_kiss_bridge --bt --inspect <ADDRESS>\n";

    bt_free(ii);
    close(sock);
}
#elif defined(__APPLE__)
static void do_bt_scan(double timeout_s) {
    bt_macos_scan(timeout_s);
}
#else
static void do_bt_scan([[maybe_unused]] double timeout_s) {
    std::cerr << "Classic Bluetooth scan is not supported on this platform.\n";
}
#endif

// Dispatch scan based on transport preference
static void do_scan(double timeout_s, BridgeConfig::Transport transport) {
    if (transport == BridgeConfig::BT) {
        do_bt_scan(timeout_s);
    } else if (transport == BridgeConfig::BLE) {
        do_ble_scan(timeout_s);
    } else {
        // AUTO: scan both
        do_ble_scan(timeout_s);
#if defined(__linux__) || defined(__APPLE__)
        std::cout << "\n";
        do_bt_scan(timeout_s);
#endif
    }
}

// =========================================================================
// INSPECT modes
// =========================================================================

// -- BLE inspect (all platforms) --
static void do_ble_inspect(const std::string& address) {
    auto opt = get_adapter();
    if (!opt) return;
    auto& adapter = *opt;

    std::cout << "Searching for " << address << " (BLE)...\n";
    auto popt = find_peripheral(adapter, address, 10000);
    if (!popt) { std::cerr << "Device not found.\n"; return; }
    auto& p = *popt;

    std::cout << "Connecting...\n";
    try { p.connect(); }
    catch (const std::exception& e) { std::cerr << "Connect failed: " << e.what() << "\n"; return; }

    std::string dev_name = p.identifier().empty() ? address : p.identifier();
    std::cout << "Connected: " << dev_name << "  MTU=" << p.mtu() << "\n\n";

    // Candidates for the bridge command suggestion
    std::string best_svc, best_write, best_read;

    for (auto& svc : p.services()) {
        std::string sname = uuid_name(svc.uuid());
        std::cout << "SERVICE " << svc.uuid() << ": " << sname << "\n";

        for (auto& chr : svc.characteristics()) {
            // Build capability list
            std::vector<std::string> caps;
            if (chr.can_read())            caps.push_back("read");
            if (chr.can_notify())          caps.push_back("notify");
            if (chr.can_indicate())        caps.push_back("indicate");
            if (chr.can_write_request())   caps.push_back("write");
            if (chr.can_write_command())   caps.push_back("write-without-response");

            std::string caps_str;
            for (size_t i = 0; i < caps.size(); ++i) {
                if (i) caps_str += ", ";
                caps_str += caps[i];
            }

            std::string cname = uuid_name(chr.uuid());
            std::cout << "     CHARACTERISTIC " << chr.uuid()
                      << ": " << cname
                      << "  [" << caps_str << "]\n";

            // Read current value for readable characteristics
            if (chr.can_read()) {
                try {
                    auto val = p.read(svc.uuid(), chr.uuid());
                    // Try to show as printable string, fall back to hex
                    bool printable = !val.empty();
                    for (auto b : val)
                        if (b < 0x20 || b > 0x7E) { printable = false; break; }
                    if (printable) {
                        std::cout << "         Value: \""
                                  << std::string(val.begin(), val.end()) << "\"\n";
                    } else if (!val.empty()) {
                        std::cout << "         Value: " << hexdump(val.data(), val.size()) << "\n";
                    }
                } catch (...) {}
            }

            // Descriptors
            for (auto& desc : chr.descriptors()) {
                std::string dname = uuid_name(desc.uuid());
                std::cout << "         DESCRIPTOR " << desc.uuid()
                          << ": " << dname << "\n";
            }

            // Track best candidates for bridge command
            if (best_svc.empty()) best_svc = svc.uuid();
            if (best_write.empty() &&
                (chr.can_write_command() || chr.can_write_request()))
                best_write = chr.uuid();
            if (best_read.empty() && (chr.can_notify() || chr.can_indicate()))
                best_read = chr.uuid();
        }
        std::cout << "\n";
    }

    std::cout << hr('=') << "\n";
    std::cout << "\nSuggested bridge command:\n";
    std::cout << "  bt_kiss_bridge --ble \\\n"
              << "      --device   " << address << " \\\n"
              << "      --service  " << (best_svc.empty()   ? "<SERVICE-UUID>"  : best_svc)   << " \\\n"
              << "      --write    " << (best_write.empty() ? "<WRITE-CHAR-UUID>": best_write) << " \\\n"
              << "      --read     " << (best_read.empty()  ? "<NOTIFY-CHAR-UUID>": best_read) << "\n";
    p.disconnect();
}

// -- Classic BT inspect (Linux only) --
#ifdef __linux__
static void do_bt_inspect(const std::string& address) {
    bdaddr_t target;
    str2ba(address.c_str(), &target);

    bdaddr_t any_addr;
    bacpy(&any_addr, BDADDR_ANY);

    std::cout << "Connecting SDP to " << address << "...\n";
    sdp_session_t* session = sdp_connect(&any_addr, &target, SDP_RETRY_IF_BUSY);
    if (!session) {
        std::cerr << "SDP connect failed: " << strerror(errno) << "\n";
        return;
    }

    // Search for all public services
    uuid_t pub_uuid;
    sdp_uuid16_create(&pub_uuid, PUBLIC_BROWSE_GROUP);

    sdp_list_t* search_list = sdp_list_append(nullptr, &pub_uuid);
    uint32_t range = 0x0000FFFF;
    sdp_list_t* attrid_list = sdp_list_append(nullptr, &range);

    sdp_list_t* response_list = nullptr;
    int err = sdp_service_search_attr_req(session, search_list,
                                           SDP_ATTR_REQ_RANGE, attrid_list,
                                           &response_list);
    sdp_list_free(search_list, nullptr);
    sdp_list_free(attrid_list, nullptr);

    if (err < 0) {
        std::cerr << "SDP search failed: " << strerror(errno) << "\n";
        sdp_close(session);
        return;
    }

    std::cout << "\nServices on " << address << ":\n\n";
    int svc_count = 0;
    int spp_channel = -1;

    for (sdp_list_t* r = response_list; r; r = r->next) {
        sdp_record_t* rec = (sdp_record_t*)r->data;
        ++svc_count;

        // Service name
        char name_buf[256]{};
        if (sdp_get_service_name(rec, name_buf, sizeof(name_buf)) < 0)
            std::strcpy(name_buf, "(unnamed)");

        std::cout << hr() << "\n";
        std::cout << "  Service: " << name_buf << "\n";

        // Service class UUIDs
        sdp_list_t* svc_class_list = nullptr;
        if (sdp_get_service_classes(rec, &svc_class_list) == 0) {
            for (sdp_list_t* sc = svc_class_list; sc; sc = sc->next) {
                uuid_t* uuid = (uuid_t*)sc->data;
                char uuid_str[64]{};
                sdp_uuid2strn(uuid, uuid_str, sizeof(uuid_str));
                std::cout << "  UUID   : " << uuid_str
                          << "  (" << uuid_name(uuid_str) << ")\n";
            }
            sdp_list_free(svc_class_list, free);
        }

        // Protocol descriptors -- find RFCOMM channel
        sdp_list_t* proto_list = nullptr;
        if (sdp_get_access_protos(rec, &proto_list) == 0) {
            for (sdp_list_t* p = proto_list; p; p = p->next) {
                sdp_list_t* pds = (sdp_list_t*)p->data;
                for (sdp_list_t* d = pds; d; d = d->next) {
                    sdp_data_t* pd = (sdp_data_t*)d->data;
                    for (; pd; pd = pd->next) {
                        if (pd->dtd == SDP_UUID16 &&
                            sdp_uuid_to_proto(&pd->val.uuid) == RFCOMM_UUID) {
                            pd = pd->next;
                            if (pd) {
                                int ch = pd->val.uint8;
                                std::cout << "  RFCOMM : channel " << ch << "\n";
                                // Track first SPP channel
                                if (spp_channel < 0) spp_channel = ch;
                            }
                            break;
                        }
                    }
                }
            }
            sdp_list_free(proto_list, nullptr);
        }

        std::cout << "\n";
        sdp_record_free(rec);
    }
    sdp_list_free(response_list, nullptr);
    sdp_close(session);

    std::cout << hr('=') << "\n";
    std::cout << "Found " << svc_count << " service(s).\n";

    if (spp_channel >= 0) {
        std::cout << "\nSuggested bridge command:\n";
        std::cout << "  bt_kiss_bridge --bt --device " << address
                  << " --channel " << spp_channel << "\n";
    }
}
#elif defined(__APPLE__)
static void do_bt_inspect(const std::string& address) {
    bt_macos_inspect(address.c_str());
}
#else
static void do_bt_inspect([[maybe_unused]] const std::string& address) {
    std::cerr << "Classic Bluetooth inspect is not supported on this platform.\n";
}
#endif

// Dispatch inspect based on transport preference
static void do_inspect(const std::string& address, BridgeConfig::Transport transport) {
    if (transport == BridgeConfig::BT) {
        do_bt_inspect(address);
    } else if (transport == BridgeConfig::BLE) {
        do_ble_inspect(address);
    } else {
        // AUTO: try BLE
        do_ble_inspect(address);
    }
}

// =========================================================================
// BRIDGE (device) mode -- transport-agnostic
// =========================================================================
static void do_bridge(const BridgeConfig& cfg, RadioTransport& transport) {
    // -- PTY is created only when NOT in TCP-server mode --
    int master_fd = -1, slave_fd = -1;
    std::string slave_path;

    const bool tcp_mode = (cfg.server_port > 0);

    if (!tcp_mode) {
        if (!open_pty(master_fd, slave_fd, slave_path)) return;

        // Symlink PTY slave to a stable path
        if (!cfg.link_path.empty()) {
            ::unlink(cfg.link_path.c_str());  // remove stale link
            if (::symlink(slave_path.c_str(), cfg.link_path.c_str()) < 0)
                std::cerr << "Warning: symlink " << cfg.link_path
                          << ": " << strerror(errno) << "\n";
        }
    }

    std::string display_path = (!cfg.link_path.empty()) ? cfg.link_path : slave_path;

    std::cout << hr('=') << "\n";
    if (tcp_mode)
        std::cout << "  " << transport.label() << " KISS TCP Server Bridge"
                  << (cfg.monitor ? " + AX.25 Monitor" : "") << "\n";
    else
        std::cout << "  " << transport.label() << " KISS Serial Bridge"
                  << (cfg.monitor ? " + AX.25 Monitor" : "") << "\n";
    std::cout << hr('=') << "\n";
    std::cout << "  Device     : " << cfg.address << "\n";
    std::cout << "  Transport  : " << transport.label() << "\n";
    if (cfg.transport == BridgeConfig::BLE) {
        std::cout << "  Service    : " << cfg.service_uuid << "\n";
        if (tcp_mode) {
            std::cout << "  Read char  : " << cfg.read_uuid << "  (notify -> TCP clients)\n";
            std::cout << "  Write char : " << cfg.write_uuid << "  (TCP clients -> BLE)\n";
        } else {
            std::cout << "  Read char  : " << cfg.read_uuid << "  (notify -> PTY)\n";
            std::cout << "  Write char : " << cfg.write_uuid << "  (PTY -> BLE)\n";
        }
    } else if (cfg.transport == BridgeConfig::BT) {
        std::cout << "  RFCOMM ch  : " << (cfg.bt_channel > 0
                     ? std::to_string(cfg.bt_channel) : "auto (SDP)") << "\n";
    }
    std::cout << hr() << "\n";
    if (!tcp_mode) {
        std::cout << "  PTY device : " << slave_path << "\n";
        if (!cfg.link_path.empty())
            std::cout << "  Symlink    : " << cfg.link_path << "  -> " << slave_path << "\n";
        std::cout << "\n";
        std::cout << "  Example:\n      ax25tnc -c W1AW -r W1BBS-1 " << display_path << "\n";
    }
    std::cout << hr() << "\n  Connecting to " << transport.label() << "...\n";
    std::cout.flush();

    // -- Signal handlers --
    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    // -- TCP server setup (once -- survives reconnects) --
    int server_sock = -1;
    std::vector<int> tcp_clients;
    std::mutex mx;
    std::atomic<int> rx_frames{0}, tx_frames{0};

    if (cfg.server_port > 0) {
        server_sock = ::socket(AF_INET6, SOCK_STREAM, 0);
        bool bound = false;

        if (server_sock >= 0) {
            int one = 1;
            ::setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            int off = 0;
            ::setsockopt(server_sock, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

            struct sockaddr_in6 a{};
            a.sin6_family = AF_INET6;
            a.sin6_port   = htons(static_cast<uint16_t>(cfg.server_port));
            if (cfg.server_host.empty()) {
                a.sin6_addr = in6addr_any;
            } else {
                if (::inet_pton(AF_INET6, cfg.server_host.c_str(), &a.sin6_addr) <= 0)
                    a.sin6_addr = in6addr_any;
            }
            bound = (::bind(server_sock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        }

        if (!bound) {
            if (server_sock >= 0) { ::close(server_sock); server_sock = -1; }
            server_sock = ::socket(AF_INET, SOCK_STREAM, 0);
            if (server_sock >= 0) {
                int one = 1;
                ::setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                struct sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_port   = htons(static_cast<uint16_t>(cfg.server_port));
                if (cfg.server_host.empty()) {
                    a.sin_addr.s_addr = INADDR_ANY;
                } else {
                    ::inet_pton(AF_INET, cfg.server_host.c_str(), &a.sin_addr);
                }
                bound = (::bind(server_sock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
                if (!bound) { ::close(server_sock); server_sock = -1; }
            }
        }

        if (server_sock >= 0 && bound) {
            ::listen(server_sock, 8);
            int fl = ::fcntl(server_sock, F_GETFL, 0);
            ::fcntl(server_sock, F_SETFL, fl | O_NONBLOCK);
            std::cout << "  TCP server : "
                      << (cfg.server_host.empty() ? "*" : cfg.server_host)
                      << ":" << cfg.server_port << "\n";
        } else {
            std::cerr << "Warning: cannot start TCP server on port "
                      << cfg.server_port << ": " << strerror(errno) << "\n";
            server_sock = -1;
        }
    }

    // -- Reconnect loop --
    static constexpr int MAX_RECONNECTS  = 10;
    static constexpr int RECONNECT_PAUSE = 5000; // ms between attempts

    for (int attempt = 0; attempt <= MAX_RECONNECTS && g_running; ++attempt) {

        if (attempt > 0) {
            std::cout << "\n[" << ts() << "]  " << transport.label() << " reconnect "
                      << attempt << "/" << MAX_RECONNECTS
                      << " -- waiting " << RECONNECT_PAUSE / 1000 << " s...\n";
            std::cout.flush();
            // Interruptible sleep
            for (int i = 0; i < RECONNECT_PAUSE / 100 && g_running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!g_running) break;
            std::cout << "[" << ts() << "]  Reconnecting to " << cfg.address << "...\n";
            std::cout.flush();
        } else {
            std::cout << "  Connecting to " << transport.label() << "...\n";
            std::cout.flush();
        }

        g_transport_disc = false;   // reset for this session

        // Disconnect callback
        transport.set_on_disconnect([&]() {
            std::lock_guard<std::mutex> lk(mx);
            std::cout << "\n  [" << transport.label() << " disconnected]\n";
            std::cout.flush();
            g_transport_disc = true;
        });

        // For callback-based transports (BLE): set receive handler
        if (transport.read_fd() < 0) {
            transport.set_on_receive([&](const uint8_t* d, size_t n) {
                std::lock_guard<std::mutex> lk(mx);

                if (master_fd >= 0 && ::write(master_fd, d, n) < 0)
                    std::cerr << "  PTY write error: " << strerror(errno) << "\n";

                std::vector<int> dead;
                for (int fd : tcp_clients) {
                    if (::write(fd, d, n) < 0) dead.push_back(fd);
                }
                for (int fd : dead) {
                    tcp_clients.erase(
                        std::remove(tcp_clients.begin(), tcp_clients.end(), fd),
                        tcp_clients.end());
                    ::close(fd);
                    if (cfg.monitor)
                        std::cout << "[" << ts() << "]  TCP client fd=" << fd
                                  << " removed (write error)\n";
                }

                ++rx_frames;

                if (cfg.monitor) {
                    // Use a thread-local decoder so we don't need another lock
                    static thread_local KissDecoder decoder;
                    std::string t = ts();
                    auto frames = decoder.feed(d, n);

                    if (frames.empty()) {
                        std::cout << DIM() << "[" << t << "]  <- " << transport.label() << "  "
                                  << n << " bytes (buffering)\n"
                                  << hex_dump(d, n, "           ") << RESET();
                    }
                    for (auto& kf : frames) {
                        if (kf.type == 0 && !kf.payload.empty()) {
                            auto ax = decode_ax25(kf.payload.data(), kf.payload.size());
                            std::cout << "[" << t << "]  <- " << transport.label()
                                      << "  " << ax.summary << "\n";
                            print_frame_detail(ax, kf.payload.data(), kf.payload.size());
                        } else {
                            static constexpr const char* cmd_names[] =
                                {"DATA","TXDELAY","P","SLOTTIME","TXTAIL",
                                 "FULLDUPLEX","SETHW","?","?","?","?","?","?","?","?","RETURN"};
                            std::cout << DIM() << "[" << t << "]  <- " << transport.label()
                                      << "  KISS cmd="
                                      << cmd_names[kf.type & 0xF] << " port=" << kf.port
                                      << "  " << kf.payload.size() << " bytes\n";
                            if (!kf.payload.empty())
                                std::cout << hex_dump(kf.payload.data(), kf.payload.size(), "           ");
                            std::cout << RESET();
                        }
                    }
                    std::cout.flush();
                }
            });
        }

        if (!transport.connect()) continue; // retry

        if (tcp_mode)
            std::cout << "  Mode       : TCP server only (no PTY)\n";
        std::cout << (cfg.monitor ? "  Monitor on.  Ctrl-C to stop.\n"
                                  : "  Running.     Ctrl-C to stop.\n");
        std::cout.flush();

        // Per-session KISS decoders (reset each reconnect)
        KissDecoder rx_decoder;  // for fd-based transport reads
        KissDecoder tx_decoder;

        // -- Main select loop --
        while (g_running && !g_transport_disc && transport.is_connected()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            int max_fd = 0;
            if (master_fd >= 0) {
                FD_SET(master_fd, &rfds);
                max_fd = master_fd;
            }
            if (server_sock >= 0) {
                FD_SET(server_sock, &rfds);
                max_fd = std::max(max_fd, server_sock);
            }

            std::vector<int> clients_snap;
            {
                std::lock_guard<std::mutex> lk(mx);
                clients_snap = tcp_clients;
            }
            for (int fd : clients_snap) {
                FD_SET(fd, &rfds);
                max_fd = std::max(max_fd, fd);
            }

            // If transport has a readable fd (BT), add it
            int tfd = transport.read_fd();
            if (tfd >= 0) {
                FD_SET(tfd, &rfds);
                max_fd = std::max(max_fd, tfd);
            }

            struct timeval tv{0, 100000};  // 100 ms
            bool got_any = select(max_fd + 1, &rfds, nullptr, nullptr, &tv) > 0;

            // BLE keep-alive (only for BLE transport)
            auto* ble_tp = dynamic_cast<BleTransport*>(&transport);
            if (ble_tp) {
                bool sent = ble_tp->maybe_keepalive();
                if (sent && cfg.monitor && cfg.show_keepalive) {
                    std::lock_guard<std::mutex> lk(mx);
                    std::cout << "\n" << hr() << "\n";
                    std::cout << "[" << ts() << "]  BLE keep-alive  (KISS null)\n";
                    std::cout.flush();
                }
            }

            if (!got_any) continue;

            // Accept new TCP client
            if (server_sock >= 0 && FD_ISSET(server_sock, &rfds)) {
                int cli = ::accept(server_sock, nullptr, nullptr);
                if (cli >= 0) {
                    int one = 1;
                    ::setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    int fl = ::fcntl(cli, F_GETFL, 0);
                    ::fcntl(cli, F_SETFL, fl | O_NONBLOCK);
                    {
                        std::lock_guard<std::mutex> lk(mx);
                        tcp_clients.push_back(cli);
                        if (cfg.monitor) {
                            std::cout << "\n" << hr() << "\n";
                            std::cout << "[" << ts() << "]  TCP client connected  fd="
                                      << cli << "\n";
                            std::cout.flush();
                        }
                    }
                }
            }

            // Read from TCP clients -> transport
            for (int fd : clients_snap) {
                if (!FD_ISSET(fd, &rfds)) continue;
                uint8_t tbuf[4096];
                ssize_t tn = ::read(fd, tbuf, sizeof(tbuf));
                if (tn <= 0) {
                    ::close(fd);
                    {
                        std::lock_guard<std::mutex> lk(mx);
                        tcp_clients.erase(
                            std::remove(tcp_clients.begin(), tcp_clients.end(), fd),
                            tcp_clients.end());
                        if (cfg.monitor) {
                            std::cout << "\n" << hr() << "\n";
                            std::cout << "[" << ts() << "]  TCP client disconnected  fd="
                                      << fd << "\n";
                            std::cout.flush();
                        }
                    }
                    continue;
                }

                ++tx_frames;
                if (cfg.monitor) {
                    std::lock_guard<std::mutex> lk(mx);
                    std::string t = ts();
                    auto frames = tx_decoder.feed(tbuf, (size_t)tn);
                    if (frames.empty()) {
                        std::cout << DIM() << "[" << t << "]  -> TCP  "
                                  << tn << " bytes (buffering)\n"
                                  << hex_dump(tbuf, (size_t)tn, "           ") << RESET();
                    }
                    for (auto& kf : frames) {
                        if (kf.type == 0 && !kf.payload.empty()) {
                            auto ax = decode_ax25(kf.payload.data(), kf.payload.size());
                            std::cout << "[" << t << "]  -> TCP  " << ax.summary << "\n";
                            print_frame_detail(ax, kf.payload.data(), kf.payload.size());
                        } else {
                            static constexpr const char* cmd_names[] =
                                {"DATA","TXDELAY","P","SLOTTIME","TXTAIL",
                                 "FULLDUPLEX","SETHW","?","?","?","?","?","?","?","?","RETURN"};
                            std::cout << DIM() << "[" << t << "]  -> TCP  KISS cmd="
                                      << cmd_names[kf.type & 0xF] << " port=" << kf.port
                                      << "  " << kf.payload.size() << " bytes\n";
                            if (!kf.payload.empty())
                                std::cout << hex_dump(kf.payload.data(), kf.payload.size(), "           ");
                            std::cout << RESET();
                        }
                    }
                    std::cout.flush();
                }
                transport.write(tbuf, (size_t)tn);
            }

            // Read from transport fd -> PTY + TCP clients (fd-based transports: BT)
            if (tfd >= 0 && FD_ISSET(tfd, &rfds)) {
                uint8_t rbuf[4096];
                ssize_t rn = ::read(tfd, rbuf, sizeof(rbuf));
                if (rn <= 0) {
                    // Transport read error -- treat as disconnect
                    if (rn == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                        std::lock_guard<std::mutex> lk(mx);
                        std::cout << "\n  [" << transport.label() << " read error / disconnected]\n";
                        std::cout.flush();
                        g_transport_disc = true;
                        continue;
                    }
                } else {
                    std::lock_guard<std::mutex> lk(mx);

                    if (master_fd >= 0 && ::write(master_fd, rbuf, (size_t)rn) < 0)
                        std::cerr << "  PTY write error: " << strerror(errno) << "\n";

                    std::vector<int> dead;
                    for (int cfd : tcp_clients) {
                        if (::write(cfd, rbuf, (size_t)rn) < 0) dead.push_back(cfd);
                    }
                    for (int cfd : dead) {
                        tcp_clients.erase(
                            std::remove(tcp_clients.begin(), tcp_clients.end(), cfd),
                            tcp_clients.end());
                        ::close(cfd);
                        if (cfg.monitor)
                            std::cout << "[" << ts() << "]  TCP client fd=" << cfd
                                      << " removed (write error)\n";
                    }

                    ++rx_frames;

                    if (cfg.monitor) {
                        std::string t = ts();
                        auto frames = rx_decoder.feed(rbuf, (size_t)rn);

                        if (frames.empty()) {
                            std::cout << DIM() << "[" << t << "]  <- " << transport.label()
                                      << "  " << rn << " bytes (buffering)\n"
                                      << hex_dump(rbuf, (size_t)rn, "           ") << RESET();
                        }
                        for (auto& kf : frames) {
                            if (kf.type == 0 && !kf.payload.empty()) {
                                auto ax = decode_ax25(kf.payload.data(), kf.payload.size());
                                std::cout << "[" << t << "]  <- " << transport.label()
                                          << "  " << ax.summary << "\n";
                                print_frame_detail(ax, kf.payload.data(), kf.payload.size());
                            } else {
                                static constexpr const char* cmd_names[] =
                                    {"DATA","TXDELAY","P","SLOTTIME","TXTAIL",
                                     "FULLDUPLEX","SETHW","?","?","?","?","?","?","?","?","RETURN"};
                                std::cout << DIM() << "[" << t << "]  <- " << transport.label()
                                          << "  KISS cmd="
                                          << cmd_names[kf.type & 0xF] << " port=" << kf.port
                                          << "  " << kf.payload.size() << " bytes\n";
                                if (!kf.payload.empty())
                                    std::cout << hex_dump(kf.payload.data(), kf.payload.size(), "           ");
                                std::cout << RESET();
                            }
                        }
                        std::cout.flush();
                    }
                }
            }

            // Read from PTY -> transport (serial mode only)
            if (master_fd < 0 || !FD_ISSET(master_fd, &rfds)) continue;

            uint8_t buf[4096];
            ssize_t nr = ::read(master_fd, buf, sizeof(buf));
            if (nr <= 0) continue;

            ++tx_frames;
            if (cfg.monitor) {
                std::lock_guard<std::mutex> lk(mx);
                std::string t = ts();
                auto frames = tx_decoder.feed(buf, (size_t)nr);
                if (frames.empty()) {
                    std::cout << DIM() << "[" << t << "]  -> PTY  "
                              << nr << " bytes (buffering)\n"
                              << hex_dump(buf, (size_t)nr, "           ") << RESET();
                }
                for (auto& kf : frames) {
                    if (kf.type == 0 && !kf.payload.empty()) {
                        auto ax = decode_ax25(kf.payload.data(), kf.payload.size());
                        std::cout << "[" << t << "]  -> PTY  " << ax.summary << "\n";
                        print_frame_detail(ax, kf.payload.data(), kf.payload.size());
                    } else {
                        static constexpr const char* cmd_names[] =
                            {"DATA","TXDELAY","P","SLOTTIME","TXTAIL",
                             "FULLDUPLEX","SETHW","?","?","?","?","?","?","?","?","RETURN"};
                        std::cout << DIM() << "[" << t << "]  -> PTY  KISS cmd="
                                  << cmd_names[kf.type & 0xF] << " port=" << kf.port
                                  << "  " << kf.payload.size() << " bytes\n";
                        if (!kf.payload.empty())
                            std::cout << hex_dump(kf.payload.data(), kf.payload.size(), "           ");
                        std::cout << RESET();
                    }
                }
                std::cout.flush();
            }
            transport.write(buf, (size_t)nr);
        }  // end session main loop

        transport.disconnect();

        if (!g_running) break;   // SIGINT -- stop immediately

        // g_transport_disc is true here: outer for-loop will retry if attempts remain
        if (attempt == MAX_RECONNECTS) {
            std::cout << "[" << ts()
                      << "]  Max reconnect attempts reached.  Giving up.\n";
        }

    }  // end reconnect loop

    // -- Final cleanup (TCP server, PTY) --
    if (server_sock >= 0) { ::close(server_sock); }
    {
        std::lock_guard<std::mutex> lk(mx);
        for (int fd : tcp_clients) ::close(fd);
        tcp_clients.clear();
    }

    if (master_fd >= 0) ::close(master_fd);
    if (slave_fd  >= 0) ::close(slave_fd);
    if (!tcp_mode && !cfg.link_path.empty()) ::unlink(cfg.link_path.c_str());

    std::cout << "\n" << hr() << "\n";
    std::cout << "  Session ended.  RX frames: " << rx_frames.load()
              << "  TX frames: " << tx_frames.load() << "\n";
    std::cout << hr() << "\n";
}

// =========================================================================
// main
// =========================================================================
static void usage(const char* prog) {
    std::cerr <<
        "Bluetooth KISS TNC bridge + AX.25 monitor (BLE + Classic BT)\n\n"
        "Usage:\n"
        "  " << prog << " --scan [--timeout <s>] [--ble|--bt]\n"
        "  " << prog << " --inspect <ADDRESS> [--ble|--bt]\n"
        "  " << prog << " --ble --device <ADDRESS>\n"
        "             --service <UUID> --write <UUID> --read <UUID>\n"
        "             [--mtu <bytes>] [--write-with-response] [--monitor]\n"
        "             [--ble-ka <secs>]\n"
        "             PTY mode  : [--link <path>]          (default)\n"
        "             TCP mode  : --server-port <port> [--server-host <host>]\n"
        "  " << prog << " --bt --device <ADDRESS> [--channel <N>]\n"
        "             [--monitor]\n"
        "             PTY mode  : [--link <path>]          (default)\n"
        "             TCP mode  : --server-port <port> [--server-host <host>]\n\n"
        "Transport selection:\n"
        "  --ble                  Use Bluetooth Low Energy (GATT)\n"
        "  --bt                   Use Classic Bluetooth (RFCOMM)\n"
        "                         (Linux only; requires libbluetooth-dev)\n\n"
        "Transport modes (mutually exclusive):\n"
        "  PTY mode (default)     A virtual serial port is created; connect with:\n"
        "                           ax25tnc -c W1AW -r W1BBS-1 /tmp/kiss\n"
        "  TCP mode               --server-port enables a TCP KISS server;\n"
        "                         NO PTY is created.  Connect with:\n"
        "                           ax25tnc -c W1AW -r W1BBS-1 localhost:<port>\n\n"
        "Options (BLE device mode):\n"
        "  --service <UUID>       GATT service UUID\n"
        "  --write <UUID>         GATT write characteristic UUID\n"
        "  --read <UUID>          GATT notify characteristic UUID\n"
        "  --write-with-response  Force write-with-response (vs write-without-response)\n"
        "  --ble-ka <secs>        BLE keep-alive: send KISS null every N seconds\n"
        "                         when idle to prevent TNC inactivity disconnect\n"
        "                         Default: 5  (use 0 to disable)\n\n"
        "Options (Classic BT device mode):\n"
        "  --channel <N>          RFCOMM channel number (0 = auto-detect via SDP)\n\n"
        "Options (all device modes):\n"
        "  --link <path>          (PTY mode) symlink pointing to the PTY slave\n"
        "                         Default: /tmp/kiss  (use --link '' to disable)\n"
        "  --server-port <port>   (TCP mode) listen for KISS-over-TCP clients;\n"
        "                         disables PTY creation entirely\n"
        "  --server-host <host>   (TCP mode) bind to this address (default: all)\n"
        "  --mtu <bytes>          Max BLE chunk cap (default 517)\n"
        "  --monitor              Rich frame monitor: hexdump-C + AX.25 ctrl decode\n"
        "                         Same format as ax25kiss/ax25tnc (dim detail lines)\n"
        "  --show-keepalive       Show BLE keep-alive messages in monitor output\n"
        "                         (hidden by default to reduce noise)\n\n"
        "Examples:\n"
        "  # Scan for all Bluetooth devices\n"
        "  " << prog << " --scan --timeout 15\n"
        "  " << prog << " --scan --ble\n"
        "  " << prog << " --scan --bt\n\n"
        "  # Inspect device services\n"
        "  " << prog << " --ble --inspect AA:BB:CC:DD:EE:FF\n"
        "  " << prog << " --bt  --inspect AA:BB:CC:DD:EE:FF\n\n"
        "  # BLE bridge (PTY mode)\n"
        "  " << prog << " --ble --device AA:BB:CC:DD:EE:FF \\\n"
        "             --service 00000001-ba2a-46c9-ae49-01b0961f68bb \\\n"
        "             --write   00000003-ba2a-46c9-ae49-01b0961f68bb \\\n"
        "             --read    00000002-ba2a-46c9-ae49-01b0961f68bb \\\n"
        "             --link /tmp/kiss --monitor\n\n"
        "  # Classic BT bridge (auto-detect RFCOMM channel)\n"
        "  " << prog << " --bt --device AA:BB:CC:DD:EE:FF --monitor\n\n"
        "  # Classic BT bridge (explicit channel, TCP mode)\n"
        "  " << prog << " --bt --device AA:BB:CC:DD:EE:FF --channel 1 \\\n"
        "             --server-port 8001 --monitor\n\n"
        "Build:\n"
        "  make ble-deps          # clone + build SimpleBLE (one time)\n"
        "  make bt_kiss_bridge\n";
}

int main(int argc, char* argv[]) {
    std::string mode;
    BridgeConfig cfg;
    double timeout = 10.0;
    bool link_explicit = false;   // true only when --link is explicitly passed

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--scan")    { mode = "scan"; }
        else if (a == "--inspect"           && i+1 < argc) { mode = "inspect"; cfg.address      = argv[++i]; }
        else if (a == "--device"            && i+1 < argc) { mode = "device";  cfg.address      = argv[++i]; }
        else if (a == "--service"           && i+1 < argc) { cfg.service_uuid = argv[++i]; }
        else if (a == "--write"             && i+1 < argc) { cfg.write_uuid   = argv[++i]; }
        else if (a == "--read"              && i+1 < argc) { cfg.read_uuid    = argv[++i]; }
        else if (a == "--mtu"               && i+1 < argc) { cfg.mtu          = std::stoi(argv[++i]); }
        else if (a == "--timeout"           && i+1 < argc) { timeout          = std::stod(argv[++i]); }
        else if (a == "--ble-ka"            && i+1 < argc) { cfg.ble_ka_ms    = (int)(std::stod(argv[++i]) * 1000); }
        else if (a == "--server-port"       && i+1 < argc) { cfg.server_port  = std::stoi(argv[++i]); }
        else if (a == "--server-host"       && i+1 < argc) { cfg.server_host  = argv[++i]; }
        else if (a == "--channel"           && i+1 < argc) { cfg.bt_channel   = std::stoi(argv[++i]); }
        else if (a == "--ble")                              { cfg.transport    = BridgeConfig::BLE; }
        else if (a == "--bt")                               { cfg.transport    = BridgeConfig::BT; }
        else if (a == "--write-with-response")              { cfg.force_response = true; }
        else if (a == "--monitor")                          { cfg.monitor        = true; }
        else if (a == "--show-keepalive")                   { cfg.show_keepalive = true; }
        else if (a == "--link"              && i+1 < argc) { cfg.link_path = argv[++i]; link_explicit = true; }
        else { std::cerr << "Unknown argument: " << a << "\n"; usage(argv[0]); return 1; }
    }

    cfg.timeout = timeout;

    // In TCP mode the default /tmp/kiss symlink is irrelevant -- clear it
    // unless the user explicitly asked for --link (which is an error).
    if (cfg.server_port > 0) {
        if (link_explicit) {
            std::cerr << "Error: --server-port (TCP mode) and --link (PTY mode) are mutually exclusive.\n"
                         "       TCP mode does not create a PTY.  Drop --link or drop --server-port.\n";
            return 1;
        }
        cfg.link_path.clear();   // discard the default "/tmp/kiss"
    }

    if (mode.empty()) { usage(argv[0]); return 1; }

    // -- Scan / Inspect modes --
    if (mode == "scan") {
        do_scan(timeout, cfg.transport);
        return 0;
    }
    if (mode == "inspect") {
        do_inspect(cfg.address, cfg.transport);
        return 0;
    }

    // -- Device (bridge) mode --
    // Auto-detect transport if not specified
    if (cfg.transport == BridgeConfig::AUTO) {
        if (!cfg.service_uuid.empty())
            cfg.transport = BridgeConfig::BLE;
        else if (cfg.bt_channel > 0)
            cfg.transport = BridgeConfig::BT;
        else {
            std::cerr << "Cannot auto-detect transport.  Use --ble (with --service/--write/--read)\n"
                         "or --bt (with optional --channel).\n";
            return 1;
        }
    }

    // Validate required options per transport
    if (cfg.transport == BridgeConfig::BLE) {
        if (cfg.service_uuid.empty() || cfg.write_uuid.empty() || cfg.read_uuid.empty()) {
            std::cerr << "--ble --device requires --service, --write, and --read\n";
            return 1;
        }
        BleTransport ble(cfg);
        do_bridge(cfg, ble);
    } else {
        BtTransport bt(cfg.address, cfg.bt_channel);
        do_bridge(cfg, bt);
    }

    return 0;
}
