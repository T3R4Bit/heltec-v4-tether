// Host unit tests for TETHER's hardware-independent code (protocol, link math, stats).
// Build and run: python scripts/run_host_tests.py
#include "TetherMath.h"
#include "TetherProtocol.h"
#include "TetherStats.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace tether;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                                                         \
    do {                                                                                                                    \
        checks++;                                                                                                           \
        if (!(cond)) {                                                                                                      \
            failures++;                                                                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                          \
        }                                                                                                                   \
    } while (0)

#define CHECK_NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

static void testPingRoundTrip()
{
    Ping in;
    in.seq = 0xFFFE;
    in.rptSeq = 1234;
    in.rptRssi = -121;
    in.rptSnrQ4 = snrToQ4(-12.25f);
    in.txPower = 22;
    in.flags = 0;
    uint8_t buf[MAX_MSG_LEN];
    CHECK(encodePing(in, buf, sizeof(buf)) == PING_LEN);

    // Exact byte layout from docs/DESIGN_NOTES.md section 3.
    const uint8_t expect[PING_LEN] = {0x54, 0x48, 1, 1, 0xFE, 0xFF, 0xD2, 0x04, 0x87, 0xFF, 0xCF, 22, 0};
    CHECK(memcmp(buf, expect, PING_LEN) == 0);

    Ping out;
    Reply unused;
    CHECK(decode(buf, PING_LEN, out, unused) == DecodeResult::PING);
    CHECK(out.seq == in.seq);
    CHECK(out.rptSeq == in.rptSeq);
    CHECK(out.rptRssi == -121);
    CHECK(out.rptSnrQ4 == -49);
    CHECK(out.txPower == 22);
}

static void testReplyRoundTrip()
{
    Reply in;
    in.seq = 42;
    in.rssi = -91;
    in.snrQ4 = snrToQ4(6.25f);
    in.txPower = -3;
    uint8_t buf[MAX_MSG_LEN];
    CHECK(encodeReply(in, buf, sizeof(buf)) == REPLY_LEN);
    Ping unused;
    Reply out;
    CHECK(decode(buf, REPLY_LEN, unused, out) == DecodeResult::REPLY);
    CHECK(out.seq == 42);
    CHECK(out.rssi == -91);
    CHECK(out.snrQ4 == 25);
    CHECK(out.txPower == -3);
}

static void testDecodeRejects()
{
    Ping p;
    Reply r;
    uint8_t buf[MAX_MSG_LEN + 4] = {0};
    encodePing(Ping(), buf, sizeof(buf));

    CHECK(decode(nullptr, 0, p, r) == DecodeResult::NOT_TETHER);
    CHECK(decode(buf, 3, p, r) == DecodeResult::NOT_TETHER);
    CHECK(decode(buf, PING_LEN - 1, p, r) == DecodeResult::TOO_SHORT);
    CHECK(decode(buf, PING_LEN + 4, p, r) == DecodeResult::PING); // trailing bytes ignored

    uint8_t bad[PING_LEN];
    memcpy(bad, buf, PING_LEN);
    bad[0] = 'X';
    CHECK(decode(bad, PING_LEN, p, r) == DecodeResult::NOT_TETHER);
    memcpy(bad, buf, PING_LEN);
    bad[2] = 2;
    CHECK(decode(bad, PING_LEN, p, r) == DecodeResult::BAD_VERSION);
    memcpy(bad, buf, PING_LEN);
    bad[3] = 9;
    CHECK(decode(bad, PING_LEN, p, r) == DecodeResult::BAD_TYPE);

    encodeReply(Reply(), buf, sizeof(buf));
    CHECK(decode(buf, REPLY_LEN - 1, p, r) == DecodeResult::TOO_SHORT);

    // Text that happens to arrive on port 256 (e.g. from another app) is not ours.
    const char *text = "hello";
    CHECK(decode((const uint8_t *)text, 5, p, r) == DecodeResult::NOT_TETHER);

    uint8_t small[4];
    CHECK(encodePing(Ping(), small, sizeof(small)) == 0);
    CHECK(encodeReply(Reply(), small, sizeof(small)) == 0);
}

