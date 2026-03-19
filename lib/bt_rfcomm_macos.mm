// ============================================================================
// bt_rfcomm_macos.mm — macOS Classic Bluetooth RFCOMM via IOBluetooth
//
// Provides C-linkage functions declared in bt_rfcomm_macos.h.
// Compiled only on macOS as Objective-C++ (.mm).
//
// Architecture:
//   - IOBluetooth uses Obj-C delegate callbacks for data reception.
//   - A pipe() pair bridges incoming data into a select()-able fd.
//   - The write path uses synchronous IOBluetoothRFCOMMChannel writeSync.
//   - Scan and inspect use IOBluetoothDeviceInquiry + SDP queries with
//     a temporary NSRunLoop to block until results arrive.
// ============================================================================
#ifdef __APPLE__

#import <IOBluetooth/IOBluetooth.h>
#import <Foundation/Foundation.h>

#include "bt_rfcomm_macos.h"

#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>
#include <sstream>

// ─── Helpers ────────────────────────────────────────────────────────────────

// Convert "XX:XX:XX:XX:XX:XX" or "XX-XX-XX-XX-XX-XX" to NSString with dashes
// IOBluetooth expects "XX-XX-XX-XX-XX-XX" format.
static NSString* normalize_address(const char* addr) {
    NSString* s = [NSString stringWithUTF8String:addr];
    s = [s stringByReplacingOccurrencesOfString:@":" withString:@"-"];
    return s;
}

// Returns true if string looks like a MAC address (XX:XX:XX:XX:XX:XX or XX-XX...)
static bool looks_like_mac(const char* s) {
    // 17 chars, hex digits separated by : or -
    int len = (int)strlen(s);
    if (len != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (s[i] != ':' && s[i] != '-') return false; }
        else             { if (!isxdigit((unsigned char)s[i])) return false; }
    }
    return true;
}

// Resolve a name or MAC to an IOBluetoothDevice.
// If address looks like a MAC, use deviceWithAddressString.
// Otherwise search paired/known devices by name (case-insensitive substring).
static IOBluetoothDevice* resolve_device(const char* address) {
    if (looks_like_mac(address)) {
        return [IOBluetoothDevice deviceWithAddressString:normalize_address(address)];
    }
    // Name-based lookup: search paired devices
    NSString* target = [NSString stringWithUTF8String:address];
    NSArray<IOBluetoothDevice*>* paired = [IOBluetoothDevice pairedDevices];
    for (IOBluetoothDevice* dev in paired) {
        NSString* name = [dev name];
        if (!name) continue;
        if ([name localizedCaseInsensitiveContainsString:target] ||
            [name isEqualToString:target]) {
            std::cout << "  Resolved \"" << address << "\" → "
                      << [[dev addressString] UTF8String]
                      << " (" << [name UTF8String] << ")\n";
            return dev;
        }
    }
    // Also check recently-found devices
    NSArray<IOBluetoothDevice*>* recent = [IOBluetoothDevice recentDevices:20];
    for (IOBluetoothDevice* dev in recent) {
        NSString* name = [dev name];
        if (!name) continue;
        if ([name localizedCaseInsensitiveContainsString:target]) {
            std::cout << "  Resolved \"" << address << "\" → "
                      << [[dev addressString] UTF8String]
                      << " (" << [name UTF8String] << ")  [recent]\n";
            return dev;
        }
    }
    return nil;
}

static std::string hr(char ch = '-') {
    return std::string(60, ch);
}

// ─── RFCOMM Connection Handle ───────────────────────────────────────────────

// Forward declare the delegate class
@class BtRfcommDelegate;

struct BtMacosHandle {
    IOBluetoothDevice*        device   = nil;
    IOBluetoothRFCOMMChannel* channel  = nil;
    BtRfcommDelegate*         delegate = nil;
    int pipe_read  = -1;
    int pipe_write = -1;
    std::atomic<bool> connected{false};
};

// ─── RFCOMM Delegate ────────────────────────────────────────────────────────

@interface BtRfcommDelegate : NSObject <IOBluetoothRFCOMMChannelDelegate>
{
    BtMacosHandle* _handle;
}
- (instancetype)initWithHandle:(BtMacosHandle*)handle;
@end

