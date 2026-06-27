// Fanning the channels across worker threads must not change a single output
// sample: the per-channel DSP is independent, so threads=N has to be byte-for-byte
// identical to threads=1. We compare the decoded audio payload (the sdrf header's
// t_usec is wall-clock, so it legitimately varies run to run and is excluded).
#include "synth.h"
#include "run.h"
#include "capture.h"
#include "sdrf.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

// read a sdrf stream, return its concatenated int16 audio (headers dropped)
static std::vector<int16_t> audio_of(int fd) {
    std::vector<int16_t> all;
    lseek(fd, 0, SEEK_SET);
    sdrf_hdr h;
    while (read(fd, &h, sizeof h) == (ssize_t)sizeof h) {
        if (h.magic != SDRF_MAGIC || h.version != SDRF_VERSION) break;
        std::vector<int16_t> pcm(h.nsamples);
        size_t want = h.nsamples * sizeof(int16_t);
        if (read(fd, pcm.data(), want) != (ssize_t)want) break;
        all.insert(all.end(), pcm.begin(), pcm.end());
    }
    return all;
}

// run the finite synth through run_producer at a given thread count, return each
// channel's audio
static std::vector<std::vector<int16_t>>
run_with(int nthreads, const std::vector<double> &dials, double center,
         double rate, long total) {
    SynthCapture cap(rate, center, dials, total, false);
    std::vector<ChannelOut> chans;
    std::vector<std::string> paths;
    for (size_t i = 0; i < dials.size(); i++) {
        std::string tmpl = "/tmp/sdrfanout_thr.XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = mkstemp(buf.data());
        chans.push_back({dials[i], fd, 0});
        paths.emplace_back(buf.data());
    }

    volatile bool stop = false;
    run_producer(&cap, chans, 1200, 1.0, &stop, nthreads);

    std::vector<std::vector<int16_t>> out;
    for (size_t i = 0; i < chans.size(); i++) {
        out.push_back(audio_of(chans[i].fd));
        close(chans[i].fd);
        unlink(paths[i].c_str());
    }
    return out;
}

int main() {
    // 80/60/40/20m, FT8 + WSPR each: 8 channels, enough to span several workers.
    std::vector<double> dials = {
        3568600, 3573000, 5364700, 5357000, 7038600, 7074000, 14095600, 14074000,
    };
    CaptureConfig cfg;
    cfg.driver = "synth";
    cfg.dials = dials;
    cfg.guard = 10000;
    double center = resolve_center(cfg);
    double rate = resolve_rate(cfg, center, {});
    long total = (long)(rate * 0.3);

    auto a1 = run_with(1, dials, center, rate, total);
    auto a4 = run_with(4, dials, center, rate, total);   // 2 channels per worker
    auto a3 = run_with(3, dials, center, rate, total);   // uneven split (2/3/3)

    int fails = 0;
    size_t total_samples = 0;
    for (size_t i = 0; i < dials.size(); i++) {
        total_samples += a1[i].size();
        if (a1[i].empty()) { std::printf("FAIL ch%zu: no output\n", i); fails++; }
        if (a1[i] != a4[i]) { std::printf("FAIL ch%zu: threads=4 differs from threads=1\n", i); fails++; }
        if (a1[i] != a3[i]) { std::printf("FAIL ch%zu: threads=3 differs from threads=1\n", i); fails++; }
    }

    std::printf("8 channels, %zu audio samples/run, threads 1 vs 4 vs 3\n", total_samples);
    std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
