#pragma once

// Pure OSC (Open Sound Control) wire-format encode/decode helpers, factored
// out of zoom-osc-server.cpp so they can be unit tested on the host without
// OBS, the Zoom SDK, or a live UDP socket. No behavior change from the
// original static functions in zoom-osc-server.cpp — this header is a
// verbatim move, marked `inline` so it can be included from multiple
// translation units (the plugin .cpp and the standalone test executable).

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Lightweight OSC argument — supports int32, float32, and string types.
struct OscArg {
    enum Type { Int32, Float32, String } type;
    int32_t     i = 0;
    float       f = 0.f;
    std::string s;
};

// Round up to the next multiple of 4.
inline int osc_pad4(int n)
{
    return (n + 3) & ~3;
}

// Read a null-terminated, 4-byte-padded OSC string from buf[offset].
// Returns "" and leaves offset unchanged on error (no NUL found before the
// end of the buffer).
inline std::string osc_read_string(const QByteArray &buf, int &offset)
{
    const int start = offset;
    const int len   = buf.size();
    while (offset < len && buf[offset] != '\0') ++offset;
    if (offset >= len) { offset = start; return {}; }
    const std::string s(buf.constData() + start, offset - start);
    ++offset; // consume NUL
    offset = osc_pad4(offset);
    return s;
}

// Read a big-endian int32 from buf[offset]. Returns 0 and leaves offset
// unchanged if fewer than 4 bytes remain (truncated buffer).
inline int32_t osc_read_int32(const QByteArray &buf, int &offset)
{
    if (offset + 4 > buf.size()) return 0;
    const auto *p = reinterpret_cast<const uint8_t *>(buf.constData() + offset);
    offset += 4;
    return static_cast<int32_t>((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                 (uint32_t(p[2]) << 8)  |  uint32_t(p[3]));
}

// Read a big-endian float32 from buf[offset]. Same truncation behavior as
// osc_read_int32 (returns 0.0f, offset unchanged) since it is implemented
// in terms of it.
inline float osc_read_float32(const QByteArray &buf, int &offset)
{
    const int32_t raw = osc_read_int32(buf, offset);
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
}

// Encode a big-endian int32.
inline void osc_write_int32(QByteArray &out, int32_t v)
{
    out.append(static_cast<char>((v >> 24) & 0xFF));
    out.append(static_cast<char>((v >> 16) & 0xFF));
    out.append(static_cast<char>((v >>  8) & 0xFF));
    out.append(static_cast<char>( v        & 0xFF));
}

// Encode an OSC string (null-terminated, padded to 4 bytes).
inline void osc_write_string(QByteArray &out, const std::string &s)
{
    out.append(s.c_str(), static_cast<int>(s.size()) + 1);
    while (out.size() % 4 != 0) out.append('\0');
}

// Parse a raw OSC message datagram into address + args.
// Returns false if the packet is malformed: no address, address does not
// start with '/', or an unsupported type tag is encountered. Note that a
// truncated numeric/string argument does NOT make this return false --
// the reader helpers above silently yield 0 / "" for a truncated value.
// This mirrors the original zoom-osc-server.cpp behavior exactly (a
// deliberate choice preserved here, not a bug being introduced).
inline bool osc_parse_message(const QByteArray &data,
                               QString &address,
                               std::vector<OscArg> &args)
{
    int offset = 0;
    const std::string addr_str = osc_read_string(data, offset);
    if (addr_str.empty() || addr_str[0] != '/') return false;
    address = QString::fromStdString(addr_str);

    if (offset >= data.size() || data[offset] != ',') return true; // no type tag — valid
    const std::string type_tags = osc_read_string(data, offset);

    for (size_t i = 1; i < type_tags.size(); ++i) {
        OscArg arg;
        switch (type_tags[i]) {
        case 'i':
            arg.type = OscArg::Int32;
            arg.i    = osc_read_int32(data, offset);
            break;
        case 'f':
            arg.type = OscArg::Float32;
            arg.f    = osc_read_float32(data, offset);
            break;
        case 's':
            arg.type = OscArg::String;
            arg.s    = osc_read_string(data, offset);
            break;
        case 'T': arg.type = OscArg::Int32; arg.i = 1; break;
        case 'F': arg.type = OscArg::Int32; arg.i = 0; break;
        default:  return false; // unsupported type
        }
        args.push_back(std::move(arg));
    }
    return true;
}

// Build a complete single-message OSC packet (address + type tags + args).
inline QByteArray osc_build_message(const std::string &address,
                                     const std::string &type_tags,
                                     const std::vector<OscArg> &args)
{
    QByteArray pkt;
    osc_write_string(pkt, address);
    osc_write_string(pkt, "," + type_tags);
    for (size_t i = 0; i < args.size(); ++i) {
        switch (type_tags[i]) {
        case 'i': osc_write_int32(pkt, args[i].i); break;
        case 's': osc_write_string(pkt, args[i].s); break;
        default: break;
        }
    }
    return pkt;
}
