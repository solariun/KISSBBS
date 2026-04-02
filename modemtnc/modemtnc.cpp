// modemtnc — Software TNC with Soundcard DSP
// Demodulates/modulates AX.25 via soundcard, exposes KISS interface (PTY + TCP)
// DSP derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)
// https://github.com/wb2osz/direwolf
//
// Usage:
//   modemtnc -c W1AW -s 1200 -d default --link /tmp/kiss --monitor
//   modemtnc -s 9600 --server-port 8001 --monitor
//   modemtnc --loopback --monitor   (self-test: modulate → demodulate)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#include "audio.h"
#include "modem.h"
#include "hdlc.h"
#include "ptt.h"
#include "ax25lib.hpp"
#include "ax25dump.hpp"

// ---------------------------------------------------------------------------
//  Globals & config
// ---------------------------------------------------------------------------
static std::atomic<bool> g_running{true};

struct Config {
    std::string callsign;
    std::string audio_device;
    int         sample_rate   = 44100;
    int         baud          = 1200;
    modem::Type modem_type    = modem::AFSK_1200;
    std::string link_path     = "/tmp/kiss";
    int         server_port   = 0;
    bool        monitor       = false;
    bool        loopback      = false;
    int         txdelay_ms    = 300;
    int         txtail_ms     = 100;
    int         persist       = 63;
    int         slottime_ms   = 100;
    int         volume        = 50;
    bool        list_devices  = false;

    // PTT control
    ptt::Config ptt;
};

static void signal_handler(int) { g_running = false; }

// ---------------------------------------------------------------------------
//  PTY helpers (same pattern as bt_kiss_bridge)
// ---------------------------------------------------------------------------
static int open_pty(int* slave_fd, std::string& slave_name) {
    int master;
    char name[256];
    if (openpty(&master, slave_fd, name, nullptr, nullptr) < 0) {
        perror("openpty");
        return -1;
    }
    slave_name = name;
    // Set master non-blocking
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    return master;
}

static void create_symlink(const std::string& target, const std::string& link) {
    unlink(link.c_str());
    if (symlink(target.c_str(), link.c_str()) < 0)
        perror("symlink");
}

// ---------------------------------------------------------------------------
//  TCP server helpers
// ---------------------------------------------------------------------------
static int create_tcp_server(int port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) { fd = socket(AF_INET, SOCK_STREAM, 0); }
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    listen(fd, 4);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

// ---------------------------------------------------------------------------
//  Monitor display
// ---------------------------------------------------------------------------
static void show_frame(const uint8_t* data, size_t len, const char* direction) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    struct tm tm;
    localtime_r(&t, &tm);
    char ts[32];
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms.count());

    std::vector<uint8_t> raw(data, data + len);
    ax25::Frame frame;
    if (ax25::Frame::decode(raw, frame)) {
        printf("[%s]  %s  %s\n", ts, direction, frame.format().c_str());
    } else {
        printf("[%s]  %s  [%zu bytes, decode failed]\n", ts, direction, len);
    }

    // Hex dump
    printf("%s", hex_dump(data, len, "           ").c_str());
    fflush(stdout);
}

// ---------------------------------------------------------------------------
//  Modem type from baud rate
// ---------------------------------------------------------------------------
static modem::Type baud_to_type(int baud) {
    switch (baud) {
        case 300:  return modem::AFSK_300;
        case 1200: return modem::AFSK_1200;
        case 9600: return modem::GMSK_9600;
        default:   return modem::AFSK_1200;
    }
}