@implementation BtRfcommDelegate

- (instancetype)initWithHandle:(BtMacosHandle*)handle {
    self = [super init];
    if (self) {
        _handle = handle;
    }
    return self;
}

// Data received from remote device → write to pipe for select()
- (void)rfcommChannelData:(IOBluetoothRFCOMMChannel*)rfcommChannel
                     data:(void*)dataPointer
                   length:(size_t)dataLength {
    (void)rfcommChannel;
    if (_handle->pipe_write >= 0 && dataLength > 0) {
        // Write all data to pipe; if pipe is full the bridge will catch up
        size_t written = 0;
        while (written < dataLength) {
            ssize_t n = write(_handle->pipe_write,
                              (const uint8_t*)dataPointer + written,
                              dataLength - written);
            if (n <= 0) break;
            written += (size_t)n;
        }
    }
}

// Channel closed by remote side
- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel*)rfcommChannel {
    (void)rfcommChannel;
    _handle->connected = false;
    // Close write end → read end gets EOF → select() detects disconnect
    if (_handle->pipe_write >= 0) {
        close(_handle->pipe_write);
        _handle->pipe_write = -1;
    }
    std::cerr << "  [BT] RFCOMM channel closed by remote.\n";
}

// Channel opened (async completion)
- (void)rfcommChannelOpenComplete:(IOBluetoothRFCOMMChannel*)rfcommChannel
                           status:(IOReturn)error {
    (void)rfcommChannel;
    if (error != kIOReturnSuccess) {
        std::cerr << "  [BT] RFCOMM open failed: 0x"
                  << std::hex << error << std::dec << "\n";
        _handle->connected = false;
    }
}

@end

// ─── SDP Query Delegate (blocks until query completes) ──────────────────────

@interface BtSdpQueryDelegate : NSObject
{
    BOOL _done;
    IOReturn _status;
}
@property (nonatomic, readonly) BOOL isDone;
@property (nonatomic, readonly) IOReturn status;
@end

@implementation BtSdpQueryDelegate

- (instancetype)init {
    self = [super init];
    if (self) { _done = NO; _status = kIOReturnSuccess; }
    return self;
}

- (BOOL)isDone { return _done; }
- (IOReturn)status { return _status; }

- (void)sdpQueryComplete:(IOBluetoothDevice*)device status:(IOReturn)status {
    (void)device;
    _status = status;
    _done = YES;
}

@end

// Perform SDP query with run loop to ensure we wait for completion.
// On macOS, SDP queries require an active baseband connection.
// We open one explicitly, run the SDP query, and optionally keep it.
static bool perform_sdp_query(IOBluetoothDevice* device,
                              double timeout_s = 15.0,
                              bool keep_connection = false)
{
    // 1. Establish baseband connection (needed for SDP on non-paired devices)
    bool we_opened = false;
    if (![device isConnected]) {
        std::cout << "  Opening baseband connection...\n";
        std::cout.flush();
        IOReturn cret = [device openConnection];
        if (cret != kIOReturnSuccess) {
            std::cerr << "  Cannot open connection to device: 0x"
                      << std::hex << cret << std::dec << "\n"
                      << "  Make sure the device is discoverable or paired.\n";
            return false;
        }
        we_opened = true;
    }

    // 2. Perform SDP query with delegate
    BtSdpQueryDelegate* delegate = [[BtSdpQueryDelegate alloc] init];
    IOReturn ret = [device performSDPQuery:delegate];
    if (ret != kIOReturnSuccess) {
        std::cerr << "  SDP query initiation failed: 0x"
                  << std::hex << ret << std::dec << "\n";
        if (we_opened && !keep_connection) [device closeConnection];
        return false;
    }

    // 3. Spin run loop until the delegate signals completion
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeout_s];
    while (![delegate isDone] &&
           [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                    beforeDate:deadline]) {
        if ([[NSDate date] compare:deadline] != NSOrderedAscending) break;
    }

    bool ok = true;
    if (![delegate isDone]) {
        std::cerr << "  SDP query timed out (" << (int)timeout_s << "s).\n";
        ok = false;
    } else if ([delegate status] != kIOReturnSuccess) {
        std::cerr << "  SDP query failed: 0x"
                  << std::hex << [delegate status] << std::dec << "\n";
        ok = false;
    }

    // 4. Close the baseband connection unless caller wants to keep it
    //    (e.g. bt_macos_connect will reuse it for RFCOMM)
    if (we_opened && !keep_connection) {
        [device closeConnection];
    }

    return ok;
}

