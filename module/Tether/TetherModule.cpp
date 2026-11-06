#include "TetherModule.h"

#include "GPSStatus.h"
#include "MeshRadio.h"
#include "PowerStatus.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RadioInterface.h"
#include "RadioLibInterface.h"
#include "Router.h"
#include "airtime.h"
#include "modules/NodeInfoModule.h"

#include "TetherMath.h"
#if HAS_SCREEN && TETHER_OLED
#include "TetherScreen.h"
#endif

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

using namespace tether;

TetherModule *tetherModule;

namespace
{
constexpr uint32_t PEER_RECHECK_MS = 10 * 1000;
constexpr uint32_t IDLE_LOG_MS = 60 * 1000;
constexpr uint32_t STATS_PERIOD_MS = 60 * 1000;
constexpr uint32_t STATS_EVERY_PINGS = 10;
constexpr uint32_t NODEINFO_REQ_MS = 5 * 60 * 1000;
constexpr uint32_t POWER_SAMPLE_MS = 10 * 1000;
constexpr uint32_t POWER_LOG_MS = 60 * 1000;

// meshtastic_Data wrapped around our payload: portnum (tag + 2-byte varint for 256), payload (tag + length byte),
// bitfield (tag + 1 byte). Used only to estimate airtime before the packet exists.
constexpr size_t DATA_OVERHEAD = 3 + 2 + 2;

/// A packet heard straight from its sender, not a relayed copy. Our own packets use hop_limit 0, so for them
/// hop_start and hop_limit are both 0.
bool isDirect(const meshtastic_MeshPacket &mp)
{
    return mp.hop_start == mp.hop_limit;
}

void fmtEwma(char *out, size_t n, const Ewma &e, int decimals)
{
    if (e.has)
        snprintf(out, n, "%.*f", decimals, (double)e.value);
    else
        snprintf(out, n, "na");
}
} // namespace

TetherModule::TetherModule() : SinglePortModule("Tether", meshtastic_PortNum_PRIVATE_APP), concurrency::OSThread("Tether") {}

// ---------------------------------------------------------------------------------------------------------------------
// Scheduler

int32_t TetherModule::runOnce()
{
    if (!booted) {
        booted = true;
        if (config.device.rebroadcast_mode == meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY)
            LOG_WARN("TETHER warn reason=core_portnums_only detail=port_256_dropped_before_modules");
        LOG_INFO("TETHER boot node=!%08x version=%u budget_pct=%u min_interval_ms=%u build_peer=!%08x",
                 (unsigned)nodeDB->getNodeNum(), (unsigned)PROTOCOL_VERSION, (unsigned)TETHER_AIRTIME_BUDGET_PCT,
                 (unsigned)TETHER_MIN_INTERVAL_MS, (unsigned)TETHER_PEER_NODENUM);
        return MESHMODULE_MIN_BROADCAST_DELAY_MS;
    }

    updatePower();

    if (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        logIdle("region_unset");
        return PEER_RECHECK_MS;
    }

    const NodeNum p = resolvePeer();
    if (p != peerNum)
        setPeer(p);
    if (!peerNum) {
        logIdle("no_peer");
        return PEER_RECHECK_MS;
    }

    maybeLogStats(false);

    // The responder only answers pings; it wakes up to re-resolve the peer and print stats.
    if (role != Role::INITIATOR)
        return PEER_RECHECK_MS;

    const uint32_t base = baseIntervalMs();

    if (!peerKeyReady()) {
        logIdle("no_peer_key");
        maybeRequestPeerNodeInfo();
        return (int32_t)base;
    }

    // The previous ping got no reply before its slot ran out.
    if (awaitingReply) {
        missStreak++;
        awaitingReply = false;
    }

    if (txAllowed("ping"))
        sendPing();

    const uint32_t next =
        applyJitter(base * backoffMultiplier(missStreak) * powerMultiplier(), (uint32_t)random(0, 1000000));
    return (int32_t)next;
}

// ---------------------------------------------------------------------------------------------------------------------
// Peer and radio parameters

