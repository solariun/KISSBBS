// =============================================================================
// ax25lib.cpp — Implementation
// =============================================================================
#include "ax25lib.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <sys/select.h>

// ─── getopt ──────────────────────────────────────────────────────────────────
#include <getopt.h>

namespace ax25 {

// =============================================================================
// Serial
// =============================================================================
bool Serial::open(const std::string& dev, int baud) {
    fd_ = ::open(dev.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) return false;
    struct termios tty{};
    if (tcgetattr(fd_, &tty) < 0) { ::close(fd_); fd_ = -1; return false; }
    orig_ = tty;
    speed_t sp = to_speed(baud);
    cfsetospeed(&tty, sp);
    cfsetispeed(&tty, sp);
    cfmakeraw(&tty);
    tty.c_cflag &= static_cast<tcflag_t>(~(PARENB | CSTOPB | CSIZE));
    tty.c_cflag |= CS8 | CREAD | CLOCAL;
    tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 1;
    if (tcsetattr(fd_, TCSANOW, &tty) < 0) { ::close(fd_); fd_ = -1; return false; }
    return true;
}

void Serial::close() {
    if (fd_ >= 0) { tcsetattr(fd_, TCSANOW, &orig_); ::close(fd_); fd_ = -1; }
}

ssize_t Serial::write(const uint8_t* buf, std::size_t len) {
    return ::write(fd_, buf, len);
}

ssize_t Serial::read(uint8_t* buf, std::size_t len) {
    return ::read(fd_, buf, len);
}

speed_t Serial::to_speed(int b) {
    switch (b) {
        case 1200:  return B1200;  case 2400:  return B2400;
        case 4800:  return B4800;  case 9600:  return B9600;
        case 19200: return B19200; case 38400: return B38400;
        case 57600: return B57600; case 115200:return B115200;
#ifdef B230400
        case 230400:return B230400;
#endif
        default:    return B9600;
    }
}

// =============================================================================
// KISS codec
// =============================================================================
namespace kiss {

std::vector<uint8_t> encode(const std::vector<uint8_t>& payload, Cmd cmd, int port) {
    std::vector<uint8_t> out;
    out.reserve(payload.size() + 4);
    out.push_back(FEND);
    out.push_back(static_cast<uint8_t>(((port & 0x0F) << 4) | (uint8_t(cmd) & 0x0F)));
    for (uint8_t b : payload) {
        if      (b == FEND) { out.push_back(FESC); out.push_back(TFEND); }
        else if (b == FESC) { out.push_back(FESC); out.push_back(TFESC); }
        else                { out.push_back(b); }
    }
    out.push_back(FEND);
    return out;
}

std::vector<Frame> Decoder::feed(const uint8_t* buf, std::size_t len) {
    std::vector<Frame> frames;
    for (std::size_t i = 0; i < len; ++i) {
        uint8_t b = buf[i];
        if (b == FEND) {
            if (!in_frame_) {
                in_frame_ = true; buf_.clear(); escaped_ = false;
            } else if (!buf_.empty()) {
                Frame f;
                f.port    = (buf_[0] >> 4) & 0x0F;
                f.command = Cmd(buf_[0] & 0x0F);
                f.data    = std::vector<uint8_t>(buf_.begin() + 1, buf_.end());
                frames.push_back(std::move(f));
                in_frame_ = false; buf_.clear(); escaped_ = false;
            }
        } else if (in_frame_) {
            if (escaped_) {
                if      (b == TFEND) buf_.push_back(FEND);
                else if (b == TFESC) buf_.push_back(FESC);
                escaped_ = false;
            } else if (b == FESC) {
                escaped_ = true;
            } else {
                buf_.push_back(b);
            }
        }
    }
    return frames;
}

} // namespace kiss

// =============================================================================
// Addr
// =============================================================================
Addr Addr::make(const std::string& callssid) {
    Addr a;
    std::string s = callssid;
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto dash = s.find('-');
    std::string cs = (dash != std::string::npos) ? s.substr(0, dash) : s;
    if (cs.size() > 6) cs.resize(6);
    std::memcpy(a.call, cs.c_str(), cs.size() + 1);  // includes NUL
    if (dash != std::string::npos) {
        try { a.ssid = std::stoi(s.substr(dash + 1)); } catch (...) {}
    }
    return a;
}

Addr Addr::decode(const uint8_t* p) {
    Addr a;
    char cs[7]{};
    for (int i = 0; i < 6; ++i) cs[i] = static_cast<char>((p[i] >> 1) & 0x7F);
    // trim trailing spaces
    int end = 5;
    while (end >= 0 && cs[end] == ' ') --end;
    cs[end + 1] = '\0';
    std::memcpy(a.call, cs, 7);
    a.ssid     = (p[6] >> 1) & 0x0F;
    a.repeated = (p[6] & 0x80) != 0;
    return a;
}

std::vector<uint8_t> Addr::encode(bool last_addr) const {
    std::vector<uint8_t> out(7, static_cast<uint8_t>(' ' << 1));
    for (int i = 0; call[i] != '\0' && i < 6; ++i)
        out[i] = static_cast<uint8_t>((call[i] & 0x7F) << 1);
    uint8_t sb = 0x60;                      // reserved bits per spec
    sb = static_cast<uint8_t>(sb | (ssid & 0x0F) << 1);
    if (repeated)  sb |= 0x80;
    if (last_addr) sb |= 0x01;
    out[6] = sb;
    return out;
}

std::string Addr::str() const {
    std::string s(call);
    if (ssid > 0) { s += '-'; s += std::to_string(ssid); }
    if (repeated)   s += '*';
    return s;
}

bool Addr::operator==(const Addr& o) const {
    return std::strcmp(call, o.call) == 0 && ssid == o.ssid;
}

// =============================================================================
// Frame helpers (local)
// =============================================================================
namespace {
constexpr uint8_t PF_BIT    = 0x10;
constexpr uint8_t SABM_BASE = 0x2F;   // P/F cleared
constexpr uint8_t DISC_BASE = 0x43;
constexpr uint8_t UA_BASE   = 0x63;
constexpr uint8_t DM_BASE   = 0x0F;
constexpr uint8_t UI_BASE   = 0x03;
constexpr uint8_t FRMR_BASE = 0x87;

inline uint8_t mk_i  (int ns,int nr,bool p){ return static_cast<uint8_t>((ns&7)<<1|(p?PF_BIT:0)|(nr&7)<<5); }
inline uint8_t mk_rr (int nr,bool p)       { return static_cast<uint8_t>(0x01|(p?PF_BIT:0)|(nr&7)<<5); }
inline uint8_t mk_rej(int nr,bool p)       { return static_cast<uint8_t>(0x09|(p?PF_BIT:0)|(nr&7)<<5); }
} // anon

// =============================================================================
// Frame
// =============================================================================
Frame::Type Frame::type() const {
    if ((ctrl & 0x01) == 0x00) return Type::IFrame;
    if ((ctrl & 0x03) == 0x01) {
        switch ((ctrl >> 2) & 3) {
            case 0: return Type::RR;
            case 1: return Type::REJ;
            case 2: return Type::RNR;
            default: return Type::SFrame;
        }
    }
    // U-frame: ignore P/F bit for matching
    switch (static_cast<uint8_t>(ctrl & ~PF_BIT)) {
        case UI_BASE:   return Type::UI;
        case SABM_BASE: return Type::SABM;
        case DISC_BASE: return Type::DISC;
        case UA_BASE:   return Type::UA;
        case DM_BASE:   return Type::DM;
        case FRMR_BASE: return Type::FRMR;
        default:        return Type::Unknown;
    }
}

bool Frame::decode(const std::vector<uint8_t>& raw, Frame& f) {
    if (raw.size() < 14) return false;
    std::size_t pos = 0;

    f.dest = Addr::decode(&raw[pos]); pos += 7;
    f.src  = Addr::decode(&raw[pos]);
    bool last = (raw[pos + 6] & 0x01) != 0;
    pos += 7;

    while (!last && pos + 7 <= raw.size()) {
        Addr d = Addr::decode(&raw[pos]);
        last = (raw[pos + 6] & 0x01) != 0;
        f.digis.push_back(d);
        pos += 7;
    }

    if (pos >= raw.size()) return false;
    f.ctrl = raw[pos++];

    Type t = f.type();
    if (t == Type::IFrame || t == Type::UI) {
        if (pos >= raw.size()) return false;
        f.pid = raw[pos++]; f.has_pid = true;
    } else {
        f.has_pid = false;
    }

    f.info = std::vector<uint8_t>(raw.begin() + pos, raw.end());
    return true;
}

std::vector<uint8_t> Frame::encode() const {
    std::vector<uint8_t> out;
    auto app = [&](const std::vector<uint8_t>& v){ out.insert(out.end(),v.begin(),v.end()); };
    app(dest.encode(false));
    app(src.encode(digis.empty()));
    for (std::size_t i = 0; i < digis.size(); ++i)
        app(digis[i].encode(i == digis.size() - 1));
    out.push_back(ctrl);
    if (has_pid) out.push_back(pid);
    out.insert(out.end(), info.begin(), info.end());
    return out;
}

std::string Frame::format() const {
    std::ostringstream os;
    os << src.str() << '>' << dest.str();
    for (const auto& d : digis) os << ',' << d.str();
    const char* ts = "?";
    switch (type()) {
        case Type::IFrame: ts="I";    break; case Type::SFrame: ts="S";    break;
        case Type::UI:     ts="UI";   break; case Type::SABM:   ts="SABM"; break;
        case Type::DISC:   ts="DISC"; break; case Type::UA:     ts="UA";   break;
        case Type::DM:     ts="DM";   break; case Type::FRMR:   ts="FRMR"; break;
        case Type::RR:     ts="RR";   break; case Type::RNR:    ts="RNR";  break;
        case Type::REJ:    ts="REJ";  break; default: break;
    }
    os << " [" << ts << "]";
    if (type() == Type::IFrame) os << " Ns=" << get_ns() << " Nr=" << get_nr();
    if (type() == Type::RR || type() == Type::RNR || type() == Type::REJ)
        os << " Nr=" << get_nr();
    if (has_pid) os << " PID=" << std::hex << std::uppercase
                    << "0x" << std::setw(2) << std::setfill('0') << (int)pid << std::dec;
    if (!info.empty()) {
        bool pr = true;
        for (uint8_t b : info) if (b<0x20 && b!='\r' && b!='\n' && b!='\t'){pr=false;break;}
        os << " | ";
        if (pr) {
            std::string txt(info.begin(), info.end());
            while (!txt.empty() && (txt.back()=='\r'||txt.back()=='\n')) txt.pop_back();
            os << txt;
        } else {
            os << "[HEX:";
            for (uint8_t b : info)
                os <<' '<< std::hex<<std::uppercase<<std::setw(2)<<std::setfill('0')<<(int)b;
            os << std::dec << ']';
        }
    }
    return os.str();
}

// =============================================================================
// Kiss layer
// =============================================================================
bool Kiss::open(const std::string& device, int baud) {
    return serial_.open(device, baud);
}

bool Kiss::open_fd(int fd) {
    ext_fd_ = fd;
    // Ensure non-blocking I/O (same contract as Serial::open)
    int fl = ::fcntl(ext_fd_, F_GETFL, 0);
    if (fl >= 0) ::fcntl(ext_fd_, F_SETFL, fl | O_NONBLOCK);
    return true;
}

void Kiss::close() {
    serial_.close();
    if (ext_fd_ >= 0) { ::close(ext_fd_); ext_fd_ = -1; }
}

bool Kiss::raw_write(const uint8_t* data, std::size_t len) {
    if (ext_fd_ >= 0) {
        // Simple loop to handle partial writes (TCP etc.)
        std::size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::write(ext_fd_, data + sent, len - sent);
            if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK
                              ? sent == len : false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }
    return serial_.write(data, len) == (ssize_t)len;
}

bool Kiss::send_frame(const std::vector<uint8_t>& ax25) {
    if (on_send_hook) return on_send_hook(ax25);   // test / simulation path
    auto f = kiss::encode(ax25);
    return raw_write(f.data(), f.size());
}

void Kiss::set_txdelay(int ms) {
    std::vector<uint8_t> d{static_cast<uint8_t>(ms / 10)};
    auto f = kiss::encode(d, kiss::Cmd::TxDelay);
    raw_write(f.data(), f.size());
}
void Kiss::set_persistence(int val) {
    std::vector<uint8_t> d{static_cast<uint8_t>(val)};
    auto f = kiss::encode(d, kiss::Cmd::Persistence);
    raw_write(f.data(), f.size());
}
void Kiss::set_slottime(int ms) {
    std::vector<uint8_t> d{static_cast<uint8_t>(ms / 10)};
    auto f = kiss::encode(d, kiss::Cmd::SlotTime);
    raw_write(f.data(), f.size());
}

void Kiss::poll() {
    if (!is_open()) return;
    uint8_t buf[512];
    ssize_t n;
    if (ext_fd_ >= 0)
        n = ::read(ext_fd_, buf, sizeof(buf));
    else
        n = serial_.read(buf, sizeof(buf));
    if (n <= 0) return;
    auto frames = decoder_.feed(buf, static_cast<std::size_t>(n));
    for (auto& kf : frames)
        if (kf.command == kiss::Cmd::Data && on_frame_)
            on_frame_(kf.data);
}

// =============================================================================
// Connection — ctor / dtor
// =============================================================================
Connection::Connection(Router* router, ObjList<Connection>& lst,
                       const Addr& local, const Addr& remote,
                       const Config& cfg, bool outgoing)
    : ObjNode<Connection>(lst),         // ← auto-insert via ObjNode ctor
      router_(router),
      local_(local), remote_(remote),
      cfg_(cfg), outgoing_(outgoing)
{
    (void)outgoing_;   // suppress unused-parameter warning
}

Connection::~Connection() {
    if (state_ == State::CONNECTED || state_ == State::CONNECTING)
        tx_disc();           // best-effort goodbye
    // ObjNode<Connection> destructor fires next → auto-removes from list
}

// =============================================================================
// Connection — frame senders
// =============================================================================
void Connection::tx_sabm() {
    Frame f;
    f.dest = remote_; f.src = local_; f.digis = cfg_.digis;
    f.ctrl = static_cast<uint8_t>(SABM_BASE | PF_BIT);
    f.has_pid = false;
    router_->send_frame(f);
}
void Connection::tx_disc() {
    Frame f;
    f.dest = remote_; f.src = local_; f.digis = cfg_.digis;
    f.ctrl = static_cast<uint8_t>(DISC_BASE | PF_BIT);
    f.has_pid = false;
    router_->send_frame(f);
}
void Connection::tx_ua(bool pf) {
    Frame f;
    f.dest = remote_; f.src = local_;
    f.ctrl = static_cast<uint8_t>(UA_BASE | (pf ? PF_BIT : 0));
    f.has_pid = false;
    router_->send_frame(f);
}
void Connection::tx_dm(bool pf) {
    Frame f;
    f.dest = remote_; f.src = local_;
    f.ctrl = static_cast<uint8_t>(DM_BASE | (pf ? PF_BIT : 0));
    f.has_pid = false;
    router_->send_frame(f);
}
void Connection::tx_rr(bool pf) {
    Frame f;
    f.dest = remote_; f.src = local_;
    f.ctrl = mk_rr(vr_, pf);
    f.has_pid = false;
    router_->send_frame(f);
}
void Connection::tx_iframe(int ns, int nr, bool pf,
                            const uint8_t* d, std::size_t len)
{
    Frame f;
    f.dest    = remote_; f.src = local_; f.digis = cfg_.digis;
    f.ctrl    = mk_i(ns, nr, pf);
    f.pid     = 0xF0;
    f.has_pid = true;
    f.info    = std::vector<uint8_t>(d, d + len);
    router_->send_frame(f);
}

// =============================================================================
// Connection — window management
// =============================================================================
void Connection::flush_window(Millis now) {
    while (!send_buf_.empty() && (int)unacked_.size() < cfg_.window) {
        // Move the chunk out of send_buf_ and into unacked_ BEFORE calling
        // tx_iframe.  This prevents a re-entrancy hazard: if tx_iframe
        // triggers a synchronous callback chain (RR → process_nr →
        // flush_window), the queues are already in a consistent state.
        auto chunk = std::move(send_buf_.front());
        send_buf_.pop_front();
        int ns = vs_;
        vs_ = (vs_ + 1) & 7;
        unacked_.push_back({ns, chunk});           // keep a copy in unacked_
        tx_iframe(ns, vr_, false, chunk.data(), chunk.size());
    }
    if (!unacked_.empty() && !t1_run_) start_t1(now);
}

void Connection::process_nr(int nr, Millis now) {
    // Pop all frames acknowledged by peer (ns != nr means it's been acked)
    while (!unacked_.empty() && unacked_.front().ns != nr)
        unacked_.pop_front();
    va_ = nr;
    retry_ = 0;
    if (unacked_.empty()) stop_t1();
    else                  start_t1(now);
    flush_window(now);
    reset_t3(now);
}

void Connection::retransmit_all(Millis now) {
    // Go-Back-N: replay all unacked frames with current vr_
    vs_ = va_;
    std::deque<UnackedFrame> tmp;
    std::swap(tmp, unacked_);
    for (auto& uf : tmp) {
        tx_iframe(vs_, vr_, false, uf.data.data(), uf.data.size());
        unacked_.push_back({vs_, std::move(uf.data)});
        vs_ = (vs_ + 1) & 7;
    }
    start_t1(now);
}

void Connection::link_failed() {
    stop_t1(); stop_t3();
    state_ = State::DISCONNECTED;
    if (on_disconnect) on_disconnect();
}

// =============================================================================
// Connection — start outgoing
// =============================================================================
void Connection::start_connect(Millis now) {
    vs_ = vr_ = va_ = retry_ = 0;
    state_ = State::CONNECTING;
    tx_sabm();
    start_t1(now);
}

// =============================================================================
// Connection — public API
// =============================================================================
bool Connection::send(const uint8_t* data, std::size_t len) {
    if (state_ != State::CONNECTED) return false;
    Millis now = now_ms();
    for (std::size_t off = 0; off < len; off += (std::size_t)cfg_.mtu) {
        std::size_t chunk = std::min((std::size_t)cfg_.mtu, len - off);
        send_buf_.push_back(std::vector<uint8_t>(data + off, data + off + chunk));
    }
    flush_window(now);
    return true;
}

void Connection::disconnect() {
    if (state_ == State::DISCONNECTED || state_ == State::DISCONNECTING) return;
    stop_t3();
    state_ = State::DISCONNECTING;
    retry_ = 0;
    send_buf_.clear();
    tx_disc();
    start_t1(now_ms());
}

// =============================================================================
// Connection — state machine  (handle_frame)
// =============================================================================
void Connection::handle_frame(const Frame& f, Millis now) {
    auto t = f.type();

    switch (state_) {
    // ────────────────────────────────────────────────────────────────────────
    case State::DISCONNECTED:
        if (t == Frame::Type::SABM) {
            vs_ = vr_ = va_ = retry_ = 0;
            tx_ua(f.get_pf());
            state_ = State::CONNECTED;
            start_t3(now);
            if (on_connect) on_connect();
        } else {
            tx_dm(f.get_pf());
        }
        break;

    // ────────────────────────────────────────────────────────────────────────
    case State::CONNECTING:
        if (t == Frame::Type::UA) {
            stop_t1();
            vs_ = vr_ = va_ = retry_ = 0;
            state_ = State::CONNECTED;
            start_t3(now);
            if (on_connect) on_connect();
            flush_window(now);
        } else if (t == Frame::Type::DM) {
            stop_t1();
            state_ = State::DISCONNECTED;
            if (on_disconnect) on_disconnect();
        } else if (t == Frame::Type::SABM) {
            // Simultaneous connect
            tx_ua(f.get_pf());
        }
        break;

    // ────────────────────────────────────────────────────────────────────────
    case State::CONNECTED:
        // Any received frame resets the keep-alive timer
        reset_t3(now);
        poll_pending_ = false;

        if (t == Frame::Type::DISC) {
            stop_t1(); stop_t3();
            send_buf_.clear(); unacked_.clear();
            tx_ua(f.get_pf());
            state_ = State::DISCONNECTED;
            if (on_disconnect) on_disconnect();
            return;
        }
        if (t == Frame::Type::SABM) {
            // Re-connect from peer: reset and ack
            vs_ = vr_ = va_ = 0;
            unacked_.clear(); send_buf_.clear();
            tx_ua(f.get_pf());
            return;
        }
        if (t == Frame::Type::DM) { link_failed(); return; }

        if (t == Frame::Type::IFrame) {
            process_nr(f.get_nr(), now);
            if (f.get_ns() == vr_) {
                vr_ = (vr_ + 1) & 7;
                if (on_data && !f.info.empty())
                    on_data(f.info.data(), f.info.size());
                tx_rr(f.get_pf());   // ack; mirrors Poll bit as Final
            } else {
                // Out-of-sequence: REJ
                Frame rej;
                rej.dest = remote_; rej.src = local_;
                rej.ctrl = mk_rej(vr_, false);
                rej.has_pid = false;
                router_->send_frame(rej);
            }
        } else if (t == Frame::Type::RR) {
            process_nr(f.get_nr(), now);
            if (f.get_pf()) tx_rr(true);  // respond to poll with F=1
        } else if (t == Frame::Type::RNR) {
            process_nr(f.get_nr(), now);
            // Peer busy; window management already handled
        } else if (t == Frame::Type::REJ) {
            process_nr(f.get_nr(), now);
            retransmit_all(now);
        }
        break;

    // ────────────────────────────────────────────────────────────────────────
    case State::DISCONNECTING:
        if (t == Frame::Type::UA || t == Frame::Type::DM) {
            stop_t1();
            state_ = State::DISCONNECTED;
            if (on_disconnect) on_disconnect();
        }
        break;
    }
}

// =============================================================================
// Connection — timer tick
// =============================================================================
void Connection::tick(Millis now) {
    // T1 — retransmit / retry
    if (t1_run_ && now >= t1_exp_) {
        t1_run_ = false;
        if (++retry_ > cfg_.n2) { link_failed(); return; }

        switch (state_) {
        case State::CONNECTING:
            tx_sabm(); start_t1(now); break;
        case State::CONNECTED:
            if (poll_pending_) { link_failed(); return; }
            retransmit_all(now);
            break;
        case State::DISCONNECTING:
            tx_disc(); start_t1(now); break;
        default: break;
        }
    }

    // T3 — keep-alive / inactivity
    if (t3_run_ && now >= t3_exp_ && state_ == State::CONNECTED) {
        t3_run_       = false;
        poll_pending_ = true;
        tx_rr(true);       // RR with P=1
        start_t1(now);
    }
}

// =============================================================================
// Router
// =============================================================================
Router::Router(Kiss& kiss, Config cfg)
    : kiss_(kiss), cfg_(std::move(cfg))
{
    // Hook into Kiss: all received AX.25 payloads come to us
    kiss_.set_on_frame([this](std::vector<uint8_t> raw){
        route(std::move(raw), now_ms());
    });
}

Connection* Router::connect(const Addr& remote) {
    auto* c = new Connection(this, conns_, cfg_.mycall, remote, cfg_, /*outgoing=*/true);
    c->start_connect(now_ms());
    return c;
}

void Router::listen(std::function<void(Connection*)> on_accept) {
    on_accept_ = std::move(on_accept);
}

void Router::send_ui(const Addr& dest, uint8_t pid,
                     const void* data, std::size_t len,
                     const std::vector<Addr>& digis)
{
    Frame f;
    f.dest    = dest;
    f.src     = cfg_.mycall;
    f.digis   = digis.empty() ? cfg_.digis : digis;
    f.ctrl    = UI_BASE;
    f.pid     = pid;
    f.has_pid = true;
    f.info    = std::vector<uint8_t>((const uint8_t*)data, (const uint8_t*)data + len);
    tx(f);
}

void Router::send_aprs(const std::string& info, const Addr& dest,
                       const std::vector<Addr>& digis)
{
    send_ui(dest, 0xF0, info.data(), info.size(), digis);
}

void Router::poll() {
    kiss_.poll();
    Millis now = now_ms();
    for (auto* c : conns_.snapshot())
        c->tick(now);
}

Connection* Router::find(const Addr& local, const Addr& remote) {
    for (auto& c : conns_)
        if (c.local() == local && c.remote() == remote) return &c;
    return nullptr;
}

bool Router::tx(const Frame& f) {
    return kiss_.send_frame(f.encode());
}

void Router::route(std::vector<uint8_t> raw, Millis now) {
    Frame f;
    if (!Frame::decode(raw, f)) return;

    // Monitor callback: fires for every decoded frame
    if (on_monitor) on_monitor(f);

    // UI frames: fire on_ui regardless of destination (APRS is broadcast)
    if (f.type() == Frame::Type::UI) {
        if (on_ui) on_ui(f);
        return;   // UI is connectionless; nothing else to do
    }

    // Connected-mode frames: only process those addressed to us
    if (!(f.dest == cfg_.mycall)) return;

    // Try to find existing session: dest=us, src=remote
    Connection* conn = find(f.dest, f.src);
    if (conn) {
        conn->handle_frame(f, now);
        return;
    }

    // No matching session exists for this addressed-to-us frame.
    // Per AX.25 spec §4.3.3, if the frame is NOT a SABM we must respond
    // with DM(F=P) so the remote knows we have no connection.
    // (Silently dropping causes the remote to loop in polling/retransmit limbo.)
    if (f.type() != Frame::Type::SABM) {
        Frame dm;
        dm.dest    = f.src;
        dm.src     = cfg_.mycall;
        dm.ctrl    = static_cast<uint8_t>(DM_BASE | (f.get_pf() ? PF_BIT : 0));
        dm.has_pid = false;
        tx(dm);
        return;
    }

    // No session — handle new SABM (incoming connection)
    if (f.type() == Frame::Type::SABM) {
        if (on_accept_) {
            // Create Connection in DISCONNECTED state; give caller a chance to
            // attach callbacks (on_connect, on_data, etc.) BEFORE we send UA.
            auto* c = new Connection(this, conns_, cfg_.mycall, f.src, cfg_, /*outgoing=*/false);
            on_accept_(c);             // caller sets callbacks here
            c->handle_frame(f, now);   // → sends UA, fires on_connect
        } else {
            // Reject: send DM
            Frame dm;
            dm.dest = f.src; dm.src = cfg_.mycall;
            dm.ctrl = static_cast<uint8_t>(DM_BASE | (f.get_pf() ? PF_BIT : 0));
            dm.has_pid = false;
            tx(dm);
        }
    }
}

// =============================================================================
// CLIParams
// =============================================================================
bool CLIParams::parse(int argc, char* argv[], const char* extra_usage) {
    optind = 1;
    int opt;
    while ((opt = getopt(argc, argv, "c:b:p:m:w:t:k:T:s:h")) != -1) {
        switch (opt) {
        case 'c': cfg.mycall = Addr::make(optarg); break;
        case 'b': baud       = std::atoi(optarg);  break;
        case 'p': {
            cfg.digis.clear();
            std::istringstream ss(optarg);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) cfg.digis.push_back(Addr::make(tok));
            break;
        }
        case 'm': cfg.mtu    = std::atoi(optarg); break;
        case 'w': cfg.window = std::max(1, std::min(7, std::atoi(optarg))); break;
        case 't': cfg.t1_ms  = std::atoi(optarg); break;
        case 'k': cfg.t3_ms  = std::atoi(optarg); break;
        case 'T': cfg.txdelay= std::atoi(optarg); break;
        case 's': cfg.persist= std::atoi(optarg); break;
        case 'h': print_help(argv[0], extra_usage); return false;
        default:  print_help(argv[0], extra_usage); return false;
        }
    }
    if (optind < argc) device = argv[optind];

