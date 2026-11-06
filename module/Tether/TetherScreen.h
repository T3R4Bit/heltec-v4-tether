// TETHER OLED screen: three candidate neo-brutalist layouts drawn into a Canvas.
// Pure C++ so the exact pixels can be previewed on a PC (scripts/render_oled_previews.py). See docs/OLED.md.
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "TetherCanvas.h"

namespace tether
{

constexpr int GRAPH_MAX = 64;        // samples kept for the graph, newest last
constexpr uint32_t LOST_AFTER_S = 90; // no sample for this long: show LOST
constexpr float GRAPH_SNR_MIN = -20.0f;
constexpr float GRAPH_SNR_MAX = 12.0f; // LoRa SNR saturates around here

enum class Layout : uint8_t { STRIP = 1, SPLIT = 2, BIG = 3 };

/// Everything the screen shows, gathered by the firmware from TetherModule state.
struct ScreenModel {
    bool havePeer = false;
    const char *idleReason = "NO PEER"; // shown when !havePeer
    const char *idleHint1 = "FAVORITE ONE";
    const char *idleHint2 = "NODE IN THE APP";
    char peerShort[5] = "";            // last 4 hex digits of the peer's node id
    bool initiator = true;

    uint8_t nLocal = 0;                // my RX of the peer, newest last
    int8_t localSnrQ4[GRAPH_MAX] = {};
    uint8_t nRemote = 0;               // the peer's RX of me, newest last
    int8_t remoteSnrQ4[GRAPH_MAX] = {};

    bool haveLocal = false;
    int16_t localRssi = 0;
    float localSnr = 0;
    bool haveRemote = false;
    int16_t remoteRssi = 0;
    float remoteSnr = 0;