NodeNum TetherModule::resolvePeer() const
{
    const NodeNum me = nodeDB->getNodeNum();
    NodeNum candidate = 0;

    if ((NodeNum)TETHER_PEER_NODENUM != 0) {
        candidate = (NodeNum)TETHER_PEER_NODENUM;
    } else {
        // Exactly one favorited node is the peer; none or several means no peer.
        uint32_t idx = 0;
        unsigned favorites = 0;
        const meshtastic_NodeInfoLite *n;
        while ((n = nodeDB->readNextMeshNode(idx)) != nullptr) {
            if (n->num == me || !(n->bitfield & NODEINFO_BITFIELD_IS_FAVORITE_MASK))
                continue;
            candidate = n->num;
            if (++favorites > 1)
                return 0;
        }
    }

    if (candidate == me || candidate == NODENUM_BROADCAST)
        return 0;
    return candidate;
}

void TetherModule::setPeer(NodeNum p)
{
    peerNum = p;
    role = roleFor(p);

    nextPingSeq = 0;
    lastPingSeq = SEQ_NONE;
    awaitingReply = false;
    missStreak = 0;
    haveLastReply = false;
    lastReplySeq = SEQ_NONE;
    lastRemoteRptSeq = SEQ_NONE;
    pingsSent = pingsHeard = repliesSent = repliesHeard = passiveHeard = skipped = 0;
    pingsSinceStats = 0;
    lastStatsMs = millis();
    nodeInfoRequested = false;
    lastIdleReason = nullptr;
    local.clear();
    remote.clear();
    delivery = DeliveryWindow();
    gaps = GapCounter();

    if (p)
        LOG_INFO("TETHER peer peer=!%08x role=%s source=%s", (unsigned)p, roleName(role),
                 (NodeNum)TETHER_PEER_NODENUM != 0 ? "build_flag" : "favorite");
    else
        LOG_INFO("TETHER peer peer=none");
}

TetherModule::Role TetherModule::roleFor(NodeNum p) const
{
    if (!p)
        return Role::NONE;
    // Lower node number pings, higher one replies: no config needed and only one side spends airtime on pings.
    return nodeDB->getNodeNum() < p ? Role::INITIATOR : Role::RESPONDER;
}

const char *TetherModule::roleName(Role r)
{
    switch (r) {
    case Role::INITIATOR:
        return "initiator";
    case Role::RESPONDER:
        return "responder";
    default:
        return "none";
    }
}

TetherModule::Preset TetherModule::currentPreset() const
{
    Preset ps{};
    if (config.lora.use_preset) {
        const bool wide = RadioLibInterface::instance && RadioLibInterface::instance->wideLora();
        modemPresetToParams(config.lora.modem_preset, wide, ps.bwKHz, ps.sf, ps.cr);
    } else {
        ps.sf = (uint8_t)config.lora.spread_factor;
        ps.bwKHz = bwCodeToKHz((uint16_t)config.lora.bandwidth);
        ps.cr = (uint8_t)config.lora.coding_rate;
    }
    ps.sf = clampSpreadFactor(ps.sf);
    ps.bwKHz = clampBandwidthKHz(ps.bwKHz);
    ps.cr = clampCodingRate(ps.cr);
    return ps;
}

bool TetherModule::usesPki() const
{
#if MESHTASTIC_EXCLUDE_PKI
    return false;
#else
    // Mirrors the key/licensing part of wouldEncryptWithPKC() in Router.cpp for a unicast on our port.
    return config.security.private_key.size == 32 && !owner.is_licensed;
#endif
}

bool TetherModule::peerKeyReady() const
{
    if (!usesPki())
        return true;
    meshtastic_NodeInfoLite_public_key_t key = {0, {0}};
    return nodeDB->copyPublicKey(peerNum, key);
}

void TetherModule::maybeRequestPeerNodeInfo()
{
    if (!nodeInfoModule)
        return;
    const uint32_t now = millis();
    if (nodeInfoRequested && now - lastNodeInfoReqMs < NODEINFO_REQ_MS)
        return;
    if (!txAllowed("nodeinfo_req"))
        return;
    nodeInfoRequested = true;
    lastNodeInfoReqMs = now;
    LOG_INFO("TETHER tx kind=nodeinfo_req peer=!%08x", (unsigned)peerNum);
    // NodeInfo is PKI-exempt, so this works before we hold the peer's key; want_response makes it answer with its own.
    nodeInfoModule->sendOurNodeInfo(peerNum, true, nodeDB->getMeshNodeChannel(peerNum));
}

