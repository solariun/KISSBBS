// ============================================================================
// bt_ble_macos.mm — Native BLE via CoreBluetooth (no SimpleBLE dependency)
//
// Implements the bt_ble_native.h C-linkage API using CoreBluetooth framework.
//
// Key design:
//   - CBCentralManager + CBPeripheral with delegate callbacks on a dispatch queue.
//   - Notify data is written to a pipe fd, making BLE select()-able.
//   - Blocking operations use dispatch_semaphore_wait for synchronization.
//   - Same pattern as bt_rfcomm_macos.mm (delegate → pipe bridge).
// ============================================================================

#ifdef __APPLE__

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include "bt_ble_native.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::string to_std(NSString* s) {
    return s ? std::string([s UTF8String]) : "";
}

static NSString* to_ns(const char* s) {
    return s ? [NSString stringWithUTF8String:s] : nil;
}

static CBUUID* uuid_from_str(const char* s) {
    if (!s || !*s) return nil;
    return [CBUUID UUIDWithString:to_ns(s)];
}

static std::string uuid_str(CBUUID* u) {
    return u ? to_std([u UUIDString]) : "";
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool is_gap_gatt(const std::string& uuid) {
    std::string lo = lower(uuid);
    return lo == "1800" || lo == "1801" ||
           lo == "00001800-0000-1000-8000-00805f9b34fb" ||
           lo == "00001801-0000-1000-8000-00805f9b34fb";
}

// UUID name lookup (subset)
static std::string uuid_short_name(const std::string& uuid) {
    std::string lo = lower(uuid);
    if (lo.size() < 4) return "Unknown";
    unsigned val = 0;
    try { val = (unsigned)std::stoul(lo.size() >= 8 ? lo.substr(0, 8) : lo, nullptr, 16); }
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

// ── NotifyWaiter — must be defined before BleDelegate @interface ─────────────

struct NotifyWaiter {
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    done = false;

    void signal() {
        std::lock_guard<std::mutex> lk(mtx);
        done = true;
        cv.notify_one();
    }

    // Returns true if signaled within timeout, false on timeout.
    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, timeout, [this]{ return done; });
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mtx);
        done = false;
    }
};

// ── Delegate ────────────────────────────────────────────────────────────────

@interface BleDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property (nonatomic, assign) dispatch_semaphore_t semaphore;
@property (nonatomic, assign) int pipeWrite;
@property (nonatomic, assign) std::atomic<bool>* connectedFlag;
@property (nonatomic, assign) BOOL servicesResolved;
@property (nonatomic, assign) BOOL didConnect;
@property (nonatomic, assign) BOOL didFail;
@property (nonatomic, assign) NotifyWaiter* notifyWaiter;  // C++ cv waiter
@property (nonatomic, strong) NSMutableArray<CBPeripheral*>* found;
@property (nonatomic, strong) CBPeripheral* target;
@property (nonatomic, assign) int discoveredServices;
@property (nonatomic, assign) int resolvedChars;
@property (nonatomic, assign) int totalChars;
@property (nonatomic, assign) int resolvedDescs;
@property (nonatomic, assign) BOOL discoverDescriptors;
@end

@implementation BleDelegate

- (instancetype)init {
    self = [super init];
    if (self) {
        _found = [NSMutableArray new];
        _pipeWrite = -1;
        _connectedFlag = nullptr;
        _notifyWaiter = nullptr;
    }
    return self;
}

// --- CBCentralManagerDelegate ---

- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
    (void)central;
    if (_semaphore && central.state == CBManagerStatePoweredOn)
        dispatch_semaphore_signal(_semaphore);
}

- (void)centralManager:(CBCentralManager*)central
 didDiscoverPeripheral:(CBPeripheral*)peripheral
     advertisementData:(NSDictionary<NSString*,id>*)ad
                  RSSI:(NSNumber*)RSSI
{
    (void)central; (void)ad; (void)RSSI;
    @synchronized(_found) {
        for (CBPeripheral* p in _found)
            if ([p.identifier isEqual:peripheral.identifier]) return;
        [_found addObject:peripheral];
    }
    if (_semaphore && _target == nil) {
        // Scanning — each discovery signals
    }
}

- (void)centralManager:(CBCentralManager*)central
  didConnectPeripheral:(CBPeripheral*)peripheral
{
    (void)central;
    _didConnect = YES;
    peripheral.delegate = self;
    [peripheral discoverServices:nil];
}

