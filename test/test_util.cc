// Unit tests for util helpers.
#include "capture.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void eq(const std::string &got, const std::string &want, const char *what) {
    if (got != want) {
        std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str());
        fails++;
    }
}

static void eqd(double got, double want, const char *what) {
    if (std::abs(got - want) > 0.5) {
        std::printf("FAIL %s: got %.1f want %.1f\n", what, got, want);
        fails++;
    }
}

static CaptureConfig cfg_of(std::vector<double> dials, CenterMode mode = CenterMode::Auto) {
    CaptureConfig c;
    c.dials = std::move(dials);
    c.guard = 10000;
    c.center_mode = mode;
    return c;
}

int main() {
    // bare device name -> driver=<name>
    eq(normalize_driver("hackrf"), "driver=hackrf", "bare name");
    eq(normalize_driver("rtlsdr"), "driver=rtlsdr", "bare name 2");
    // empty -> empty (first device)
    eq(normalize_driver(""), "", "empty");

    // --- center placement -------------------------------------------------
    // Auto centers two channels at their midpoint (gap wide enough for the LO).
    {
        CaptureConfig c = cfg_of({7038600, 7074000});
        double center = resolve_center(c);
        eqd(center, 7056300, "auto two-channel midpoint");
        eqd(max_offset(c, center), 17700, "auto two-channel offset");
        eqd(resolve_rate(c, center, {}), 60000, "auto two-channel rate");
    }
    // Edge mode keeps the old one-sided LO just below the lowest channel.
    {
        CaptureConfig c = cfg_of({7038600, 7074000}, CenterMode::Edge);
        double center = resolve_center(c);
        eqd(center, 7028600, "edge center");
        eqd(resolve_rate(c, center, {}), 120000, "edge rate");
    }
    // A single channel has no gap, so auto falls back to one-sided.
    {
        CaptureConfig c = cfg_of({14074000});
        double center = resolve_center(c);
        eqd(center, 14064000, "single-channel center");
        eqd(resolve_rate(c, center, {}), 48000, "single-channel rate");
    }
    // Channels closer than 2*guard leave no room for the LO between them: one-sided.
    {
        CaptureConfig c = cfg_of({7074000, 7075000});
        eqd(resolve_center(c), 7064000, "tight cluster one-sided");
    }
    // Three channels: skip the narrow gap, center in the wide one (clamped to mid).
    {
        CaptureConfig c = cfg_of({7000000, 7005000, 7100000});
        eqd(resolve_center(c), 7050000, "three-channel wide gap");
    }
    // Fixed mode returns the explicit frequency verbatim.
    {
        CaptureConfig c = cfg_of({14074000}, CenterMode::Fixed);
        c.want_center = 14000000;
        eqd(resolve_center(c), 14000000, "fixed center");
    }

    // --- channel validation ----------------------------------------------
    {
        CaptureConfig c = cfg_of({7038600, 7074000});
        double center = resolve_center(c);
        // auto rate has room: no aliasing error
        eq(check_channels(c, center, resolve_rate(c, center, {})), "", "auto fits");
        // a too-small explicit rate aliases the outer channel
        bool aliased = !check_channels(c, center, 24000).empty();
        if (!aliased) { std::printf("FAIL too-small rate not caught\n"); fails++; }
    }

    std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
