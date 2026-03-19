# KISSBBS — Design & Architecture Reference

> This document is the engineering reference for contributors. It covers protocol background,
> internal architecture, class relationships, state machines, and design rationale.
> For usage and operator documentation see [README.md](README.md).

## Table of Contents

1. [AX.25 Protocol](#1-ax25-protocol)
2. [KISS Protocol](#2-kiss-protocol)
3. [APRS](#3-aprs)
4. [ax25lib — Layer Architecture](#4-ax25lib--layer-architecture)
5. [Object Relationship Diagram (ax25lib)](#5-object-relationship-diagram-ax25lib)
6. [UML Class Diagram (ax25lib)](#6-uml-class-diagram-ax25lib)
7. [AX.25 State Machine](#7-ax25-state-machine)
8. [Connection Sequence Diagrams](#8-connection-sequence-diagrams)
9. [Intrusive Container — Design Notes (ObjNode / ObjList)](#9-intrusive-container--design-notes-objnode--objlist)
10. [bt_kiss_bridge — Architecture](#10-bt_kiss_bridge--architecture)
    - [Object Relationship Diagram](#object-relationship-diagram)
    - [UML Class Diagram](#uml-class-diagram)
    - [Data Flow Diagram](#data-flow-diagram)
11. [BLE Transport Implementation Notes](#11-ble-transport-implementation-notes)

---

## 1. AX.25 Protocol

AX.25 is the link-layer protocol used in amateur (ham) radio packet networks.
Think of it as a stripped-down Ethernet designed for half-duplex radio channels.

**Addresses** — Every station has a *callsign* (up to 6 characters, e.g. `W1AW`)
plus a 0–15 *SSID* suffix, written `W1AW-7`.  On the wire each address occupies
exactly 7 bytes: the 6 callsign characters shifted left by one bit, followed by
a flag byte carrying the SSID and housekeeping bits.

**Frame types**

| Type | Purpose |
|------|---------|
| UI (Unnumbered Information) | Connectionless datagram — used for APRS beacons |
| SABM | Set Asynchronous Balanced Mode — opens a connection |
| UA | Unnumbered Acknowledgement — accepts SABM or DISC |
| DM | Disconnected Mode — rejects SABM |
| DISC | Disconnect — closes a connection |
| I-frame | Information frame — carries sequenced data |
| RR | Receive Ready — acknowledges I-frames, resumes suspended flow |
| REJ | Reject — requests retransmission from a given sequence number |

**Connected mode** (what `Connection` implements) uses a sliding window
(Go-Back-N, mod-8) with two timers:

* **T1** — Retransmit timer.  Dynamically computed:
  `max(t1_ms, window × mtu × 40000 / baud)`.  This ensures T1 is long enough
  for the full window to transit slow links (BLE, 1200 baud, etc.).  Default
  minimum is 15 000 ms.  If T1 expires before an ACK arrives the frame is
  retransmitted with P=1 to poll the remote.  After *N2* retries the link is
  declared failed.
* **T3** — Keep-alive / inactivity timer.  If no data is exchanged within T3 the
  station sends an RR poll (P=1) to verify the link is still alive.

**P/F poll tracking** — The library tracks outstanding P=1 polls internally.
When the window fills, the last I-frame is sent with P=1 to solicit an RR
response from the remote.  Incoming RR/RNR with F=1 are matched against
outstanding polls so the library never echoes back a spurious RR.  Applications
never need to manage polling — it is fully transparent.

**TX pacing** — Outgoing frames are spaced by TXDELAY (default 400 ms) to give
half-duplex radios time for TX/RX turnaround.  After receiving a frame, the
router also enforces a turnaround delay before responding.

---

## 2. KISS Protocol

KISS ("Keep It Simple, Stupid") is a thin serial framing protocol that lets a
computer talk to a TNC (Terminal Node Controller — the radio modem).

The computer sends and receives raw AX.25 frames wrapped in a simple envelope:

```
FEND  CMD  DATA...  FEND
```

Special byte values are escaped inside DATA so they cannot be confused with
envelope markers:

| Raw byte | On wire |
|----------|---------|
| `0xC0` (FEND) | `0xDB 0xDC` |
| `0xDB` (FESC) | `0xDB 0xDD` |

The TNC handles everything physical: radio timing, flag bytes, and FCS
checksums.  The library never sees or generates those.

---

## 3. APRS

APRS (Automatic Packet Reporting System) is built on top of AX.25 UI frames
with PID `0xF0`, sent to the destination callsign `APRS`.  The library lets you
send position reports and person-to-person messages and receive/route incoming
ones.

---

## 4. ax25lib — Layer Architecture

```
Your Application
       │
       ▼
   ┌────────┐
   │ Router │  Manages connections; routes incoming frames; exposes on_ui
   └────────┘
       │
       ▼
   ┌────────┐
   │  Kiss  │  Transport-agnostic KISS framing layer
   └────────┘
       │  open(dev, baud)  ← serial port (Serial / termios)
       │  open_fd(fd)      ← any POSIX fd: TCP socket, PTY, pipe
       │
    (wire / socket / PTY)
       │
      TNC  ──── Radio ──── Remote station
```

The layer stack is **intentionally thin**: each layer does exactly one job and
calls the layer above via a `std::function` callback, making the stack easy to
test (swap the serial layer with an in-memory hook) and easy to adapt (plug in a
different physical layer without touching the rest).

---

## 5. Object Relationship Diagram (ax25lib)

```
                              ┌─────────────────────────────────────────┐
                              │              ax25lib.hpp/cpp             │
                              └─────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────────┐
  │  ObjNode<T>  (template)  — self-managing intrusive node                  │
  │  ─────────────────────────────────────────────────────────────────────── │
  │  # ObjNode(ObjList<T>&)   ← protected; auto-inserts on construction      │
  │  # ~ObjNode()              ← protected; auto-removes on destruction       │
  │  - next_ : T*                                                             │
  │  - prev_ : T*                                                             │
  │  - list_ : ObjList<T>*                                                    │
  └──────────────────────────────────────────────────────────────────────────┘
          ▲ inherits
          │
  ┌───────────────────────────────────────────────────────────────────────┐
  │  Connection  extends ObjNode<Connection>                               │
  │  ─────────────────────────────────────────────────────────────────────│
  │  Callbacks: on_connect, on_disconnect, on_data                         │
  │  State: DISCONNECTED / CONNECTING / CONNECTED / DISCONNECTING          │
  │  AX.25 vars: vs_, vr_, va_, retry_                                     │
  │  Timers: T1 (retransmit), T3 (keep-alive)                              │
  │  Queues: send_buf_, unacked_                                            │
  │  ─────────────────────────────────────────────────────────────────────│
  │  + send(data)                                                           │
  │  + disconnect()                                                         │
  │  + tick(now_ms)                                                         │
  │  + has_unacked() → bool   (true if unacked_ or send_buf_ non-empty)    │
  └───────────────────────────────────────────────────────────────────────┘
          │ lives in (inserted/removed automatically via ObjNode ctor/dtor)
          ▼
  ┌───────────────────────────────────────────────────────────────────────┐
  │  ObjList<Connection>  (intrusive doubly-linked list)                   │
  │  ─────────────────────────────────────────────────────────────────────│
  │  - head_, tail_, size_   (private)                                      │
  │  - insert_back(item)     (called by ObjNode ctor — not public)          │
  │  - erase(item)           (called by ObjNode dtor — not public)          │
  │  + empty()  size()  begin()  end()  snapshot()                          │
  └───────────────────────────────────────────────────────────────────────┘
          │ owned by
          ▼
  ┌───────────────────────────────────────────────────────────────────────┐
  │  Router                                                                │
  │  ─────────────────────────────────────────────────────────────────────│
  │  + connect(remote) → Connection*                                       │
  │  + listen(on_accept)                                                   │
  │  + send_ui(dest, pid, data)                                            │
  │  + send_aprs(info)                                                     │
  │  + poll()                                                              │
  │  Callbacks: on_ui (all UI frames), on_monitor (all frames)             │
  └───────────────────────────────────────────────────────────────────────┘
          │ holds reference to
          ▼
  ┌───────────────────────────────────────────────────────────────────────┐
  │  Kiss                                                                  │
  │  ─────────────────────────────────────────────────────────────────────│
  │  + open(device, baud)   ← serial port                                  │
  │  + open_fd(fd)          ← any POSIX fd (TCP socket, PTY, pipe…)        │
  │  + fd() → int           ← active file descriptor                       │
  │  + is_open() → bool                                                     │
  │  + send_frame(ax25_bytes)                                              │
  │  + poll()  — reads fd, fires on_frame for each complete AX.25 frame    │
  │  Hooks: on_send_hook (test/simulation), test_inject(payload)           │
  └───────────────────────────────────────────────────────────────────────┘
          │ owns
          ▼
  ┌───────────────────────────────────────────────────────────────────────┐
  │  Serial                                                                │
  │  ─────────────────────────────────────────────────────────────────────│
  │  + open(dev, baud)   close()                                           │
  │  + read(buf, len)    write(buf, len)                                   │
  │  fd_ : int           (non-blocking POSIX file descriptor)              │
  └───────────────────────────────────────────────────────────────────────┘

  Supporting types (used by the layers above)

  ┌──────────────┐   ┌───────────────────────────────────┐
  │  Addr        │   │  Frame                             │
  │  ────────────│   │  ─────────────────────────────────│
  │  call[7]     │   │  dest, src : Addr                  │
  │  ssid : int  │   │  digis : vector<Addr>              │
  │  make(str)   │   │  ctrl, pid : uint8_t               │
  │  encode()    │   │  info : vector<uint8_t>            │
  │  decode()    │   │  type() → IFrame/UI/SABM/...       │
  │  str()       │   │  encode() / decode()               │
  └──────────────┘   └───────────────────────────────────┘

  ┌────────────────────────────────────────────────────────────┐
  │  kiss namespace                                             │
  │  ──────────────────────────────────────────────────────────│
  │  Constants: FEND, FESC, TFEND, TFESC                        │
  │  encode(payload) → KISS-wrapped bytes                       │
  │  Decoder::feed(buf, len) → vector<kiss::Frame>              │
  └────────────────────────────────────────────────────────────┘

  ┌────────────────────────────────────────────────────────────┐
  │  Config                                                     │
  │  ──────────────────────────────────────────────────────────│
  │  mycall, digis, mtu, window, t1_ms, t3_ms, n2, …           │
  └────────────────────────────────────────────────────────────┘
```

---

## 6. UML Class Diagram (ax25lib)

```mermaid
classDiagram
    class ObjNode~T~ {
        #ObjNode(ObjList~T~)
        #~ObjNode()
        -T* next_
        -T* prev_
        -ObjList~T~* list_
    }

    class ObjList~T~ {
        -T* head_
        -T* tail_
        -size_t size_
        -insert_back(T*)
        -erase(T*)
        +empty() bool
        +size() size_t
        +snapshot() vector~T*~
        +begin() iterator
        +end() iterator
    }

    class Serial {
        -int fd_
        -termios orig_
        +open(dev, baud) bool
        +close()
        +is_open() bool
        +fd() int
        +read(buf, len) ssize_t
        +write(buf, len) ssize_t
    }

    class Kiss {
        -Serial serial_
        -int ext_fd_
        -kiss_Decoder decoder_
        -function on_frame_
        +open(dev, baud) bool
        +open_fd(fd) bool
        +close()
        +is_open() bool
        +fd() int
        +send_frame(ax25) bool
        +set_txdelay(ms)
        +set_persistence(val)
        +poll()
        +test_inject(ax25)
        +on_send_hook
    }

    class Addr {
        +char call[7]
        +int ssid
        +bool repeated
        +make(str)$ Addr
        +encode(last) vector
        +decode(bytes)$ Addr
        +str() string
        +operator==()
    }

    class Frame {
        +Addr dest
        +Addr src
        +vector~Addr~ digis
        +uint8_t ctrl
        +uint8_t pid
        +vector~uint8_t~ info
        +type() Type
        +get_ns() int
        +get_nr() int
        +get_pf() bool
        +encode()$ vector
        +decode(raw, out)$ bool
    }

    class Config {
        +Addr mycall
        +vector~Addr~ digis
        +int mtu
        +int window
        +int t1_ms
        +int t3_ms
        +int n2
        +int txdelay
        +int persist
    }

    class Connection {
        +function on_connect
        +function on_disconnect
        +function on_data
        -State state_
        -Addr local_
        -Addr remote_
        -int vs_, vr_, va_, retry_
        -bool t1_run_, t3_run_
        -deque send_buf_
        -deque unacked_
        +send(data) bool
        +disconnect()
        +state() State
        +connected() bool
        +remote() Addr
        +local() Addr
        +tick(now)
        +has_unacked() bool
    }

    class Router {
        -Kiss kiss_
        -Config cfg_
        -ObjList~Connection~ conns_
        -function on_accept_
        +connect(remote) Connection*
        +listen(on_accept)
        +send_ui(dest, pid, data)
        +send_aprs(info)
        +poll()
        +on_ui
        +on_monitor
        +connections() ObjList~Connection~
    }

    ObjNode~T~ <|-- Connection : inherits
    ObjList~T~ "1" *-- "0..*" Connection : contains
    Router "1" *-- "1" ObjList~Connection~ : owns
    Router "1" --> "1" Kiss : uses
    Kiss "1" *-- "1" Serial : owns
    Connection "1" --> "1" Router : back-ref
    Frame "1" *-- "2..*" Addr : has
    Router "1" --> "1" Config : holds
    Connection "1" --> "1" Config : holds copy
```

---

## 7. AX.25 State Machine

```mermaid
stateDiagram-v2
    [*] --> DISCONNECTED

    DISCONNECTED --> CONNECTING : connect()\nsend SABM, start T1
    CONNECTING --> CONNECTED   : rcv UA\nstop T1, start T3\nfire on_connect
    CONNECTING --> DISCONNECTED : rcv DM\nor T1 × N2\nfire on_disconnect

    CONNECTED --> DISCONNECTING : disconnect()\nsend DISC, start T1
    DISCONNECTING --> DISCONNECTED : rcv UA\nor T1 × N2\nfire on_disconnect

    CONNECTED --> DISCONNECTED  : rcv DISC\nsend UA\nfire on_disconnect
    CONNECTED --> CONNECTED     : rcv I-frame\nsend RR, fire on_data
    CONNECTED --> CONNECTED     : send(data)\nsend I-frames (window)
    CONNECTED --> CONNECTED     : rcv RR\nslide window, send more I-frames
    CONNECTED --> CONNECTED     : T1 expires (retry < N2)\nretransmit
    CONNECTED --> CONNECTED     : T3 expires\nsend RR poll
    CONNECTED --> DISCONNECTED  : T1 expires (retry >= N2)\nlink_failed, fire on_disconnect

    DISCONNECTED --> CONNECTED  : rcv SABM (listening)\nsend UA\nfire on_accept then on_connect
    DISCONNECTED --> DISCONNECTED : rcv SABM (not listening)\nsend DM
    DISCONNECTED --> DISCONNECTED : rcv non-SABM (no connection)\nsend DM (AX.25 §4.3.3)
```

---

## 8. Connection Sequence Diagrams

### Successful connect + data exchange + disconnect

```mermaid
sequenceDiagram
    participant A as Station A (initiator)
    participant B as Station B (listener)

    A->>B: SABM (Set Async Balanced Mode)
    Note over A: state = CONNECTING, start T1
    B-->>A: UA (Unnumbered Ack)
    Note over A: state = CONNECTED, fire on_connect
    Note over B: state = CONNECTED, fire on_connect

    A->>B: I(N(S)=0, N(R)=0) "Hello"
    Note over A: add to unacked, start T1
    B-->>A: RR(N(R)=1)
    Note over A: ack frame 0, slide window

    B->>A: I(N(S)=0, N(R)=1) "World"
    A-->>B: RR(N(R)=1)

    A->>B: DISC
    Note over A: state = DISCONNECTING
    B-->>A: UA
    Note over A,B: state = DISCONNECTED, fire on_disconnect
```

### T1 retransmit and link failure

```mermaid
sequenceDiagram
    participant A
    participant B

    A->>B: I(N(S)=0)
    Note over A: T1 starts
    Note over B: (no response — link broken)
    Note over A: T1 expires, retry=1 → retransmit
    A->>B: I(N(S)=0) [retry 1]
    Note over A: T1 expires, retry=2 → retransmit
    A->>B: I(N(S)=0) [retry 2]
    Note over A: T1 expires, retry=3 ≥ N2=3 → link_failed
    Note over A: state=DISCONNECTED, fire on_disconnect
```

---

## 9. Intrusive Container — Design Notes (ObjNode / ObjList)

`ObjNode<T>` / `ObjList<T>` is an intrusive doubly-linked container inspired by
the Linux kernel's `list_head`.  Unlike `std::list`, which heap-allocates a
wrapper node for each element, the linkage (`next_`/`prev_` pointers) lives
**inside** the object itself — no extra allocation needed.

### Self-managing lifetime

The key improvement over a plain `Node<T>` base class is that **`ObjNode<T>`
owns the insert/remove responsibility** so developers never call `push_back` or
`remove` explicitly:

```cpp
// T must inherit ObjNode<T>.
// The constructor takes the list — insertion is automatic.
struct MySession : ObjNode<MySession> {
    std::string call;
    MySession(ObjList<MySession>& list, std::string c)
        : ObjNode<MySession>(list),   // ← inserts into list immediately
          call(std::move(c)) {}
    // destructor: ObjNode<MySession>::~ObjNode() fires automatically
    //             → removes from list with O(1), no search
};

ObjList<MySession> sessions;
{
    MySession a(sessions, "W1AW");
    MySession b(sessions, "N0CALL");
    assert(sessions.size() == 2);
}   // a and b destroyed → auto-removed
assert(sessions.empty());

// Heap allocation: delete triggers auto-remove too
auto* s = new MySession(sessions, "PY2XXX");
assert(sessions.size() == 1);
delete s;          // ← safe: auto-removed from list before memory is freed
assert(sessions.empty());
```

### API restrictions

* **Default constructor is `= delete`** — every `ObjNode<T>` must bind to an
  `ObjList<T>` at construction time.
* **Copy and move are `= delete`** — nodes are identity-based, not value-based.
* `ObjList<T>::insert_back` and `erase` are **private**, only callable by
  `ObjNode<T>` (friend).  User code never calls them.
* An object can belong to **one** list at a time (same trade-off as all
  intrusive containers).

### Advantages

| Property | Benefit |
|----------|---------|
| Zero extra allocation | No wrapper `list_node` struct on the heap |
| O(1) insert / remove | Pointer surgery only; no search |
| Safety by construction | Can't forget to insert; can't double-free the link |
| RAII-friendly | Scope exit or `delete` → automatic deregistration |

---

## 10. bt_kiss_bridge — Architecture

### Object Relationship Diagram

```
  ┌──────────────────────────────────────────────────────────────────────────────┐
  │  bt_kiss_bridge.cpp + bt_ble_native.h + bt_rfcomm_macos.h                   │
  └──────────────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────────────┐
  │  RadioTransport  «interface»                                                 │
  │  ──────────────────────────────────────────────────────────────────────────  │
  │  + connect() → bool                                                          │
  │  + disconnect()                                                              │
  │  + is_connected() → bool                                                     │
  │  + write(data, len)                                                          │
  │  + read_fd() → int           (≥0 = fd for select(), all transports)         │
  │  + set_on_disconnect(cb)                                                     │
  │  + label() → "BLE" | "BT"                                                   │
  └──────────────────────────────────────────────────────────────────────────────┘
          ▲ implements                          ▲ implements
          │                                     │
  ┌───────────────────────────────┐   ┌────────────────────────────────────────┐
  │  BleTransport                 │   │  BtTransport                            │
  │  ─────────────────────────── │   │  ────────────────────────────────────── │
  │  ble_handle_t (native API)    │   │  Linux: BlueZ RFCOMM socket (fd_)       │
  │  Async TX queue + writer thd  │   │  macOS: IOBluetooth RFCOMM + pipe() fd  │
  │  Notify → pipe fd (RX)        │   │  Direct TX (::write / writeSync)        │
  │  BLE keep-alive timer         │   │  SDP auto-detect SPP channel (0x1101)   │
  │  read_fd() = pipe read fd     │   │  read_fd() = socket fd / pipe read fd   │
  └───────────────────────────────┘   └────────────────────────────────────────┘
                                              │
                                      ┌───────┴──────────────┐
                                      │                      │
                              ┌──────────────┐    ┌─────────────────────────┐
                              │ Linux (BlueZ) │    │ macOS (IOBluetooth)     │
                              │ ──────────── │    │ ─────────────────────── │
                              │ AF_BLUETOOTH  │    │ bt_rfcomm_macos.mm      │
                              │ BTPROTO_RFCOMM│    │ C-linkage API (extern)  │
                              │ socket fd     │    │ IOBluetoothRFCOMMChannel│
                              │ SDP via BlueZ │    │ delegate → pipe() fd    │
                              └──────────────┘    │ performSDPQuery         │
                                                  └─────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────────────┐
  │  Bridge Core  (do_bridge)                                                    │
  │  ──────────────────────────────────────────────────────────────────────────  │
  │  PTY pair (/tmp/kiss symlink) ←──── mutual exclusive ────→ TCP server       │
  │  select() loop: PTY + TCP clients + transport.read_fd()                      │
  │  KISS + AX.25 monitor (--monitor)                                            │
  │  Auto-reconnect (up to 10 retries, 5s pause)                                 │
  └──────────────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────────────┐
  │  Discovery                                                                   │
  │  ──────────────────────────────────────────────────────────────────────────  │
  │  do_ble_scan()  → native BLE scan (BlueZ D-Bus / CoreBluetooth)              │
  │  do_bt_scan()   → HCI inquiry (Linux) / IOBluetoothDeviceInquiry (macOS)    │
  │  do_ble_inspect()→ GATT service + characteristic enumeration                 │
  │  do_bt_inspect() → SDP service browsing + RFCOMM channel extraction          │
  │  do_scan(AUTO)   → scans both BLE + BT on Linux and macOS                   │
  └──────────────────────────────────────────────────────────────────────────────┘
```

### UML Class Diagram

```mermaid
classDiagram
    class RadioTransport {
        <<interface>>
        +connect() bool
        +disconnect()
        +is_connected() bool
        +write(data, len)
        +read_fd() int
        +set_on_receive(cb)
        +set_on_disconnect(cb)
        +label() string
    }

    class BleTransport {
        -ble_handle_t handle_
        -thread writer_thread_
        -mutex tx_mx_
        -queue tx_queue_
        -SteadyClock last_write_
        -int chunk_size_
        +connect() bool
        +write(data, len)
        +read_fd() int  «returns pipe fd»
        +maybe_keepalive()
        +label() string «BLE»
    }

    class BtTransport {
        -string address_
        -int channel_
        -int fd_ «Linux: RFCOMM socket»
        -bt_macos_handle_t handle_ «macOS: opaque»
        -function on_disconnect_
        +connect() bool
        +write(data, len)
        +read_fd() int «returns fd»
        +label() string «BT»
    }

    class BridgeConfig {
        +string address
        +Transport transport «AUTO/BLE/BT»
        +int server_port
        +string server_host
        +string link_path
        +bool monitor
        +int ble_ka_ms
    }

    class BtMacosHandle {
        -IOBluetoothDevice* device
        -IOBluetoothRFCOMMChannel* channel
        -BtRfcommDelegate* delegate
        -int pipe_read
        -int pipe_write
        -atomic~bool~ connected
    }

    class BtRfcommDelegate {
        -BtMacosHandle* handle_
        +rfcommChannelData(ch, data, len)
        +rfcommChannelClosed(ch)
        +rfcommChannelOpenComplete(ch, status)
    }

    RadioTransport <|.. BleTransport : implements
    RadioTransport <|.. BtTransport : implements
    BtTransport "1" --> "0..1" BtMacosHandle : macOS only
    BtMacosHandle "1" --> "1" BtRfcommDelegate : delegate
    BtRfcommDelegate ..> BtMacosHandle : writes to pipe
```

### Data Flow Diagram

```mermaid
flowchart LR
    subgraph Radio
        TNC["TNC / Radio<br/>(BLE or BT SPP)"]
    end

    subgraph bt_kiss_bridge
        subgraph Transport
            BLE["BleTransport<br/>Native BLE (pipe fd)"]
            BT_L["BtTransport (Linux)<br/>RFCOMM socket fd"]
            BT_M["BtTransport (macOS)<br/>IOBluetooth → pipe fd"]
        end
        SEL["select() loop"]
        MON["KISS/AX.25<br/>Monitor"]
    end

    subgraph Clients
        PTY["PTY<br/>/tmp/kiss"]
        TCP["TCP Server<br/>:8001"]
        A1["ax25tnc"]
        A2["bbs"]
    end

    TNC <-->|GATT notify/write| BLE
    TNC <-->|RFCOMM| BT_L
    TNC <-->|RFCOMM| BT_M

    BLE --> SEL
    BT_L --> SEL
    BT_M --> SEL

    SEL <--> PTY
    SEL <--> TCP
    SEL -.-> MON

    PTY --- A1
    TCP --- A2
```

---

## 11. BLE Transport Implementation Notes

### CCCD Subscription (Notify)

BLE peripherals only enter data-forwarding mode after the central device subscribes to
notifications — this is done by writing `0x0001` to the Client Characteristic Configuration
Descriptor (CCCD, UUID `0x2902`) on the notify characteristic.

The subscription is confirmed asynchronously via a delegate callback
(`didUpdateNotificationStateForCharacteristic` on macOS CoreBluetooth,
`StartNotify` D-Bus reply on Linux BlueZ). The transport **must wait** for this
confirmation before sending any KISS data — some radios (including the GA-5WB and
Vero VR-N76/VR-N7600) only enter KISS mode after CCCD is acknowledged.

Implementation: `NotifyWaiter` (`bt_ble_macos.mm`) — a `std::mutex` + `std::condition_variable`
that the CB delegate signals when the notification state update fires. The connect
function waits up to 5 s; if it times out it proceeds with a warning (some devices
fire the callback before we start waiting).

### Chunking vs ATT MTU

ATT MTU is the maximum ATT PDU size (default 23 bytes = 20 bytes payload + 3 header).
After MTU negotiation the effective payload is `ATT_MTU − 3`.

- `auto_chunk` = max payload the OS will accept in one write call (CoreBluetooth:
  `maximumWriteValueLengthForType:` already returns payload; BlueZ: ATT MTU − 3).
- `chunk_sz = 0` (no `--mtu`): write entire KISS frame in one call — let the OS/stack
  handle fragmentation internally.
- `chunk_sz = N` (`--mtu N`): split frame into N-byte writes. Use this only if the
  radio firmware has known issues with large writes.

### Write Without Response (WWR)

BLE write types:
- **Write With Response** (0x12): reliable, ACK'd, max payload = ATT_MTU − 3.
- **Write Without Response** (0x52): fire-and-forget, no ACK, max payload = ATT_MTU − 3.

Most KISS TNC BLE implementations use Write Without Response for throughput.
The transport auto-detects the write type from the characteristic properties.

### Direct Write (no buffering)

The write path calls `ble_write()` (which dispatches `writeValue:` on the CoreBluetooth
queue on macOS, or calls `org.bluez.GattCharacteristic1.WriteValue` on Linux) directly
from the calling thread. No separate writer thread or queue is used. This eliminates
latency from the former 50 ms polling loop and ensures KISS frames hit the radio
in the order and at the timing they arrive from the PTY/TCP client.