// ─── SDP helper: find SPP RFCOMM channel ────────────────────────────────────

static int sdp_find_spp_channel(IOBluetoothDevice* device,
                                bool keep_connection = false) {
    if (!perform_sdp_query(device, 15.0, keep_connection)) return -1;

    // SPP UUID: 00001101-0000-1000-8000-00805F9B34FB
    IOBluetoothSDPUUID* sppUUID =
        [IOBluetoothSDPUUID uuid16:kBluetoothSDPUUID16ServiceClassSerialPort];

    NSArray* services = [device services];
    if (!services) return -1;

    for (IOBluetoothSDPServiceRecord* record in services) {
        if ([record hasServiceFromArray:@[sppUUID]]) {
            BluetoothRFCOMMChannelID channelID = 0;
            if ([record getRFCOMMChannelID:&channelID] == kIOReturnSuccess) {
                return (int)channelID;
            }
        }
    }
    return -1;
}

// ─── Connection API ─────────────────────────────────────────────────────────

extern "C" bt_macos_handle_t bt_macos_connect(const char* address, int channel) {
    @autoreleasepool {
        IOBluetoothDevice* device = resolve_device(address);
        if (!device) {
            std::cerr << "  Cannot find Bluetooth device \"" << address << "\".\n"
                      << "  Make sure it is paired or pass the MAC address directly.\n";
            return nullptr;
        }

        int ch = channel;

        // Auto-detect via SDP if channel == 0
        if (ch == 0) {
            std::cout << "  SDP lookup for SPP on " << address << "...\n";
            std::cout.flush();
            ch = sdp_find_spp_channel(device, true /* keep_connection */);
            if (ch < 0) {
                std::cerr << "  SDP: no SPP service found.  Use --channel to specify manually.\n";
                return nullptr;
            }
            std::cout << "  SDP: found SPP on RFCOMM channel " << ch << "\n";
        }

        // Create pipe pair for RX bridging
        int pipefds[2];
        if (pipe(pipefds) < 0) {
            std::cerr << "  pipe() failed: " << strerror(errno) << "\n";
            return nullptr;
        }

        auto* handle = new BtMacosHandle();
        handle->device     = device;
        handle->pipe_read  = pipefds[0];
        handle->pipe_write = pipefds[1];

        // Create delegate
        handle->delegate = [[BtRfcommDelegate alloc] initWithHandle:handle];

        // Open RFCOMM channel
        IOBluetoothRFCOMMChannel* rfcommChannel = nil;
        std::cout << "  Connecting RFCOMM channel " << ch
                  << " on " << address << "...\n";
        std::cout.flush();

        IOReturn ret = [device openRFCOMMChannelSync:&rfcommChannel
                                       withChannelID:(BluetoothRFCOMMChannelID)ch
                                            delegate:handle->delegate];

        if (ret != kIOReturnSuccess || !rfcommChannel) {
            std::cerr << "  RFCOMM connect failed: 0x"
                      << std::hex << ret << std::dec << "\n";
            close(handle->pipe_read);
            close(handle->pipe_write);
            delete handle;
            return nullptr;
        }

        handle->channel   = rfcommChannel;
        handle->connected = true;

        // Set non-blocking on read end of pipe
        int fl = fcntl(handle->pipe_read, F_GETFL, 0);
        fcntl(handle->pipe_read, F_SETFL, fl | O_NONBLOCK);

        std::cout << "  Connected.  RFCOMM channel=" << ch << "\n";
        std::cout.flush();
        return (bt_macos_handle_t)handle;
    }
}

