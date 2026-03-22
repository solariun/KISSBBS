// ============================================================================
// bt_ble_linux.cpp — Native BLE via BlueZ D-Bus (no SimpleBLE dependency)
//
// Implements the bt_ble_native.h C-linkage API using libdbus to communicate
// directly with the BlueZ daemon over the system D-Bus.
//
// Key design:
//   - Notify data arrives via PropertiesChanged signals on the read
//     characteristic's Value property.  A dedicated dispatch thread runs
//     dbus_connection_read_write_dispatch() in a loop and writes received
//     bytes to a pipe fd, making the transport select()-able.
//   - Scan, inspect, and connect are synchronous (blocking the caller).
//   - Thread safety: dbus_threads_init_default() is called once.
// ============================================================================

#ifdef __linux__

#include "bt_ble_native.h"

#include <dbus/dbus.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static void init_dbus_threads() {
    static bool done = false;
    if (!done) { dbus_threads_init_default(); done = true; }
}

// ── D-Bus call helpers ──────────────────────────────────────────────────────

// Call a method that takes no arguments and returns nothing interesting.
static bool dbus_call_void(DBusConnection* conn, const char* dest,
                           const char* path, const char* iface,
                           const char* method, int timeout_ms = 30000)
{
    DBusMessage* msg = dbus_message_new_method_call(dest, path, iface, method);
    if (!msg) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                             conn, msg, timeout_ms, &err);
    dbus_message_unref(msg);
    if (reply) { dbus_message_unref(reply); }
    bool ok = !dbus_error_is_set(&err);
    dbus_error_free(&err);
    return ok;
}

// Get a string property from an interface.
static std::string dbus_get_string_prop(DBusConnection* conn,
                                         const char* path,
                                         const char* iface,
                                         const char* prop)
{
    DBusMessage* msg = dbus_message_new_method_call(
        "org.bluez", path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return {};
    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                             conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return {};
    }

    DBusMessageIter args, variant;
    if (dbus_message_iter_init(reply, &args) &&
        dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&args, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
            const char* val = nullptr;
            dbus_message_iter_get_basic(&variant, &val);
            dbus_message_unref(reply);
            return val ? val : "";
        }
    }
    dbus_message_unref(reply);
    return {};
}

// Get a boolean property.
static bool dbus_get_bool_prop(DBusConnection* conn, const char* path,
                                const char* iface, const char* prop,
                                bool default_val = false)
{
    DBusMessage* msg = dbus_message_new_method_call(
        "org.bluez", path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return default_val;
    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                             conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return default_val;
    }

    dbus_bool_t val = default_val;
    DBusMessageIter args, variant;
    if (dbus_message_iter_init(reply, &args) &&
        dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&args, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN)
            dbus_message_iter_get_basic(&variant, &val);
    }
    dbus_message_unref(reply);
    return val;
}

// Get a uint16 property (for MTU).
static uint16_t dbus_get_uint16_prop(DBusConnection* conn, const char* path,
                                      const char* iface, const char* prop,
                                      uint16_t default_val = 0)
{
    DBusMessage* msg = dbus_message_new_method_call(
        "org.bluez", path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return default_val;
    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                             conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return default_val;
    }

    uint16_t val = default_val;
    DBusMessageIter args, variant;
    if (dbus_message_iter_init(reply, &args) &&
        dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&args, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT16)
            dbus_message_iter_get_basic(&variant, &val);
    }
    dbus_message_unref(reply);
    return val;
}

// Get an int16 property (for RSSI).
static int16_t dbus_get_int16_prop(DBusConnection* conn, const char* path,
                                    const char* iface, const char* prop,
                                    int16_t default_val = 0)
{
    DBusMessage* msg = dbus_message_new_method_call(
        "org.bluez", path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return default_val;
    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                             conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return default_val;
    }

    int16_t val = default_val;
    DBusMessageIter args, variant;
    if (dbus_message_iter_init(reply, &args) &&
        dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&args, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_INT16)
            dbus_message_iter_get_basic(&variant, &val);
    }
    dbus_message_unref(reply);
    return val;
}

// Get a string array property (e.g. Flags on GattCharacteristic1).
static std::vector<std::string> dbus_get_string_array_prop(
    DBusConnection* conn, const char* path,
    const char* iface, const char* prop)
{
    std::vector<std::string> result;
    DBusMessage* msg = dbus_message_new_method_call(
        "org.bluez", path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return result;
    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                             conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return result;
    }

    DBusMessageIter args, variant, arr;
    if (dbus_message_iter_init(reply, &args) &&
        dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&args, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&variant, &arr);
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
                const char* val = nullptr;
                dbus_message_iter_get_basic(&arr, &val);
                if (val) result.push_back(val);
                dbus_message_iter_next(&arr);
            }
        }
    }
    dbus_message_unref(reply);
    return result;
}

// ── GetManagedObjects helper ────────────────────────────────────────────────

struct BluezObject {
    std::string path;
    std::map<std::string, std::map<std::string, std::string>> interfaces;
    // interfaces[iface_name][prop_name] = value (string representation)
};

// Retrieve all BlueZ managed objects.  Lightweight: only extracts
// string-typed properties (sufficient for UUID, Address, Name, etc.).
static std::vector<BluezObject> get_managed_objects(DBusConnection* conn) {
    std::vector<BluezObject> objs;

    DBusMessage* msg = dbus_message_new_method_call(
        "org.bluez", "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!msg) return objs;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                             conn, msg, 10000, &err);
    dbus_message_unref(msg);
    if (!reply || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return objs;
    }

    // Reply is a{oa{sa{sv}}}
    DBusMessageIter root, obj_iter;
    if (!dbus_message_iter_init(reply, &root) ||
        dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return objs;
    }

    dbus_message_iter_recurse(&root, &obj_iter);
    while (dbus_message_iter_get_arg_type(&obj_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter obj_entry, iface_array;
        dbus_message_iter_recurse(&obj_iter, &obj_entry);

        const char* obj_path = nullptr;
        dbus_message_iter_get_basic(&obj_entry, &obj_path);
        dbus_message_iter_next(&obj_entry);

        BluezObject bo;
        bo.path = obj_path ? obj_path : "";

        if (dbus_message_iter_get_arg_type(&obj_entry) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&obj_entry, &iface_array);
            while (dbus_message_iter_get_arg_type(&iface_array) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter iface_entry, prop_array;
                dbus_message_iter_recurse(&iface_array, &iface_entry);

                const char* iface_name = nullptr;
                dbus_message_iter_get_basic(&iface_entry, &iface_name);
                dbus_message_iter_next(&iface_entry);

                std::string iface = iface_name ? iface_name : "";

                if (dbus_message_iter_get_arg_type(&iface_entry) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&iface_entry, &prop_array);
                    while (dbus_message_iter_get_arg_type(&prop_array) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter prop_entry, variant;
                        dbus_message_iter_recurse(&prop_array, &prop_entry);

                        const char* prop_name = nullptr;
                        dbus_message_iter_get_basic(&prop_entry, &prop_name);
                        dbus_message_iter_next(&prop_entry);

                        if (dbus_message_iter_get_arg_type(&prop_entry) == DBUS_TYPE_VARIANT) {
                            dbus_message_iter_recurse(&prop_entry, &variant);
                            int vtype = dbus_message_iter_get_arg_type(&variant);
                            if (vtype == DBUS_TYPE_STRING || vtype == DBUS_TYPE_OBJECT_PATH) {
                                const char* val = nullptr;
                                dbus_message_iter_get_basic(&variant, &val);
                                if (prop_name && val)
                                    bo.interfaces[iface][prop_name] = val;
                            }
                        }
                        dbus_message_iter_next(&prop_array);
                    }
                }
                dbus_message_iter_next(&iface_array);
            }
        }
        objs.push_back(std::move(bo));
        dbus_message_iter_next(&obj_iter);
    }

    dbus_message_unref(reply);
    return objs;
}