bool TetherModule::txAllowed(const char *what)
{
    const char *reason = nullptr;
    if (!airTime->isTxAllowedChannelUtil(true))
        reason = "chutil";
    else if (!airTime->isTxAllowedAirUtil())
        reason = "airutil";
    if (!reason)
        return true;
    skipped++;
    LOG_INFO("TETHER skip kind=%s reason=%s ch_util=%.1f air_util_tx=%.1f", what, reason,
             (double)airTime->channelUtilizationPercent(), (double)airTime->utilizationTXPercent());
    return false;
}

uint32_t TetherModule::packetAirtimeMs(size_t payloadLen) const
{
    const uint32_t onAir =
        MESHTASTIC_HEADER_LENGTH + DATA_OVERHEAD + (uint32_t)payloadLen + (usesPki() ? MESHTASTIC_PKC_OVERHEAD : 0);
    if (RadioLibInterface::instance)
        return RadioLibInterface::instance->getPacketTime(onAir);
    const Preset ps = currentPreset();
    return loraAirtimeMs(onAir, ps.sf, ps.bwKHz, ps.cr);
}

uint32_t TetherModule::baseIntervalMs() const
{
    const uint32_t exchange = packetAirtimeMs(PING_LEN) + packetAirtimeMs(REPLY_LEN);
    return pingIntervalMs(exchange, TETHER_AIRTIME_BUDGET_PCT, TETHER_MIN_INTERVAL_MS);
}

int8_t TetherModule::txPowerDbm() const
{
    int32_t p = config.lora.tx_power;
    if (p > 127)
        p = 127;
    if (p < -128)
        p = -128;
    return (int8_t)p;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sending

bool TetherModule::sendPayload(const uint8_t *buf, size_t len)
{
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) {
        LOG_WARN("TETHER err reason=alloc");
        return false;
    }
    p->to = peerNum;
    p->hop_limit = 0; // never relayed, same as RangeTestModule
    p->want_ack = false;
    p->decoded.want_response = false; // a suppressed reply must not turn into a NO_RESPONSE NAK
    p->decoded.payload.size = (pb_size_t)len;
    memcpy(p->decoded.payload.bytes, buf, len);

    const ErrorCode res = service->sendToMesh(p); // ownership passes to the mesh service in every case
    if (res != ERRNO_OK) {
        LOG_WARN("TETHER err reason=send_failed code=%d", (int)res);
        return false;
    }
    return true;
}

void TetherModule::sendPing()
{
    Ping ping;
    ping.seq = nextPingSeq;
    if (haveLastReply) {
        ping.rptSeq = lastReplySeq;
        ping.rptRssi = lastReplyRssi;
        ping.rptSnrQ4 = lastReplySnrQ4;
    }
    ping.txPower = txPowerDbm();

    uint8_t buf[MAX_MSG_LEN];
    const size_t n = encodePing(ping, buf, sizeof(buf));
    if (!n || !sendPayload(buf, n))
        return;

    lastPingSeq = ping.seq;
    nextPingSeq = nextSeq(nextPingSeq);
    pingsSent++;
    pingsSinceStats++;
    awaitingReply = true;
    delivery.recordPing(ping.seq);
    lastIdleReason = nullptr;

    LOG_INFO("TETHER tx kind=ping peer=!%08x seq=%u interval_ms=%u airtime_ms=%u backoff=%u power=%u", (unsigned)peerNum,
             (unsigned)ping.seq, (unsigned)baseIntervalMs(), (unsigned)packetAirtimeMs(PING_LEN),
             (unsigned)backoffMultiplier(missStreak), (unsigned)powerMultiplier());
}

void TetherModule::sendReply(uint16_t pingSeq, const meshtastic_MeshPacket &ping)
{
    if (!peerKeyReady()) {
        logIdle("no_peer_key");
        maybeRequestPeerNodeInfo();
        return;
    }

    Reply reply;
    reply.seq = pingSeq;
    reply.rssi = clampRssi(ping.rx_rssi);
    reply.snrQ4 = snrToQ4(ping.rx_snr);
    reply.txPower = txPowerDbm();

    uint8_t buf[MAX_MSG_LEN];
    const size_t n = encodeReply(reply, buf, sizeof(buf));
    if (!n || !sendPayload(buf, n))
        return;

    repliesSent++;
    lastIdleReason = nullptr;
    LOG_INFO("TETHER tx kind=reply peer=!%08x seq=%u", (unsigned)peerNum, (unsigned)pingSeq);
}

