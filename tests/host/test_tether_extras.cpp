// Host unit tests for TETHER's OLED canvas/screen, GPS gate and battery tracking.
// Build and run: python scripts/run_host_tests.py
#include "TetherCanvas.h"
#include "TetherGeo.h"
#include "TetherPower.h"
#include "TetherScreen.h"

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

static int litPixels(const Canvas &c, int x0 = 0, int y0 = 0, int x1 = Canvas::W, int y1 = Canvas::H)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            n += c.get(x, y) ? 1 : 0;
    return n;
}

static void testCanvasBasics()
{
    Canvas c;
    CHECK(litPixels(c) == 0);
    c.set(0, 0);
    c.set(9, 0);
    c.set(127, 63);
    CHECK(c.buf[0] == 0x01);       // XBM: LSB is the leftmost pixel
    CHECK(c.buf[1] == 0x02);       // x = 9 -> byte 1, bit 1
    CHECK(c.buf[sizeof(c.buf) - 1] == 0x80);
    c.set(-1, 5);                  // out of range: ignored
    c.set(128, 5);
    c.set(5, 64);
    CHECK(litPixels(c) == 3);
    c.set(0, 0, false);
    CHECK(!c.get(0, 0));
    c.clear();
    c.fillRect(10, 10, 4, 3);
    CHECK(litPixels(c) == 12);
    c.clear();
    c.rect(0, 0, 10, 10, 2);
    CHECK(litPixels(c) == 100 - 36);
}

static void testText()
{
    CHECK(Canvas::textWidth("") == 0);
    CHECK(Canvas::textWidth("0") == 5);
    CHECK(Canvas::textWidth("00") == 11);  // 5 + gap + 5
    CHECK(Canvas::textWidth("00", 2) == 22);
    CHECK(Canvas::textWidth("a") == Canvas::textWidth("A")); // lowercase drawn as uppercase
    CHECK(Canvas::textWidth("~") == Canvas::textWidth("?")); // unknown falls back to '?'
    CHECK(Canvas::textWidth(".") == 2);                     // narrow punctuation

    Canvas c;
    int end = c.text(0, 0, "8");
    CHECK(end == 6);
    // '8' is a closed box with a middle bar: 5 + 5 + 5 across, 2 x 5 down the sides.
    CHECK(litPixels(c) == 5 * 3 + 2 * 4);
    CHECK(c.get(0, 0) && c.get(4, 0) && c.get(2, 3) && !c.get(2, 1));
    c.clear();
    c.text(0, 0, "1", 3);
    CHECK(litPixels(c) == 9 * (1 + 2 + 1 + 1 + 1 + 1 + 5)); // every font pixel becomes a 3x3 block
}

static ScreenModel modelWithSamples(int n, float snr)
{
    ScreenModel m;
    m.havePeer = true;
    strcpy(m.peerShort, "3C4D");
    m.haveLocal = true;
    m.localSnr = snr;
    m.localRssi = -90;
    m.nLocal = (uint8_t)n;
    for (int i = 0; i < n; i++)
        m.localSnrQ4[i] = (int8_t)(snr * 4);
    m.pdrPct = 90;
    m.ageS = 5;
    return m;
}

static void testScreens()
{
    Canvas c;
    const Layout all[] = {Layout::STRIP, Layout::SPLIT, Layout::BIG};
    for (Layout l : all) {
        ScreenModel none;
        renderScreen(c, none, l);
        CHECK(litPixels(c) > 100); // "NO PEER" message drawn

        ScreenModel weak = modelWithSamples(GRAPH_MAX, -15.0f);
        ScreenModel strong = modelWithSamples(GRAPH_MAX, 10.0f);
        renderScreen(c, weak, l);
        const int weakPx = litPixels(c);
        renderScreen(c, strong, l);
        const int strongPx = litPixels(c);
        CHECK(strongPx > weakPx); // taller bars for a stronger signal

        ScreenModel lost = modelWithSamples(10, -5.0f);
        lost.ageS = 600;
        renderScreen(c, lost, l);
        const int lostPx = litPixels(c);
        lost.ageS = 5;
        renderScreen(c, lost, l);
        CHECK(lostPx != litPixels(c)); // LOST badge changes the picture
    }

    // renderScreen clears first: drawing twice gives the same frame.
    ScreenModel m = modelWithSamples(20, 3.0f);
    renderScreen(c, m, Layout::STRIP);
    Canvas d;
    d.fillRect(0, 0, 128, 64);
    renderScreen(d, m, Layout::STRIP);
    CHECK(memcmp(c.buf, d.buf, sizeof(c.buf)) == 0);

    // Graph bars sit inside the STRIP graph box (rows 34..61) and the newest sample is at the right edge.
    ScreenModel one = modelWithSamples(1, 12.0f);
    one.pdrPct = -1;
    renderScreen(c, one, Layout::STRIP);
    CHECK(c.get(124, 40));
    CHECK(!c.get(60, 40));
}

