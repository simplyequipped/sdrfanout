#include "run.h"
#include "capture.h"
#include "dsp.h"
#include "framer.h"
#include "util.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

static const double STATS_SEC = 60.0;   // how often to report ongoing frame drops

int run_producer(Capture *cap, std::vector<ChannelOut> &chans,
                 int frame_samples, double buffer_sec, volatile bool *stop) {
    const int fs = (int)cap->rate();
    const double center = cap->center();

    std::vector<ChannelChain> chains;
    std::vector<Framer> framers;
    chains.reserve(chans.size());
    framers.reserve(chans.size());
    for (auto &c : chans) {
        chains.emplace_back(c.dial - center, fs);
        framers.emplace_back(c.fd, ChannelChain::OUT_RATE, frame_samples, buffer_sec);
    }

    std::vector<std::complex<float>> iq(1 << 16);
    std::vector<int16_t> audio;
    int rc = 0;

    std::vector<long> last_dropped(chans.size(), 0);  // drop counts at last report
    double last_report = now();

    while (!*stop) {
        double t_recv;
        int n = cap->read(iq.data(), (int)iq.size(), t_recv);
        if (n < 0) { rc = 0; break; }            // end-of-stream (synth) or restartable error
        if (n == 0) {                             // nothing yet, yield briefly
            struct timespec ts{0, 1000000};       // 1 ms
            nanosleep(&ts, 0);
            continue;
        }
        for (size_t i = 0; i < chans.size(); i++) {
            audio.clear();
            chains[i].process(iq.data(), n, audio);
            if (!audio.empty()) framers[i].add(audio.data(), audio.size(), t_recv);
        }

        // Periodic drop report: one stderr line per channel that lost frames
        // since the last window, so a slow consumer is visible while running
        // (not just at shutdown). Silent when nothing dropped.
        double t = now();
        if (t - last_report >= STATS_SEC) {
            for (size_t i = 0; i < chans.size(); i++) {
                long d = framers[i].dropped();
                if (d > last_dropped[i])
                    std::fprintf(stderr, "sdrfanout: ch %.0f dropped %ld frames (+%ld in %.0fs)\n",
                                 chans[i].dial, d, d - last_dropped[i], t - last_report);
                last_dropped[i] = d;
            }
            last_report = t;
        }
    }

    for (size_t i = 0; i < chans.size(); i++) chans[i].dropped = framers[i].dropped();
    return rc;
}
