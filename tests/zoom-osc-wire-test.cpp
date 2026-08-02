#include "zoom-osc-wire.h"

#include <cstring>
#include <iostream>

static int g_failures = 0;

static void expect(const char *name, bool cond)
{
    if (!cond) {
        std::cerr << name << ": failed\n";
        ++g_failures;
    }
}

// Build a raw OSC message buffer by hand (independent of osc_build_message)
// so the parser tests aren't just round-tripping against themselves.
static QByteArray raw_osc_string(const std::string &s)
{
    QByteArray out;
    out.append(s.c_str(), static_cast<int>(s.size()) + 1);
    while (out.size() % 4 != 0) out.append('\0');
    return out;
}

int main()
{
    // pad4 boundary values. osc_pad4() is a pure function of a literal
    // argument, so cppcheck's value-flow analysis correctly proves each of
    // these comparisons is always true -- that is exactly what a regression
    // assertion on a pure function's known output is supposed to do.
    // cppcheck-suppress knownConditionTrueFalse
    expect("pad4(0) == 0", osc_pad4(0) == 0);
    // cppcheck-suppress knownConditionTrueFalse
    expect("pad4(1) == 4", osc_pad4(1) == 4);
    // cppcheck-suppress knownConditionTrueFalse
    expect("pad4(3) == 4", osc_pad4(3) == 4);
    // cppcheck-suppress knownConditionTrueFalse
    expect("pad4(4) == 4", osc_pad4(4) == 4);
    // cppcheck-suppress knownConditionTrueFalse
    expect("pad4(5) == 8", osc_pad4(5) == 8);

    // ── Address matching / no-type-tag messages ─────────────────────────
    {
        QByteArray buf = raw_osc_string("/zoom/ping");
        QString address;
        std::vector<OscArg> args;
        expect("address-only message parses", osc_parse_message(buf, address, args));
        expect("address matches", address == "/zoom/ping");
        expect("no args for address-only message", args.empty());
    }

    // Explicit empty type-tag string (",") is valid and yields no args.
    {
        QByteArray buf = raw_osc_string("/zoom/ping") + raw_osc_string(",");
        QString address;
        std::vector<OscArg> args;
        expect("empty type-tag message parses", osc_parse_message(buf, address, args));
        expect("empty type-tag message has no args", args.empty());
    }

    // ── Type tags i / f / s / T / F ──────────────────────────────────────
    // Hand-built rather than via osc_build_message(): that helper's write
    // switch only serializes 'i' and 's' payloads (which matches every
    // build_osc() call site actually used in zoom-osc-server.cpp today --
    // no production message sends a float or boolean tag), so it is not a
    // valid oracle for 'f'/'T'/'F' wire bytes. This test constructs the
    // datagram directly with the same primitives, matching exactly what a
    // real OSC client would put on the wire.
    {
        QByteArray pkt;
        osc_write_string(pkt, "/zoom/test");
        osc_write_string(pkt, ",ifsTF");
        osc_write_int32(pkt, 42); // i
        float fval = 3.5f;
        int32_t fraw;
        std::memcpy(&fraw, &fval, 4);
        osc_write_int32(pkt, fraw); // f (big-endian bit pattern)
        osc_write_string(pkt, "hello"); // s
        // T and F carry no payload bytes on the wire.

        QString address;
        std::vector<OscArg> out;
        expect("full type-tag message parses", osc_parse_message(pkt, address, out));
        expect("address round-trips", address == "/zoom/test");
        if (out.size() == 5) {
            expect("int32 round-trips", out[0].type == OscArg::Int32 && out[0].i == 42);
            expect("float32 round-trips", out[1].type == OscArg::Float32 && out[1].f == 3.5f);
            expect("string round-trips", out[2].type == OscArg::String && out[2].s == "hello");
            expect("T tag decodes to int 1", out[3].type == OscArg::Int32 && out[3].i == 1);
            expect("F tag decodes to int 0", out[4].type == OscArg::Int32 && out[4].i == 0);
        } else {
            expect("expected 5 args", false);
        }
    }

    // Negative int32 round-trips through the two's-complement encoding.
    {
        std::vector<OscArg> in(1);
        in[0].type = OscArg::Int32;
        in[0].i = -12345;
        const QByteArray pkt = osc_build_message("/zoom/negative", "i", in);
        QString address;
        std::vector<OscArg> out;
        expect("negative int32 message parses", osc_parse_message(pkt, address, out));
        expect("negative int32 round-trips", out.size() == 1 && out[0].i == -12345);
    }

    // ── Malformed packets ────────────────────────────────────────────────

    // Empty buffer: no address at all.
    {
        QByteArray buf;
        QString address;
        std::vector<OscArg> args;
        expect("empty buffer is rejected", !osc_parse_message(buf, address, args));
    }

    // Address missing the leading '/'.
    {
        QByteArray buf = raw_osc_string("zoom/ping");
        QString address;
        std::vector<OscArg> args;
        expect("address without leading slash is rejected",
               !osc_parse_message(buf, address, args));
    }

    // Address with no NUL terminator before the end of the buffer
    // (truncated mid-address).
    {
        QByteArray buf = "/zoom/pi"; // no NUL, no padding
        QString address;
        std::vector<OscArg> args;
        expect("truncated address (no NUL) is rejected",
               !osc_parse_message(buf, address, args));
    }

    // ── Unsupported type tags ────────────────────────────────────────────
    {
        // 'd' (double) is not a supported tag in this implementation.
        // Hand-build so the unsupported tag doesn't trip up osc_build_message
        // (which only knows how to encode 'i' and 's').
        QByteArray pkt = raw_osc_string("/zoom/bad") + raw_osc_string(",id");
        osc_write_int32(pkt, 7); // value for the 'i' tag; nothing follows for 'd'

        QString address;
        std::vector<OscArg> args;
        expect("unsupported type tag is rejected cleanly",
               !osc_parse_message(pkt, address, args));
        // The 'i' arg preceding the unsupported tag is still parsed before
        // the parser bails -- callers (dispatch()) discard args on failure,
        // but pinning this documents the exact current behavior.
        expect("args parsed before the bad tag are preserved",
               args.size() == 1 && args[0].i == 7);
    }

    // ── Truncated buffers (numeric args silently read as 0, not rejected) ──
    {
        // Type tag says one int32 arg follows, but the buffer ends right
        // after the type-tag string. osc_read_int32() returns 0 without
        // advancing the offset or failing the parse -- this mirrors the
        // original zoom-osc-server.cpp behavior exactly.
        QByteArray pkt = raw_osc_string("/zoom/trunc") + raw_osc_string(",i");
        QString address;
        std::vector<OscArg> args;
        expect("truncated int32 arg does not fail the parse",
               osc_parse_message(pkt, address, args));
        expect("truncated int32 arg reads as 0",
               args.size() == 1 && args[0].type == OscArg::Int32 && args[0].i == 0);
    }
    {
        // Same for float32 (implemented in terms of osc_read_int32).
        QByteArray pkt = raw_osc_string("/zoom/trunc") + raw_osc_string(",f");
        QString address;
        std::vector<OscArg> args;
        expect("truncated float32 arg does not fail the parse",
               osc_parse_message(pkt, address, args));
        expect("truncated float32 arg reads as 0",
               args.size() == 1 && args[0].type == OscArg::Float32 && args[0].f == 0.0f);
    }
    {
        // A type-tag string itself can be truncated (comma present but no
        // NUL before the end of the buffer). osc_read_string() then returns
        // "" and leaves the offset unchanged, so the tag loop (which starts
        // at index 1) never runs -- the message is treated as valid with
        // zero args. This is existing, slightly surprising behavior that
        // this test pins rather than "fixes".
        QByteArray pkt = raw_osc_string("/zoom/trunc") + QByteArray(",ii", 3);
        QString address;
        std::vector<OscArg> args;
        expect("truncated type-tag string does not fail the parse",
               osc_parse_message(pkt, address, args));
        expect("truncated type-tag string yields no args", args.empty());
    }

    // Truncated string arg (no NUL before end of buffer): read_osc_string
    // returns "" for that arg; since it's the only tag, the message is
    // still reported as parsed.
    {
        QByteArray pkt = raw_osc_string("/zoom/trunc") + raw_osc_string(",s") + QByteArray("abc");
        QString address;
        std::vector<OscArg> args;
        expect("truncated string arg does not fail the parse",
               osc_parse_message(pkt, address, args));
        expect("truncated string arg reads as empty",
               args.size() == 1 && args[0].type == OscArg::String && args[0].s.empty());
    }

    return g_failures == 0 ? 0 : 1;
}