// ── SetDiscoveryFilter ──────────────────────────────────────────────────────

static void set_discovery_filter_le(DBusConnection* conn,
                                     const std::string& adapter_path)
{
    DBusMessage* msg = dbus_message_new_method_call(
        "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1",
        "SetDiscoveryFilter");
    if (!msg) return;

    DBusMessageIter args, dict, entry, variant;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);

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

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                             conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) {
        std::cerr << "  SetDiscoveryFilter warning: " << err.message << "\n";
        dbus_error_free(&err);
    } else if (reply) {
        dbus_message_unref(reply);
    }
}

// ── Find first adapter path ─────────────────────────────────────────────────

static std::string find_adapter(DBusConnection* conn) {
    auto objs = get_managed_objects(conn);
    for (auto& o : objs) {
        if (o.interfaces.count("org.bluez.Adapter1"))
            return o.path;
    }
    return {};
}

// ── Find device by address (among known objects) ────────────────────────────

static std::string find_device_path(DBusConnection* conn,
                                     const std::string& address)
{
    std::string target = lower(address);
    auto objs = get_managed_objects(conn);
    for (auto& o : objs) {
        auto it = o.interfaces.find("org.bluez.Device1");
        if (it == o.interfaces.end()) continue;
        auto ait = it->second.find("Address");
        if (ait != it->second.end() && lower(ait->second) == target)
            return o.path;
        // Also match by Name
        auto nit = it->second.find("Name");
        if (nit != it->second.end() && lower(nit->second) == target)
            return o.path;
    }
    return {};
}

// ── Wait for ServicesResolved ───────────────────────────────────────────────