extern "C" void bt_macos_disconnect(bt_macos_handle_t h) {
    if (!h) return;
    @autoreleasepool {
        auto* handle = (BtMacosHandle*)h;

        if (handle->channel) {
            [handle->channel closeChannel];
            handle->channel = nil;
        }
        if (handle->device) {
            [handle->device closeConnection];
            handle->device = nil;
        }
        handle->connected = false;

        if (handle->pipe_write >= 0) {
            close(handle->pipe_write);
            handle->pipe_write = -1;
        }
        if (handle->pipe_read >= 0) {
            close(handle->pipe_read);
            handle->pipe_read = -1;
        }

        handle->delegate = nil;
        delete handle;
    }
}

extern "C" int bt_macos_read_fd(bt_macos_handle_t h) {
    if (!h) return -1;
    return ((BtMacosHandle*)h)->pipe_read;
}

extern "C" void bt_macos_write(bt_macos_handle_t h, const uint8_t* data, size_t len) {
    if (!h || len == 0) return;
    @autoreleasepool {
        auto* handle = (BtMacosHandle*)h;
        if (!handle->connected || !handle->channel) return;

        // writeSync blocks until data is sent
        IOReturn ret = [handle->channel writeSync:(void*)data length:(UInt16)len];
        if (ret != kIOReturnSuccess) {
            std::cerr << "  [BT] write error: 0x"
                      << std::hex << ret << std::dec << "\n";
            handle->connected = false;
            // Close write end to signal EOF on read fd
            if (handle->pipe_write >= 0) {
                close(handle->pipe_write);
                handle->pipe_write = -1;
            }
        }
    }
}

extern "C" bool bt_macos_is_connected(bt_macos_handle_t h) {
    if (!h) return false;
    return ((BtMacosHandle*)h)->connected.load();
}

// ─── Device Inquiry Delegate (for scan) ─────────────────────────────────────

@interface BtScanDelegate : NSObject <IOBluetoothDeviceInquiryDelegate>
{
    BOOL _done;
}
@property (nonatomic, strong) NSMutableArray<IOBluetoothDevice*>* devices;
@property (nonatomic, readonly) BOOL isDone;
@end

@implementation BtScanDelegate

- (instancetype)init {
    self = [super init];
    if (self) {
        _devices = [NSMutableArray new];
        _done = NO;
    }
    return self;
}

- (BOOL)isDone { return _done; }

- (void)deviceInquiryDeviceFound:(IOBluetoothDeviceInquiry*)sender
                          device:(IOBluetoothDevice*)device {
    (void)sender;
    [_devices addObject:device];
}

- (void)deviceInquiryComplete:(IOBluetoothDeviceInquiry*)sender
                        error:(IOReturn)error
                      aborted:(BOOL)aborted {
    (void)sender; (void)error; (void)aborted;
    _done = YES;
}

@end

// ─── Scan API ───────────────────────────────────────────────────────────────

extern "C" void bt_macos_scan(double timeout_s) {
    @autoreleasepool {
        BtScanDelegate* delegate = [[BtScanDelegate alloc] init];

        IOBluetoothDeviceInquiry* inquiry =
            [IOBluetoothDeviceInquiry inquiryWithDelegate:delegate];
        [inquiry setInquiryLength:(uint8_t)std::min(timeout_s, 255.0)];
        [inquiry setUpdateNewDeviceNames:YES];

        std::cout << "Scanning for Classic BT devices ("
                  << (int)timeout_s << "s)...\n\n";

        IOReturn ret = [inquiry start];
        if (ret != kIOReturnSuccess) {
            std::cerr << "Bluetooth inquiry start failed: 0x"
                      << std::hex << ret << std::dec << "\n"
                      << "Make sure Bluetooth is enabled in System Settings.\n";
            return;
        }

        // Run the run loop until inquiry completes or timeout
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeout_s + 2.0];
        while (![delegate isDone] &&
               [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                        beforeDate:deadline]) {
            if ([[NSDate date] compare:deadline] != NSOrderedAscending) break;
        }
        [inquiry stop];

        // Print results
        NSArray<IOBluetoothDevice*>* found = delegate.devices;
        for (IOBluetoothDevice* dev in found) {
            NSString* name = [dev name];
            NSString* addr = [dev addressString];
            BluetoothClassOfDevice cod = [dev classOfDevice];

            std::cout << hr() << "\n";
            std::cout << "  Name   : "
                      << (name ? [name UTF8String] : "(unknown)") << "\n";
            std::cout << "  Address: "
                      << (addr ? [addr UTF8String] : "??") << "\n";
            std::cout << "  CoD    : 0x" << std::hex << std::setw(6)
                      << std::setfill('0') << cod << std::dec << "\n";
            std::cout << "\n";
        }

        std::cout << hr('=') << "\n";
        std::cout << "Found " << [found count] << " Classic BT device(s).\n";
        std::cout << "\nNext step:\n  bt_kiss_bridge --bt --inspect <ADDRESS>\n";
    }
}

