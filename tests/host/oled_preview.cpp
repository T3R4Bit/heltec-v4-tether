// Renders every OLED layout for a set of SYNTHETIC link scenarios into PBM files, using exactly the drawing code the
// firmware uses. Driven by scripts/render_oled_previews.py, which turns the PBMs into PNGs.
// Usage: oled_preview <out_dir>
#include "TetherCanvas.h"
#include "TetherScreen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace tether;

static uint32_t rngState = 12345;
static float noise() // deterministic, roughly uniform in [-1, 1]
{
    rngState = rngState * 1664525u + 1013904223u;
    return (float)((rngState >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

static ScreenModel base()
{
    ScreenModel m;
    m.havePeer = true;
    strcpy(m.peerShort, "3C4D");
    m.initiator = true;
    m.snrFloor = -17.5f;
    return m;
}

static void fill(ScreenModel &m, float start, float end, float jitter, int n, int rssiStart, int rssiEnd)
{
    m.nLocal = m.nRemote = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        float f = n > 1 ? (float)i / (float)(n - 1) : 1.0f;
        float snr = start + (end - start) * f + jitter * noise();
        float rs = snr - 2.0f + jitter * noise();
        if (snr > 12)
            snr = 12;
        if (rs > 12)
            rs = 12;
        m.localSnrQ4[i] = (int8_t)lroundf(snr * 4);
        m.remoteSnrQ4[i] = (int8_t)lroundf(rs * 4);
    }
    m.haveLocal = m.haveRemote = n > 0;
    m.localSnr = m.localSnrQ4[n - 1] / 4.0f;
    m.remoteSnr = m.remoteSnrQ4[n - 1] / 4.0f;
    m.localRssi = (int16_t)rssiEnd;
    m.remoteRssi = (int16_t)(rssiEnd - 3);
    (void)rssiStart;
}

static bool writePbm(const Canvas &c, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "P1\n%d %d\n", Canvas::W, Canvas::H);
    for (int y = 0; y < Canvas::H; y++) {
        for (int x = 0; x < Canvas::W; x++)
            fputc(c.get(x, y) ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: oled_preview <out_dir>\n");
        return 2;
    }
    struct Scenario {
        const char *name;
        ScreenModel m;
    } scenarios[5];

    scenarios[0].name = "good";
    scenarios[0].m = base();
    fill(scenarios[0].m, 9, 7, 1.5f, GRAPH_MAX, -78, -84);
    scenarios[0].m.pdrPct = 100;
    scenarios[0].m.ageS = 4;

    scenarios[1].name = "fading";
    scenarios[1].m = base();
    fill(scenarios[1].m, 10, -13, 2.5f, GRAPH_MAX, -95, -121);
    scenarios[1].m.pdrPct = 72;
    scenarios[1].m.ageS = 9;

    scenarios[2].name = "lost";
    scenarios[2].m = base();
    fill(scenarios[2].m, -6, -16, 2.0f, 40, -118, -126);
    scenarios[2].m.pdrPct = 9;
    scenarios[2].m.ageS = 240;

    scenarios[3].name = "nopeer";
    scenarios[3].m = ScreenModel();

    scenarios[4].name = "waiting";
    scenarios[4].m = base();

    const Layout layouts[] = {Layout::STRIP, Layout::SPLIT, Layout::BIG};
    const char *layoutNames[] = {"strip", "split", "big"};
    Canvas c;
    int written = 0;
    for (int li = 0; li < 3; li++) {
        for (auto &s : scenarios) {
            renderScreen(c, s.m, layouts[li]);
            char path[512];
            snprintf(path, sizeof(path), "%s/%d-%s-%s.pbm", argv[1], (int)layouts[li], layoutNames[li], s.name);
            if (!writePbm(c, path)) {
                fprintf(stderr, "cannot write %s\n", path);
                return 1;
            }
            written++;
        }
    }
    printf("oled_preview: wrote %d frames\n", written);
    return 0;
}
