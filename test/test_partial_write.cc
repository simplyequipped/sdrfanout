// Frames larger than the pipe's free space are written in pieces. The reader must
// see every byte exactly once, in order, with no drops, framed or raw.
#include "framer.h"
#include "sdrf.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

static void drain_pipe(int fd, std::vector<uint8_t> &out) {
    uint8_t buf[65536];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) out.insert(out.end(), buf, buf + r);
}

static bool run(bool framed) {
    const int outrate = 48000, frame = outrate / 10, nframes = 20;
    const size_t frame_bytes = (size_t)frame * sizeof(int16_t);

    int p[2];
    if (pipe(p) != 0) { perror("pipe"); return false; }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    int pipe_bytes = fcntl(p[1], F_SETPIPE_SZ, 4096);   // smaller than one frame

    std::vector<int16_t> sent((size_t)frame * nframes);
    for (size_t i = 0; i < sent.size(); i++) sent[i] = (int16_t)i;

    std::vector<uint8_t> got;
    {
        Framer fr(p[1], outrate, frame, 10.0, sizeof(int16_t), framed, false);
        for (int b = 0; b < nframes; b++) {
            fr.add((const uint8_t *)&sent[(size_t)b * frame], frame_bytes, 1000000.0 + (b + 1) * 0.1);
            drain_pipe(p[0], got);
        }
        for (int i = 0; i < 1000; i++) {
            fr.add(nullptr, 0, 0);
            drain_pipe(p[0], got);
        }
        if (fr.dropped()) { std::printf("  dropped %ld frames\n", fr.dropped()); return false; }
    }
    close(p[0]);
    close(p[1]);

    std::vector<int16_t> samples;
    size_t at = 0;
    while (at < got.size()) {
        if (framed) {
            sdrf_hdr h;
            if (got.size() - at < sizeof h) break;
            std::memcpy(&h, &got[at], sizeof h);
            if (h.magic != SDRF_MAGIC || h.nsamples != (uint32_t)frame) {
                std::printf("  bad header at byte %zu\n", at);
                return false;
            }
            at += sizeof h;
        }
        size_t take = frame_bytes < got.size() - at ? frame_bytes : got.size() - at;
        size_t old = samples.size();
        samples.resize(old + take / sizeof(int16_t));
        std::memcpy(&samples[old], &got[at], take);
        at += take;
    }

    bool ok = samples == sent;
    std::printf("%s: pipe %d bytes, frame %zu bytes, received %zu of %zu samples, %s\n",
                framed ? "framed" : "raw", pipe_bytes, frame_bytes + (framed ? sizeof(sdrf_hdr) : 0),
                samples.size(), sent.size(), ok ? "in order" : "MISMATCH");
    return ok;
}

int main() {
    bool ok = run(true);
    ok = run(false) && ok;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