    if (device.empty()) {
        std::cerr << "Error: serial device required.\n";
        print_help(argv[0], extra_usage);
        return false;
    }
    if (cfg.mycall.empty()) {
        std::cerr << "Error: callsign required  (-c CALL[-SSID])\n";
        return false;
    }
    return true;
}

void CLIParams::print_help(const char* prog, const char* extra) {
    std::cerr
        << "Usage: " << prog << " [OPTIONS] <serial_device>\n\n"
        << "AX.25 / KISS parameters:\n"
        << "  -c <CALL[-N]>   My callsign (required)\n"
        << "  -b <baud>       Baud rate (default: 9600)\n"
        << "  -p <path>       Digipeater path, comma-separated\n"
        << "  -m <bytes>      I-frame MTU (default: 128)\n"
        << "  -w <1-7>        Window size (default: 3)\n"
        << "  -t <ms>         T1 retransmit timer ms (default: 3000)\n"
        << "  -k <ms>         T3 keep-alive timer ms (default: 60000)\n"
        << "  -T <units>      KISS TX delay ×10 ms (default: 30)\n"
        << "  -s <0-255>      KISS persistence (default: 63)\n";
    if (extra && *extra) std::cerr << extra;
}

// =============================================================================
// APRS helpers
// =============================================================================
namespace aprs {

std::string make_pos(double lat, double lon, char sym, const std::string& comment) {
    char latch = lat >= 0 ? 'N' : 'S';
    char lonch = lon >= 0 ? 'E' : 'W';
    double alat = lat >= 0 ? lat : -lat;
    double alon = lon >= 0 ? lon : -lon;
    int latd = (int)alat;  double latm = (alat - latd) * 60.0;
    int lond = (int)alon;  double lonm = (alon - lond) * 60.0;
    char buf[96];
    snprintf(buf, sizeof(buf), "!%02d%05.2f%c/%03d%05.2f%c%c%s",
             latd, latm, latch, lond, lonm, lonch, sym, comment.c_str());
    return buf;
}

static int g_aprs_seq = 0;
std::string make_msg(const std::string& dest, const std::string& text) {
    char addr[10];
    snprintf(addr, sizeof(addr), "%-9s", dest.c_str());
    ++g_aprs_seq;
    char buf[256];
    snprintf(buf, sizeof(buf), ":%s:%s{%03d}", addr, text.c_str(), g_aprs_seq);
    return buf;
}

bool parse_msg(const std::string& info, Msg& m) {
    if (info.size() < 11 || info[0] != ':') return false;
    std::string::size_type sep = info.find(':', 1);
    if (sep == std::string::npos || sep > 10) return false;
    // trim the addressee
    std::string to = info.substr(1, sep - 1);
    std::string::size_type b = to.find_first_not_of(" \t");
    std::string::size_type e = to.find_last_not_of(" \t");
    m.to = (b == std::string::npos) ? "" : to.substr(b, e - b + 1);
    m.text = info.substr(sep + 1);
    // strip {seq}
    std::string::size_type brace = m.text.rfind('{');
    if (brace != std::string::npos) {
        m.seq = m.text.substr(brace + 1);
        std::string::size_type cb = m.seq.find('}');
        if (cb != std::string::npos) m.seq.resize(cb);
        m.text.resize(brace);
    }
    // trim text
    b = m.text.find_first_not_of(" \t");
    e = m.text.find_last_not_of(" \t");
    m.text = (b == std::string::npos) ? "" : m.text.substr(b, e - b + 1);
    return true;
}

bool is_pos(const std::string& info) {
    if (info.empty()) return false;
    char c = info[0];
    return c == '!' || c == '=' || c == '@' || c == '/' || c == '`';
}

std::string info_str(const Frame& f) {
    std::string s(f.info.begin(), f.info.end());
    for (auto& c : s)
        if ((unsigned char)c < 0x20 && c != '\r' && c != '\n') c = '.';
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

} // namespace aprs

} // namespace ax25
