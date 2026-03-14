// ax25dump.hpp — hexdump-C formatter + AX.25 control field decoder
// Header-only, no external dependencies.
// Shared by ax25kiss.cpp and ax25client.cpp.
//
// Provides:
//   hex_dump(data, len, prefix)  →  hexdump -C style multi-line string
//   ctrl_detail(ctrl, nbytes)    →  "ctrl=0xXX  I  N(S)=x N(R)=y P/F=z  (N bytes)"
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// hex_dump — hexdump -C style output
//
// Format per line (16 bytes):
//   <prefix>OOOOOOOO  XX XX XX XX XX XX XX XX  XX XX XX XX XX XX XX XX  |ASCII...|
//
// Non-printable bytes in the ASCII column are shown as '.'.
// ─────────────────────────────────────────────────────────────────────────────
inline std::string hex_dump(const uint8_t* data, std::size_t len,
                             const std::string& prefix = "")
{
    if (len == 0) return "";
    std::ostringstream os;
    for (std::size_t off = 0; off < len; off += 16) {
        os << prefix
           << std::hex << std::setw(8) << std::setfill('0') << off << "  ";

        // hex columns — two groups of 8 separated by an extra space
        for (int i = 0; i < 16; ++i) {
            if (i == 8) os << ' ';
            if (off + static_cast<std::size_t>(i) < len)
                os << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(data[off + i]) << ' ';
            else
                os << "   ";
        }

        // ASCII column
        os << " |";
        for (int i = 0; i < 16 && off + static_cast<std::size_t>(i) < len; ++i) {
            uint8_t b = data[off + i];
            os << static_cast<char>(b >= 0x20 && b < 0x7f ? b : '.');
        }
        os << "|\n";
    }
    return os.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// ctrl_detail — decode one AX.25 control byte
//
// Returns a compact one-liner, e.g.:
//   "ctrl=0x04  I     N(S)=2 N(R)=3 P/F=0  (20 bytes)"
//   "ctrl=0x61  S/RR  N(R)=3 P/F=0  (15 bytes)"
//   "ctrl=0x2f  U/SABM  P/F=1  (15 bytes)"
// ─────────────────────────────────────────────────────────────────────────────
inline std::string ctrl_detail(uint8_t ctrl, std::size_t frame_bytes)
{
    std::ostringstream os;
    os << "ctrl=0x" << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(ctrl) << std::dec << "  ";

    bool pf = (ctrl & 0x10) != 0;

    if ((ctrl & 0x01) == 0x00) {
        // ── I-frame ───────────────────────────────────────────────────────
        int ns = (ctrl >> 1) & 7;
        int nr = (ctrl >> 5) & 7;
        os << "I     N(S)=" << ns << " N(R)=" << nr << " P/F=" << static_cast<int>(pf);

    } else if ((ctrl & 0x03) == 0x01) {
        // ── S-frame ───────────────────────────────────────────────────────
        int nr = (ctrl >> 5) & 7;
        static const char* const st[4] = {"RR", "RNR", "REJ", "S?"};
        os << "S/" << st[(ctrl >> 2) & 0x03]
           << "  N(R)=" << nr << " P/F=" << static_cast<int>(pf);

    } else {
        // ── U-frame ───────────────────────────────────────────────────────
        const char* ut = "U?";
        switch (ctrl & ~static_cast<uint8_t>(0x10)) {
            case 0x2F: ut = "SABM"; break;
            case 0x43: ut = "DISC"; break;
            case 0x63: ut = "UA";   break;
            case 0x0F: ut = "DM";   break;
            case 0x03: ut = "UI";   break;
            case 0x87: ut = "FRMR"; break;
            default:   break;
        }
        os << "U/" << ut << "  P/F=" << static_cast<int>(pf);
    }

    os << "  (" << frame_bytes << " bytes)";
    return os.str();
}
