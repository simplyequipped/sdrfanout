// End-to-end: synthetic IQ -> ChannelChain -> Framer -> file, then read the file
// back as a consumer and validate the wire format, the audio tone, and the
// per-frame timestamps.
#include "dsp.h"
#include "framer.h"
#include "sdrf.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <complex>
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
    return 2.0 * std::sqrt(re * re + im * im) / out.size();
}

int main() {
    const int fs = 96000;
    const double delta = 9000.0, t0 = 1000000.0;   // synthetic wall-clock origin (s)
    const int frame = 1200;                          // 0.1 s @ 12 kHz

    char path[] = "/tmp/sdrfanout_test.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }

    // produce: 10 blocks of 0.1 s IQ (USB tone at +1500 Hz), each -> one frame
    ChannelChain ch(delta, fs);
    Framer fr(fd, 12000, frame, 1.0);
    int blk = fs / 10;
    for (int b = 0; b < 10; b++) {
        std::vector<std::complex<float>> iq(blk);
        for (int n = 0; n < blk; n++) {
            long idx = (long)b * blk + n;
            double ph = 2 * PI * (delta + 1500.0) * idx / fs;
            iq[n] = std::complex<float>(0.5f * std::cos(ph), 0.5f * std::sin(ph));
        }
        std::vector<int16_t> audio;
        ch.process(iq.data(), iq.size(), audio);
        double t_newest = t0 + (b + 1) * 0.1;        // newest sample of this block
        fr.add(audio.data(), audio.size(), t_newest);
    }
    lseek(fd, 0, SEEK_SET);

    // consume: parse frames, validate, collect audio + stamps
    std::vector<int16_t> all;
    std::vector<double> stamps;
    int frames = 0;
    bool fmt_ok = true;
    sdrf_hdr h;
    while (read(fd, &h, sizeof h) == (ssize_t)sizeof h) {
        if (h.magic != SDRF_MAGIC || h.version != SDRF_VERSION ||
            h.rate_hz != 12000 || h.nsamples != (uint32_t)frame) { fmt_ok = false; break; }
        std::vector<int16_t> pcm(h.nsamples);
        if (read(fd, pcm.data(), h.nsamples * sizeof(int16_t)) !=
            (ssize_t)(h.nsamples * sizeof(int16_t))) { fmt_ok = false; break; }
        all.insert(all.end(), pcm.begin(), pcm.end());
        stamps.push_back(h.t_usec / 1e6);
        frames++;
    }
    close(fd);
    unlink(path);

    double amp = tone_amp(all, 1500.0);

    // stamps: monotonic, spaced ~0.1 s, first ~ t0
    bool ts_ok = (frames == 10);
    double max_dt_err = 0, first_err = std::fabs(stamps.empty() ? 1e9 : stamps[0] - t0);
    for (int i = 1; i < (int)stamps.size(); i++) {
        double dt = stamps[i] - stamps[i - 1];
        if (dt <= 0) ts_ok = false;
        max_dt_err = std::fmax(max_dt_err, std::fabs(dt - 0.1));
    }

    std::printf("frames: %d  audio samples: %zu  dropped: %ld\n", frames, all.size(), fr.dropped());
    std::printf("format valid: %s\n", fmt_ok ? "yes" : "NO");
    std::printf("USB tone |A| @1500Hz: %.3f\n", amp);
    std::printf("first stamp err vs t0: %.6f s   max frame-spacing err: %.6f s\n", first_err, max_dt_err);

    bool ok = fmt_ok && ts_ok && frames == 10 && fr.dropped() == 0 &&
              amp > 0.3 && first_err < 0.005 && max_dt_err < 1e-4;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
