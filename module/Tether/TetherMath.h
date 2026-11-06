// Link math for TETHER: SNR floors, sensitivity, fade margin, LoRa airtime, ping interval.
// Pure C++ (no Arduino/Meshtastic headers) so it can be unit-tested on a PC. See docs/DESIGN_NOTES.md 4 and 6.
#pragma once

#include <math.h>
#include <stdint.h>

namespace tether
{

/// Receiver noise figure used for the sensitivity estimate. 6 dB reproduces the SX1262 datasheet
/// figure of -137 dBm at SF12 / 125 kHz.
constexpr float NOISE_FIGURE_DB = 6.0f;

/// Minimum demodulation SNR per spreading factor (SX126x datasheet). Out-of-range SF is clamped.
inline float snrFloorDb(uint8_t sf)
{
    switch (sf) {
    case 5:
        return -2.5f;
    case 6:
        return -5.0f;
    case 7:
        return -7.5f;
    case 8:
        return -10.0f;
    case 9:
        return -12.5f;
    case 10:
        return -15.0f;
    case 11:
        return -17.5f;
    default:
        return sf < 5 ? -2.5f : -20.0f;
    }
}

/// Estimated receiver sensitivity in dBm: -174 + 10*log10(BW_Hz) + NF + SNR floor.
inline float sensitivityDbm(uint8_t sf, float bwKHz)
{
    if (bwKHz <= 0.0f)
        bwKHz = 250.0f;
    return -174.0f + 10.0f * log10f(bwKHz * 1000.0f) + NOISE_FIGURE_DB + snrFloorDb(sf);
}

/// Margin above the demodulation limit, from SNR. Comparable across board revisions; underestimates on strong
/// links because LoRa SNR saturates around +10 dB.
inline float marginSnrDb(float snrDb, uint8_t sf)
{
    return snrDb - snrFloorDb(sf);
}

/// Margin above estimated sensitivity, from absolute RSSI. Only meaningful as a trend on the same hardware
/// (V4.2 / V4.3 / V3 front ends report RSSI differently).
inline float marginRssiDb(int32_t rssiDbm, uint8_t sf, float bwKHz)
{
    return (float)rssiDbm - sensitivityDbm(sf, bwKHz);
}

/// LoRa time on air (Semtech formula): explicit header, CRC on, low-data-rate optimisation when a symbol lasts
/// >= 16 ms. cr is the denominator of the coding rate (5..8 for 4/5..4/8). Used for tests and as a fallback when the
/// radio driver is not available; the firmware prefers RadioInterface::getPacketTime().
inline uint32_t loraAirtimeMs(uint32_t payloadLen, uint8_t sf, float bwKHz, uint8_t cr, uint16_t preambleSymbols = 16)
{
    if (sf < 5 || sf > 12)
        sf = 11;
    if (bwKHz <= 0.0f)
        bwKHz = 250.0f;
    if (cr < 5 || cr > 8)
        cr = 5;
    const float symMs = (float)(1UL << sf) / bwKHz; // 2^SF / BW(kHz) gives ms
    const int de = symMs >= 16.0f ? 1 : 0;
    const float num = 8.0f * (float)payloadLen - 4.0f * sf + 28.0f + 16.0f;
    const float den = 4.0f * (float)(sf - 2 * de);
    float payloadSymbols = 8.0f + fmaxf(ceilf(num / den) * (float)cr, 0.0f);
    float total = ((float)preambleSymbols + 4.25f + payloadSymbols) * symMs;
    return (uint32_t)lroundf(total);
}

/// Ping interval for an airtime budget: exchange airtime / budget, never below minMs. There is deliberately no
/// upper clamp: on slow presets the budget wins.
inline uint32_t pingIntervalMs(uint32_t exchangeAirtimeMs, uint8_t budgetPct, uint32_t minMs)
{
    if (budgetPct == 0 || budgetPct > 100)
        budgetPct = 10;
    uint64_t iv = (uint64_t)exchangeAirtimeMs * 100u / budgetPct;
    if (iv < minMs)
        iv = minMs;
    if (iv > 0xFFFFFFFFull)
        iv = 0xFFFFFFFFull;
    return (uint32_t)iv;
}

/// Back-off multiplier from the number of consecutive pings without a reply:
/// 0-2 misses x1, 3 x2, 4 x4, 5+ x8.
inline uint32_t backoffMultiplier(uint32_t missStreak)
{
    if (missStreak < 3)
        return 1;
    uint32_t shift = missStreak - 2;
    if (shift > 3)
        shift = 3;
    return 1u << shift;
}

/// +/-10 % jitter. r is any random number; only r % 201 is used.
inline uint32_t applyJitter(uint32_t intervalMs, uint32_t r)
{
    uint64_t permille = 900u + (r % 201u);
    return (uint32_t)((uint64_t)intervalMs * permille / 1000u);
}

} // namespace tether