// ---------------------------------------------------------------------------
//  Usage
// ---------------------------------------------------------------------------
static void usage() {
    printf(
        "modemtnc — Software TNC with Soundcard DSP\n"
        "DSP derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)\n"
        "\n"
        "Usage: modemtnc [options]\n"
        "\n"
        "Audio:\n"
        "  -d DEVICE         Audio device (ALSA: \"default\", \"plughw:1,0\"; macOS: auto)\n"
        "  -r RATE           Sample rate in Hz (default: 44100, auto 96000 for 9600)\n"
        "  --list-devices    List all audio devices and exit\n"
        "\n"
        "Modem:\n"
        "  -s SPEED          Baud rate: 300, 1200, 9600 (default: 1200)\n"
        "  --volume N        TX amplitude 0-100 (default: 50)\n"
        "\n"
        "KISS interface:\n"
        "  --link PATH       PTY symlink path (default: /tmp/kiss)\n"
        "  --server-port N   TCP KISS server port (disabled by default)\n"
        "\n"
        "PTT control:\n"
        "  --ptt METHOD      PTT method (default: vox):\n"
        "                      vox       audio-triggered (no hardware control)\n"
        "                      rts, +rts assert RTS on serial port\n"
        "                      -rts      assert RTS inverted (active low)\n"
        "                      dtr, +dtr assert DTR on serial port\n"
        "                      -dtr      assert DTR inverted (active low)\n"
        "                      cm108     CM108/CM119 USB GPIO (Digirig)\n"
        "                      gpio      Linux sysfs GPIO\n"
        "  --ptt-device DEV  Serial port or HID device for PTT\n"
        "                      rts/dtr: /dev/ttyUSB0, /dev/cu.usbserial-*\n"
        "                      cm108:   /dev/hidraw0 (auto-detected if omitted)\n"
        "  --ptt-gpio N      GPIO pin number (cm108: 1-8, default 3; gpio: sysfs num)\n"
        "  --ptt-invert      Invert PTT signal (active low)\n"
        "\n"
        "TX timing:\n"
        "  --txdelay N       Preamble delay in ms (default: 300)\n"
        "  --txtail N        TX tail in ms (default: 100)\n"
        "  --persist N       CSMA persistence 0-255 (default: 63)\n"
        "  --slottime N      CSMA slot time in ms (default: 100)\n"
        "\n"
        "Display:\n"
        "  -c CALL           Callsign (shown in monitor output)\n"
        "  --monitor         Print decoded frames to stdout\n"
        "\n"
        "Testing:\n"
        "  --loopback        Self-test: TX -> RX in memory (no audio device)\n"
        "  -h, --help        Show this help\n"
        "\n"
        "Examples:\n"
        "  modemtnc --list-devices                           # find your audio device\n"
        "  modemtnc --loopback --monitor                     # self-test\n"
        "  modemtnc -d plughw:1,0 -s 1200 --link /tmp/kiss --monitor\n"
        "  modemtnc --ptt cm108 -s 1200 --link /tmp/kiss --monitor   # Digirig\n"
        "  modemtnc --ptt rts --ptt-device /dev/ttyUSB0 -s 1200 --monitor\n"
        "  modemtnc -s 9600 --link /tmp/kiss --server-port 8001 --monitor\n"
        "\n"
        "Connect a KISS client:\n"
        "  ax25tnc -c W1AW -r W1BBS /tmp/kiss\n"
        "  ax25send -c W1AW /tmp/kiss --pos 42.36,-71.06 \"Mobile\"\n"
    );
}

// ---------------------------------------------------------------------------
//  Parse CLI
// ---------------------------------------------------------------------------
static Config parse_args(int argc, char* argv[]) {
    Config cfg;
    static struct option long_opts[] = {
        {"link",        required_argument, nullptr, 'L'},
        {"server-port", required_argument, nullptr, 'P'},
        {"monitor",     no_argument,       nullptr, 'M'},
        {"loopback",    no_argument,       nullptr, 'B'},
        {"list-devices",no_argument,       nullptr, 'D'},
        {"txdelay",     required_argument, nullptr, 1},
        {"txtail",      required_argument, nullptr, 2},
        {"persist",     required_argument, nullptr, 3},
        {"slottime",    required_argument, nullptr, 4},
        {"volume",      required_argument, nullptr, 5},
        {"ptt",         required_argument, nullptr, 6},
        {"ptt-device",  required_argument, nullptr, 7},
        {"ptt-gpio",    required_argument, nullptr, 8},
        {"ptt-invert",  no_argument,       nullptr, 9},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:s:d:r:h", long_opts, nullptr)) != -1) {
        switch (c) {
            case 'c': cfg.callsign = optarg; break;
            case 's': cfg.baud = atoi(optarg); break;
            case 'd': cfg.audio_device = optarg; break;
            case 'r': cfg.sample_rate = atoi(optarg); break;
            case 'L': cfg.link_path = optarg; break;
            case 'P': cfg.server_port = atoi(optarg); break;
            case 'M': cfg.monitor = true; break;
            case 'B': cfg.loopback = true; break;
            case 'D': cfg.list_devices = true; break;
            case 1:   cfg.txdelay_ms = atoi(optarg); break;
            case 2:   cfg.txtail_ms = atoi(optarg); break;
            case 3:   cfg.persist = atoi(optarg); break;
            case 4:   cfg.slottime_ms = atoi(optarg); break;
            case 5:   cfg.volume = atoi(optarg); break;
            case 6: { // --ptt METHOD
                std::string m = optarg;
                if (m == "vox")        cfg.ptt.method = ptt::VOX;
                else if (m == "rts"  || m == "+rts")  { cfg.ptt.method = ptt::SERIAL_RTS; cfg.ptt.invert = false; }
                else if (m == "-rts")                  { cfg.ptt.method = ptt::SERIAL_RTS; cfg.ptt.invert = true; }
                else if (m == "dtr"  || m == "+dtr")  { cfg.ptt.method = ptt::SERIAL_DTR; cfg.ptt.invert = false; }
                else if (m == "-dtr")                  { cfg.ptt.method = ptt::SERIAL_DTR; cfg.ptt.invert = true; }
                else if (m == "cm108") cfg.ptt.method = ptt::CM108;
                else if (m == "gpio") cfg.ptt.method = ptt::GPIO;
                else if (m == "hamlib" || m == "cat") cfg.ptt.method = ptt::HAMLIB;
                else { fprintf(stderr, "Unknown PTT method: %s\n", optarg); exit(1); }
                break;
            }
            case 7:   cfg.ptt.device = optarg; break;
            case 8:   cfg.ptt.gpio_pin = atoi(optarg); break;
            case 9:   cfg.ptt.invert = true; break;
            case 'h': usage(); exit(0);
            default:  usage(); exit(1);
        }
    }

    cfg.modem_type = baud_to_type(cfg.baud);

    // Auto-select sample rate for 9600 baud if user didn't specify
    if (cfg.baud >= 9600 && cfg.sample_rate == 44100)
        cfg.sample_rate = 96000;

    return cfg;
}