static void testFormatting()
{
    char b[16];
    screen_detail::fmtSnr(b, sizeof(b), 6.25f);
    CHECK(strcmp(b, "+6.2") == 0 || strcmp(b, "+6.3") == 0);
    screen_detail::fmtSnr(b, sizeof(b), -12.4f);
    CHECK(strcmp(b, "-12") == 0);
    screen_detail::fmtSnr(b, sizeof(b), 10.0f);
    CHECK(strcmp(b, "+10") == 0);
    screen_detail::fmtAge(b, sizeof(b), 42);
    CHECK(strcmp(b, "42S") == 0);
    screen_detail::fmtAge(b, sizeof(b), 240);
    CHECK(strcmp(b, "4M") == 0);
    screen_detail::fmtAge(b, sizeof(b), 7200);
    CHECK(strcmp(b, "2H") == 0);
    screen_detail::fmtAge(b, sizeof(b), 0xFFFFFFFFu);
    CHECK(strcmp(b, "--") == 0);
    CHECK(screen_detail::snrToY(GRAPH_SNR_MAX, 10, 20) == 10);
    CHECK(screen_detail::snrToY(GRAPH_SNR_MIN, 10, 20) == 29);
    CHECK(screen_detail::snrToY(99.0f, 10, 20) == 10); // clamped
}

static void testGps()
{
    CHECK(gpsQuality(false, 12, 80) == GpsQuality::NO_FIX);
    CHECK(gpsQuality(true, 5, 80) == GpsQuality::FEW_SATS);
    CHECK(gpsQuality(true, 6, 201) == GpsQuality::HIGH_HDOP);
    CHECK(gpsQuality(true, 6, 200) == GpsQuality::OK);
    CHECK(gpsQuality(true, 9, 0) == GpsQuality::OK); // HDOP not reported
    CHECK(strcmp(gpsQualityName(GpsQuality::HIGH_HDOP), "hdop") == 0);

    CHECK_NEAR(approxDistanceM(39.7, -105.0, 39.701, -105.0), 111.2, 0.5);

    PositionSmoother s;
    CHECK(!s.valid());
    CHECK(s.update(39.7000, -105.0, 1000));
    CHECK(!s.update(39.7010, -105.0, 1000)); // same fix again: ignored
    CHECK_NEAR(s.lat(), 39.7000, 1e-9);
    CHECK(s.update(39.7001, -105.0, 2000));   // ~11 m step: smoothed
    CHECK_NEAR(s.lat(), 39.7000 + 0.35 * 0.0001, 1e-9);
    CHECK(s.update(39.7100, -105.0, 3000));   // ~1 km jump: restart there
    CHECK_NEAR(s.lat(), 39.7100, 1e-9);
    CHECK(s.resetCount() == 1);
}

static void testPower()
{
    CHECK(lowBatteryMultiplier(false, false, 5) == 1); // no battery (USB powered)
    CHECK(lowBatteryMultiplier(true, true, 5) == 1);   // charging
    CHECK(lowBatteryMultiplier(true, false, 50) == 1);
    CHECK(lowBatteryMultiplier(true, false, 19) == 2);
    CHECK(lowBatteryMultiplier(true, false, 9) == 4);
    CHECK(lowBatteryMultiplier(true, false, -1) == 1);

    BatteryTracker b;
    float v;
    CHECK(!b.runtimeHours(v));
    // Discharging 10 % per hour, one reading a minute for 40 minutes.
    for (uint32_t t = 0; t <= 2400; t += 60)
        b.add(t, 4000 - (int)(t / 36), (int)(80 - t / 360.0f + 0.5f), false, false);
    CHECK(b.haveVoltage());
    CHECK(b.slopePctPerHour(v));
    CHECK_NEAR(v, -10.0, 1.5);
    CHECK(b.runtimeHours(v));
    CHECK_NEAR(v, b.percent() / 10.0, 1.2);

    // Starting to charge throws away the discharge trend.
    b.add(2460, 4100, 74, true, true);
    CHECK(!b.runtimeHours(v));
    CHECK(!b.slopePctPerHour(v));
    CHECK(b.charging() && b.usb());

    // Too little history for a trend.
    BatteryTracker shortRun;
    for (uint32_t t = 0; t <= 600; t += 60)
        shortRun.add(t, 4000, 80 - (int)(t / 120), false, false);
    CHECK(!shortRun.runtimeHours(v));
}

int main()
{
    testCanvasBasics();
    testText();
    testScreens();
    testFormatting();
    testGps();
    testPower();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
