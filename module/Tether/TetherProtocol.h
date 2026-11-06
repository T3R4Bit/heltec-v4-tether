// TETHER wire format. Pure C++ (no Arduino/Meshtastic headers) so it can be unit-tested on a PC.
// Layout is documented in docs/DESIGN_NOTES.md section 3; keep the two in sync.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace tether
{

constexpr uint8_t MAGIC0 = 0x54; // 'T'
constexpr uint8_t MAGIC1 = 0x48; // 'H'
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint16_t SEQ_NONE = 0xFFFF; // "no reply heard yet"; never used as a real sequence number

constexpr size_t HEADER_LEN = 4;
constexpr size_t PING_LEN = 13;
constexpr size_t REPLY_LEN = 11;
constexpr size_t MAX_MSG_LEN = PING_LEN;

enum class MsgType : uint8_t { PING = 1, REPLY = 2 };

struct Ping {
    uint16_t seq = 0;
    uint16_t rptSeq = SEQ_NONE; // seq of the last REPLY the sender heard
    int16_t rptRssi = 0;        // sender's RX RSSI of that reply, dBm
    int8_t rptSnrQ4 = 0;        // sender's RX SNR of that reply, 0.25 dB steps
    int8_t txPower = 0;         // dBm, 0 = region default
    uint8_t flags = 0;
};

struct Reply {
    uint16_t seq = 0;    // echoes the ping
    int16_t rssi = 0;    // replier's RX RSSI of the ping, dBm
    int8_t snrQ4 = 0;    // replier's RX SNR of the ping, 0.25 dB steps
    int8_t txPower = 0;  // dBm, 0 = region default
    uint8_t flags = 0;
};

enum class DecodeResult : uint8_t { PING, REPLY, NOT_TETHER, BAD_VERSION, BAD_TYPE, TOO_SHORT };

inline void putU16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)(v >> 8);
}

inline uint16_t getU16(const uint8_t *b)
{
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

inline void putHeader(uint8_t *b, MsgType type)
{
    b[0] = MAGIC0;
    b[1] = MAGIC1;
    b[2] = PROTOCOL_VERSION;
    b[3] = (uint8_t)type;
}

/// @return bytes written, or 0 if cap is too small.
inline size_t encodePing(const Ping &p, uint8_t *buf, size_t cap)
{
    if (cap < PING_LEN)
        return 0;
    putHeader(buf, MsgType::PING);
    putU16(buf + 4, p.seq);
    putU16(buf + 6, p.rptSeq);
    putU16(buf + 8, (uint16_t)p.rptRssi);
    buf[10] = (uint8_t)p.rptSnrQ4;
    buf[11] = (uint8_t)p.txPower;
    buf[12] = p.flags;
    return PING_LEN;
}

/// @return bytes written, or 0 if cap is too small.
inline size_t encodeReply(const Reply &r, uint8_t *buf, size_t cap)
{
    if (cap < REPLY_LEN)
        return 0;
    putHeader(buf, MsgType::REPLY);
    putU16(buf + 4, r.seq);
    putU16(buf + 6, (uint16_t)r.rssi);
    buf[8] = (uint8_t)r.snrQ4;
    buf[9] = (uint8_t)r.txPower;
    buf[10] = r.flags;
    return REPLY_LEN;
}

/// Parses buf into ping or reply depending on the type. Trailing bytes beyond the known layout are ignored, so
/// later fields can be appended without a version bump.
inline DecodeResult decode(const uint8_t *buf, size_t len, Ping &ping, Reply &reply)
{
    if (!buf || len < HEADER_LEN || buf[0] != MAGIC0 || buf[1] != MAGIC1)
        return DecodeResult::NOT_TETHER;
    if (buf[2] != PROTOCOL_VERSION)
        return DecodeResult::BAD_VERSION;
    switch ((MsgType)buf[3]) {
    case MsgType::PING:
        if (len < PING_LEN)
            return DecodeResult::TOO_SHORT;
        ping.seq = getU16(buf + 4);
        ping.rptSeq = getU16(buf + 6);
        ping.rptRssi = (int16_t)getU16(buf + 8);
        ping.rptSnrQ4 = (int8_t)buf[10];
        ping.txPower = (int8_t)buf[11];
        ping.flags = buf[12];
        return DecodeResult::PING;
    case MsgType::REPLY:
        if (len < REPLY_LEN)
            return DecodeResult::TOO_SHORT;
        reply.seq = getU16(buf + 4);
        reply.rssi = (int16_t)getU16(buf + 6);
        reply.snrQ4 = (int8_t)buf[8];
        reply.txPower = (int8_t)buf[9];
        reply.flags = buf[10];
        return DecodeResult::REPLY;
    default:
        return DecodeResult::BAD_TYPE;
    }
}

/// SNR (dB) to 0.25 dB steps, rounded and clamped to int8.
inline int8_t snrToQ4(float snrDb)
{
    long q = lroundf(snrDb * 4.0f);
    if (q > 127)
        q = 127;
    if (q < -128)
        q = -128;
    return (int8_t)q;
}

inline float q4ToSnr(int8_t q)
{
    return (float)q / 4.0f;
}

inline int16_t clampRssi(int32_t rssi)
{
    if (rssi > INT16_MAX)
        return INT16_MAX;
    if (rssi < INT16_MIN)
        return INT16_MIN;
    return (int16_t)rssi;
}

/// Sequence numbers live in 0..65534 (SEQ_NONE = 65535 is reserved), so all sequence arithmetic is modulo 65535.
constexpr uint32_t SEQ_MOD = 65535u;

/// Next ping sequence number; wraps 65534 -> 0, never producing SEQ_NONE.
inline uint16_t nextSeq(uint16_t seq)
{
    return (uint16_t)(((uint32_t)seq + 1u) % SEQ_MOD);
}

/// Forward distance from `from` to `to` in sequence space (0 = same). Values >= SEQ_MOD / 2 mean `to` is behind.
inline uint16_t seqDist(uint16_t from, uint16_t to)
{
    return (uint16_t)(((uint32_t)to + SEQ_MOD - (uint32_t)from) % SEQ_MOD);
}

} // namespace tether