static bool wait_services_resolved(DBusConnection* conn,
                                    const std::string& dev_path,
                                    double timeout_s)
{
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds((int)(timeout_s * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        if (dbus_get_bool_prop(conn, dev_path.c_str(),
                               "org.bluez.Device1", "ServicesResolved"))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// ── GATT enumeration ────────────────────────────────────────────────────────

struct GattChar {
    std::string path;
    std::string uuid;
    std::string service_uuid;
    std::vector<std::string> flags;  // read, write, write-without-response, notify, indicate

    bool can_write() const {
        for (auto& f : flags)
            if (f == "write" || f == "write-without-response") return true;
        return false;
    }
    bool can_write_without_response() const {
        for (auto& f : flags) if (f == "write-without-response") return true;
        return false;
    }
    bool can_notify() const {
        for (auto& f : flags) if (f == "notify" || f == "indicate") return true;
        return false;
    }
    bool can_read() const {
        for (auto& f : flags) if (f == "read") return true;
        return false;
    }
};

struct GattService {
    std::string path;
    std::string uuid;
    std::vector<GattChar> chars;
};

static std::vector<GattService> enumerate_gatt(DBusConnection* conn,
                                                const std::string& dev_path)
{
    std::vector<GattService> services;
    std::map<std::string, size_t> svc_idx; // path -> index

    auto objs = get_managed_objects(conn);

    // First pass: collect services under this device
    for (auto& o : objs) {
        if (o.path.find(dev_path + "/") != 0) continue;
        auto it = o.interfaces.find("org.bluez.GattService1");
        if (it == o.interfaces.end()) continue;

        GattService svc;
        svc.path = o.path;
        auto uid = it->second.find("UUID");
        svc.uuid = uid != it->second.end() ? uid->second : "";
        svc_idx[svc.path] = services.size();
        services.push_back(std::move(svc));
    }

    // Second pass: collect characteristics
    for (auto& o : objs) {
        if (o.path.find(dev_path + "/") != 0) continue;
        auto it = o.interfaces.find("org.bluez.GattCharacteristic1");
        if (it == o.interfaces.end()) continue;

        GattChar chr;
        chr.path = o.path;
        auto uid = it->second.find("UUID");
        chr.uuid = uid != it->second.end() ? uid->second : "";

        // Get the service path — it's the parent of the char path
        auto svc_it = it->second.find("Service");
        std::string svc_path;
        if (svc_it != it->second.end()) {
            svc_path = svc_it->second;
        } else {
            // Derive from path: /org/bluez/hci0/dev_XX/serviceXXXX/charXXXX
            auto pos = o.path.rfind('/');
            if (pos != std::string::npos) svc_path = o.path.substr(0, pos);
        }

        // Get flags
        chr.flags = dbus_get_string_array_prop(conn, o.path.c_str(),
                        "org.bluez.GattCharacteristic1", "Flags");

        auto idx = svc_idx.find(svc_path);
        if (idx != svc_idx.end()) {
            chr.service_uuid = services[idx->second].uuid;
            services[idx->second].chars.push_back(std::move(chr));
        }
    }

    return services;
}

// ── UUID name lookup (subset for inspect display) ───────────────────────────

static std::string uuid_short_name(const std::string& uuid) {
    // Extract 16-bit SIG number from 0000XXXX-0000-1000-8000-00805f9b34fb
    std::string lo = lower(uuid);
    if (lo.size() < 8) return "Unknown";
    unsigned val = 0;
    try { val = (unsigned)std::stoul(lo.substr(0, 8), nullptr, 16); }
    catch (...) { return "Unknown"; }
    uint16_t sig = (uint16_t)(val & 0xFFFF);

    static const std::pair<uint16_t, const char*> names[] = {
        // Protocol Identifiers (Bluetooth SIG assigned)
        {0x0001, "SDP"}, {0x0002, "UDP"}, {0x0003, "RFCOMM"},
        {0x0004, "TCP"}, {0x0005, "TCS-BIN"}, {0x0006, "TCS-AT"},
        {0x0007, "ATT"}, {0x0008, "OBEX"}, {0x0009, "IP"},
        {0x000A, "FTP"}, {0x000C, "HTTP"}, {0x000E, "WSP"},
        {0x000F, "BNEP"}, {0x0010, "UPNP"}, {0x0011, "HIDP"},
        {0x0012, "HardcopyControlChannel"},
        {0x0014, "HardcopyDataChannel"},
        {0x0016, "HardcopyNotification"},
        {0x0017, "AVCTP"}, {0x0019, "AVDTP"}, {0x001B, "CMTP"},
        {0x001E, "MCAPControlChannel"}, {0x001F, "MCAPDataChannel"},
        {0x0100, "L2CAP"},
        // Service Classes (Bluetooth SIG assigned)
        {0x1000, "Service Discovery Server"},
        {0x1001, "Browse Group Descriptor"},
        {0x1101, "Serial Port"}, {0x1102, "LAN Access Using PPP"},
        {0x1103, "Dialup Networking"}, {0x1104, "IrMC Sync"},
        {0x1105, "OBEX Object Push"}, {0x1106, "OBEX File Transfer"},
        {0x1107, "IrMC Sync Command"}, {0x1108, "Headset"},
        {0x1109, "Cordless Telephony"}, {0x110A, "Audio Source"},
        {0x110B, "Audio Sink"}, {0x110C, "A/V Remote Control Target"},
        {0x110D, "Advanced Audio"}, {0x110E, "A/V Remote Control"},
        {0x110F, "A/V Remote Control Controller"},
        {0x1110, "Intercom"}, {0x1111, "Fax"},
        {0x1112, "Headset Audio Gateway"},
        {0x1115, "PANU"}, {0x1116, "NAP"}, {0x1117, "GN"},
        {0x111E, "Handsfree"}, {0x111F, "Handsfree Audio Gateway"},
        {0x1124, "HID"}, {0x112D, "SIM Access"},
        {0x112F, "Phonebook Access (PCE)"},
        {0x1130, "Phonebook Access (PSE)"},
        {0x1132, "Message Access (MAS)"},
        {0x1133, "Message Access (MNS)"},
        // GATT Services
        {0x1800, "Generic Access"}, {0x1801, "Generic Attribute"},
        {0x1802, "Immediate Alert"}, {0x1803, "Link Loss"},
        {0x1804, "Tx Power"}, {0x1805, "Current Time"},
        {0x1806, "Reference Time Update"},
        {0x1808, "Glucose"}, {0x1809, "Health Thermometer"},
        {0x180A, "Device Information"}, {0x180D, "Heart Rate"},
        {0x180E, "Phone Alert Status"}, {0x180F, "Battery Service"},
        {0x1810, "Blood Pressure"}, {0x1811, "Alert Notification"},
        {0x1812, "Human Interface Device"}, {0x1813, "Scan Parameters"},
        {0x1814, "Running Speed and Cadence"},
        {0x1816, "Cycling Speed and Cadence"},
        {0x1818, "Cycling Power"}, {0x1819, "Location and Navigation"},
        {0x181A, "Environmental Sensing"},
        {0x181C, "User Data"}, {0x181D, "Weight Scale"},
        {0x181E, "Bond Management"},
        {0x1820, "Internet Protocol Support"},
        // GATT Characteristics
        {0x2A00, "Device Name"}, {0x2A01, "Appearance"},
        {0x2A02, "Peripheral Privacy Flag"},
        {0x2A04, "Peripheral Preferred Connection Parameters"},
        {0x2A05, "Service Changed"},
        {0x2A19, "Battery Level"}, {0x2A24, "Model Number"},
        {0x2A25, "Serial Number"}, {0x2A26, "Firmware Revision"},
        {0x2A27, "Hardware Revision"}, {0x2A28, "Software Revision"},
        {0x2A29, "Manufacturer Name"},
        {0x2A37, "Heart Rate Measurement"},
        {0x2A6E, "Temperature"}, {0x2A6F, "Humidity"},
        // GATT Descriptors
        {0x2900, "Characteristic Extended Properties"},
        {0x2901, "Characteristic User Description"},
        {0x2902, "Client Characteristic Configuration"},
        {0x2903, "Server Characteristic Configuration"},
        {0x2904, "Characteristic Presentation Format"},
        // Vendor-specific well-known
        {0xFFE0, "Vendor specific"}, {0xFFE1, "Vendor specific"},
    };
    for (auto& [k, v] : names) if (sig == k) return v;
    return "Unknown";
}

// ── Auto-detect: find service with both write+notify ────────────────────────

static bool is_standard_gap_gatt(const std::string& uuid) {
    std::string lo = lower(uuid);
    return lo == "00001800-0000-1000-8000-00805f9b34fb" ||
           lo == "00001801-0000-1000-8000-00805f9b34fb" ||
           lo == "1800" || lo == "1801";
}

struct AutoDetectResult {
    std::string service_uuid, write_uuid, read_uuid;
    std::string write_char_path, read_char_path;
};

static AutoDetectResult auto_detect_from_gatt(
    const std::vector<GattService>& services)
{
    AutoDetectResult best;
    int best_score = -1;

    // Score each candidate service: prefer write-without-response + notify
    //   +2 if write char has write-without-response (avoids "In Progress")
    //   +1 if read char has notify (better than indicate for streaming)
    for (auto& svc : services) {
        if (is_standard_gap_gatt(svc.uuid)) continue;

        // Find best write and read chars within this service
        const GattChar* w_chr = nullptr;
        const GattChar* n_chr = nullptr;
        for (auto& chr : svc.chars) {
            if (chr.can_write()) {
                if (!w_chr || (chr.can_write_without_response() &&
                               !w_chr->can_write_without_response()))
                    w_chr = &chr;
            }
            if (chr.can_notify()) {
                bool has_notify = false;
                for (auto& f : chr.flags) if (f == "notify") { has_notify = true; break; }
                if (!n_chr) {
                    n_chr = &chr;
                } else {
                    bool old_notify = false;
                    for (auto& f : n_chr->flags) if (f == "notify") { old_notify = true; break; }
                    if (has_notify && !old_notify) n_chr = &chr;
                }
            }
        }

        if (w_chr && n_chr) {
            int score = 0;
            if (w_chr->can_write_without_response()) score += 2;
            for (auto& f : n_chr->flags) if (f == "notify") { score += 1; break; }

            if (score > best_score) {
                best_score = score;
                best.service_uuid    = svc.uuid;
                best.write_uuid      = w_chr->uuid;
                best.read_uuid       = n_chr->uuid;
                best.write_char_path = w_chr->path;
                best.read_char_path  = n_chr->path;
            }
        }
    }

    if (best_score >= 0) return best;

    // Fallback — first writable + first notifiable globally
    AutoDetectResult r;
    for (auto& svc : services) {
        if (is_standard_gap_gatt(svc.uuid)) continue;
        for (auto& chr : svc.chars) {
            if (r.write_uuid.empty() && chr.can_write()) {
                r.write_uuid = chr.uuid;
                r.write_char_path = chr.path;
                if (r.service_uuid.empty()) r.service_uuid = svc.uuid;
            }
            if (r.read_uuid.empty() && chr.can_notify()) {
                r.read_uuid = chr.uuid;
                r.read_char_path = chr.path;
                if (r.service_uuid.empty()) r.service_uuid = svc.uuid;
            }
        }
    }
    return r;
}

// Find char path by UUID within the enumerated GATT tree
static std::string find_char_path(const std::vector<GattService>& services,
                                   const std::string& svc_uuid,
                                   const std::string& chr_uuid)
{
    std::string lo_svc = lower(svc_uuid);
    std::string lo_chr = lower(chr_uuid);
    for (auto& svc : services) {
        if (!lo_svc.empty() && lower(svc.uuid) != lo_svc) continue;
        for (auto& chr : svc.chars) {
            if (lower(chr.uuid) == lo_chr) return chr.path;
        }
    }
    return {};
}

// ── Connection handle ───────────────────────────────────────────────────────

struct BleLinuxHandle {
    // Two separate D-Bus connections to avoid thread-safety issues:
    // - conn_dispatch: owned by dispatch thread for signal delivery (notifications)
    // - conn_write: owned by caller thread for WriteValue and property queries
    DBusConnection* conn_dispatch = nullptr;
    DBusConnection* conn_write    = nullptr;

    std::string device_path;
    std::string adapter_path;
    std::string write_char_path;
    std::string read_char_path;
    std::string svc_uuid, write_uuid, read_uuid;

    // All readable characteristic paths (for wake-up)
    std::vector<std::string> readable_char_paths;

    int pipe_read  = -1;
    int pipe_write = -1;
    int mtu_val    = 23;
    int chunk_sz   = 20;
    bool use_response = false;
    bool can_wwr   = false; // write-without-response

    std::atomic<bool> connected{false};
    std::atomic<bool> stop{false};
    std::thread dispatch_thread;
};

// ── Signal filter: PropertiesChanged for notify Value ───────────────────────

static DBusHandlerResult notify_filter(DBusConnection*,
                                        DBusMessage* msg, void* user_data)
{
    auto* h = static_cast<BleLinuxHandle*>(user_data);

    if (!dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties",
                                "PropertiesChanged"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // Check path matches our read characteristic
    const char* path = dbus_message_get_path(msg);
    if (!path || h->read_char_path != path)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // Parse: (s, a{sv}, as) — interface, changed_props, invalidated
    DBusMessageIter args;
    if (!dbus_message_iter_init(msg, &args))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // Skip interface name
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    dbus_message_iter_next(&args);

    // Iterate changed properties dict
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessageIter dict;
    dbus_message_iter_recurse(&args, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(&dict, &entry);

        const char* prop_name = nullptr;
        dbus_message_iter_get_basic(&entry, &prop_name);
        dbus_message_iter_next(&entry);

        if (prop_name && std::string(prop_name) == "Value") {
            dbus_message_iter_recurse(&entry, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
                DBusMessageIter arr;
                dbus_message_iter_recurse(&variant, &arr);
                int count = dbus_message_iter_get_element_count(&variant);
                if (count > 0) {
                    // Get fixed array
                    DBusMessageIter sub;
                    dbus_message_iter_recurse(&variant, &sub);
                    if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_BYTE) {
                        const uint8_t* data = nullptr;
                        int n = 0;
                        dbus_message_iter_get_fixed_array(&sub, &data, &n);
                        if (data && n > 0 && h->pipe_write >= 0) {
                            // Write to pipe (non-blocking to avoid stalling BT thread)
                            (void)::write(h->pipe_write, data, (size_t)n);
                        }
                    }
                }
            }
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_next(&dict);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// Also watch for device disconnect
static DBusHandlerResult disconnect_filter(DBusConnection*,
                                            DBusMessage* msg, void* user_data)
{
    auto* h = static_cast<BleLinuxHandle*>(user_data);

    if (!dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties",
                                "PropertiesChanged"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char* path = dbus_message_get_path(msg);
    if (!path || h->device_path != path)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // Parse: (s, a{sv}, as)
    DBusMessageIter args;
    if (!dbus_message_iter_init(msg, &args))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // Check interface is Device1
    const char* iface = nullptr;
    dbus_message_iter_get_basic(&args, &iface);
    if (!iface || std::string(iface) != "org.bluez.Device1")
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    dbus_message_iter_next(&args);

    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessageIter dict;
    dbus_message_iter_recurse(&args, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(&dict, &entry);

        const char* prop_name = nullptr;
        dbus_message_iter_get_basic(&entry, &prop_name);
        dbus_message_iter_next(&entry);

        if (prop_name && std::string(prop_name) == "Connected") {
            dbus_message_iter_recurse(&entry, &variant);
            dbus_bool_t val = TRUE;
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN) {
                dbus_message_iter_get_basic(&variant, &val);
                if (!val) {
                    h->connected = false;
                    // Close write end -> EOF on read fd -> select detects disconnect
                    if (h->pipe_write >= 0) {
                        ::close(h->pipe_write);
                        h->pipe_write = -1;
                    }
                }
            }
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_next(&dict);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// ── WriteValue helper ───────────────────────────────────────────────────────
// Builds the D-Bus WriteValue message (shared between sync and async paths).

static DBusMessage* build_write_msg(const std::string& char_path,
                                      const uint8_t* data, size_t len,
                                      bool with_response)
{
    DBusMessage* msg = dbus_message_new_method_call(
        "org.bluez", char_path.c_str(),
        "org.bluez.GattCharacteristic1", "WriteValue");
    if (!msg) return nullptr;

    DBusMessageIter args, arr, dict, entry, variant;
    dbus_message_iter_init_append(msg, &args);

    // Byte array
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY,
                                     DBUS_TYPE_BYTE_AS_STRING, &arr);
    dbus_message_iter_append_fixed_array(&arr, DBUS_TYPE_BYTE, &data, (int)len);
    dbus_message_iter_close_container(&args, &arr);

    // Options dict: {"type": "command"} or {"type": "request"}
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    {
        const char* key = "type";
        const char* val = with_response ? "request" : "command";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }
    dbus_message_iter_close_container(&args, &dict);
    return msg;
}

// Write via a dedicated conn_write connection (no dispatch thread on it).
// Uses blocking send_with_reply_and_block — safe because conn_write is
// never used by the dispatch thread.
// Return: 1 = ok, 0 = permanent error, -1 = "In Progress" (transient, retry)
static int gatt_write_once(DBusConnection* conn_write, const std::string& char_path,
                            const uint8_t* data, size_t len,
                            bool with_response)
{
    DBusMessage* msg = build_write_msg(char_path, data, len, with_response);
    if (!msg) return 0;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                             conn_write, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply || dbus_error_is_set(&err)) {
        int result = 0;
        if (dbus_error_is_set(&err)) {
            if (std::string(err.name) == "org.bluez.Error.InProgress")
                result = -1; // transient — caller should retry
            else
                std::cerr << "  BLE write error: " << err.message << "\n";
        }
        dbus_error_free(&err);
        if (reply) dbus_message_unref(reply);
        return result;
    }

    dbus_message_unref(reply);
    return 1;
}

// Wrapper with retry on "In Progress" (up to ~2s with exponential backoff).
static bool gatt_write_value_async(DBusConnection* conn, const std::string& char_path,
                                     const uint8_t* data, size_t len,
                                     bool with_response)
{
    int backoff_ms = 20;
    for (int attempt = 0; attempt < 8; ++attempt) {
        int rc = gatt_write_once(conn, char_path, data, len, with_response);
        if (rc == 1) return true;   // success
        if (rc == 0) return false;  // permanent error
        // rc == -1: "In Progress" — wait and retry
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        backoff_ms = std::min(backoff_ms * 2, 500);
    }
    std::cerr << "  BLE write: gave up after retries (In Progress)\n";
    return false;
}

// ── ReadValue helper ────────────────────────────────────────────────────────

static std::vector<uint8_t> gatt_read_value(DBusConnection* conn,
                                              const std::string& char_path)
{
    std::vector<uint8_t> result;
    DBusMessage* msg = dbus_message_new_method_call(
        "org.bluez", char_path.c_str(),
        "org.bluez.GattCharacteristic1", "ReadValue");
    if (!msg) return result;

    // Empty options dict
    DBusMessageIter args, dict;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&args, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                             conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return result;
    }

    DBusMessageIter rargs;
    if (dbus_message_iter_init(reply, &rargs) &&
        dbus_message_iter_get_arg_type(&rargs) == DBUS_TYPE_ARRAY) {
        DBusMessageIter sub;
        dbus_message_iter_recurse(&rargs, &sub);
        if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_BYTE) {
            const uint8_t* data = nullptr;
            int n = 0;
            dbus_message_iter_get_fixed_array(&sub, &data, &n);
            if (data && n > 0) result.assign(data, data + n);
        }
    }
    dbus_message_unref(reply);
    return result;
}

// ── StartNotify / StopNotify ────────────────────────────────────────────────

static bool gatt_start_notify(DBusConnection* conn, const std::string& char_path) {
    return dbus_call_void(conn, "org.bluez", char_path.c_str(),
                          "org.bluez.GattCharacteristic1", "StartNotify");
}

static bool gatt_stop_notify(DBusConnection* conn, const std::string& char_path) {
    return dbus_call_void(conn, "org.bluez", char_path.c_str(),
                          "org.bluez.GattCharacteristic1", "StopNotify");
}

// =========================================================================
// Public API implementation
// =========================================================================

extern "C" {

// ── ble_scan ────────────────────────────────────────────────────────────────

void ble_scan(double timeout_s, bool show_all) {
    init_dbus_threads();

    DBusError err;
    dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn || dbus_error_is_set(&err)) {
        std::cerr << "Cannot connect to system D-Bus: "
                  << (dbus_error_is_set(&err) ? err.message : "unknown") << "\n";
        dbus_error_free(&err);
        return;
    }

    std::string adapter = find_adapter(conn);
    if (adapter.empty()) {
        std::cerr << "No Bluetooth adapters found.\n";
        dbus_connection_unref(conn);
        return;
    }

    set_discovery_filter_le(conn, adapter);

    std::cout << "Scanning for BLE devices (" << (int)timeout_s << "s)...\n\n";
    std::cout.flush();

    if (!dbus_call_void(conn, "org.bluez", adapter.c_str(),
                        "org.bluez.Adapter1", "StartDiscovery")) {
        std::cerr << "  StartDiscovery failed — listing cached devices only.\n";
    }

    // Pump D-Bus dispatch while scanning so BlueZ device objects appear
    auto scan_end = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds((int)(timeout_s * 1000));
    while (std::chrono::steady_clock::now() < scan_end) {
        dbus_connection_read_write_dispatch(conn, 200);
    }

    dbus_call_void(conn, "org.bluez", adapter.c_str(),
                   "org.bluez.Adapter1", "StopDiscovery");

    // Small delay for last results to settle
    dbus_connection_read_write_dispatch(conn, 200);

    // Enumerate discovered devices
    struct DevInfo {
        std::string name, address;
        int16_t rssi;
        std::vector<std::string> uuids;
    };
    std::vector<DevInfo> found;

    auto objs = get_managed_objects(conn);
    for (auto& o : objs) {
        auto it = o.interfaces.find("org.bluez.Device1");
        if (it == o.interfaces.end()) continue;
        // Only devices under our adapter
        if (o.path.find(adapter + "/") != 0) continue;

        DevInfo di;
        auto ait = it->second.find("Address");
        if (ait != it->second.end()) di.address = ait->second;
        auto nit = it->second.find("Name");
        if (nit != it->second.end()) di.name = nit->second;

        di.rssi = dbus_get_int16_prop(conn, o.path.c_str(),
                                       "org.bluez.Device1", "RSSI", -127);

        di.uuids = dbus_get_string_array_prop(conn, o.path.c_str(),
                                               "org.bluez.Device1", "UUIDs");
        found.push_back(std::move(di));
    }

    // Sort by RSSI descending
    std::sort(found.begin(), found.end(),
              [](const DevInfo& a, const DevInfo& b){ return a.rssi > b.rssi; });

    int shown = 0, named = 0;
    for (auto& d : found) {
        if (!d.name.empty()) ++named;
        if (!show_all && d.name.empty()) continue;
        std::cout << d.address << "\t"
                  << std::setw(4) << d.rssi << " dBm\t"
                  << (d.name.empty() ? "(no name)" : d.name) << "\n";
        ++shown;
    }
    std::cout << "\nFound " << shown << " BLE device(s)"
              << " (" << named << " named, " << found.size() << " total).\n";

    dbus_connection_unref(conn);
}

// ── ble_inspect ─────────────────────────────────────────────────────────────

void ble_inspect(const char* address, double timeout_s) {
    init_dbus_threads();

    DBusError err;
    dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn || dbus_error_is_set(&err)) {
        std::cerr << "Cannot connect to system D-Bus.\n";
        dbus_error_free(&err);
        return;
    }

    std::string adapter = find_adapter(conn);
    if (adapter.empty()) {
        std::cerr << "No Bluetooth adapters found.\n";
        dbus_connection_unref(conn);
        return;
    }

    std::cout << "Searching for " << address << " (BLE)...\n";
    std::cout.flush();

    // Start discovery to find the device
    set_discovery_filter_le(conn, adapter);
    dbus_call_void(conn, "org.bluez", adapter.c_str(),
                   "org.bluez.Adapter1", "StartDiscovery");

    std::string dev_path;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds((int)(timeout_s * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        dev_path = find_device_path(conn, address);
        if (!dev_path.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    dbus_call_void(conn, "org.bluez", adapter.c_str(),
                   "org.bluez.Adapter1", "StopDiscovery");

    if (dev_path.empty()) {
        std::cerr << "Device not found.\n";
        dbus_connection_unref(conn);
        return;
    }

    std::cout << "Connecting...\n";
    std::cout.flush();

    if (!dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                        "org.bluez.Device1", "Connect", 30000)) {
        std::cerr << "Connect failed.\n";
        dbus_connection_unref(conn);
        return;
    }

    if (!wait_services_resolved(conn, dev_path, 10.0)) {
        std::cerr << "Services not resolved (timeout).\n";
        dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                       "org.bluez.Device1", "Disconnect");
        dbus_connection_unref(conn);
        return;
    }

    std::string dev_name = dbus_get_string_prop(conn, dev_path.c_str(),
                                                 "org.bluez.Device1", "Name");
    if (dev_name.empty()) dev_name = address;
    std::string dev_mac = dbus_get_string_prop(conn, dev_path.c_str(),
                                                "org.bluez.Device1", "Address");
    if (dev_mac.empty()) dev_mac = address;

    uint16_t mtu = dbus_get_uint16_prop(conn, dev_path.c_str(),
                                         "org.bluez.Device1", "MTU", 23);
    // MTU might be on the characteristic instead (BlueZ >= 5.62)
    // We'll check per-char below

    std::cout << "Device : " << dev_name << "\n";
    std::cout << "ID     : " << dev_mac << "\n";
    std::cout << "MTU    : " << mtu << "\n\n";

    auto services = enumerate_gatt(conn, dev_path);

    // Use auto-detect scoring to pick the best service
    auto ad = auto_detect_from_gatt(services);
    std::string best_svc   = ad.service_uuid;
    std::string best_write = ad.write_uuid;
    std::string best_read  = ad.read_uuid;

    for (auto& svc : services) {
        std::string sname = uuid_short_name(svc.uuid);
        bool is_best = (svc.uuid == best_svc);
        std::cout << "SERVICE " << svc.uuid << ": " << sname
                  << (is_best ? "  ★" : "") << "\n";

        for (auto& chr : svc.chars) {
            std::string caps;
            for (auto& f : chr.flags) {
                if (!caps.empty()) caps += ", ";
                caps += f;
            }

            std::string cname = uuid_short_name(chr.uuid);
            bool is_w = (chr.uuid == best_write);
            bool is_r = (chr.uuid == best_read);
            std::string role;
            if (is_w) role = " ←write";
            if (is_r) role += " ←read";
            std::cout << "     CHARACTERISTIC " << chr.uuid
                      << ": " << cname
                      << "  [" << caps << "]" << role << "\n";

            // Read value if readable
            if (chr.can_read()) {
                auto val = gatt_read_value(conn, chr.path);
                if (!val.empty()) {
                    bool printable = true;
                    for (auto b : val)
                        if (b < 0x20 || b > 0x7E) { printable = false; break; }
                    if (printable) {
                        std::cout << "         Value: \""
                                  << std::string(val.begin(), val.end()) << "\"\n";
                    } else {
                        std::cout << "         Value: ";
                        for (auto b : val)
                            std::cout << std::hex << std::setw(2) << std::setfill('0')
                                      << (int)b;
                        std::cout << std::dec << "\n";
                    }
                }
            }

            // Enumerate descriptors (they are also managed objects under the char path)
            auto all_objs = get_managed_objects(conn);
            for (auto& o : all_objs) {
                if (o.path.find(chr.path + "/") != 0) continue;
                auto dit = o.interfaces.find("org.bluez.GattDescriptor1");
                if (dit == o.interfaces.end()) continue;
                auto duid = dit->second.find("UUID");
                std::string desc_uuid = duid != dit->second.end() ? duid->second : "";
                std::cout << "         DESCRIPTOR " << desc_uuid
                          << ": " << uuid_short_name(desc_uuid) << "\n";
            }
        }
        std::cout << "\n";
    }

    std::cout << std::string(68, '=') << "\n";
    std::cout << "\nSuggested bridge command:\n";
    std::cout << "  bt_kiss_bridge --ble \\\n"
              << "      --device   " << dev_mac << " \\\n"
              << "      --service  " << (best_svc.empty()   ? "<SERVICE-UUID>"   : best_svc) << " \\\n"
              << "      --write    " << (best_write.empty() ? "<WRITE-CHAR-UUID>": best_write) << " \\\n"
              << "      --read     " << (best_read.empty()  ? "<NOTIFY-CHAR-UUID>": best_read) << "\n";

    dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                   "org.bluez.Device1", "Disconnect");
    dbus_connection_unref(conn);
}

// ── ble_auto_detect ─────────────────────────────────────────────────────────

bool ble_auto_detect(const char* address, double timeout_s,
                      char* svc_out, char* write_out, char* read_out,
                      size_t buf_len)
{
    init_dbus_threads();

    DBusError err;
    dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn || dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return false;
    }

    std::string adapter = find_adapter(conn);
    if (adapter.empty()) {
        dbus_connection_unref(conn);
        return false;
    }

    // Scan for device
    set_discovery_filter_le(conn, adapter);
    dbus_call_void(conn, "org.bluez", adapter.c_str(),
                   "org.bluez.Adapter1", "StartDiscovery");

    std::string dev_path;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds((int)(timeout_s * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        dev_path = find_device_path(conn, address);
        if (!dev_path.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    dbus_call_void(conn, "org.bluez", adapter.c_str(),
                   "org.bluez.Adapter1", "StopDiscovery");

    if (dev_path.empty()) {
        dbus_connection_unref(conn);
        return false;
    }

    // Connect + resolve services
    if (!dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                        "org.bluez.Device1", "Connect", 30000)) {
        dbus_connection_unref(conn);
        return false;
    }

    if (!wait_services_resolved(conn, dev_path, 10.0)) {
        dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                       "org.bluez.Device1", "Disconnect");
        dbus_connection_unref(conn);
        return false;
    }

    auto services = enumerate_gatt(conn, dev_path);
    auto r = auto_detect_from_gatt(services);

    dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                   "org.bluez.Device1", "Disconnect");
    dbus_connection_unref(conn);

    if (r.service_uuid.empty() || r.write_uuid.empty() || r.read_uuid.empty())
        return false;

    snprintf(svc_out,   buf_len, "%s", r.service_uuid.c_str());
    snprintf(write_out, buf_len, "%s", r.write_uuid.c_str());
    snprintf(read_out,  buf_len, "%s", r.read_uuid.c_str());
    return true;
}

// ── ble_connect ─────────────────────────────────────────────────────────────

ble_handle_t ble_connect(const char* address,
                          const char* svc_uuid,
                          const char* write_uuid,
                          const char* read_uuid,
                          double timeout_s,
                          int mtu_cap,
                          bool write_with_response)
{
    init_dbus_threads();

    // Open two D-Bus connections: one for dispatch (signals/notifications),
    // one for writes (blocking calls from caller thread).  This avoids the
    // thread-safety issue of concurrent send + dispatch on the same conn.
    DBusError err;
    dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn || dbus_error_is_set(&err)) {
        std::cerr << "  Cannot connect to system D-Bus.\n";
        dbus_error_free(&err);
        return nullptr;
    }

    DBusError err2;
    dbus_error_init(&err2);
    DBusConnection* conn_write = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err2);
    if (!conn_write || dbus_error_is_set(&err2)) {
        std::cerr << "  Cannot open second D-Bus connection for writes.\n";
        dbus_error_free(&err2);
        dbus_connection_unref(conn);
        return nullptr;
    }

    std::string adapter = find_adapter(conn);
    if (adapter.empty()) {
        std::cerr << "  No Bluetooth adapter available.\n";
        dbus_connection_close(conn_write);
        dbus_connection_unref(conn_write);
        dbus_connection_unref(conn);
        return nullptr;
    }

    // Try to find device among known/paired objects first (skip scan)
    std::string dev_path = find_device_path(conn, address);
    if (!dev_path.empty()) {
        std::cout << "  Device " << address << " known to BlueZ (paired/cached).\n";
        std::cout.flush();
    } else {
        // Only scan if device not already known
        std::cout << "  Scanning for " << address << "...\n";
        std::cout.flush();
        set_discovery_filter_le(conn, adapter);
        dbus_call_void(conn, "org.bluez", adapter.c_str(),
                       "org.bluez.Adapter1", "StartDiscovery");

        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds((int)(timeout_s * 1000));
        while (std::chrono::steady_clock::now() < deadline) {
            dev_path = find_device_path(conn, address);
            if (!dev_path.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        dbus_call_void(conn, "org.bluez", adapter.c_str(),
                       "org.bluez.Adapter1", "StopDiscovery");
    }

    if (dev_path.empty()) {
        std::cerr << "[BLE] Device " << address << " not found.\n";
        dbus_connection_close(conn_write);
        dbus_connection_unref(conn_write);
        dbus_connection_unref(conn);
        return nullptr;
    }

    // Preventive disconnect — clear stale state if device was left connected
    if (dbus_get_bool_prop(conn, dev_path.c_str(),
                            "org.bluez.Device1", "Connected")) {
        std::cout << "  Clearing stale connection...\n";
        std::cout.flush();
        dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                       "org.bluez.Device1", "Disconnect", 2000);
        auto dc_deadline = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < dc_deadline) {
            if (!dbus_get_bool_prop(conn, dev_path.c_str(),
                                     "org.bluez.Device1", "Connected"))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Connect
    std::cout << "  Connecting...\n";
    std::cout.flush();
    if (!dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                        "org.bluez.Device1", "Connect", 30000)) {
        std::cerr << "[BLE] Connect failed.\n";
        dbus_connection_close(conn_write);
        dbus_connection_unref(conn_write);
        dbus_connection_unref(conn);
        return nullptr;
    }

    if (!wait_services_resolved(conn, dev_path, 10.0)) {
        std::cerr << "[BLE] Services not resolved.\n";
        dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                       "org.bluez.Device1", "Disconnect");
        dbus_connection_close(conn_write);
        dbus_connection_unref(conn_write);
        dbus_connection_unref(conn);
        return nullptr;
    }

    // Enumerate GATT
    auto services = enumerate_gatt(conn, dev_path);

    // Resolve UUIDs
    std::string s_uuid = svc_uuid   ? svc_uuid   : "";
    std::string w_uuid = write_uuid ? write_uuid : "";
    std::string r_uuid = read_uuid  ? read_uuid  : "";

    std::string w_path, r_path;

    if (s_uuid.empty() || w_uuid.empty() || r_uuid.empty()) {
        auto ad = auto_detect_from_gatt(services);
        if (s_uuid.empty()) s_uuid = ad.service_uuid;
        if (w_uuid.empty()) { w_uuid = ad.write_uuid; w_path = ad.write_char_path; }
        if (r_uuid.empty()) { r_uuid = ad.read_uuid;  r_path = ad.read_char_path; }

        if (s_uuid.empty() || w_uuid.empty() || r_uuid.empty()) {
            std::cerr << "  Cannot auto-detect BLE UUIDs.  Use --service, --write, --read.\n";
            dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                           "org.bluez.Device1", "Disconnect");
            dbus_connection_close(conn_write);
            dbus_connection_unref(conn_write);
            dbus_connection_unref(conn);
            return nullptr;
        }

        std::cout << "  Auto-detected UUIDs:\n"
                  << "    service: " << s_uuid << "\n"
                  << "    write  : " << w_uuid << "\n"
                  << "    read   : " << r_uuid << "\n";
    }

    if (w_path.empty()) w_path = find_char_path(services, s_uuid, w_uuid);
    if (r_path.empty()) r_path = find_char_path(services, s_uuid, r_uuid);

    if (w_path.empty() || r_path.empty()) {
        std::cerr << "  Cannot find GATT characteristic paths.\n"
                  << "  Requested:  service=" << s_uuid
                  << "  write=" << w_uuid << "  read=" << r_uuid << "\n"
                  << "  Available GATT tree:\n";
        for (auto& svc : services) {
            std::cerr << "    service: " << svc.uuid << "\n";
            for (auto& chr : svc.chars) {
                std::cerr << "      char: " << chr.uuid
                          << "  flags:";
                for (auto& f : chr.flags) std::cerr << " " << f;
                std::cerr << "\n";
            }
        }
        dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                       "org.bluez.Device1", "Disconnect");
        dbus_connection_close(conn_write);
        dbus_connection_unref(conn_write);
        dbus_connection_unref(conn);
        return nullptr;
    }

    // Create handle
    auto* h = new BleLinuxHandle();
    h->conn_dispatch = conn;
    h->conn_write = conn_write;
    h->device_path = dev_path;
    h->adapter_path = adapter;
    h->write_char_path = w_path;
    h->read_char_path = r_path;
    h->svc_uuid = s_uuid;
    h->write_uuid = w_uuid;
    h->read_uuid = r_uuid;

    // Collect all readable characteristic paths (for wake-up)
    for (auto& svc : services) {
        for (auto& chr : svc.chars) {
            if (chr.can_read())
                h->readable_char_paths.push_back(chr.path);
        }
    }

    // Check capabilities
    auto w_flags = dbus_get_string_array_prop(conn, w_path.c_str(),
                       "org.bluez.GattCharacteristic1", "Flags");
    h->can_wwr = false;
    bool can_wr = false;
    for (auto& f : w_flags) {
        if (f == "write-without-response") h->can_wwr = true;
        if (f == "write") can_wr = true;
    }
    h->use_response = write_with_response ? true : (!h->can_wwr && can_wr);

    // MTU — try characteristic first (BlueZ >= 5.62), then device
    uint16_t mtu_val = dbus_get_uint16_prop(conn, r_path.c_str(),
                           "org.bluez.GattCharacteristic1", "MTU", 0);
    if (mtu_val == 0) {
        mtu_val = dbus_get_uint16_prop(conn, w_path.c_str(),
                      "org.bluez.GattCharacteristic1", "MTU", 0);
    }
    if (mtu_val == 0) mtu_val = 23; // fallback
    h->mtu_val = mtu_val;
    // Always chunk based on negotiated MTU (same as SimpleBLE).
    h->chunk_sz = std::max(1, std::min(mtu_cap > 0 ? mtu_cap : 517,
                                       (int)mtu_val) - 3);

    std::cout << "  Connected.  MTU=" << mtu_val
              << "  chunk=" << h->chunk_sz << "b"
              << "  wwr=" << (h->can_wwr ? "yes" : "no")
              << "  response=" << (h->use_response ? "yes" : "no") << "\n";
    std::cout.flush();

    // Create pipe
    int pfd[2];
    if (::pipe(pfd) < 0) {
        std::cerr << "  pipe() failed: " << strerror(errno) << "\n";
        dbus_call_void(conn, "org.bluez", dev_path.c_str(),
                       "org.bluez.Device1", "Disconnect");
        dbus_connection_close(conn_write);
        dbus_connection_unref(conn_write);
        dbus_connection_unref(conn);
        delete h;
        return nullptr;
    }
    h->pipe_read = pfd[0];
    h->pipe_write = pfd[1];

    // Non-blocking write end (don't stall dispatch thread)
    int fl = ::fcntl(h->pipe_write, F_GETFL, 0);
    ::fcntl(h->pipe_write, F_SETFL, fl | O_NONBLOCK);
    // Non-blocking read end (for select)
    fl = ::fcntl(h->pipe_read, F_GETFL, 0);
    ::fcntl(h->pipe_read, F_SETFL, fl | O_NONBLOCK);

    // Add signal match rules on the dispatch connection
    std::string match1 = "type='signal',interface='org.freedesktop.DBus.Properties',"
                         "member='PropertiesChanged',path='" + r_path + "'";
    std::string match2 = "type='signal',interface='org.freedesktop.DBus.Properties',"
                         "member='PropertiesChanged',path='" + dev_path + "'";
    dbus_bus_add_match(conn, match1.c_str(), nullptr);
    dbus_bus_add_match(conn, match2.c_str(), nullptr);

    // Add message filters on dispatch connection
    dbus_connection_add_filter(conn, notify_filter, h, nullptr);
    dbus_connection_add_filter(conn, disconnect_filter, h, nullptr);

    // Start notify on read characteristic (synchronous — BlueZ blocks until CCCD acknowledged)
    if (!gatt_start_notify(conn, r_path)) {
        std::cerr << "  Warning: StartNotify failed.\n";
    }
    // Settle: let radio transition into KISS data mode after CCCD write
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    h->connected = true;

    // Start dispatch thread (uses conn_dispatch only, 20ms for low latency)
    h->dispatch_thread = std::thread([h]() {
        while (!h->stop) {
            dbus_connection_read_write_dispatch(h->conn_dispatch, 20);
        }
    });

    return static_cast<ble_handle_t>(h);
}

