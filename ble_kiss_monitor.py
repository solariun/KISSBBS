#!/usr/bin/env python3
"""
ble_kiss_monitor.py — BLE KISS TNC scanner, AX.25 monitor, and PTY serial bridge

Requires:  pip install bleak

Modes:
  --scan               Discover nearby BLE devices
  --inspect  <ADDR>    List every service/characteristic of a device
  --device   <ADDR>    Connect and create a virtual serial port (PTY) bridged
                       to the BLE TNC, while decoding traffic in real time.
                       Requires --service / --write / --read.

Examples:
  python3 ble_kiss_monitor.py --scan --timeout 15
  python3 ble_kiss_monitor.py --inspect AA:BB:CC:DD:EE:FF
  python3 ble_kiss_monitor.py \\
      --device   AA:BB:CC:DD:EE:FF \\
      --service  00000001-ba2a-46c9-ae49-01b0961f68bb \\
      --write    00000003-ba2a-46c9-ae49-01b0961f68bb \\
      --read     00000002-ba2a-46c9-ae49-01b0961f68bb

  Then in another terminal:
      ax25client -c W1AW -r W1BBS-1 /dev/pts/3   # (use the PTY path printed above)
"""

import asyncio
import sys
import os
import tty
import termios
import fcntl
import argparse
import datetime
from typing import List, Tuple, Optional

try:
    from bleak import BleakScanner, BleakClient
    from bleak.backends.characteristic import BleakGATTCharacteristic
except ImportError:
    print("bleak not found.  Install with:  pip install bleak")
    sys.exit(1)

# ── KISS constants ────────────────────────────────────────────────────────────
FEND  = 0xC0
FESC  = 0xDB
TFEND = 0xDC
TFESC = 0xDD

KISS_CMD = {
    0:  "DATA", 1:  "TXDELAY", 2:  "P",    3: "SLOTTIME",
    4:  "TXTAIL", 5: "FULLDUPLEX", 6: "SETHW", 15: "RETURN",
}

# ── AX.25 frame type tables ───────────────────────────────────────────────────
U_FRAMES = {
    0x2F: "SABM",  0x3F: "SABM(P)",
    0x43: "DISC",  0x53: "DISC(P)",
    0x63: "UA",    0x73: "UA(F)",
    0x0F: "DM",    0x1F: "DM(F)",
    0x87: "FRMR",  0x97: "FRMR(F)",
    0x03: "UI",    0x13: "UI(P)",
}
S_TYPES = {0x01: "RR", 0x05: "RNR", 0x09: "REJ", 0x0D: "SREJ"}

PID_NAMES = {
    0xF0: "NoL3", 0xCF: "NET/ROM", 0xCC: "IP", 0xCD: "ARP",
    0x08: "ROSE", 0x01: "X25PLP",
}

# ── Address decoder ───────────────────────────────────────────────────────────
def decode_addr(data: bytes, offset: int) -> Tuple[str, bool]:
    """Return (callsign-ssid, end_of_address_flag) from 7 bytes at offset."""
    call = "".join(chr(data[offset + i] >> 1) for i in range(6)).rstrip()
    ssid_byte = data[offset + 6]
    ssid = (ssid_byte >> 1) & 0x0F
    end  = bool(ssid_byte & 0x01)
    label = f"{call}-{ssid}" if ssid else call
    return label, end