static void testScalarHelpers()
{
    CHECK(snrToQ4(6.25f) == 25);
    CHECK(snrToQ4(-20.0f) == -80);
    CHECK(snrToQ4(0.12f) == 0);
    CHECK(snrToQ4(0.13f) == 1);
    CHECK(snrToQ4(40.0f) == 127);
    CHECK(snrToQ4(-40.0f) == -128);
    CHECK_NEAR(q4ToSnr(-49), -12.25, 1e-6);
    CHECK(clampRssi(-140) == -140);
    CHECK(clampRssi(100000) == INT16_MAX);
    CHECK(clampRssi(-100000) == INT16_MIN);
}

static void testSeqArithmetic()
{
    CHECK(nextSeq(5) == 6);
    CHECK(nextSeq(0xFFFE) == 0); // never produces SEQ_NONE
    CHECK(seqDist(5, 5) == 0);
    CHECK(seqDist(5, 8) == 3);
    CHECK(seqDist(0xFFFE, 0) == 1); // wrap skips 0xFFFF
    CHECK(seqDist(0xFFFD, 1) == 3);
    CHECK(seqDist(8, 5) >= SEQ_MOD / 2); // behind
}

static void testLinkMath()
{
    CHECK_NEAR(snrFloorDb(7), -7.5, 1e-6);
    CHECK_NEAR(snrFloorDb(12), -20.0, 1e-6);
    CHECK_NEAR(snrFloorDb(20), -20.0, 1e-6); // clamped
    CHECK_NEAR(sensitivityDbm(12, 125), -137.0, 0.1); // SX1262 datasheet
    CHECK_NEAR(sensitivityDbm(7, 125), -124.5, 0.1);
    CHECK_NEAR(sensitivityDbm(11, 250), -131.5, 0.1);
    CHECK_NEAR(marginSnrDb(6.25f, 11), 23.75, 1e-4);
    CHECK_NEAR(marginRssiDb(-91, 11, 250), 40.5, 0.1);
}

static void testAirtime()
{
    // docs/DESIGN_NOTES.md section 4 table (48-byte ping, 46-byte reply).
    CHECK_NEAR(loraAirtimeMs(48, 7, 500, 5), 26, 1);
    CHECK_NEAR(loraAirtimeMs(48, 7, 250, 5), 53, 1);
    CHECK_NEAR(loraAirtimeMs(48, 9, 250, 5), 170, 1);
    CHECK_NEAR(loraAirtimeMs(48, 11, 250, 5), 600, 1);
    CHECK_NEAR(loraAirtimeMs(46, 11, 250, 5), 600, 1);
    CHECK_NEAR(loraAirtimeMs(48, 11, 125, 8), 1905, 1);
    CHECK_NEAR(loraAirtimeMs(48, 12, 125, 8), 3547, 1);
    // Bigger packets take longer; slower presets take longer.
    CHECK(loraAirtimeMs(200, 11, 250, 5) > loraAirtimeMs(48, 11, 250, 5));
    CHECK(loraAirtimeMs(48, 12, 125, 8) > loraAirtimeMs(48, 11, 250, 5));
}

static void testInterval()
{
    CHECK(pingIntervalMs(1200, 10, 2000) == 12000); // LongFast
    CHECK(pingIntervalMs(103, 10, 2000) == 2000);   // ShortFast hits the minimum
    CHECK(pingIntervalMs(7094, 10, 2000) == 70940); // LongSlow: no upper clamp
    CHECK(pingIntervalMs(1200, 0, 2000) == 12000);  // invalid budget falls back to 10 %
    CHECK(pingIntervalMs(1200, 20, 2000) == 6000);

    CHECK(backoffMultiplier(0) == 1);
    CHECK(backoffMultiplier(2) == 1);
    CHECK(backoffMultiplier(3) == 2);
    CHECK(backoffMultiplier(4) == 4);
    CHECK(backoffMultiplier(5) == 8);
    CHECK(backoffMultiplier(1000) == 8);

    CHECK(applyJitter(10000, 0) == 9000);
    CHECK(applyJitter(10000, 100) == 10000);
    CHECK(applyJitter(10000, 200) == 11000);
    CHECK(applyJitter(10000, 201) == 9000); // only r % 201 matters
}