// ---------------------------------------------------------------------------------------------------------------------
// Receiving

bool TetherModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return p->which_payload_variant == meshtastic_MeshPacket_decoded_tag && p->decoded.portnum == ourPortNum;
}

ProcessMessage TetherModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    Ping ping;
    Reply reply;
    const DecodeResult r = decode(mp.decoded.payload.bytes, mp.decoded.payload.size, ping, reply);
    if (r != DecodeResult::PING && r != DecodeResult::REPLY) {
        if (r != DecodeResult::NOT_TETHER)
            LOG_DEBUG("TETHER drop reason=decode result=%d from=!%08x", (int)r, (unsigned)mp.from);
        return ProcessMessage::CONTINUE; // not ours; port 256 is shared
    }
    if (!peerNum || mp.from != peerNum) {
        LOG_DEBUG("TETHER drop reason=not_peer from=!%08x", (unsigned)mp.from);
        return ProcessMessage::CONTINUE;
    }
    if (!mp.has_rx_rssi) // not received over our radio (e.g. injected by a client)
        return ProcessMessage::CONTINUE;
    if (!isDirect(mp)) {
        LOG_DEBUG("TETHER drop reason=relayed hop_start=%u hop_limit=%u", (unsigned)mp.hop_start, (unsigned)mp.hop_limit);
        return ProcessMessage::CONTINUE;
    }

    if (r == DecodeResult::PING)
        handlePing(ping, mp);
    else
        handleReply(reply, mp);
    return ProcessMessage::CONTINUE;
}

void TetherModule::handlePing(const Ping &ping, const meshtastic_MeshPacket &mp)
{
    if (role != Role::RESPONDER) {
        LOG_DEBUG("TETHER drop reason=role kind=ping role=%s", roleName(role));
        return;
    }

    const GapCounter::Result g = gaps.onSeq(ping.seq);
    if (g == GapCounter::Result::DUPLICATE)
        return;
    if (g == GapCounter::Result::GAP)
        LOG_INFO("TETHER gap peer=!%08x seq=%u missed_total=%u", (unsigned)peerNum, (unsigned)ping.seq,
                 (unsigned)gaps.missedCount());
    else if (g == GapCounter::Result::RESTART)
        LOG_INFO("TETHER restart peer=!%08x seq=%u", (unsigned)peerNum, (unsigned)ping.seq);

    pingsHeard++;
    pingsSinceStats++;
    recordLocal(mp, ping.seq, SampleKind::PING);

    // The ping carries how the initiator heard our last reply. It repeats until it hears a newer one.
    if (ping.rptSeq != SEQ_NONE && ping.rptSeq != lastRemoteRptSeq) {
        lastRemoteRptSeq = ping.rptSeq;
        recordRemote(ping.rptSeq, ping.rptRssi, ping.rptSnrQ4, SampleKind::REPLY);
    }

    if (txAllowed("reply"))
        sendReply(ping.seq, mp);
}

void TetherModule::handleReply(const Reply &reply, const meshtastic_MeshPacket &mp)
{
    if (role != Role::INITIATOR) {
        LOG_DEBUG("TETHER drop reason=role kind=reply role=%s", roleName(role));
        return;
    }
    if (!delivery.recordReply(reply.seq)) {
        LOG_DEBUG("TETHER drop reason=stale_reply seq=%u", (unsigned)reply.seq);
        return;
    }

    repliesHeard++;
    missStreak = 0;
    if (reply.seq == lastPingSeq)
        awaitingReply = false;

    recordLocal(mp, reply.seq, SampleKind::REPLY);

    haveLastReply = true;
    lastReplySeq = reply.seq;
    lastReplyRssi = clampRssi(mp.rx_rssi);
    lastReplySnrQ4 = snrToQ4(mp.rx_snr);

    recordRemote(reply.seq, reply.rssi, reply.snrQ4, SampleKind::PING);
}

