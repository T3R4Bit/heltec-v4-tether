// Battery tracking for TETHER (Phase 3): smoothed voltage and charge, charge detection, runtime estimate, and the
// low-battery ping slow-down. Pure C++ so it can be unit-tested on a PC. Charge percent comes from Meshtastic's own
// LiPo OCV curve (Power.cpp via PowerStatus); this file does not reimplement it.
#pragma once

#include <stdint.h>

#include "TetherStats.h" // Ewma

namespace tether
{

constexpr uint32_t POWER_TREND_PERIOD_S = 300; // one trend point every 5 minutes
constexpr uint8_t POWER_TREND_POINTS = 12;     // so the trend covers the last hour
constexpr uint32_t POWER_TREND_MIN_SPAN_S = 15 * 60;
constexpr uint8_t LOW_BATTERY_PCT = 20;
constexpr uint8_t CRITICAL_BATTERY_PCT = 10;

/// Slow the ping schedule on a low battery that is not charging: x2 below 20 %, x4 below 10 %.
inline uint32_t lowBatteryMultiplier(bool hasBattery, bool charging, int pct)
{
    if (!hasBattery || charging || pct < 0)
        return 1;
    if (pct < CRITICAL_BATTERY_PCT)
        return 4;
    if (pct < LOW_BATTERY_PCT)
        return 2;
    return 1;
}

class BatteryTracker
{
  public:
    /// Feed one reading. tS = seconds since boot, mv = battery millivolts, pct = 0..100 from the OCV curve.
    void add(uint32_t tS, int mv, int pct, bool charging, bool usb)
    {
        if (mv > 0)
            mvEwma.add((float)mv, 0.2f);
        if (pct >= 0 && pct <= 100)
            pctEwma.add((float)pct, 0.2f);
        lastCharging = charging;
        lastUsb = usb;
        if (charging != wasCharging) {
            nPoints = 0; // the trend before a charge state change says nothing about the trend after it
            wasCharging = charging;
        }
        if (pctEwma.has && (nPoints == 0 || tS - pointT[(head + POWER_TREND_POINTS - 1) % POWER_TREND_POINTS] >=
                                                 POWER_TREND_PERIOD_S)) {
            pointT[head] = tS;
            pointPct[head] = pctEwma.value;
            head = (uint8_t)((head + 1) % POWER_TREND_POINTS);
            if (nPoints < POWER_TREND_POINTS)
                nPoints++;
        }
    }

    bool haveVoltage() const { return mvEwma.has; }
    float voltageMv() const { return mvEwma.value; }
    bool havePercent() const { return pctEwma.has; }
    float percent() const { return pctEwma.value; }
    bool charging() const { return lastCharging; }
    bool usb() const { return lastUsb; }

    /// Least-squares slope of charge over the trend window, in percent per hour. false if not enough history.
    bool slopePctPerHour(float &out) const
    {
        if (nPoints < 3)
            return false;
        const uint8_t oldest = (uint8_t)((head + POWER_TREND_POINTS - nPoints) % POWER_TREND_POINTS);
        const uint32_t t0 = pointT[oldest];
        const uint32_t tLast = pointT[(head + POWER_TREND_POINTS - 1) % POWER_TREND_POINTS];
        if (tLast - t0 < POWER_TREND_MIN_SPAN_S)
            return false;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (uint8_t i = 0; i < nPoints; i++) {
            const uint8_t k = (uint8_t)((oldest + i) % POWER_TREND_POINTS);
            const double x = (double)(pointT[k] - t0) / 3600.0;
            const double y = pointPct[k];
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        const double den = nPoints * sxx - sx * sx;
        if (den <= 0)
            return false;
        out = (float)((nPoints * sxy - sx * sy) / den);
        return true;
    }

    /// Hours until empty at the current discharge rate; false while charging, flat, or without enough history.
    bool runtimeHours(float &out) const
    {
        float slope;
        if (lastCharging || !havePercent() || !slopePctPerHour(slope) || slope > -0.1f)
            return false;
        out = percent() / -slope;
        return true;
    }

  private:
    Ewma mvEwma, pctEwma;
    bool lastCharging = false, lastUsb = false, wasCharging = false;
    uint32_t pointT[POWER_TREND_POINTS] = {};
    float pointPct[POWER_TREND_POINTS] = {};
    uint8_t head = 0, nPoints = 0;
};

} // namespace tether