// ── ble_disconnect ──────────────────────────────────────────────────────────

void ble_disconnect(ble_handle_t handle) {
    if (!handle) return;
    auto* h = static_cast<BleLinuxHandle*>(handle);

    h->stop = true;
    if (h->dispatch_thread.joinable()) h->dispatch_thread.join();

    // Only call BlueZ disconnect if still connected
    if (h->connected.load()) {
        if (h->conn_dispatch)
            gatt_stop_notify(h->conn_dispatch, h->read_char_path);

        // Check BlueZ-level connected state before calling Disconnect
        if (h->conn_write &&
            dbus_get_bool_prop(h->conn_write, h->device_path.c_str(),
                                "org.bluez.Device1", "Connected")) {
            dbus_call_void(h->conn_write, "org.bluez", h->device_path.c_str(),
                           "org.bluez.Device1", "Disconnect", 5000);
        }
    }

    // Remove filters from dispatch connection
    if (h->conn_dispatch) {
        dbus_connection_remove_filter(h->conn_dispatch, notify_filter, h);
        dbus_connection_remove_filter(h->conn_dispatch, disconnect_filter, h);
        dbus_connection_unref(h->conn_dispatch);
    }

    // Close private write connection
    if (h->conn_write) {
        dbus_connection_close(h->conn_write);
        dbus_connection_unref(h->conn_write);
    }

    if (h->pipe_read >= 0)  ::close(h->pipe_read);
    if (h->pipe_write >= 0) ::close(h->pipe_write);

    h->connected = false;
    delete h;
}