# ── AX.25 full decoder ────────────────────────────────────────────────────────
def decode_ax25(raw: bytes) -> dict:
    result = {"dest": "?", "src": "?", "via": [], "ctrl_hex": "",
              "type": "?", "info": b"", "summary": ""}

    if len(raw) < 15:
        result["type"]    = "TRUNCATED"
        result["summary"] = f"[frame too short: {len(raw)} bytes] {raw.hex()}"
        return result

    dest, _       = decode_addr(raw, 0)
    src,  end_src = decode_addr(raw, 7)
    result["dest"] = dest
    result["src"]  = src

    offset = 14
    via: List[str] = []
    while not end_src and offset + 7 <= len(raw):
        rep, end_src = decode_addr(raw, offset)
        via.append(rep)
        offset += 7
    result["via"] = via

    if offset >= len(raw):
        result["type"]    = "NO-CTRL"
        result["summary"] = f"{src} → {dest}  [no control byte]"
        return result

    ctrl = raw[offset]
    result["ctrl_hex"] = f"0x{ctrl:02X}"
    pf   = bool(ctrl & 0x10)

    if (ctrl & 0x01) == 0:                            # ── I-frame ──
        ns  = (ctrl >> 1) & 0x07
        nr  = (ctrl >> 5) & 0x07
        pid = raw[offset + 1] if offset + 1 < len(raw) else None
        info = raw[offset + 2:] if offset + 2 < len(raw) else b""
        result["info"] = info
        pid_str  = f"PID={PID_NAMES.get(pid, f'0x{pid:02X}')}" if pid is not None else ""
        data_str = ""
        if info:
            try:
                data_str = f' "{info.decode("ascii", errors="replace")}"'
            except Exception:
                data_str = f" [{info.hex()}]"
        ftype = f"I(NS={ns},NR={nr}{'P' if pf else ''})"
        result["type"]    = "I"
        result["summary"] = f"{src} → {dest}  [{ftype}] {pid_str}{data_str}"

    elif (ctrl & 0x03) == 0x01:                       # ── S-frame ──
        nr    = (ctrl >> 5) & 0x07
        stype = S_TYPES.get(ctrl & 0x0F, f"S?{ctrl & 0x0F:X}")
        pf_s  = "P/F" if pf else ""
        result["type"]    = stype
        result["summary"] = f"{src} → {dest}  [{stype}(NR={nr}{pf_s})]"

    else:                                              # ── U-frame ──
        base  = ctrl & ~0x10
        ftype = U_FRAMES.get(ctrl, U_FRAMES.get(base, f"U?0x{ctrl:02X}"))
        result["type"]    = ftype.split("(")[0]
        via_str = f" via {','.join(via)}" if via else ""
        result["summary"] = f"{src} → {dest}{via_str}  [{ftype}]"

    return result

# ── Stateful KISS decoder ─────────────────────────────────────────────────────
class KissDecoder:
    def __init__(self):
        self._buf:      bytearray = bytearray()
        self._in_frame: bool      = False
        self._escape:   bool      = False

    def feed(self, data: bytes) -> List[Tuple[int, int, bytes]]:
        frames = []
        for b in data:
            if b == FEND:
                if self._in_frame and len(self._buf) > 1:
                    cmd  = self._buf[0]
                    port = (cmd >> 4) & 0x0F
                    typ  = cmd & 0x0F
                    frames.append((port, typ, bytes(self._buf[1:])))
                self._buf.clear()
                self._in_frame = True
                self._escape   = False
            elif not self._in_frame:
                pass
            elif b == FESC:
                self._escape = True
            elif self._escape:
                self._escape = False
                self._buf.append(FEND if b == TFEND else FESC if b == TFESC else b)
            else:
                self._buf.append(b)
        return frames

# ── PTY helpers ───────────────────────────────────────────────────────────────
def open_pty() -> Tuple[int, int, str]:
    """
    Create a PTY pair.  Returns (master_fd, slave_fd, slave_path).
    The slave behaves like a raw serial port — set to raw, no echo.
    Keep slave_fd open so the PTY stays alive even when no client is attached.
    """
    master_fd, slave_fd = os.openpty()

    # Set slave to raw mode (like a serial port — no line editing, no echo)
    tty.setraw(slave_fd)

    # Set master to non-blocking so asyncio can poll it without deadlock
    flags = fcntl.fcntl(master_fd, fcntl.F_GETFL)
    fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    slave_path = os.ttyname(slave_fd)
    return master_fd, slave_fd, slave_path

# ── Helpers ───────────────────────────────────────────────────────────────────
def ts() -> str:
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

def hr(ch: str = "─", n: int = 68) -> str:
    return ch * n

# ── SCAN mode ─────────────────────────────────────────────────────────────────
async def scan(timeout: float) -> None:
    print(f"Scanning for BLE devices ({timeout:.0f}s)…\n")
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)

    if not found:
        print("No devices found.")
        return

    for addr, (dev, adv) in sorted(found.items(), key=lambda x: -(x[1][1].rssi or -999)):
        name = dev.name or "(no name)"
        print(hr())
        print(f"  Name   : {name}")
        print(f"  Address: {addr}")
        print(f"  RSSI   : {adv.rssi} dBm")
        if adv.service_uuids:
            print(f"  Services advertised:")
            for u in adv.service_uuids:
                print(f"    {u}")
        if adv.manufacturer_data:
            for cid, mdata in adv.manufacturer_data.items():
                print(f"  Manufacturer data  [0x{cid:04X}]: {mdata.hex()}")
        print()

    print(hr())
    print(f"Found {len(found)} device(s).")
    print(f"\nNext step → enumerate characteristics:")
    print(f"  python3 {sys.argv[0]} --inspect <ADDRESS>")