void TetherModule::onPassiveRx(const meshtastic_MeshPacket &mp)
{
    passiveHeard++;
    recordLocal(mp, SEQ_NONE, SampleKind::PASSIVE);
}

// ---------------------------------------------------------------------------------------------------------------------
// Stats and logging

void TetherModule::recordLocal(const meshtastic_MeshPacket &mp, uint16_t seq, SampleKind kind)
{
    Sample s;
    s.tMs = millis();
    s.seq = seq;
    s.rssi = clampRssi(mp.rx_rssi);
    s.snrQ4 = snrToQ4(mp.rx_snr);
    s.kind = kind;
    local.add(s);

    const Preset ps = currentPreset();
    const float snr = q4ToSnr(s.snrQ4);
    const double mSnr = marginSnrDb(snr, ps.sf);
    const double mRssi = marginRssiDb(s.rssi, ps.sf, ps.bwKHz);

    char line[256];
    int len;
    if (kind == SampleKind::PASSIVE) {
        const int port = mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag ? (int)mp.decoded.portnum : -1;
        len = snprintf(line, sizeof(line), "TETHER rx kind=passive peer=!%08x rssi=%d snr=%.2f margin_snr=%.2f margin_rssi=%.1f port=%d",
                       (unsigned)peerNum, (int)s.rssi, (double)snr, mSnr, mRssi, port);
    } else {
        len = snprintf(line, sizeof(line), "TETHER rx kind=%s peer=!%08x seq=%u rssi=%d snr=%.2f margin_snr=%.2f margin_rssi=%.1f",
                       kindName(kind), (unsigned)peerNum, (unsigned)seq, (int)s.rssi, (double)snr, mSnr, mRssi);
    }
    if (len > 0 && (size_t)len < sizeof(line))
        appendGps(line + len, sizeof(line) - (size_t)len);
    LOG_INFO("%s", line);
}

