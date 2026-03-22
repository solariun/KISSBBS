// ============================================================================
// bt_ble_native.h — C-linkage API for native BLE (no SimpleBLE dependency)
//
// Linux : BlueZ D-Bus (libdbus)
// macOS : CoreBluetooth framework
//
// Provides opaque-handle functions callable from C++ (bt_kiss_bridge.cpp)
// without requiring platform-specific includes.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque connection handle
typedef void* ble_handle_t;

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

// Connect to a BLE peripheral.
//   address     : "XX:XX:XX:XX:XX:XX" (or device name for matching)
//   svc_uuid    : GATT service UUID, or NULL for auto-detect
//   write_uuid  : GATT write characteristic UUID, or NULL for auto-detect
//   read_uuid   : GATT notify characteristic UUID, or NULL for auto-detect
//   timeout_s   : scan/connect timeout in seconds
//   mtu_cap     : maximum MTU to request (0 = use default)
//   write_with_response : force write-with-response mode
// Returns opaque handle on success, NULL on failure.
// Auto-detect: skips GAP/GATT services, finds first service with both
// a writable and notifiable characteristic.
ble_handle_t ble_connect(const char* address,
                          const char* svc_uuid,
                          const char* write_uuid,
                          const char* read_uuid,
                          double timeout_s,
                          int mtu_cap,
                          bool write_with_response);

// Disconnect and release all resources.
void ble_disconnect(ble_handle_t h);

// True while the BLE connection is active.
bool ble_is_connected(ble_handle_t h);

// Returns a pipe read fd suitable for select()/poll().
// Notify data from the read characteristic is bridged to this fd.
// Returns -1 if not connected.
int ble_read_fd(ble_handle_t h);

// Write data to the write characteristic.
// Data is chunked internally based on negotiated MTU.
void ble_write(ble_handle_t h, const uint8_t* data, size_t len);

// Negotiated MTU (after connect).  Returns 23 if unknown.
int ble_mtu(ble_handle_t h);

// Chunk size = MTU - 3 (ATT overhead).
int ble_chunk_size(ble_handle_t h);

// Whether the write characteristic supports write-without-response.
bool ble_can_write_without_response(ble_handle_t h);

// Wake a connected BLE peripheral by reading a standard GATT characteristic.
// Call after connect to ensure the device's GATT server is active.
// No-op if not connected.
void ble_wake(ble_handle_t h);

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

// Scan for nearby BLE peripherals (blocks for timeout_s seconds).
// Prints results to stdout.
// show_all: if true, include unnamed devices; if false, only named devices.
void ble_scan(double timeout_s, bool show_all);

// Inspect a device's GATT services.  Connects, enumerates services and
// characteristics, prints to stdout, then disconnects.
void ble_inspect(const char* address, double timeout_s);

// Auto-detect service/write/read UUIDs without full connect.
// Returns true on success, fills svc_out, write_out, read_out.
// Buffers must be at least buf_len bytes.
bool ble_auto_detect(const char* address, double timeout_s,
                      char* svc_out, char* write_out, char* read_out,
                      size_t buf_len);

#ifdef __cplusplus
}
#endif
