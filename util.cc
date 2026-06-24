#include "capture.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <sys/time.h>

double now() {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

double resolve_center(const CaptureConfig &c) {
    if (c.want_center > 0) return c.want_center;
    double mn = *std::min_element(c.dials.begin(), c.dials.end());
    return mn - c.guard;   // LO below the lowest channel: all channels at positive offset
}

std::string normalize_driver(const std::string &name) {
    return name.empty() ? name : "driver=" + name;   // "hackrf" -> "driver=hackrf"
}

bool rate_ok(double rate) {
    if (rate <= 0) return false;
    long n = (long)std::lround(rate / 12000.0);
    return n >= 1 && std::fabs(rate - (double)n * 12000.0) < 1.0;
}

double resolve_rate(const CaptureConfig &c, double center, const std::vector<double> &ranges) {
    const int OUT = 12000;
    if (c.want_rate > 0) return c.want_rate;   // explicit override (trust the operator)

    // Need rate/2 to reach the top channel's USB passband, plus LPF transition margin.
    double mx = *std::max_element(c.dials.begin(), c.dials.end());
    double lo = 2.0 * ((mx - center) + 6000.0);

    int n = (int)std::ceil(lo / OUT);
    if (n < 1) n = 1;
    for (; n < 1000000; n++) {
        double r = (double)n * OUT;
        if (ranges.empty()) return r;                    // any rate ok (synth)
        for (size_t i = 0; i + 1 < ranges.size(); i += 2)
            if (r >= ranges[i] - 1 && r <= ranges[i + 1] + 1) return r;
    }
    return 0;   // no integer-x12k rate the device supports spans the channels
}
