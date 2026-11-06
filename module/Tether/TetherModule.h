// TETHER link-quality module: ping/reply on PRIVATE_APP (port 256) between a fixed pair of nodes, plus passive
// RSSI/SNR from the peer's normal traffic. Design: docs/DESIGN_NOTES.md.
#pragma once

#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "configuration.h"

#include "TetherGeo.h"
#include "TetherPower.h"
#include "TetherProtocol.h"
#include "TetherStats.h"

// Build-time settings. Override with -D flags in platformio.ini build_flags or a userPrefs-style env.
#ifndef TETHER_PEER_NODENUM
#define TETHER_PEER_NODENUM 0 // 0 = use the single favorited node instead
#endif
#ifndef TETHER_AIRTIME_BUDGET_PCT
#define TETHER_AIRTIME_BUDGET_PCT 10 // share of airtime one ping+reply exchange may use
#endif
#ifndef TETHER_MIN_INTERVAL_MS
#define TETHER_MIN_INTERVAL_MS 2000
#endif
#ifndef TETHER_HISTORY_LEN
#define TETHER_HISTORY_LEN 128 // samples kept per direction
#endif
#ifndef TETHER_OLED
#define TETHER_OLED 1 // add a TETHER page to the OLED carousel (only on builds with a screen)
#endif
#ifndef TETHER_OLED_LAYOUT
#define TETHER_OLED_LAYOUT 1 // 1 STRIP, 2 SPLIT, 3 BIG (docs/OLED.md). Provisional default, pending a pick.
#endif
#ifndef TETHER_GPS_TAG
#define TETHER_GPS_TAG 1 // append the local GPS fix to rx/rpt lines when it passes the quality gate
#endif
#ifndef TETHER_LOW_BATTERY_SLOWDOWN
#define TETHER_LOW_BATTERY_SLOWDOWN 1 // ping less often on a low, non-charging battery
#endif

class TetherModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    TetherModule();

    /// Current peer, 0 if none. Read by TetherSniffer.
    NodeNum peer() const { return peerNum; }

    /// Called by TetherSniffer for a direct packet from the peer on any other port.
    void onPassiveRx(const meshtastic_MeshPacket &mp);

#if HAS_SCREEN && TETHER_OLED
    bool wantUIFrame() override { return true; }
    void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  private:
    /// Appends " lat=.. lon=.. alt=.. hdop=.. sats=.. fix=.. gps_t=.." (or " gps=<reason>") for the GPS-tagged log.
    void appendGps(char *out, size_t n);
    void updatePower();
    void logPower();
    uint32_t powerMultiplier() const;
    enum class Role : uint8_t { NONE, INITIATOR, RESPONDER };

    struct Preset {
        uint8_t sf;
        float bwKHz;
        uint8_t cr;
    };

    NodeNum resolvePeer() const;
    void setPeer(NodeNum p);
    Role roleFor(NodeNum p) const;
    static const char *roleName(Role r);
    Preset currentPreset() const;
    bool usesPki() const;
    bool peerKeyReady() const;
    void maybeRequestPeerNodeInfo();
    bool txAllowed(const char *what);
    uint32_t packetAirtimeMs(size_t payloadLen) const;
    uint32_t baseIntervalMs() const;
    bool sendPayload(const uint8_t *buf, size_t len);
    void sendPing();
    void sendReply(uint16_t pingSeq, const meshtastic_MeshPacket &ping);
    void handlePing(const tether::Ping &ping, const meshtastic_MeshPacket &mp);
    void handleReply(const tether::Reply &reply, const meshtastic_MeshPacket &mp);
    void recordLocal(const meshtastic_MeshPacket &mp, uint16_t seq, tether::SampleKind kind);
    void recordRemote(uint16_t seq, int16_t rssi, int8_t snrQ4, tether::SampleKind kind);
    void maybeLogStats(bool force);
    void logIdle(const char *reason);
    int8_t txPowerDbm() const;

    NodeNum peerNum = 0;
    Role role = Role::NONE;
    bool booted = false;

    // Initiator state
    uint16_t nextPingSeq = 0;
    uint16_t lastPingSeq = tether::SEQ_NONE;
    bool awaitingReply = false;
    uint32_t missStreak = 0;
    bool haveLastReply = false;
    uint16_t lastReplySeq = tether::SEQ_NONE;
    int16_t lastReplyRssi = 0;
    int8_t lastReplySnrQ4 = 0;

    // Responder state
    uint16_t lastRemoteRptSeq = tether::SEQ_NONE;

    // Counters
    uint32_t pingsSent = 0, pingsHeard = 0, repliesSent = 0, repliesHeard = 0, passiveHeard = 0, skipped = 0;
    uint32_t pingsSinceStats = 0;
    uint32_t lastStatsMs = 0;

    // Rate limits
    uint32_t lastIdleLogMs = 0;
    const char *lastIdleReason = nullptr;
    uint32_t lastNodeInfoReqMs = 0;
    bool nodeInfoRequested = false;

    tether::Direction<TETHER_HISTORY_LEN> local;  // my RX of the peer
    tether::Direction<TETHER_HISTORY_LEN> remote; // the peer's RX of me
    tether::DeliveryWindow delivery;              // initiator: replies per ping
    tether::GapCounter gaps;                      // responder: ping sequence gaps

    tether::PositionSmoother gpsSmooth;
    tether::BatteryTracker battery;
    uint32_t lastPowerSampleMs = 0;
    uint32_t lastPowerLogMs = 0;
    uint32_t lastPowerMult = 1;
};

/// Promiscuous listener that turns the peer's ordinary traffic into passive RSSI/SNR samples.
class TetherSniffer : public MeshModule
{
  public:
    explicit TetherSniffer(TetherModule &owner);

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    TetherModule &owner;
};

extern TetherModule *tetherModule;