static Sample mk(uint16_t seq, int16_t rssi, float snr)
{
    Sample s;
    s.seq = seq;
    s.rssi = rssi;
    s.snrQ4 = snrToQ4(snr);
    s.kind = SampleKind::REPLY;
    return s;
}

static void testRing()
{
    Ring<4> r;
    CHECK(r.size() == 0);
    for (uint16_t i = 0; i < 6; i++)
        r.push(mk(i, -90, 0));
    CHECK(r.size() == 4);
    CHECK(r.at(0).seq == 2);
    CHECK(r.at(3).seq == 5);
    CHECK(r.latest().seq == 5);
    r.clear();
    CHECK(r.size() == 0);
}

static void testDirection()
{
    Direction<8> d;
    CHECK(d.empty());
    d.add(mk(1, -90, 5.0f));
    d.add(mk(2, -100, -2.5f));
    CHECK(d.samples == 2);
    CHECK(d.minRssi == -100);
    CHECK(d.maxRssi == -90);
    CHECK(d.minSnrQ4 == -10);
    CHECK(d.maxSnrQ4 == 20);
    CHECK_NEAR(d.rssi.value, -92.5, 1e-4);   // EWMA alpha 0.25
    CHECK_NEAR(d.snrEwma.value, 3.125, 1e-4);
    d.clear();
    CHECK(d.empty());
    CHECK(!d.rssi.has);
    CHECK(d.ring.size() == 0);
}

static void testDeliveryWindow()
{
    DeliveryWindow w;
    CHECK(!w.recordReply(0)); // nothing sent yet
    for (uint16_t s = 0; s < 4; s++)
        w.recordPing(s);
    CHECK(w.recordReply(1));
    CHECK(w.recordReply(3));
    CHECK(!w.recordReply(3)); // duplicate
    CHECK(!w.recordReply(10)); // not sent
    CHECK(w.pings() == 4);
    CHECK(w.replies() == 2);
    CHECK_NEAR(w.ratio(), 0.5, 1e-6);
    CHECK(w.recordReply(0)); // late reply still inside the window
    CHECK_NEAR(w.ratio(), 0.75, 1e-6);

    DeliveryWindow full;
    uint16_t s = 0;
    for (int i = 0; i < 40; i++, s = nextSeq(s)) {
        full.recordPing(s);
        CHECK(full.recordReply(s));
    }
    CHECK(full.pings() == 32);
    CHECK(full.replies() == 32);

    // Across the 65534 -> 0 wrap, a late reply to 65534 is one ping old, not two.
    DeliveryWindow wrap;
    wrap.recordPing(0xFFFD);
    wrap.recordPing(0xFFFE);
    wrap.recordPing(0);
    CHECK(wrap.recordReply(0xFFFE));
    CHECK(wrap.recordReply(0xFFFD));
    CHECK(wrap.replies() == 2);
    CHECK(!wrap.recordReply(0xFFFC)); // older than the window
}

static void testGapCounter()
{
    GapCounter g;
    CHECK(g.onSeq(10) == GapCounter::Result::FIRST);
    CHECK(g.onSeq(11) == GapCounter::Result::IN_ORDER);
    CHECK(g.onSeq(11) == GapCounter::Result::DUPLICATE);
    CHECK(g.onSeq(14) == GapCounter::Result::GAP);
    CHECK(g.missedCount() == 2);
    CHECK(g.receivedCount() == 3);
    CHECK_NEAR(g.lossRatio(), 0.4, 1e-6);
    CHECK(g.onSeq(0) == GapCounter::Result::RESTART); // initiator rebooted
    CHECK(g.restartCount() == 1);
    CHECK(g.missedCount() == 2); // a restart is not loss

    GapCounter w;
    w.onSeq(0xFFFD);
    CHECK(w.onSeq(0xFFFE) == GapCounter::Result::IN_ORDER);
    CHECK(w.onSeq(0) == GapCounter::Result::IN_ORDER); // wrap skips SEQ_NONE: no phantom loss
    CHECK(w.missedCount() == 0);
}

int main()
{
    testPingRoundTrip();
    testReplyRoundTrip();
    testDecodeRejects();
    testScalarHelpers();
    testSeqArithmetic();
    testLinkMath();
    testAirtime();
    testInterval();
    testRing();
    testDirection();
    testDeliveryWindow();
    testGapCounter();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