- (void)centralManager:(CBCentralManager*)central
didFailToConnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error
{
    (void)central; (void)peripheral; (void)error;
    _didFail = YES;
    if (_semaphore) dispatch_semaphore_signal(_semaphore);
}

- (void)centralManager:(CBCentralManager*)central
didDisconnectPeripheral:(CBPeripheral*)peripheral
                  error:(NSError*)error
{
    (void)central; (void)peripheral; (void)error;
    if (_connectedFlag) _connectedFlag->store(false);
    if (_pipeWrite >= 0) {
        ::close(_pipeWrite);
        _pipeWrite = -1;
    }
}

// --- CBPeripheralDelegate ---

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverServices:(NSError*)error
{
    (void)error;
    _discoveredServices = (int)peripheral.services.count;
    _resolvedChars = 0;
    if (peripheral.services.count == 0) {
        _servicesResolved = YES;
        if (_semaphore) dispatch_semaphore_signal(_semaphore);
        return;
    }
    for (CBService* svc in peripheral.services) {
        [peripheral discoverCharacteristics:nil forService:svc];
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverCharacteristicsForService:(CBService*)service
             error:(NSError*)error
{
    (void)error;
    _resolvedChars++;
    if (_resolvedChars >= _discoveredServices) {
        if (_discoverDescriptors) {
            // Count total characteristics and discover descriptors
            _totalChars = 0;
            _resolvedDescs = 0;
            for (CBService* svc in peripheral.services)
                _totalChars += (int)svc.characteristics.count;
            if (_totalChars == 0) {
                _servicesResolved = YES;
                if (_semaphore) dispatch_semaphore_signal(_semaphore);
                return;
            }
            for (CBService* svc in peripheral.services)
                for (CBCharacteristic* chr in svc.characteristics)
                    [peripheral discoverDescriptorsForCharacteristic:chr];
        } else {
            _servicesResolved = YES;
            if (_semaphore) dispatch_semaphore_signal(_semaphore);
        }
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverDescriptorsForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
    (void)peripheral; (void)characteristic; (void)error;
    _resolvedDescs++;
    if (_resolvedDescs >= _totalChars) {
        _servicesResolved = YES;
        if (_semaphore) dispatch_semaphore_signal(_semaphore);
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
    (void)peripheral; (void)error;
    NSData* data = characteristic.value;
    if (data && data.length > 0 && _pipeWrite >= 0) {
        (void)::write(_pipeWrite, data.bytes, data.length);
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
    (void)peripheral; (void)characteristic; (void)error;
    // Notify subscription confirmed — wake C++ waiter in ble_connect
    if (_notifyWaiter) {
        _notifyWaiter->signal();
    }
}

@end

// ── Handle ──────────────────────────────────────────────────────────────────

struct BleMacosHandle {
    CBCentralManager* manager;
    CBPeripheral* peripheral;
    BleDelegate* delegate;
    dispatch_queue_t queue;

    CBCharacteristic* writeChr;
    CBCharacteristic* readChr;
    std::string svc_uuid, write_uuid, read_uuid;

    int pipe_read  = -1;
    int pipe_write = -1;
    int mtu_val    = 23;
    int chunk_sz   = 20;
    bool use_response = false;
    bool can_wwr   = false;

    std::atomic<bool> connected{false};
    NotifyWaiter      notify_waiter;   // for CCCD subscription confirmation
};

// ── Wait for CBManagerStatePoweredOn ────────────────────────────────────────

static CBCentralManager* create_manager(BleDelegate* delegate,
                                         dispatch_queue_t queue,
                                         double timeout_s)
{
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    delegate.semaphore = sem;

    CBCentralManager* mgr = [[CBCentralManager alloc]
        initWithDelegate:delegate queue:queue];

    if (mgr.state != CBManagerStatePoweredOn) {
        dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW,
                          (int64_t)(timeout_s * NSEC_PER_SEC)));
    }

    if (mgr.state != CBManagerStatePoweredOn) {
        std::cerr << "Bluetooth not powered on.\n";
        return nil;
    }
    return mgr;
}

// ── Find peripheral by address/name match ───────────────────────────────────

static CBPeripheral* find_target(BleDelegate* delegate, const char* address) {
    std::string target = lower(address);
    @synchronized(delegate.found) {
        for (CBPeripheral* p in delegate.found) {
            // Match by name or by CB UUID
            if (lower(to_std(p.name)) == target ||
                lower(to_std([p.identifier UUIDString])) == target)
                return p;
        }
    }
    return nil;
}

// ── Auto-detect UUIDs ───────────────────────────────────────────────────────

struct MacAutoDetect {
    CBCharacteristic* writeChr = nil;
    CBCharacteristic* readChr = nil;
    std::string svc_uuid, write_uuid, read_uuid;
};

static MacAutoDetect mac_auto_detect(CBPeripheral* peripheral) {
    MacAutoDetect r;

    // Phase 1: find a service with BOTH write + notify
    for (CBService* svc in peripheral.services) {
        if (is_gap_gatt(uuid_str(svc.UUID))) continue;
        CBCharacteristic* w = nil;
        CBCharacteristic* n = nil;
        for (CBCharacteristic* chr in svc.characteristics) {
            if (!w && (chr.properties & (CBCharacteristicPropertyWrite |
                                         CBCharacteristicPropertyWriteWithoutResponse)))
                w = chr;
            if (!n && (chr.properties & (CBCharacteristicPropertyNotify |
                                         CBCharacteristicPropertyIndicate)))
                n = chr;
        }
        if (w && n) {
            r.writeChr = w;
            r.readChr = n;
            r.svc_uuid = uuid_str(svc.UUID);
            r.write_uuid = uuid_str(w.UUID);
            r.read_uuid = uuid_str(n.UUID);
            return r;
        }
    }

    // Phase 2: fallback
    for (CBService* svc in peripheral.services) {
        if (is_gap_gatt(uuid_str(svc.UUID))) continue;
        for (CBCharacteristic* chr in svc.characteristics) {
            if (!r.writeChr &&
                (chr.properties & (CBCharacteristicPropertyWrite |
                                   CBCharacteristicPropertyWriteWithoutResponse))) {
                r.writeChr = chr;
                r.write_uuid = uuid_str(chr.UUID);
                if (r.svc_uuid.empty()) r.svc_uuid = uuid_str(svc.UUID);
            }
            if (!r.readChr &&
                (chr.properties & (CBCharacteristicPropertyNotify |
                                   CBCharacteristicPropertyIndicate))) {
                r.readChr = chr;
                r.read_uuid = uuid_str(chr.UUID);
                if (r.svc_uuid.empty()) r.svc_uuid = uuid_str(svc.UUID);
            }
        }
    }
    return r;
}

// Find specific characteristics by UUID
static CBCharacteristic* find_chr(CBPeripheral* peripheral,
                                   const char* svc_uuid_str,
                                   const char* chr_uuid_str)
{
    CBUUID* svc_id = uuid_from_str(svc_uuid_str);
    CBUUID* chr_id = uuid_from_str(chr_uuid_str);
    if (!chr_id) return nil;

    for (CBService* svc in peripheral.services) {
        if (svc_id && ![svc.UUID isEqual:svc_id]) continue;
        for (CBCharacteristic* chr in svc.characteristics) {
            if ([chr.UUID isEqual:chr_id]) return chr;
        }
    }
    return nil;
}

// =========================================================================
// Public API implementation
// =========================================================================

extern "C" {

// ── ble_scan ────────────────────────────────────────────────────────────────

void ble_scan(double timeout_s, bool show_all) {
    @autoreleasepool {
        dispatch_queue_t q = dispatch_queue_create("ble.scan", DISPATCH_QUEUE_SERIAL);
        BleDelegate* del = [[BleDelegate alloc] init];
        CBCentralManager* mgr = create_manager(del, q, 5.0);
        if (!mgr) return;

        std::cout << "Scanning for BLE devices (" << (int)timeout_s << "s)...\n\n";
        std::cout.flush();

        [mgr scanForPeripheralsWithServices:nil options:nil];
        std::this_thread::sleep_for(
            std::chrono::milliseconds((int)(timeout_s * 1000)));
        [mgr stopScan];

        // Small delay for last results
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        NSArray<CBPeripheral*>* found;
        @synchronized(del.found) {
            found = [del.found copy];
        }

        int shown = 0, named = 0;
        for (CBPeripheral* p in found) {
            bool has_name = (p.name && [p.name length] > 0);
            if (has_name) ++named;
            if (!show_all && !has_name) continue;
            std::string name = has_name ? to_std(p.name) : "(no name)";
            std::cout << to_std([p.identifier UUIDString])
                      << "\t" << name << "\n";
            ++shown;
        }
        std::cout << "\nFound " << shown << " BLE device(s)"
                  << " (" << named << " named, " << (int)found.count << " total).\n";
    }
}

// ── ble_inspect ─────────────────────────────────────────────────────────────

void ble_inspect(const char* address, double timeout_s) {
    @autoreleasepool {
        dispatch_queue_t q = dispatch_queue_create("ble.inspect", DISPATCH_QUEUE_SERIAL);
        BleDelegate* del = [[BleDelegate alloc] init];
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        del.semaphore = sem;

        CBCentralManager* mgr = create_manager(del, q, 5.0);
        if (!mgr) return;

        std::cout << "Searching for " << address << " (BLE)...\n";
        std::cout.flush();

        [mgr scanForPeripheralsWithServices:nil options:nil];

        // Wait for target
        CBPeripheral* target = nil;
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds((int)(timeout_s * 1000));
        while (std::chrono::steady_clock::now() < deadline) {
            target = find_target(del, address);
            if (target) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        [mgr stopScan];

        if (!target) {
            std::cerr << "Device not found.\n";
            return;
        }

        std::cout << "Connecting...\n";
        std::cout.flush();

        del.servicesResolved = NO;
        del.discoverDescriptors = YES;  // inspect needs descriptors
        del.didConnect = NO;
        del.didFail = NO;
        [mgr connectPeripheral:target options:nil];

        // Wait for connect + service discovery
        dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));

        if (!del.didConnect || del.didFail) {
            std::cerr << "Connect failed.\n";
            return;
        }

        // Wait for services + descriptors resolved
        if (!del.servicesResolved) {
            dispatch_semaphore_wait(sem,
                dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        }

        std::string dev_name = target.name ? to_std(target.name) : address;
        std::string dev_uuid = to_std([target.identifier UUIDString]);
        NSUInteger mtu = [target maximumWriteValueLengthForType:
                          CBCharacteristicWriteWithoutResponse] + 3;
        std::cout << "Device : " << dev_name << "\n";
        std::cout << "ID     : " << dev_uuid << "\n";
        std::cout << "MTU    : " << mtu << "\n\n";

        std::string best_svc, best_write, best_read;

        for (CBService* svc in target.services) {
            std::string svc_str = uuid_str(svc.UUID);
            std::cout << "SERVICE " << svc_str << ": "
                      << uuid_short_name(svc_str) << "\n";

            for (CBCharacteristic* chr in svc.characteristics) {
                std::string chr_str = uuid_str(chr.UUID);
                CBCharacteristicProperties props = chr.properties;

                std::vector<std::string> caps;
                if (props & CBCharacteristicPropertyRead) caps.push_back("read");
                if (props & CBCharacteristicPropertyNotify) caps.push_back("notify");
                if (props & CBCharacteristicPropertyIndicate) caps.push_back("indicate");
                if (props & CBCharacteristicPropertyWrite) caps.push_back("write");
                if (props & CBCharacteristicPropertyWriteWithoutResponse)
                    caps.push_back("write-without-response");

                std::string caps_str;
                for (size_t i = 0; i < caps.size(); i++) {
                    if (i) caps_str += ", ";
                    caps_str += caps[i];
                }

                std::cout << "     CHARACTERISTIC " << chr_str
                          << ": " << uuid_short_name(chr_str)
                          << "  [" << caps_str << "]\n";

                // Read value
                if ((props & CBCharacteristicPropertyRead) && chr.value) {
                    NSData* val = chr.value;
                    if (val.length > 0) {
                        bool printable = true;
                        const uint8_t* bytes = (const uint8_t*)val.bytes;
                        for (NSUInteger i = 0; i < val.length; i++)
                            if (bytes[i] < 0x20 || bytes[i] > 0x7E)
                                { printable = false; break; }
                        if (printable) {
                            std::cout << "         Value: \""
                                      << std::string((const char*)bytes, val.length)
                                      << "\"\n";
                        } else {
                            std::cout << "         Value: ";
                            for (NSUInteger i = 0; i < val.length; i++)
                                std::cout << std::hex << std::setw(2)
                                          << std::setfill('0') << (int)bytes[i];
                            std::cout << std::dec << "\n";
                        }
                    }
                }

                // Descriptors
                for (CBDescriptor* desc in chr.descriptors) {
                    std::string ds = uuid_str(desc.UUID);
                    std::cout << "         DESCRIPTOR " << ds
                              << ": " << uuid_short_name(ds) << "\n";
                }

                if (best_svc.empty()) best_svc = svc_str;
                if (best_write.empty() &&
                    (props & (CBCharacteristicPropertyWrite |
                              CBCharacteristicPropertyWriteWithoutResponse)))
                    best_write = chr_str;
                if (best_read.empty() &&
                    (props & (CBCharacteristicPropertyNotify |
                              CBCharacteristicPropertyIndicate)))
                    best_read = chr_str;
            }
            std::cout << "\n";
        }

        std::cout << std::string(68, '=') << "\n";
        std::cout << "\nSuggested bridge command:\n";
        std::cout << "  bt_kiss_bridge --ble \\\n"
                  << "      --device   " << dev_uuid << " \\\n"
                  << "      --service  "
                  << (best_svc.empty() ? "<SERVICE-UUID>" : best_svc) << " \\\n"
                  << "      --write    "
                  << (best_write.empty() ? "<WRITE-CHAR-UUID>" : best_write) << " \\\n"
                  << "      --read     "
                  << (best_read.empty() ? "<NOTIFY-CHAR-UUID>" : best_read) << "\n";

        [mgr cancelPeripheralConnection:target];
    }
}

// ── ble_auto_detect ─────────────────────────────────────────────────────────

bool ble_auto_detect(const char* address, double timeout_s,
                      char* svc_out, char* write_out, char* read_out,
                      size_t buf_len)
{
    @autoreleasepool {
        dispatch_queue_t q = dispatch_queue_create("ble.autodetect", DISPATCH_QUEUE_SERIAL);
        BleDelegate* del = [[BleDelegate alloc] init];
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        del.semaphore = sem;

        CBCentralManager* mgr = create_manager(del, q, 5.0);
        if (!mgr) return false;

        [mgr scanForPeripheralsWithServices:nil options:nil];

        CBPeripheral* target = nil;
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds((int)(timeout_s * 1000));
        while (std::chrono::steady_clock::now() < deadline) {
            target = find_target(del, address);
            if (target) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        [mgr stopScan];
        if (!target) return false;

        del.servicesResolved = NO;
        del.didConnect = NO;
        del.didFail = NO;
        [mgr connectPeripheral:target options:nil];
        dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
        if (!del.didConnect) return false;
        if (!del.servicesResolved)
            dispatch_semaphore_wait(sem,
                dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));

        auto r = mac_auto_detect(target);
        [mgr cancelPeripheralConnection:target];

        if (r.svc_uuid.empty() || r.write_uuid.empty() || r.read_uuid.empty())
            return false;

        snprintf(svc_out,   buf_len, "%s", r.svc_uuid.c_str());
        snprintf(write_out, buf_len, "%s", r.write_uuid.c_str());
        snprintf(read_out,  buf_len, "%s", r.read_uuid.c_str());
        return true;
    }
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
    @autoreleasepool {
        dispatch_queue_t q = dispatch_queue_create("ble.bridge",
                                                    DISPATCH_QUEUE_SERIAL);
        BleDelegate* del = [[BleDelegate alloc] init];
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        del.semaphore = sem;

        CBCentralManager* mgr = create_manager(del, q, 5.0);
        if (!mgr) return nullptr;

        // Scan for target (AllowDuplicates so recently-seen devices re-announce)
        [mgr scanForPeripheralsWithServices:nil
             options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];

        CBPeripheral* target = nil;
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds((int)(timeout_s * 1000));
        while (std::chrono::steady_clock::now() < deadline) {
            target = find_target(del, address);
            if (target) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        [mgr stopScan];

        if (!target) {
            std::cerr << "[BLE] Device " << address << " not found.\n";
            return nullptr;
        }

        // Preventive disconnect — clear any stale connection (ignore errors)
        [mgr cancelPeripheralConnection:target];
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Connect
        del.servicesResolved = NO;
        del.didConnect = NO;
        del.didFail = NO;
        [mgr connectPeripheral:target options:nil];

        dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC));

        if (!del.didConnect || del.didFail) {
            std::cerr << "[BLE] Connect failed.\n";
            return nullptr;
        }

        // Wait for service discovery
        if (!del.servicesResolved) {
            dispatch_semaphore_wait(sem,
                dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC));
        }

        if (!del.servicesResolved) {
            std::cerr << "[BLE] Services not resolved.\n";
            [mgr cancelPeripheralConnection:target];
            return nullptr;
        }

        // Resolve UUIDs
        std::string s_uuid = svc_uuid   ? svc_uuid   : "";
        std::string w_uuid = write_uuid ? write_uuid : "";
        std::string r_uuid = read_uuid  ? read_uuid  : "";

        CBCharacteristic* writeChr = nil;
        CBCharacteristic* readChr = nil;

        if (s_uuid.empty() || w_uuid.empty() || r_uuid.empty()) {
            auto ad = mac_auto_detect(target);
            if (s_uuid.empty()) s_uuid = ad.svc_uuid;
            if (w_uuid.empty()) { w_uuid = ad.write_uuid; writeChr = ad.writeChr; }
            if (r_uuid.empty()) { r_uuid = ad.read_uuid;  readChr = ad.readChr; }

            if (s_uuid.empty() || w_uuid.empty() || r_uuid.empty()) {
                std::cerr << "  Cannot auto-detect BLE UUIDs.  Use --service, --write, --read.\n";
                [mgr cancelPeripheralConnection:target];
                return nullptr;
            }

            std::cout << "  Auto-detected UUIDs:\n"
                      << "    service: " << s_uuid << "\n"
                      << "    write  : " << w_uuid << "\n"
                      << "    read   : " << r_uuid << "\n";
        }

        if (!writeChr) writeChr = find_chr(target, s_uuid.c_str(), w_uuid.c_str());
        if (!readChr)  readChr  = find_chr(target, s_uuid.c_str(), r_uuid.c_str());

        if (!writeChr || !readChr) {
            std::cerr << "  Cannot find GATT characteristics.\n";
            [mgr cancelPeripheralConnection:target];
            return nullptr;
        }

        // Build handle
        auto* h = new BleMacosHandle();
        h->manager = mgr;
        h->peripheral = target;
        h->delegate = del;
        h->queue = q;
        h->writeChr = writeChr;
        h->readChr = readChr;
        h->svc_uuid = s_uuid;
        h->write_uuid = w_uuid;
        h->read_uuid = r_uuid;

        // Capabilities
        h->can_wwr = (writeChr.properties & CBCharacteristicPropertyWriteWithoutResponse) != 0;
        bool can_wr = (writeChr.properties & CBCharacteristicPropertyWrite) != 0;
        h->use_response = write_with_response ? true : (!h->can_wwr && can_wr);

        // MTU — always chunk based on negotiated MTU (same as SimpleBLE).
        // maximumWriteValueLengthForType returns the usable payload size
        // (CoreBluetooth subtracts the 3-byte ATT header internally).
        NSUInteger maxWrite = [target maximumWriteValueLengthForType:
            CBCharacteristicWriteWithoutResponse];
        h->mtu_val  = (int)(maxWrite + 3);      // ATT MTU (display)
        if (h->mtu_val < 23) h->mtu_val = 23;
        h->chunk_sz = std::max(1, std::min(mtu_cap > 0 ? mtu_cap : 517,
                                           h->mtu_val) - 3);

        std::cout << "  Connected.  MTU=" << h->mtu_val
                  << "  chunk=" << h->chunk_sz << "b"
                  << "  wwr=" << (h->can_wwr ? "yes" : "no")
                  << "  response=" << (h->use_response ? "yes" : "no") << "\n";
        std::cout.flush();

        // Create pipe
        int pfd[2];
        if (::pipe(pfd) < 0) {
            std::cerr << "  pipe() failed: " << strerror(errno) << "\n";
            [mgr cancelPeripheralConnection:target];
            delete h;
            return nullptr;
        }
        h->pipe_read = pfd[0];
        h->pipe_write = pfd[1];

        // Non-blocking
        int fl = ::fcntl(h->pipe_write, F_GETFL, 0);
        ::fcntl(h->pipe_write, F_SETFL, fl | O_NONBLOCK);
        fl = ::fcntl(h->pipe_read, F_GETFL, 0);
        ::fcntl(h->pipe_read, F_SETFL, fl | O_NONBLOCK);

        // Wire delegate to pipe
        del.pipeWrite = h->pipe_write;
        del.connectedFlag = &h->connected;
        h->connected = true;

        // Subscribe to notifications and WAIT for CCCD confirmation (like SimpleBLE).
        // Uses C++ condition_variable — no polling, wakes immediately on callback.
        h->notify_waiter.reset();
        del.notifyWaiter = &h->notify_waiter;
        [target setNotifyValue:YES forCharacteristic:h->readChr];
        if (!h->notify_waiter.wait(std::chrono::seconds(5))) {
            std::cerr << "  [BLE] Warning: notify subscription timeout (proceeding anyway).\n";
        }
        del.notifyWaiter = nullptr;
        // Settle: let radio transition into KISS data mode
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        return static_cast<ble_handle_t>(h);
    }
}

// ── ble_disconnect ──────────────────────────────────────────────────────────

void ble_disconnect(ble_handle_t handle) {
    if (!handle) return;
    @autoreleasepool {
        auto* h = static_cast<BleMacosHandle*>(handle);

        if (h->peripheral && h->readChr) {
            [h->peripheral setNotifyValue:NO forCharacteristic:h->readChr];
        }
        if (h->manager && h->peripheral) {
            [h->manager cancelPeripheralConnection:h->peripheral];
        }

        h->connected = false;
        if (h->pipe_read >= 0)  ::close(h->pipe_read);
        if (h->pipe_write >= 0) ::close(h->pipe_write);
        h->pipe_read = h->pipe_write = -1;

        delete h;
    }
}

// ── ble_is_connected ────────────────────────────────────────────────────────

bool ble_is_connected(ble_handle_t handle) {
    if (!handle) return false;
    return static_cast<BleMacosHandle*>(handle)->connected;
}

// ── ble_read_fd ─────────────────────────────────────────────────────────────

int ble_read_fd(ble_handle_t handle) {
    if (!handle) return -1;
    auto* h = static_cast<BleMacosHandle*>(handle);
    return h->connected ? h->pipe_read : -1;
}

// ── ble_write ───────────────────────────────────────────────────────────────

void ble_write(ble_handle_t handle, const uint8_t* data, size_t len) {
    if (!handle || !data || len == 0) return;
    auto* h = static_cast<BleMacosHandle*>(handle);
    if (!h->connected || !h->peripheral || !h->writeChr) return;

    CBCharacteristicWriteType wtype = h->use_response
        ? CBCharacteristicWriteWithResponse
        : CBCharacteristicWriteWithoutResponse;

    int cs = h->chunk_sz;
    for (size_t off = 0; off < len; off += (size_t)cs) {
        size_t clen = std::min((size_t)cs, len - off);
        NSData* chunk = [NSData dataWithBytes:(data + off) length:clen];
        // Dispatch on CB queue — required by CoreBluetooth (same as SimpleBLE)
        dispatch_async(h->queue, ^{
            if (h->peripheral && h->writeChr)
                [h->peripheral writeValue:chunk
                       forCharacteristic:h->writeChr
                                    type:wtype];
        });
    }
}

// ── ble_mtu / ble_chunk_size / ble_can_write_without_response ───────────────

int ble_mtu(ble_handle_t handle) {
    if (!handle) return 23;
    return static_cast<BleMacosHandle*>(handle)->mtu_val;
}

int ble_chunk_size(ble_handle_t handle) {
    if (!handle) return 20;
    return static_cast<BleMacosHandle*>(handle)->chunk_sz;
}

bool ble_can_write_without_response(ble_handle_t handle) {
    if (!handle) return false;
    return static_cast<BleMacosHandle*>(handle)->can_wwr;
}

void ble_wake(ble_handle_t handle) {
    if (!handle) return;
    auto* h = static_cast<BleMacosHandle*>(handle);
    if (!h->connected || !h->peripheral) return;

    // Read first readable characteristic to wake the GATT server
    for (CBService* svc in h->peripheral.services) {
        for (CBCharacteristic* chr in svc.characteristics) {
            if (chr.properties & CBCharacteristicPropertyRead) {
                [h->peripheral readValueForCharacteristic:chr];
                return;
            }
        }
    }
}

} // extern "C"

#endif // __APPLE__