# ── INSPECT mode ──────────────────────────────────────────────────────────────
async def inspect(address: str) -> None:
    print(f"Connecting to {address} for service inspection…")
    async with BleakClient(address) as client:
        print(f"Connected.  MTU={client.mtu_size}\n")
        for svc in client.services:
            print(f"{'═'*68}")
            print(f"Service : {svc.uuid}")
            print(f"          {svc.description}")
            for ch in svc.characteristics:
                props = " | ".join(sorted(ch.properties))
                print(f"\n  Characteristic: {ch.uuid}")
                print(f"  Handle        : 0x{ch.handle:04X}")
                print(f"  Properties    : {props}")
                print(f"  Description   : {ch.description}")
                if "read" in ch.properties:
                    try:
                        val = await client.read_gatt_char(ch.uuid)
                        print(f"  Current value : {val.hex()}  {val!r}")
                    except Exception as e:
                        print(f"  Current value : (read error: {e})")
                for desc in ch.descriptors:
                    print(f"    Descriptor  : {desc.uuid}  {desc.description}")
        print(f"\n{'═'*68}")
        print("\nSerial bridge command:")
        print(f"  python3 {sys.argv[0]} \\")
        print(f"      --device   {address} \\")
        print(f"      --service  <SERVICE-UUID> \\")
        print(f"      --write    <WRITE-CHAR-UUID> \\")
        print(f"      --read     <NOTIFY-CHAR-UUID>")

# ── SERIAL BRIDGE + MONITOR mode ──────────────────────────────────────────────
async def bridge(address: str, service_uuid: str,
                 write_uuid: str, read_uuid: str,
                 requested_mtu: int = 517) -> None:

    decoder   = KissDecoder()
    rx_frames = 0
    tx_frames = 0

    # ── Open PTY ──────────────────────────────────────────────────────────────
    master_fd, slave_fd, slave_path = open_pty()

    print(hr("═"))
    print(f"  BLE KISS Serial Bridge + AX.25 Monitor")
    print(hr("═"))
    print(f"  Device     : {address}")
    print(f"  Service    : {service_uuid}")
    print(f"  Read char  : {read_uuid}  (notify → PTY)")
    print(f"  Write char : {write_uuid}  (PTY → BLE)")
    print(hr("─"))
    print(f"  Virtual serial port ready:")
    print(f"")
    print(f"      {slave_path}")
    print(f"")
    print(f"  Use this path as the serial device in ax25client / Direwolf / etc.")
    print(f"  Example:")
    print(f"      ax25client -c W1AW -r W1BBS-1 {slave_path}")
    print(hr("─"))
    print(f"  Connecting to BLE…")

    async with BleakClient(address) as client:
        # Request MTU (bleak 0.21+ on Linux; silently ignored on macOS/Windows)
        if requested_mtu != client.mtu_size:
            try:
                await client.request_mtu(requested_mtu)
            except AttributeError:
                pass  # backend does not support request_mtu (macOS, Windows)
            except Exception as e:
                print(f"  MTU request note: {e}")

        mtu        = client.mtu_size
        chunk_size = max(1, mtu - 3)   # ATT overhead = 3 bytes

        # Explicitly await full service discovery — async with only calls
        # connect(); on some backends _services_resolved stays False until
        # get_services() is awaited, causing write_gatt_char to error.
        svcs = await client.get_services()
        write_char = svcs.get_characteristic(write_uuid)
        if write_char is None:
            print(f"  ERROR: write characteristic {write_uuid} not found in discovered services.")
            print(f"  Available characteristics:")
            for svc in svcs:
                for ch in svc.characteristics:
                    print(f"    {ch.uuid}  [{', '.join(ch.properties)}]")
            return

        # Prefer write-without-response for throughput (KISS TNC standard)
        use_response = "write-without-response" not in write_char.properties

        print(f"  Connected.  MTU requested={requested_mtu}  negotiated={mtu}  chunk={chunk_size}b")
        print(f"  Write char : {write_char.uuid}  props=[{', '.join(write_char.properties)}]  response={use_response}")
        print(f"  Monitoring traffic.  Ctrl-C to stop.\n")

        loop = asyncio.get_running_loop()

        # ── BLE → PTY ─────────────────────────────────────────────────────────
        def on_notify(char: BleakGATTCharacteristic, data: bytearray) -> None:
            nonlocal rx_frames
            raw = bytes(data)
            now = ts()

            # Write raw bytes to PTY master → delivered to slave reader
            try:
                os.write(master_fd, raw)
            except OSError as e:
                print(f"[{now}]  PTY write error: {e}")
                return

            # Decode and display
            print(f"\n{hr('─')}")
            print(f"[{now}]  ← BLE→PTY  {len(raw):3d} bytes  raw: {raw.hex()}")

            frames = decoder.feed(raw)
            if not frames:
                print("  (buffering — no complete KISS frame yet)")
                return

            for port, typ, payload in frames:
                rx_frames += 1
                cmd_name = KISS_CMD.get(typ, f"cmd{typ}")
                print(f"  KISS  port={port}  type={typ}({cmd_name})")
                print(f"  AX25  payload ({len(payload)}b): {payload.hex()}")

                if typ == 0 and payload:
                    ax = decode_ax25(payload)
                    print(f"  ┌─ {ax['summary']}")
                    print(f"  │  ctrl={ax['ctrl_hex']}  type={ax['type']}")
                    if ax["via"]:
                        print(f"  │  via: {', '.join(ax['via'])}")
                    if ax["info"]:
                        try:
                            txt = ax["info"].decode("ascii", errors="replace")
                        except Exception:
                            txt = ax["info"].hex()
                        print(f"  └─ info ({len(ax['info'])}b): {txt!r}")
                    else:
                        print(f"  └─")

        # ── PTY → BLE ─────────────────────────────────────────────────────────
        def on_pty_readable() -> None:
            nonlocal tx_frames
            try:
                data = os.read(master_fd, 4096)
            except (OSError, BlockingIOError):
                return
            if not data:
                return

            now = ts()
            tx_frames += 1
            print(f"\n{hr('─')}")
            print(f"[{now}]  → PTY→BLE  {len(data):3d} bytes  raw: {data.hex()}")

            # Schedule async BLE write (chunked to MTU)
            async def do_write(payload: bytes) -> None:
                try:
                    for i in range(0, len(payload), chunk_size):
                        chunk = payload[i : i + chunk_size]
                        await client.write_gatt_char(write_char, chunk,
                                                     response=use_response)
                except Exception as e:
                    print(f"  BLE write error: {e}")

            loop.create_task(do_write(data))

        # ── Wire up and run ───────────────────────────────────────────────────
        await client.start_notify(read_uuid, on_notify)
        loop.add_reader(master_fd, on_pty_readable)

        try:
            while True:
                await asyncio.sleep(0.5)
        except (asyncio.CancelledError, KeyboardInterrupt):
            pass
        finally:
            loop.remove_reader(master_fd)
            await client.stop_notify(read_uuid)
            os.close(master_fd)
            os.close(slave_fd)

    print(f"\n{hr()}")
    print(f"  Session ended.  RX frames: {rx_frames}  TX frames: {tx_frames}")
    print(hr())