// ─── Inspect API ────────────────────────────────────────────────────────────

extern "C" void bt_macos_inspect(const char* address) {
    @autoreleasepool {
        IOBluetoothDevice* device = resolve_device(address);
        if (!device) {
            std::cerr << "Cannot find Bluetooth device \"" << address << "\".\n"
                      << "  Make sure it is paired or pass the MAC address directly.\n";
            return;
        }

        std::string resolved = device.addressString
                               ? std::string([device.addressString UTF8String])
                               : std::string(address);
        std::cout << "Querying SDP on " << resolved << "...\n";
        if (!perform_sdp_query(device)) {
            return;
        }

        NSArray* services = [device services];
        if (!services || [services count] == 0) {
            std::cout << "No SDP services found.\n";
            return;
        }

        std::cout << "\nServices on " << address << ":\n\n";
        int spp_channel = -1;

        for (IOBluetoothSDPServiceRecord* record in services) {
            // Service name
            NSString* name = nil;
            NSDictionary* attrs = [record attributes];
            // Attribute 0x0100 = ServiceName
            IOBluetoothSDPDataElement* nameElem = attrs[@(0x0100)];
            if (nameElem) {
                name = [nameElem getStringValue];
            }

            std::cout << hr() << "\n";
            std::cout << "  Service: "
                      << (name ? [name UTF8String] : "(unnamed)") << "\n";

            // Service class UUIDs (attribute 0x0001)
            IOBluetoothSDPDataElement* classListElem = attrs[@(0x0001)];
            if (classListElem) {
                NSArray* uuidArray = [classListElem getArrayValue];
                if (uuidArray) {
                    for (IOBluetoothSDPDataElement* elem in uuidArray) {
                        IOBluetoothSDPUUID* uuid = [elem getUUIDValue];
                        if (uuid) {
                            const uint8_t* bytes = (const uint8_t*)[uuid bytes];
                            NSUInteger len = [uuid length];
                            std::ostringstream oss;
                            if (len == 2) {
                                oss << "0x" << std::hex << std::setw(4)
                                    << std::setfill('0')
                                    << ((unsigned)bytes[0] << 8 | bytes[1]);
                            } else {
                                for (NSUInteger i = 0; i < len; i++) {
                                    if (i == 4 || i == 6 || i == 8 || i == 10)
                                        oss << '-';
                                    oss << std::hex << std::setw(2)
                                        << std::setfill('0') << (unsigned)bytes[i];
                                }
                            }
                            std::cout << "  UUID   : " << oss.str() << "\n";
                        }
                    }
                }
            }

            // RFCOMM channel
            BluetoothRFCOMMChannelID channelID = 0;
            if ([record getRFCOMMChannelID:&channelID] == kIOReturnSuccess) {
                std::cout << "  RFCOMM : channel " << (int)channelID << "\n";

                // Check if it's SPP
                IOBluetoothSDPUUID* sppUUID =
                    [IOBluetoothSDPUUID uuid16:kBluetoothSDPUUID16ServiceClassSerialPort];
                if ([record hasServiceFromArray:@[sppUUID]]) {
                    if (spp_channel < 0) spp_channel = (int)channelID;
                }
            }

            std::cout << "\n";
        }

        std::cout << hr('=') << "\n";
        std::cout << "Found " << [services count] << " service(s).\n";

        if (spp_channel >= 0) {
            std::cout << "\nSuggested bridge command:\n";
            std::cout << "  bt_kiss_bridge --bt --device " << address
                      << " --channel " << spp_channel << "\n";
        }
    }
}

#endif // __APPLE__
