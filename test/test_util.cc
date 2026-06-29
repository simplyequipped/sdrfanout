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

    // --- analog bandwidth selection --------------------------------------
    {
        // RSP-style discrete IF filters: pick the smallest that covers the span.
        std::vector<double> rsp = {200e3, 300e3, 600e3, 1.536e6, 5e6, 6e6, 7e6, 8e6};
        eqd(resolve_bandwidth(3.525e6, rsp, {}), 5e6, "discrete smallest covering");
        eqd(resolve_bandwidth(1.5e6, rsp, {}), 1.536e6, "discrete exact-ish");
        eqd(resolve_bandwidth(9e6, rsp, {}), 8e6, "discrete none covers -> widest");
        // Continuous range (e.g. a HackRF baseband filter): clamp the request in.
        std::vector<double> cont = {1.75e6, 28e6};
        eqd(resolve_bandwidth(3.525e6, {}, cont), 3.525e6, "continuous within range");
        eqd(resolve_bandwidth(1e6, {}, cont), 1.75e6, "continuous below min -> min");
        eqd(resolve_bandwidth(30e6, {}, cont), 28e6, "continuous above max -> widest");
        // No bandwidth control reported: 0 tells the caller to leave it alone.
        eqd(resolve_bandwidth(3e6, {}, {}), 0, "no bandwidth control -> 0");
    }

    // --- auto thread count -----------------------------------------------
    // pick the fewest workers that still minimize the busiest worker's load
    eqd(auto_threads(6, 4), 3, "auto_threads 6ch/4core");    // 4 would tie up a core for nothing
    eqd(auto_threads(8, 4), 4, "auto_threads 8ch/4core");    // even split, use all cores
    eqd(auto_threads(5, 4), 3, "auto_threads 5ch/4core");
    eqd(auto_threads(9, 4), 3, "auto_threads 9ch/4core");    // 3x3 beats 3,2,2,2
    eqd(auto_threads(2, 4), 2, "auto_threads 2ch/4core");    // never more workers than channels
    eqd(auto_threads(100, 4), 4, "auto_threads 100ch/4core");// channels >> cores: use all cores
    eqd(auto_threads(6, 12), 6, "auto_threads 6ch/12core");  // cores to spare: one per channel
    eqd(auto_threads(1, 4), 1, "auto_threads 1ch");

    std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
