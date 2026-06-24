// sdrfanout: fan one SDR out to several offset USB channels, each a stamped
// 12 kHz sdrf stream on its own FIFO. See README for the stream format.
#include "capture.h"
#include "run.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define SDRFANOUT_TMPDIR "/tmp/sdrfanout"   // default home for auto-named channel FIFOs

static volatile bool g_stop = false;
static void on_signal(int) { g_stop = true; }

static void usage() {
    std::fprintf(stderr,
        "usage: sdrfanout -ch <dial_hz>:<path> [-ch ...] [options]\n"
        "  -driver <dev>     SoapySDR device name (ex. hackrf) or 'synth'\n"
        "  -gain <db|auto>   default: auto (AGC / device default)\n"
        "  -rate <hz|auto>   default: auto (smallest integer x 12k spanning the channels)\n"
        "  -center <hz|auto> default: auto (lowest dial minus guard)\n"
        "  -guard <hz>       guard band below lowest channel, default: 10000\n"
        "  -ppm <n>          frequency correction, default: 0\n"
        "  -antenna <name>   Soapy antenna, default: device default\n"
        "  -buffer <sec>     per-channel output buffer in seconds, default: 1\n"
        "  -ch <dial>[:<path>] channel: dial freq (Hz) + output FIFO, path defaults to\n"
        "                    " SDRFANOUT_TMPDIR "/<dial>.fifo, set path to '-' for stdout\n");
    std::exit(2);
}

int main(int argc, char **argv) {
    CaptureConfig cfg;
    std::vector<std::pair<double, std::string>> chspec;
    const int frame = 1200;  // 0.1 s @ 12 kHz
    double buffer_sec = 1.0; // per-channel output backlog cap

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&](const char *name) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); usage(); }
            return argv[++i];
        };
        if (a == "-driver") cfg.driver = val("-driver");
        else if (a == "-gain") { std::string v = val("-gain"); cfg.gain = (v == "auto") ? -1 : atof(v.c_str()); }
        else if (a == "-rate") { std::string v = val("-rate"); cfg.want_rate = (v == "auto") ? 0 : atof(v.c_str()); }
        else if (a == "-center") { std::string v = val("-center"); cfg.want_center = (v == "auto") ? 0 : atof(v.c_str()); }
        else if (a == "-guard") cfg.guard = atof(val("-guard").c_str());
        else if (a == "-ppm") cfg.ppm = atof(val("-ppm").c_str());
        else if (a == "-antenna") cfg.antenna = val("-antenna");
        else if (a == "-buffer") buffer_sec = atof(val("-buffer").c_str());
        else if (a == "-ch") {
            std::string v = val("-ch");
            auto colon = v.rfind(':');
            // "<dial>" (no colon) means empty path, resolved to the default FIFO below.
            // "<dial>:<path>" is an explicit path ('-' = stdout).
            double dial = atof((colon == std::string::npos ? v : v.substr(0, colon)).c_str());
            std::string path = (colon == std::string::npos) ? "" : v.substr(colon + 1);
            chspec.push_back({dial, path});
        }
        else if (a == "-h" || a == "--help") usage();
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(); }
    }
    if (chspec.empty()) { std::fprintf(stderr, "need at least one -ch\n"); usage(); }
    for (auto &c : chspec) cfg.dials.push_back(c.first);

    std::string err;
    bool synth = (cfg.driver == "synth" || cfg.driver.rfind("synth", 0) == 0);
    Capture *cap = synth ? make_synth(cfg, err) : make_soapy(cfg, err);
    if (!cap) { std::fprintf(stderr, "sdrfanout: %s\n", err.c_str()); return 1; }

    // Open each channel output. FIFOs are opened O_RDWR so the open never blocks
    // or fails for want of a reader. Non-blocking writes plus drop-not-stall handle a
    // slow/absent consumer. "-" = stdout.
    std::vector<ChannelOut> chans;
    for (auto &cs : chspec) {
        int fd;
        if (cs.second == "-") {
            fd = STDOUT_FILENO;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        } else {
            std::string path = cs.second;
            if (path.empty()) {                          // auto: deterministic per-dial FIFO
                char buf[64];
                std::snprintf(buf, sizeof buf, SDRFANOUT_TMPDIR "/%ld.fifo", (long)cs.first);
                path = buf;
                mkdir(SDRFANOUT_TMPDIR, 0755);           // ignore EEXIST
                mkfifo(path.c_str(), 0644);              // ignore EEXIST. A real FIFO, not a file
                std::fprintf(stderr, "sdrfanout: ch %.0f -> %s\n", cs.first, path.c_str());
            }
            fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CREAT, 0644);
            if (fd < 0) { perror(path.c_str()); delete cap; return 1; }
        }
        chans.push_back({cs.first, fd, 0});
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    int rc = run_producer(cap, chans, frame, buffer_sec, &g_stop);

    for (auto &c : chans)
        if (c.dropped) std::fprintf(stderr, "sdrfanout: ch %.0f dropped %ld frames\n", c.dial, c.dropped);
    delete cap;
    return rc;
}
