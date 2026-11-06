// GPS quality gate and position smoothing for TETHER's GPS-tagged signal log (Phase 2).
// Pure C++ so it can be unit-tested on a PC.
#pragma once

#include <math.h>
#include <stdint.h>

namespace tether
{

constexpr uint32_t GPS_MAX_HDOP_X100 = 200; // drop samples above HDOP 2.0
constexpr uint32_t GPS_MIN_SATS = 6;
constexpr double GPS_RESET_JUMP_M = 100.0; // a jump this large restarts smoothing instead of averaging across it
constexpr double GPS_SMOOTH_ALPHA = 0.35;

enum class GpsQuality : uint8_t { OK, NO_FIX, FEW_SATS, HIGH_HDOP };

inline const char *gpsQualityName(GpsQuality q)
{
    switch (q) {
    case GpsQuality::OK:
        return "ok";
    case GpsQuality::NO_FIX:
        return "nofix";
    case GpsQuality::FEW_SATS:
        return "sats";
    case GpsQuality::HIGH_HDOP:
        return "hdop";
    }
    return "?";
}

/// hdopX100 = 0 means the receiver did not report HDOP; then only the satellite count is checked.
inline GpsQuality gpsQuality(bool hasLock, uint32_t sats, uint32_t hdopX100)
{
    if (!hasLock)
        return GpsQuality::NO_FIX;
    if (sats < GPS_MIN_SATS)
        return GpsQuality::FEW_SATS;
    if (hdopX100 > GPS_MAX_HDOP_X100)
        return GpsQuality::HIGH_HDOP;
    return GpsQuality::OK;
}

inline double approxDistanceM(double lat1, double lon1, double lat2, double lon2)
{
    // Equirectangular: plenty for the few-hundred-metre jumps this is used to detect.
    const double k = 3.14159265358979323846 / 180.0;
    const double x = (lon2 - lon1) * k * cos((lat1 + lat2) * 0.5 * k);
    const double y = (lat2 - lat1) * k;
    return 6371008.8 * sqrt(x * x + y * y);
}

/// Exponential smoothing of good fixes; one update per new fix (keyed by fix time).
class PositionSmoother
{
  public:
    /// @return true if this fix was new and was taken into the average.
    bool update(double lat, double lon, uint32_t fixMs)
    {
        if (has && fixMs == lastFixMs)
            return false;
        if (!has || approxDistanceM(sLat, sLon, lat, lon) > GPS_RESET_JUMP_M) {
            sLat = lat;
            sLon = lon;
            resets += has ? 1 : 0;
        } else {
            sLat += GPS_SMOOTH_ALPHA * (lat - sLat);
            sLon += GPS_SMOOTH_ALPHA * (lon - sLon);
        }
        has = true;
        lastFixMs = fixMs;
        return true;
    }
    bool valid() const { return has; }
    double lat() const { return sLat; }
    double lon() const { return sLon; }
    uint32_t resetCount() const { return resets; }
    void clear() { has = false; }

  private:
    bool has = false;
    double sLat = 0, sLon = 0;
    uint32_t lastFixMs = 0;
    uint32_t resets = 0;
};

} // namespace tether
