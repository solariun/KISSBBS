// ============================================================================
// bt_rfcomm_macos.h — C-linkage API for macOS Classic Bluetooth RFCOMM
//
// Provides opaque-handle functions callable from C++ (bt_kiss_bridge.cpp)
// without requiring Objective-C.  Implementation lives in bt_rfcomm_macos.mm.
// ============================================================================
#pragma once
#ifdef __APPLE__

#include <cstdint>
#include <cstddef>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque connection handle
typedef void* bt_macos_handle_t;

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

// Connect RFCOMM to a Classic BT device.
//   address : "XX:XX:XX:XX:XX:XX" (or "XX-XX-XX-XX-XX-XX")
//   channel : RFCOMM channel number, or 0 to auto-detect via SDP (SPP 0x1101)
// Returns opaque handle on success, NULL on failure.
bt_macos_handle_t bt_macos_connect(const char* address, int channel);

// Disconnect and release all resources.
void bt_macos_disconnect(bt_macos_handle_t h);

// Returns a pipe read fd suitable for select()/poll().
// Data arriving on the RFCOMM channel is bridged to this fd.
// Returns -1 if not connected.
int bt_macos_read_fd(bt_macos_handle_t h);

// Write data to the RFCOMM channel (blocking).
void bt_macos_write(bt_macos_handle_t h, const uint8_t* data, size_t len);

// True while the RFCOMM channel is open.
bool bt_macos_is_connected(bt_macos_handle_t h);

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

// Scan for nearby Classic BT devices (blocks for timeout_s seconds).
// Prints results to stdout in the same format as the Linux HCI scan.
void bt_macos_scan(double timeout_s);

// Inspect a device's SDP services.  Prints service names, UUIDs, and
// RFCOMM channel numbers.  Suggests a bridge command if SPP is found.
void bt_macos_inspect(const char* address);

#ifdef __cplusplus
}
#endif

#endif // __APPLE__
