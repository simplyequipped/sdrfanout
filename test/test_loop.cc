// Full producer loop: SynthCapture (2 channels) -> run_producer -> two output
// files, then validate each file's frames + audio tone.
#include "synth.h"
#include "run.h"
#include "sdrf.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

static const double PI = 3.14159265358979323846;

static double tone_amp(const std::vector<int16_t> &out, double f_hz, int fs = 12000) {
    double re = 0, im = 0;
    for (size_t n = 0; n < out.size(); n++) {
        double a = out[n] / 32768.0;
        re += a * std::cos(2 * PI * f_hz * n / fs);
        im += a * std::sin(2 * PI * f_hz * n / fs);
    }
    return out.empty() ? 0 : 2.0 * std::sqrt(re * re + im * im) / out.size();
}

// read+validate a sdrf file, return audio, set ok / frame count
static std::vector<int16_t> consume(int fd, bool &ok, int &frames) {
    std::vector<int16_t> all;
    frames = 0; ok = true;
    lseek(fd, 0, SEEK_SET);
    sdrf_hdr h;
    while (read(fd, &h, sizeof h) == (ssize_t)sizeof h) {
        if (h.magic != SDRF_MAGIC || h.version != SDRF_VERSION || h.rate_hz != 12000) { ok = false; break; }
        std::vector<int16_t> pcm(h.nsamples);
        if (read(fd, pcm.data(), h.nsamples * sizeof(int16_t)) != (ssize_t)(h.nsamples * sizeof(int16_t))) { ok = false; break; }
        all.insert(all.end(), pcm.begin(), pcm.end());
        frames++;
    }
    return all;
}

int main() {
    // 40m: WSPR 7.0386 MHz + FT8 7.074 MHz, LO 10 kHz below the lower one.
    std::vector<double> dials = {7038600.0, 7074000.0};
    CaptureConfig cfg;
    cfg.driver = "synth";
    cfg.dials = dials;
    cfg.guard = 10000;
    double center = resolve_center(cfg);
    double rate = resolve_rate(cfg, center, {});

    // finite, unpaced synth: ~0.5 s of IQ
    long total = (long)(rate * 0.5);
    SynthCapture cap(rate, center, dials, total, false);

    char p0[] = "/tmp/sdrfanout_ch0.XXXXXX", p1[] = "/tmp/sdrfanout_ch1.XXXXXX";
    std::vector<ChannelOut> chans = {
        {dials[0], mkstemp(p0), 0},
        {dials[1], mkstemp(p1), 0},
    };
    if (chans[0].fd < 0 || chans[1].fd < 0) { perror("mkstemp"); return 2; }

    volatile bool stop = false;
    int rc = run_producer(&cap, chans, 1200, 1.0, &stop);

    bool ok0, ok1; int f0, f1;
    auto a0 = consume(chans[0].fd, ok0, f0);
    auto a1 = consume(chans[1].fd, ok1, f1);

    double t0 = tone_amp(a0, 1500.0), t1 = tone_amp(a1, 1500.0);

    std::printf("rate=%.0f center=%.0f  (D=%d)\n", rate, center, (int)(rate / 12000));
    std::printf("ch0: frames=%d samples=%zu tone@1500=%.3f dropped=%ld fmt=%s\n",
                f0, a0.size(), t0, chans[0].dropped, ok0 ? "ok" : "BAD");
    std::printf("ch1: frames=%d samples=%zu tone@1500=%.3f dropped=%ld fmt=%s\n",
                f1, a1.size(), t1, chans[1].dropped, ok1 ? "ok" : "BAD");

    close(chans[0].fd); close(chans[1].fd); unlink(p0); unlink(p1);

    bool ok = rc == 0 && ok0 && ok1 && f0 >= 4 && f1 >= 4 &&
              t0 > 0.2 && t1 > 0.2 && chans[0].dropped == 0 && chans[1].dropped == 0;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