void TetherModule::appendGps(char *out, size_t n)
{
    if (!n)
        return;
    out[0] = '\0';
#if TETHER_GPS_TAG && !MESHTASTIC_EXCLUDE_GPS
    if (!gpsStatus) {
        snprintf(out, n, " gps=none");
        return;
    }
    const uint32_t sats = gpsStatus->getNumSatellites();
    const uint32_t hdop = localPosition.HDOP; // x100; 0 when the receiver does not report it
    const GpsQuality q = config.position.fixed_position ? GpsQuality::OK
                                                         : gpsQuality(gpsStatus->getHasLock(), sats, hdop);
    if (q != GpsQuality::OK) {
        snprintf(out, n, " gps=%s sats=%u hdop=%.2f", gpsQualityName(q), (unsigned)sats, (double)hdop / 100.0);
        return;
    }
    gpsSmooth.update((double)gpsStatus->getLatitude() * 1e-7, (double)gpsStatus->getLongitude() * 1e-7,
                     gpsStatus->getLastFixMillis());
    snprintf(out, n, " lat=%.6f lon=%.6f alt=%d hdop=%.2f sats=%u fix=%u gps_t=%u gps=%s", gpsSmooth.lat(), gpsSmooth.lon(),
             (int)gpsStatus->getAltitude(), (double)hdop / 100.0, (unsigned)sats, (unsigned)localPosition.fix_type,
             (unsigned)localPosition.timestamp, config.position.fixed_position ? "fixed" : "ok");
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// Power

void TetherModule::updatePower()
{
    if (!powerStatus)
        return;
    const uint32_t now = millis();
    if (lastPowerSampleMs && now - lastPowerSampleMs < POWER_SAMPLE_MS)
        return;
    lastPowerSampleMs = now;

    const bool hasBattery = powerStatus->getHasBattery();
    battery.add(now / 1000, hasBattery ? powerStatus->getBatteryVoltageMv() : 0,
                hasBattery ? (int)powerStatus->getBatteryChargePercent() : -1, powerStatus->getIsCharging(),
                powerStatus->getHasUSB());

    if (!lastPowerLogMs || now - lastPowerLogMs >= POWER_LOG_MS) {
        lastPowerLogMs = now;
        logPower();
    }
    const uint32_t mult = powerMultiplier();
    if (mult != lastPowerMult) {
        LOG_INFO("TETHER power_mode mult=%u pct=%.0f charging=%d", (unsigned)mult, (double)battery.percent(),
                 battery.charging() ? 1 : 0);
        lastPowerMult = mult;
    }
}

uint32_t TetherModule::powerMultiplier() const
{
#if TETHER_LOW_BATTERY_SLOWDOWN
    const bool hasBattery = powerStatus && powerStatus->getHasBattery();
    return lowBatteryMultiplier(hasBattery, battery.charging(), battery.havePercent() ? (int)(battery.percent() + 0.5f) : -1);
#else
    return 1;
#endif
}

void TetherModule::logPower()
{
    char mv[16] = "na", pct[16] = "na", trend[16] = "na", runtime[16] = "na";
    float v;
    if (battery.haveVoltage())
        snprintf(mv, sizeof(mv), "%.0f", (double)battery.voltageMv());
    if (battery.havePercent())
        snprintf(pct, sizeof(pct), "%.1f", (double)battery.percent());
    if (battery.slopePctPerHour(v))
        snprintf(trend, sizeof(trend), "%.2f", (double)v);
    if (battery.runtimeHours(v))
        snprintf(runtime, sizeof(runtime), "%.1f", (double)v);
    LOG_INFO("TETHER power vbat_mv=%d vbat_ewma=%s pct=%s usb=%d charging=%d trend_pct_h=%s runtime_h=%s mult=%u",
             powerStatus ? powerStatus->getBatteryVoltageMv() : 0, mv, pct, battery.usb() ? 1 : 0,
             battery.charging() ? 1 : 0, trend, runtime, (unsigned)powerMultiplier());
}

void TetherModule::recordRemote(uint16_t seq, int16_t rssi, int8_t snrQ4, SampleKind kind)
{
    Sample s;
    s.tMs = millis();
    s.seq = seq;
    s.rssi = rssi;
    s.snrQ4 = snrQ4;
    s.kind = kind;
    remote.add(s);

    const Preset ps = currentPreset();
    const float snr = q4ToSnr(snrQ4);
    char line[256];
    const int len = snprintf(line, sizeof(line),
                             "TETHER rpt kind=%s peer=!%08x seq=%u rssi=%d snr=%.2f margin_snr=%.2f margin_rssi=%.1f",
                             kindName(kind), (unsigned)peerNum, (unsigned)seq, (int)rssi, (double)snr,
                             (double)marginSnrDb(snr, ps.sf), (double)marginRssiDb(rssi, ps.sf, ps.bwKHz));
    if (len > 0 && (size_t)len < sizeof(line))
        appendGps(line + len, sizeof(line) - (size_t)len);
    LOG_INFO("%s", line);
}

void TetherModule::maybeLogStats(bool force)
{
    const uint32_t now = millis();
    if (!force && pingsSinceStats < STATS_EVERY_PINGS && now - lastStatsMs < STATS_PERIOD_MS)
        return;
    lastStatsMs = now;
    pingsSinceStats = 0;
    if (!peerNum)
        return;

    char rssiE[16], snrE[16], rptRssiE[16], rptSnrE[16];
    fmtEwma(rssiE, sizeof(rssiE), local.rssi, 1);
    fmtEwma(snrE, sizeof(snrE), local.snrEwma, 2);
    fmtEwma(rptRssiE, sizeof(rptRssiE), remote.rssi, 1);
    fmtEwma(rptSnrE, sizeof(rptSnrE), remote.snrEwma, 2);

    if (role == Role::INITIATOR) {
        LOG_INFO("TETHER stats peer=!%08x role=initiator sent=%u replies=%u pdr32=%.2f window=%u backoff=%u skipped=%u "
                 "passive=%u rssi_ewma=%s snr_ewma=%s rpt_rssi_ewma=%s rpt_snr_ewma=%s",
                 (unsigned)peerNum, (unsigned)pingsSent, (unsigned)repliesHeard, (double)delivery.ratio(),
                 (unsigned)delivery.pings(), (unsigned)backoffMultiplier(missStreak), (unsigned)skipped,
                 (unsigned)passiveHeard, rssiE, snrE, rptRssiE, rptSnrE);
    } else {
        LOG_INFO("TETHER stats peer=!%08x role=responder heard=%u replied=%u missed=%u loss=%.2f restarts=%u skipped=%u "
                 "passive=%u rssi_ewma=%s snr_ewma=%s rpt_rssi_ewma=%s rpt_snr_ewma=%s",
                 (unsigned)peerNum, (unsigned)pingsHeard, (unsigned)repliesSent, (unsigned)gaps.missedCount(),
                 (double)gaps.lossRatio(), (unsigned)gaps.restartCount(), (unsigned)skipped, (unsigned)passiveHeard, rssiE,
                 snrE, rptRssiE, rptSnrE);
    }
}

void TetherModule::logIdle(const char *reason)
{
    const uint32_t now = millis();
    if (reason == lastIdleReason && now - lastIdleLogMs < IDLE_LOG_MS)
        return;
    lastIdleReason = reason;
    lastIdleLogMs = now;
    LOG_INFO("TETHER idle reason=%s", reason);
}

// ---------------------------------------------------------------------------------------------------------------------
// OLED page

#if HAS_SCREEN && TETHER_OLED
namespace
{
template <size_t N> uint8_t copyGraph(const Direction<N> &d, int8_t *out)
{
    const size_t n = d.ring.size();
    const size_t first = n > (size_t)GRAPH_MAX ? n - GRAPH_MAX : 0;
    uint8_t k = 0;
    for (size_t i = first; i < n; i++)
        out[k++] = d.ring.at(i).snrQ4;
    return k;
}
} // namespace

void TetherModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;
    static Canvas canvas;     // 1 KB, kept off the stack
    static ScreenModel model; // ~170 B
    model = ScreenModel();

    model.havePeer = peerNum != 0;
    if (!model.havePeer) {
        if (lastIdleReason && strcmp(lastIdleReason, "region_unset") == 0) {
            model.idleReason = "NO REGION";
            model.idleHint1 = "SET THE LORA";
            model.idleHint2 = "REGION FIRST";
        }
    } else {
        snprintf(model.peerShort, sizeof(model.peerShort), "%04X", (unsigned)(peerNum & 0xFFFF));
        model.initiator = role == Role::INITIATOR;
        model.snrFloor = snrFloorDb(currentPreset().sf);
        model.nLocal = copyGraph(local, model.localSnrQ4);
        model.nRemote = copyGraph(remote, model.remoteSnrQ4);
        if (!local.empty()) {
            const Sample &s = local.ring.latest();
            model.haveLocal = true;
            model.localRssi = s.rssi;
            model.localSnr = q4ToSnr(s.snrQ4);
            model.ageS = (millis() - s.tMs) / 1000;
        }
        if (!remote.empty()) {
            const Sample &s = remote.ring.latest();
            model.haveRemote = true;
            model.remoteRssi = s.rssi;
            model.remoteSnr = q4ToSnr(s.snrQ4);
        }
        if (role == Role::INITIATOR && delivery.pings())
            model.pdrPct = (int)(delivery.ratio() * 100.0f + 0.5f);
        else if (role == Role::RESPONDER && gaps.receivedCount())
            model.pdrPct = (int)((1.0f - gaps.lossRatio()) * 100.0f + 0.5f);
    }

    renderScreen(canvas, model, (Layout)TETHER_OLED_LAYOUT);
    display->setColor(WHITE);
    display->drawXbm(x, y, Canvas::W, Canvas::H, canvas.buf);
}
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Sniffer

TetherSniffer::TetherSniffer(TetherModule &o) : MeshModule("TetherSniffer"), owner(o)
{
    isPromiscuous = true; // see packets not addressed to us
    encryptedOk = true;   // header and RX metrics are usable even when we can't decrypt the payload
}

bool TetherSniffer::wantPacket(const meshtastic_MeshPacket *p)
{
    const NodeNum peer = owner.peer();
    if (!peer || p->from != peer || !p->has_rx_rssi || !isDirect(*p))
        return false;
    // TETHER's own packets are counted by TetherModule.
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag && p->decoded.portnum == meshtastic_PortNum_PRIVATE_APP)
        return false;
    return true;
}

ProcessMessage TetherSniffer::handleReceived(const meshtastic_MeshPacket &mp)
{
    owner.onPassiveRx(mp);
    return ProcessMessage::CONTINUE;
}
