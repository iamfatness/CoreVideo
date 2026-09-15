#pragma once

// Streaming Matroska transport to FFmpeg over one pipe. One fixed I420 track
// and one PCM S16LE stereo track, both on a microsecond clock. This is an
// internal transport; FFmpeg writes the user's single H.264/AAC MP4.
// Codec mapping: https://www.matroska.org/technical/codec_specs.html
#include <cstdint>
#include <cstring>
#include <vector>

namespace iso_av
{
using Bytes = std::vector<uint8_t>;
inline void be(Bytes &out, uint64_t value, int count)
{
    for (int i = count - 1; i >= 0; --i)
        out.push_back(uint8_t(value >> (i * 8)));
}
inline void element(Bytes &out, uint32_t id, const Bytes &value)
{
    int n = id > 0xffffff ? 4 : id > 0xffff ? 3 : id > 0xff ? 2 : 1;
    be(out, id, n);
    // Fixed eight-byte EBML size, valid for every element in this transport.
    be(out, (uint64_t(1) << 56) | value.size(), 8);
    out.insert(out.end(), value.begin(), value.end());
}
inline void number(Bytes &out, uint32_t id, uint64_t value)
{
    Bytes b;
    be(b, value, 8);
    element(out, id, b);
}
inline void string(Bytes &out, uint32_t id, const char *value)
{
    element(out, id, Bytes(value, value + std::strlen(value)));
}
inline Bytes header()
{
    Bytes out, ebml, info, tracks, video, audio, v, a;
    number(ebml, 0x4286, 1);
    number(ebml, 0x42f7, 1);
    number(ebml, 0x42f2, 4);
    number(ebml, 0x42f3, 8);
    string(ebml, 0x4282, "matroska");
    number(ebml, 0x4287, 4);
    number(ebml, 0x4285, 2);
    element(out, 0x1a45dfa3, ebml);
    be(out, 0x18538067, 4);
    be(out, 0x01ffffffffffffffULL, 8); // unknown segment size
    number(info, 0x2ad7b1, 1000);      // one tick = one microsecond
    string(info, 0x4d80, "CoreVideo");
    string(info, 0x5741, "CoreVideo");
    element(out, 0x1549a966, info);
    number(video, 0xd7, 1);
    number(video, 0x73c5, 1);
    number(video, 0x83, 1);
    string(video, 0x86, "V_MS/VFW/FOURCC");
    Bytes bitmap(40, 0);
    const auto le = [&](size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i)
            bitmap[at + i] = uint8_t(value >> (8 * i));
    };
    le(0, 40);
    le(4, 1920);
    le(8, 1080);
    bitmap[12] = 1;
    bitmap[14] = 12;
    le(16, 0x30323449);
    le(20, 1920 * 1080 * 3 / 2); // I420 BITMAPINFOHEADER
    element(video, 0x63a2, bitmap);
    number(video, 0x23e383, 33333333);
    number(v, 0xb0, 1920);
    number(v, 0xba, 1080);
    // The engine normalizes Zoom pixels to full-range BT.709 before delivery.
    Bytes colour;
    number(colour, 0x55b1, 1);
    number(colour, 0x55b9, 2);
    number(colour, 0x55ba, 1);
    number(colour, 0x55bb, 1);
    element(v, 0x55b0, colour);
    element(video, 0xe0, v);
    element(tracks, 0xae, video);
    number(audio, 0xd7, 2);
    number(audio, 0x73c5, 2);
    number(audio, 0x83, 2);
    string(audio, 0x86, "A_PCM/INT/LIT");
    double rate = 48000;
    uint64_t bits;
    std::memcpy(&bits, &rate, 8);
    Bytes frequency;
    be(frequency, bits, 8);
    element(a, 0xb5, frequency);
    number(a, 0x9f, 2);
    number(a, 0x6264, 16);
    element(audio, 0xe1, a);
    element(tracks, 0xae, audio);
    element(out, 0x1654ae6b, tracks);
    return out;
}
inline Bytes frame(uint64_t index, const Bytes &video, const Bytes &audio)
{
    Bytes cluster, block;
    number(cluster, 0xe7, index * 1000000ULL / 30);
    block = {0x81, 0, 0, 0x80};
    block.insert(block.end(), video.begin(), video.end());
    element(cluster, 0xa3, block);
    block = {0x82, 0, 0, 0x80};
    block.insert(block.end(), audio.begin(), audio.end());
    element(cluster, 0xa3, block);
    Bytes out;
    element(out, 0x1f43b675, cluster);
    return out;
}
} // namespace iso_av