# ── Entry point ───────────────────────────────────────────────────────────────
def main() -> None:
    p = argparse.ArgumentParser(
        prog="ble_kiss_monitor.py",
        description="BLE KISS TNC serial bridge and AX.25 monitor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--scan",    action="store_true",
                      help="Scan for nearby BLE devices")
    mode.add_argument("--inspect", metavar="ADDRESS",
                      help="Enumerate all services/characteristics of a device")
    mode.add_argument("--device",  metavar="ADDRESS",
                      help="Connect and create a PTY serial bridge")

    p.add_argument("--timeout", type=float, default=10.0,
                   help="Scan timeout in seconds (default: 10)")
    p.add_argument("--mtu", type=int, default=517,
                   help="Max chunk size cap in bytes (default: 517; actual MTU is negotiated by the OS)")
    p.add_argument("--service", metavar="UUID",
                   help="GATT service UUID")
    p.add_argument("--write",   metavar="UUID",
                   help="Write characteristic UUID  (PTY → BLE)")
    p.add_argument("--read",    metavar="UUID",
                   help="Notify characteristic UUID  (BLE → PTY)")

    args = p.parse_args()

    if args.device and not (args.service and args.write and args.read):
        p.error("--device requires --service, --write, and --read")

    try:
        if args.scan:
            asyncio.run(scan(args.timeout))
        elif args.inspect:
            asyncio.run(inspect(args.inspect))
        else:
            asyncio.run(bridge(args.device, args.service, args.write, args.read,
                               requested_mtu=args.mtu))
    except KeyboardInterrupt:
        print("\nInterrupted.")

if __name__ == "__main__":
    main()