// ── ble_is_connected ────────────────────────────────────────────────────────

bool ble_is_connected(ble_handle_t handle) {
    if (!handle) return false;
    return static_cast<BleLinuxHandle*>(handle)->connected;
}

// ── ble_read_fd ─────────────────────────────────────────────────────────────

int ble_read_fd(ble_handle_t handle) {
    if (!handle) return -1;
    auto* h = static_cast<BleLinuxHandle*>(handle);
    return h->connected ? h->pipe_read : -1;
}

// ── ble_write ───────────────────────────────────────────────────────────────

void ble_write(ble_handle_t handle, const uint8_t* data, size_t len) {
    if (!handle || !data || len == 0) return;
    auto* h = static_cast<BleLinuxHandle*>(handle);
    if (!h->connected) return;

    // Chunk and send via dedicated write connection (thread-safe)
    int cs = h->chunk_sz;
    for (size_t off = 0; off < len; off += (size_t)cs) {
        size_t clen = std::min((size_t)cs, len - off);
        if (!gatt_write_value_async(h->conn_write, h->write_char_path,
                                     data + off, clen, h->use_response)) {
            std::cerr << "  BLE write failed (chunk " << off << "+" << clen
                      << " of " << len << ")\n";
        }
    }
}

// ── ble_mtu / ble_chunk_size / ble_can_write_without_response ───────────────

int ble_mtu(ble_handle_t handle) {
    if (!handle) return 23;
    return static_cast<BleLinuxHandle*>(handle)->mtu_val;
}

int ble_chunk_size(ble_handle_t handle) {
    if (!handle) return 20;
    return static_cast<BleLinuxHandle*>(handle)->chunk_sz;
}

bool ble_can_write_without_response(ble_handle_t handle) {
    if (!handle) return false;
    return static_cast<BleLinuxHandle*>(handle)->can_wwr;
}

void ble_wake(ble_handle_t handle) {
    if (!handle) return;
    auto* h = static_cast<BleLinuxHandle*>(handle);
    if (!h->connected || !h->conn_write) return;

    // Read the first readable characteristic to wake the GATT server
    for (auto& path : h->readable_char_paths) {
        auto val = gatt_read_value(h->conn_write, path);
        if (!val.empty()) {
            std::cout << "  BLE wake: read " << val.size() << " bytes from GATT.\n";
            std::cout.flush();
            return;
        }
    }
}

} // extern "C"

#endif // __linux__
