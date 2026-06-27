#include "run.h"
#include "capture.h"
#include "dsp.h"
#include "framer.h"
#include "util.h"
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

static const double STATS_SEC = 60.0;   // how often to report ongoing frame drops

// One channel range -> its framers. The DSP is per-channel and shares no mutable
// state, so a buffer's channels split cleanly across workers. `audio` is reused
// scratch (one per caller) to avoid reallocating each buffer.
static void process_range(std::vector<ChannelChain> &chains, std::vector<Framer> &framers,
                          const std::complex<float> *iq, int n, double t_recv,
                          size_t lo, size_t hi, std::vector<int16_t> &audio) {
    for (size_t i = lo; i < hi; i++) {
        audio.clear();
        chains[i].process(iq, (size_t)n, audio);
        if (!audio.empty()) framers[i].add(audio.data(), audio.size(), t_recv);
    }
}

// Fork-join worker pool: the read thread publishes each buffer and releases the
// workers (bump `gen`), each worker processes its fixed channel range, then the
// read thread waits for all of them (`pending` back to 0) before the next read.
// The published iq buffer is read-only for a worker's whole pass, so the only
// shared state is this handoff, guarded by one mutex.
namespace {
struct Pool {
    std::mutex m;
    std::condition_variable cv_work, cv_done;
    uint64_t gen = 0;                       // bumped to release a buffer to the workers
    int pending = 0;                        // workers still on the current buffer
    bool quit = false;
    const std::complex<float> *iq = nullptr;
    int n = 0;
    double t_recv = 0;
};
}

int run_producer(Capture *cap, std::vector<ChannelOut> &chans,
                 int frame_samples, double buffer_sec, volatile bool *stop,
                 int threads) {
    const int fs = (int)cap->rate();
    const double center = cap->center();
    const size_t nch = chans.size();

    std::vector<ChannelChain> chains;
    std::vector<Framer> framers;
    chains.reserve(nch);
    framers.reserve(nch);
    for (auto &c : chans) {
        chains.emplace_back(c.dial - center, fs);
        framers.emplace_back(c.fd, ChannelChain::OUT_RATE, frame_samples, buffer_sec);
    }

    int nthreads = threads;
    if (nthreads <= 0) {                             // auto: balance channels over cores
        unsigned hw = std::thread::hardware_concurrency();
        nthreads = auto_threads((int)nch, (hw == 0) ? 1 : (int)hw);
    }
    if (nthreads > (int)nch) nthreads = (int)nch;   // no empty workers (explicit -threads)
    if (nthreads < 1) nthreads = 1;
    std::fprintf(stderr, "sdrfanout: %d channel(s), %d worker thread(s)\n", (int)nch, nthreads);

    std::vector<std::complex<float>> iq(1 << 16);
    int rc = 0;
    std::vector<long> last_dropped(nch, 0);          // drop counts at last report
    double last_report = now();

    // Worker pool (only when fanning out). Each worker owns a contiguous channel
    // range and its own audio scratch.
    Pool pool;
    std::vector<std::thread> workers;
    if (nthreads > 1) {
        for (int t = 0; t < nthreads; t++) {
            size_t lo = (size_t)t * nch / nthreads;
            size_t hi = (size_t)(t + 1) * nch / nthreads;
            workers.emplace_back([&, lo, hi] {
                std::vector<int16_t> audio;
                uint64_t seen = 0;
                for (;;) {
                    std::unique_lock<std::mutex> lk(pool.m);
                    pool.cv_work.wait(lk, [&] { return pool.gen != seen || pool.quit; });
                    if (pool.quit) return;
                    seen = pool.gen;
                    const std::complex<float> *biq = pool.iq;
                    int bn = pool.n;
                    double bt = pool.t_recv;
                    lk.unlock();
                    process_range(chains, framers, biq, bn, bt, lo, hi, audio);
                    lk.lock();
                    if (--pool.pending == 0) pool.cv_done.notify_one();
                }
            });
        }
    }

    std::vector<int16_t> audio;   // single-thread scratch
    while (!*stop) {
        double t_recv;
        int n = cap->read(iq.data(), (int)iq.size(), t_recv);
        if (n < 0) { rc = 0; break; }            // end-of-stream (synth) or restartable error
        if (n == 0) {                             // nothing yet, yield briefly
            struct timespec ts{0, 1000000};       // 1 ms
            nanosleep(&ts, 0);
            continue;
        }

        if (nthreads > 1) {
            std::unique_lock<std::mutex> lk(pool.m);
            pool.iq = iq.data(); pool.n = n; pool.t_recv = t_recv;
            pool.pending = nthreads;
            pool.gen++;
            pool.cv_work.notify_all();
            pool.cv_done.wait(lk, [&] { return pool.pending == 0; });
        } else {
            process_range(chains, framers, iq.data(), n, t_recv, 0, nch, audio);
        }

        // Periodic drop report: one stderr line per channel that lost frames
        // since the last window, so a slow consumer is visible while running
        // (not just at shutdown). Silent when nothing dropped.
        double t = now();
        if (t - last_report >= STATS_SEC) {
            for (size_t i = 0; i < nch; i++) {
                long d = framers[i].dropped();
                if (d > last_dropped[i])
                    std::fprintf(stderr, "sdrfanout: ch %.0f dropped %ld frames (+%ld in %.0fs)\n",
                                 chans[i].dial, d, d - last_dropped[i], t - last_report);
                last_dropped[i] = d;
            }
            last_report = t;
        }
    }

    if (nthreads > 1) {
        { std::unique_lock<std::mutex> lk(pool.m); pool.quit = true; pool.cv_work.notify_all(); }
        for (auto &w : workers) w.join();
    }

    for (size_t i = 0; i < nch; i++) chans[i].dropped = framers[i].dropped();
    return rc;
}
