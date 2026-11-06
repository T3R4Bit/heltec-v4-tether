// TETHER link statistics: ring-buffer history, running aggregates, delivery and gap counters.
// Pure C++ (no Arduino/Meshtastic headers) so it can be unit-tested on a PC. See docs/DESIGN_NOTES.md 6.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "TetherProtocol.h" // seqDist

namespace tether
{

enum class SampleKind : uint8_t { PING = 1, REPLY = 2, PASSIVE = 3 };

inline const char *kindName(SampleKind k)
{
    switch (k) {
    case SampleKind::PING:
        return "ping";
    case SampleKind::REPLY:
        return "reply";
    case SampleKind::PASSIVE:
        return "passive";
    }
    return "?";
}

struct Sample {
    uint32_t tMs = 0;  // millis() when recorded
    uint16_t seq = 0;  // TETHER seq, or SEQ_NONE for passive samples
    int16_t rssi = 0;  // dBm
    int8_t snrQ4 = 0;  // 0.25 dB steps
    SampleKind kind = SampleKind::PASSIVE;
};

/// Fixed-size ring; when full, the oldest sample is overwritten.
template <size_t N> class Ring
{
  public:
    void push(const Sample &s)
    {
        buf[head] = s;
        head = (head + 1) % N;
        if (count < N)
            count++;
    }
    size_t size() const { return count; }
    static constexpr size_t capacity() { return N; }
    /// i = 0 is the oldest sample still held.
    const Sample &at(size_t i) const { return buf[(head + N - count + i) % N]; }
    const Sample &latest() const { return buf[(head + N - 1) % N]; }
    void clear() { head = count = 0; }

  private:
    Sample buf[N];
    size_t head = 0;
    size_t count = 0;
};

struct Ewma {
    bool has = false;
    float value = 0.0f;
    void add(float x, float alpha = 0.25f)
    {
        value = has ? value + alpha * (x - value) : x;
        has = true;
    }
};

/// One direction of the link (my RX of the peer, or the peer's RX of me).
template <size_t N> class Direction
{
  public:
    void add(const Sample &s)
    {
        ring.push(s);
        const float snr = (float)s.snrQ4 / 4.0f;
        rssi.add((float)s.rssi);
        snrEwma.add(snr);
        if (samples == 0 || s.rssi < minRssi)
            minRssi = s.rssi;
        if (samples == 0 || s.rssi > maxRssi)
            maxRssi = s.rssi;
        if (samples == 0 || s.snrQ4 < minSnrQ4)
            minSnrQ4 = s.snrQ4;
        if (samples == 0 || s.snrQ4 > maxSnrQ4)
            maxSnrQ4 = s.snrQ4;
        samples++;
    }
    bool empty() const { return samples == 0; }
    void clear()
    {
        ring.clear();
        rssi = Ewma();
        snrEwma = Ewma();
        minRssi = maxRssi = 0;
        minSnrQ4 = maxSnrQ4 = 0;
        samples = 0;
    }

    Ring<N> ring;
    Ewma rssi;
    Ewma snrEwma;
    int16_t minRssi = 0, maxRssi = 0;
    int8_t minSnrQ4 = 0, maxSnrQ4 = 0;
    uint32_t samples = 0;
};

/// Initiator side: which of the last 32 pings got a reply. Late replies (arriving after later pings were sent) still
/// count as long as their ping is inside the window.
class DeliveryWindow
{
  public:
    void recordPing(uint16_t seq)
    {
        mask <<= 1;
        if (n < 32)
            n++;
        lastSeq = seq;
        havePing = true;
    }
    /// @return false if the reply does not match a ping in the window (stale, duplicate or unknown).
    bool recordReply(uint16_t seq)
    {
        if (!havePing)
            return false;
        uint16_t age = seqDist(seq, lastSeq);
        if (age >= n)
            return false;
        uint32_t bit = 1u << age;
        if (mask & bit)
            return false; // duplicate
        mask |= bit;
        return true;
    }
    uint8_t pings() const { return n; }
    uint8_t replies() const
    {
        uint32_t m = n >= 32 ? mask : (mask & ((1u << n) - 1u));
        uint8_t c = 0;
        while (m) {
            c += m & 1u;
            m >>= 1;
        }
        return c;
    }
    float ratio() const { return n ? (float)replies() / (float)n : 0.0f; }

  private:
    uint32_t mask = 0; // bit k = ping (lastSeq - k) was answered
    uint8_t n = 0;
    uint16_t lastSeq = 0;
    bool havePing = false;
};

/// Responder side: counts gaps in received ping sequence numbers. A big backwards jump is treated as the
/// initiator rebooting (sequence restarted), not as loss.
class GapCounter
{
  public:
    enum class Result : uint8_t { FIRST, IN_ORDER, GAP, DUPLICATE, RESTART };

    Result onSeq(uint16_t seq)
    {
        if (!has) {
            has = true;
            last = seq;
            received++;
            return Result::FIRST;
        }
        uint16_t d = seqDist(last, seq);
        if (d == 0)
            return Result::DUPLICATE;
        if (d < SEQ_MOD / 2) {
            received++;
            last = seq;
            if (d == 1)
                return Result::IN_ORDER;
            missed += (uint32_t)(d - 1);
            return Result::GAP;
        }
        // Backwards: an old straggler or a reboot. Either way, resync on it.
        restarts++;
        received++;
        last = seq;
        return Result::RESTART;
    }
    uint32_t receivedCount() const { return received; }
    uint32_t missedCount() const { return missed; }
    uint32_t restartCount() const { return restarts; }
    float lossRatio() const
    {
        uint32_t total = received + missed;
        return total ? (float)missed / (float)total : 0.0f;
    }

  private:
    bool has = false;
    uint16_t last = 0;
    uint32_t received = 0, missed = 0, restarts = 0;
};

} // namespace tether