// ---------------------------------------------------------------------------
//  KISS parameter handling
// ---------------------------------------------------------------------------
struct KissParams {
    int txdelay_10ms = 30;   // in 10ms units
    int persist = 63;
    int slottime_10ms = 10;
    int txtail_10ms = 10;
    bool fullduplex = false;
};

// ---------------------------------------------------------------------------
//  Loopback self-test
// ---------------------------------------------------------------------------
static void run_loopback(const Config& cfg) {
    printf("=== modemtnc loopback self-test ===\n");
    printf("Modem: %d baud, sample rate: %d\n\n", cfg.baud, cfg.sample_rate);

    modem::Modulator mod;
    modem::Demodulator demod;
    hdlc::Encoder enc;
    hdlc::Decoder dec;

    int amp = 16000 * cfg.volume / 100;
    mod.init(cfg.modem_type, cfg.sample_rate, amp);
    demod.init(cfg.modem_type, cfg.sample_rate);
    dec.init();

    int frames_decoded = 0;
    dec.set_on_frame([&](const uint8_t* data, size_t len) {
        frames_decoded++;
        printf("  [RX] Frame %d: %zu bytes\n", frames_decoded, len);
        if (cfg.monitor) show_frame(data, len, "<- LOOPBACK");
    });

    // Wire: modulator → demodulator
    demod.set_on_bit([&dec](int bit) { dec.receive_bit(bit); });

    std::vector<int16_t> audio_buf;
    mod.set_on_sample([&](int16_t s) {
        audio_buf.push_back(s);
        demod.process_sample(s);
    });

    // HDLC encoder → modulator
    enc.set_on_bit([&mod](int bit) { mod.put_bit(bit); });

    // Build a test AX.25 UI frame
    ax25::Frame f;
    f.dest = ax25::Addr::make("CQ");
    f.src  = ax25::Addr::make(cfg.callsign.empty() ? "TEST" : cfg.callsign.c_str());
    f.ctrl = 0x03;  // UI
    f.pid  = 0xF0;
    const char* msg = "modemtnc loopback test 1234567890";
    f.info.assign(msg, msg + strlen(msg));
    auto raw = f.encode();

    printf("TX: %s -> %s [UI] \"%s\"\n", f.src.str().c_str(), f.dest.str().c_str(), msg);

    // Calculate preamble flags for txdelay
    int flags = cfg.txdelay_ms * cfg.baud / (8 * 1000);
    if (flags < 5) flags = 5;

    // Encode and modulate
    enc.send_frame(raw.data(), raw.size(), flags, 2);

    // Add some trailing silence for the demodulator to flush
    mod.put_quiet_ms(100);

    printf("\nGenerated %zu audio samples (%.1f ms)\n",
           audio_buf.size(), 1000.0 * audio_buf.size() / cfg.sample_rate);
    printf("Frames decoded: %d\n", frames_decoded);
    printf("Result: %s\n", frames_decoded > 0 ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
//  Main bridge loop
// ---------------------------------------------------------------------------
static void run_bridge(const Config& cfg) {
    // Open PTY
    int slave_fd;
    std::string slave_name;
    int pty_master = open_pty(&slave_fd, slave_name);
    if (pty_master < 0) { fprintf(stderr, "Failed to open PTY\n"); exit(1); }
    ::close(slave_fd);  // we only use master side

    create_symlink(slave_name, cfg.link_path);

    // Open TCP server (optional)
    int tcp_srv = -1;
    std::vector<int> tcp_clients;
    if (cfg.server_port > 0)
        tcp_srv = create_tcp_server(cfg.server_port);

    // Open audio device
    AudioDevice* audio = AudioDevice::create();
    int amp = 16000 * cfg.volume / 100;
    if (!audio->open(cfg.audio_device.c_str(), cfg.sample_rate, true, true)) {
        fprintf(stderr, "Failed to open audio device\n");
        exit(1);
    }

    printf("====================================================================\n");
    printf("  modemtnc — Software TNC with Soundcard DSP\n");
    printf("====================================================================\n");
    printf("  Modem      : %d baud\n", cfg.baud);
    printf("  Audio      : %s @ %d Hz\n",
           cfg.audio_device.empty() ? "default" : cfg.audio_device.c_str(), cfg.sample_rate);
    printf("--------------------------------------------------------------------\n");
    printf("  PTY device : %s\n", slave_name.c_str());
    printf("  Symlink    : %s  -> %s\n", cfg.link_path.c_str(), slave_name.c_str());
    if (cfg.server_port > 0)
        printf("  TCP server : port %d\n", cfg.server_port);
    printf("\n  Example:\n      ax25tnc -c W1AW -r W1BBS-1 %s\n", cfg.link_path.c_str());
    printf("--------------------------------------------------------------------\n");
    // Init PTT
    ptt::Controller ptt_ctl;
    if (!ptt_ctl.init(cfg.ptt)) {
        fprintf(stderr, "Failed to initialize PTT — exiting\n");
        exit(1);
    }

    if (cfg.monitor) printf("  Monitor on.  Ctrl-C to stop.\n\n");

    // Init modem
    modem::Demodulator demod;
    modem::Modulator modulator;
    hdlc::Decoder hdlc_dec;
    hdlc::Encoder hdlc_enc;

    demod.init(cfg.modem_type, cfg.sample_rate);
    modulator.init(cfg.modem_type, cfg.sample_rate, amp);
    hdlc_dec.init();

    // KISS decoder for host → radio TX path
    ax25::kiss::Decoder kiss_dec;

    // Wire RX: demod bit → HDLC → frame → KISS encode → PTY/TCP
    demod.set_on_bit([&hdlc_dec](int bit) { hdlc_dec.receive_bit(bit); });

    hdlc_dec.set_on_frame([&](const uint8_t* data, size_t len) {
        if (cfg.monitor) show_frame(data, len, "<- AIR");

        // KISS-wrap and send to PTY + TCP clients
        auto kissed = ax25::kiss::encode(std::vector<uint8_t>(data, data + len));
        ::write(pty_master, kissed.data(), kissed.size());
        for (int fd : tcp_clients)
            ::write(fd, kissed.data(), kissed.size());
    });

    // Wire TX: modulator samples → audio output
    std::vector<int16_t> tx_audio;
    modulator.set_on_sample([&tx_audio](int16_t s) { tx_audio.push_back(s); });
    hdlc_enc.set_on_bit([&modulator](int bit) { modulator.put_bit(bit); });

    // KISS params
    KissParams kp;
    kp.txdelay_10ms = cfg.txdelay_ms / 10;
    kp.persist = cfg.persist;
    kp.slottime_10ms = cfg.slottime_ms / 10;
    kp.txtail_10ms = cfg.txtail_ms / 10;

    // RX audio thread
    std::thread rx_thread([&]() {
        int16_t buf[1024];
        while (g_running) {
            int n = audio->read(buf, 1024);
            for (int i = 0; i < n; i++)
                demod.process_sample(buf[i]);
        }
    });

    // Main select() loop — handles PTY, TCP, and TX
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pty_master, &rfds);
        int maxfd = pty_master;

        if (tcp_srv >= 0) { FD_SET(tcp_srv, &rfds); if (tcp_srv > maxfd) maxfd = tcp_srv; }
        for (int fd : tcp_clients) { FD_SET(fd, &rfds); if (fd > maxfd) maxfd = fd; }

        struct timeval tv = {0, 50000}; // 50ms
        int ret = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        // Accept TCP connections
        if (tcp_srv >= 0 && FD_ISSET(tcp_srv, &rfds)) {
            int cfd = accept(tcp_srv, nullptr, nullptr);
            if (cfd >= 0) {
                fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) | O_NONBLOCK);
                tcp_clients.push_back(cfd);
                if (cfg.monitor) printf("  TCP client connected fd=%d\n", cfd);
            }
        }

        // Read from PTY
        if (FD_ISSET(pty_master, &rfds)) {
            uint8_t buf[2048];
            ssize_t n = ::read(pty_master, buf, sizeof(buf));
            if (n > 0) {
                auto frames = kiss_dec.feed(buf, n);
                for (auto& kf : frames) {
                    if (kf.command == ax25::kiss::Cmd::Data && kf.data.size() > 0) {
                        // Data frame → TX
                        if (cfg.monitor) show_frame(kf.data.data(), kf.data.size(), "-> AIR");

                        int flags = kp.txdelay_10ms * cfg.baud / (8 * 100);
                        if (flags < 5) flags = 5;

                        tx_audio.clear();
                        hdlc_enc.send_frame(kf.data.data(), kf.data.size(), flags, 2);
                        modulator.put_quiet_ms(kp.txtail_10ms * 10);

                        // Write TX audio with PTT control
                        if (!tx_audio.empty()) {
                            ptt_ctl.set(true);   // Key transmitter
                            size_t off = 0;
                            while (off < tx_audio.size()) {
                                int chunk = std::min((int)(tx_audio.size() - off), 1024);
                                int written = audio->write(tx_audio.data() + off, chunk);
                                if (written > 0) off += written;
                                else break;
                            }
                            audio->flush();
                            ptt_ctl.set(false);  // Release transmitter
                        }
                    }
                    // Handle KISS parameter commands
                    else if (kf.command == ax25::kiss::Cmd::TxDelay && kf.data.size() >= 1)
                        kp.txdelay_10ms = kf.data[0];
                    else if (kf.command == ax25::kiss::Cmd::Persistence && kf.data.size() >= 1)
                        kp.persist = kf.data[0];
                    else if (kf.command == ax25::kiss::Cmd::SlotTime && kf.data.size() >= 1)
                        kp.slottime_10ms = kf.data[0];
                    else if (kf.command == ax25::kiss::Cmd::TxTail && kf.data.size() >= 1)
                        kp.txtail_10ms = kf.data[0];
                }
            }
        }

        // Read from TCP clients
        for (auto it = tcp_clients.begin(); it != tcp_clients.end(); ) {
            if (FD_ISSET(*it, &rfds)) {
                uint8_t buf[2048];
                ssize_t n = ::read(*it, buf, sizeof(buf));
                if (n <= 0) {
                    if (cfg.monitor) printf("  TCP client disconnected fd=%d\n", *it);
                    ::close(*it);
                    it = tcp_clients.erase(it);
                    continue;
                }
                auto frames = kiss_dec.feed(buf, n);
                for (auto& kf : frames) {
                    if (kf.command == ax25::kiss::Cmd::Data && kf.data.size() > 0) {
                        if (cfg.monitor) show_frame(kf.data.data(), kf.data.size(), "-> AIR");
                        int flags = kp.txdelay_10ms * cfg.baud / (8 * 100);
                        if (flags < 5) flags = 5;
                        tx_audio.clear();
                        hdlc_enc.send_frame(kf.data.data(), kf.data.size(), flags, 2);
                        modulator.put_quiet_ms(kp.txtail_10ms * 10);
                        if (!tx_audio.empty()) {
                            ptt_ctl.set(true);
                            size_t off = 0;
                            while (off < tx_audio.size()) {
                                int chunk = std::min((int)(tx_audio.size() - off), 1024);
                                int written = audio->write(tx_audio.data() + off, chunk);
                                if (written > 0) off += written;
                                else break;
                            }
                            audio->flush();
                            ptt_ctl.set(false);
                        }
                    }
                }
            }
            ++it;
        }
    }

    g_running = false;
    audio->close();
    rx_thread.join();

    unlink(cfg.link_path.c_str());
    ::close(pty_master);
    if (tcp_srv >= 0) ::close(tcp_srv);
    for (int fd : tcp_clients) ::close(fd);

    ptt_ctl.close();
    delete audio;
    printf("\n  Session ended.\n");
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    Config cfg = parse_args(argc, argv);

    if (cfg.list_devices) {
        AudioDevice::list_devices();
    } else if (cfg.loopback) {
        run_loopback(cfg);
    } else {
        run_bridge(cfg);
    }

    return 0;
}