    float snrFloor = -17.5f;           // demodulation limit for the current preset
    int pdrPct = -1;                   // delivery over the last 32 pings, -1 = unknown
    uint32_t ageS = 0xFFFFFFFFu;       // seconds since the last local sample
};

namespace screen_detail
{

inline void fmtSnr(char *out, size_t n, float snr)
{
    if (snr > -10.0f && snr < 10.0f)
        snprintf(out, n, "%+.1f", (double)snr);
    else
        snprintf(out, n, "%+d", (int)(snr >= 0 ? snr + 0.5f : snr - 0.5f));
}

inline void fmtAge(char *out, size_t n, uint32_t s)
{
    if (s == 0xFFFFFFFFu)
        snprintf(out, n, "--");
    else if (s < 100)
        snprintf(out, n, "%uS", (unsigned)s);
    else if (s < 6000)
        snprintf(out, n, "%uM", (unsigned)(s / 60));
    else
        snprintf(out, n, "%uH", (unsigned)(s / 3600 > 999 ? 999 : s / 3600));
}

inline int snrToY(float snr, int y, int h)
{
    if (snr < GRAPH_SNR_MIN)
        snr = GRAPH_SNR_MIN;
    if (snr > GRAPH_SNR_MAX)
        snr = GRAPH_SNR_MAX;
    const float f = (snr - GRAPH_SNR_MIN) / (GRAPH_SNR_MAX - GRAPH_SNR_MIN);
    return y + h - 1 - (int)(f * (float)(h - 1) + 0.5f);
}

/// Solid bars rising from the bottom of the box, newest at the right, with a dotted line at the SNR floor.
inline void barGraph(Canvas &c, int x, int y, int w, int h, const int8_t *q4, int n, float floorSnr, int barW = 3)
{
    const int slots = w / barW;
    const int first = n > slots ? n - slots : 0;
    const int right = x + w;
    const int solid = barW > 1 ? barW - 1 : 1; // one-pixel gap between bars
    for (int i = n - 1, col = 0; i >= first; i--, col++) {
        const int top = snrToY((float)q4[i] / 4.0f, y, h);
        c.fillRect(right - (col + 1) * barW, top, solid, y + h - top);
    }
    // Floor line: inverted over bars so it stays visible.
    const int fy = snrToY(floorSnr, y, h);
    for (int i = x; i < right; i += 3)
        c.set(i, fy, !c.get(i, fy));
}

inline void lostBadge(Canvas &c, int rightX, int y, uint32_t ageS)
{
    char age[16], msg[32];
    fmtAge(age, sizeof(age), ageS);
    snprintf(msg, sizeof(msg), "LOST %s", age);
    const int w = Canvas::textWidth(msg) + 6;
    c.fillRect(rightX - w, y, w, 11, true);
    c.text(rightX - w + 3, y + 2, msg, 1, false);
}

inline bool centeredMessage(Canvas &c, const ScreenModel &m)
{
    if (!m.havePeer) {
        c.textCentered(64, 14, m.idleReason ? m.idleReason : "NO PEER", 2);
        c.rect(8, 36, 112, 22, 2);
        c.textCentered(64, 40, m.idleHint1 ? m.idleHint1 : "", 1);
        c.textCentered(64, 49, m.idleHint2 ? m.idleHint2 : "", 1);
        return true;
    }
    if (!m.haveLocal) {
        c.textCentered(64, 18, "WAITING", 2);
        char line[32];
        snprintf(line, sizeof(line), "%s %s", m.initiator ? "PINGING" : "LISTENING", m.peerShort);
        c.textCentered(64, 42, line, 1);
        return true;
    }
    return false;
}

// Layout 1: header bar, big RX SNR, small stats, full-width bar graph.
inline void drawStrip(Canvas &c, const ScreenModel &m)
{
    c.fillRect(0, 0, 128, 11, true);
    c.text(2, 2, "TETHER", 1, false);
    if (m.havePeer) {
        char hdr[24];
        snprintf(hdr, sizeof(hdr), "%s%s", m.initiator ? ">" : "<", m.peerShort);
        c.textRight(126, 2, hdr, 1, false);
    }
    if (centeredMessage(c, m))
        return;

    char snr[16], buf[32];
    fmtSnr(snr, sizeof(snr), m.localSnr);
    int x = c.text(2, 14, snr, 2);
    c.text(x, 21, "DB", 1);

    snprintf(buf, sizeof(buf), "RX%d", (int)m.localRssi);
    c.textRight(126, 14, buf);
    if (m.haveRemote) {
        fmtSnr(snr, sizeof(snr), m.remoteSnr);
        snprintf(buf, sizeof(buf), "TX%s", snr);
    } else {
        snprintf(buf, sizeof(buf), "TX--");
    }
    c.textRight(126, 23, buf);

    c.rect(0, 32, 128, 32, 2);
    barGraph(c, 2, 34, 124, 28, m.localSnrQ4, m.nLocal, m.snrFloor);
    if (m.pdrPct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", m.pdrPct);
        const int w = Canvas::textWidth(buf) + 4;
        c.fillRect(2, 34, w, 9, false);
        c.text(4, 35, buf);
    }
    if (m.ageS > LOST_AFTER_S && m.ageS != 0xFFFFFFFFu)
        lostBadge(c, 126, 34, m.ageS);
}

// Layout 2: two stacked panels, RX (I hear peer) and TX (peer hears me), each with a tag, graph and values.
inline void splitPanel(Canvas &c, int y, const char *tag, bool have, float snrV, int rssi, const int8_t *q4, int n,
                       float floorSnr, const char *foot)
{
    c.fillRect(0, y, 16, 30, true);
    c.text(2, y + 11, tag, 1, false);
    barGraph(c, 18, y + 1, 75, 28, q4, n, floorSnr);
    char snr[16], buf[16];
    if (have) {
        fmtSnr(snr, sizeof(snr), snrV);
        c.textRight(127, y + 2, snr);
        snprintf(buf, sizeof(buf), "%d", rssi);
        c.textRight(127, y + 11, buf);
    } else {
        c.textRight(127, y + 2, "--");
    }
    if (foot)
        c.textRight(127, y + 21, foot);
}

inline void drawSplit(Canvas &c, const ScreenModel &m)
{
    if (centeredMessage(c, m))
        return;
    char pdr[16], age[16];
    if (m.pdrPct >= 0)
        snprintf(pdr, sizeof(pdr), "%d%%", m.pdrPct);
    else
        snprintf(pdr, sizeof(pdr), "--");
    fmtAge(age, sizeof(age), m.ageS);
    splitPanel(c, 0, "RX", m.haveLocal, m.localSnr, m.localRssi, m.localSnrQ4, m.nLocal, m.snrFloor, m.peerShort);
    c.fillRect(0, 31, 128, 2, true);
    splitPanel(c, 34, "TX", m.haveRemote, m.remoteSnr, m.remoteRssi, m.remoteSnrQ4, m.nRemote, m.snrFloor, pdr);
    if (m.ageS > LOST_AFTER_S && m.ageS != 0xFFFFFFFFu)
        lostBadge(c, 92, 2, m.ageS);
}

// Layout 3: one huge number (fade margin), stats line, short graph.
inline void drawBig(Canvas &c, const ScreenModel &m)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "TETHER %s", m.peerShort);
    c.text(0, 0, buf);
    if (m.havePeer && m.pdrPct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", m.pdrPct);
        c.textRight(127, 0, buf);
    }
    c.hline(0, 9, 128);
    c.hline(0, 10, 128);
    if (!m.havePeer || !m.haveLocal) {
        Canvas tmp;
        centeredMessage(tmp, m);
        for (int y = 12; y < 64; y++)
            for (int x = 0; x < 128; x++)
                if (tmp.get(x, y))
                    c.set(x, y);
        return;
    }
    const int margin = (int)(m.localSnr - m.snrFloor + (m.localSnr >= m.snrFloor ? 0.5f : -0.5f));
    snprintf(buf, sizeof(buf), "%d", margin);
    c.text(2, 14, "MARGIN", 1);
    c.text(2, 23, "DB", 1);
    c.textRight(127, 13, buf, 3);

    char snr[16];
    fmtSnr(snr, sizeof(snr), m.localSnr);
    snprintf(buf, sizeof(buf), "RX%d %s", (int)m.localRssi, snr);
    c.text(0, 37, buf);
    if (m.haveRemote) {
        fmtSnr(snr, sizeof(snr), m.remoteSnr);
        snprintf(buf, sizeof(buf), "TX%s", snr);
        c.textRight(127, 37, buf);
    }
    barGraph(c, 0, 46, 128, 18, m.localSnrQ4, m.nLocal, m.snrFloor);
    if (m.ageS > LOST_AFTER_S && m.ageS != 0xFFFFFFFFu)
        lostBadge(c, 127, 46, m.ageS);
}

} // namespace screen_detail

inline void renderScreen(Canvas &c, const ScreenModel &m, Layout layout)
{
    c.clear();
    switch (layout) {
    case Layout::SPLIT:
        screen_detail::drawSplit(c, m);
        break;
    case Layout::BIG:
        screen_detail::drawBig(c, m);
        break;
    default:
        screen_detail::drawStrip(c, m);
        break;
    }
}

} // namespace tether
