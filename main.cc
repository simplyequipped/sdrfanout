// sdrfanout: fan one SDR out to several offset USB channels, each a stamped
// 12 kHz sdrf stream on its own FIFO. See README for the stream format.
#include "capture.h"
#include "run.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <numeric>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/prctl.h>   // PR_SET_PDEATHSIG: avoid being orphoned
#endif

#define SDRFANOUT_TMPDIR "/tmp/sdrfanout"   // default home for auto-named channel FIFOs

static volatile bool g_stop = false;
static void on_signal(int) { g_stop = true; }

static void usage() {
    std::fprintf(stderr,
        "usage: sdrfanout -ch <dial_hz>:<path> [-ch ...] [options]\n"
        "  -driver <dev>           SoapySDR device name (ex. hackrf, rtlsdr, etc) or 'synth'\n"
        "  -gain <db|auto>         default: auto (AGC / device default)\n"
        "  -rate <hz|auto>         sample rate in Hz, default: auto\n"
        "                          auto: smallest multiple of the output rate spanning all channels\n"
        "  -center <hz|auto|edge>  center frequency in Hz, default: auto\n"
        "                          auto: centered amidst channels, offset from DC by *guard*\n"
        "                          edge: *guard* Hz below the lowest channel\n"
        "  -guard <hz>             offset around center (DC) and band edges in Hz, default: 10000\n"
        "  -ppm <n>                frequency correction, default: 0\n"
        "  -antenna <name>         Soapy antenna, default: not set (device default)\n"
        "  -buffer <sec>           per-channel output buffer in seconds, default: 1\n"
        "  -threads <n>            DSP worker threads, default: auto (one per channel, capped at cores)\n"
        "  -outrate <hz>           channel output sample rate in Hz, default: 12000\n"
        "  -mode <usb|lsb|iq>      default channel output mode: usb (iq = raw complex baseband)\n"
        "  -format <i16|f32>       output sample type, default: i16\n"
        "  -ch <dial>[:<path>[:<mode>[:<outrate>]]]\n"
        "                          channel dial freq (Hz) + output FIFO, with optional per-\n"
        "                          channel mode and outrate (empty field = use the global).\n"
        "                          path defaults to " SDRFANOUT_TMPDIR "/<dial>.fifo, '-' = stdout\n");
    std::exit(2);
}

static Mode parse_mode(const std::string &v) {
    if (v == "usb") return Mode::USB;
    if (v == "lsb") return Mode::LSB;
    if (v == "iq")  return Mode::IQ;
    std::fprintf(stderr, "invalid -mode '%s' (usb|lsb|iq)\n", v.c_str());
    std::exit(2);
}

static Format parse_format(const std::string &v) {
    if (v == "i16") return Format::I16;
    if (v == "f32") return Format::F32;
    std::fprintf(stderr, "invalid -format '%s' (i16|f32)\n", v.c_str());
    std::exit(2);
}

// One -ch spec: dial[:path[:mode[:outrate]]]. mode/outrate < 0 means "use the global".
struct ChSpec {
    double dial;
    std::string path;
    int mode = -1;
    int outrate = -1;
};

int main(int argc, char **argv) {
#ifdef __linux__
    // If parent exits for any reason, even a hard kill that skips its
    // teardown, the kernel sends SIGTERM
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1) return 0;
#endif
    CaptureConfig cfg;
    std::vector<ChSpec> chspec;
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
        else if (a == "-center") {
            std::string v = val("-center");
            if (v == "auto") cfg.center_mode = CenterMode::Auto;
            else if (v == "edge") cfg.center_mode = CenterMode::Edge;
            else { cfg.center_mode = CenterMode::Fixed; cfg.want_center = atof(v.c_str()); }
        }
        else if (a == "-guard") cfg.guard = atof(val("-guard").c_str());
        else if (a == "-ppm") cfg.ppm = atof(val("-ppm").c_str());
        else if (a == "-antenna") cfg.antenna = val("-antenna");
        else if (a == "-buffer") buffer_sec = atof(val("-buffer").c_str());
        else if (a == "-threads") cfg.threads = atoi(val("-threads").c_str());
        else if (a == "-outrate") cfg.outrate = atoi(val("-outrate").c_str());
        else if (a == "-mode") cfg.mode = parse_mode(val("-mode"));
        else if (a == "-format") cfg.format = parse_format(val("-format"));
        else if (a == "-ch") {
            std::string v = val("-ch");
            // dial[:path[:mode[:outrate]]] split on ':'. An empty field uses the global.
            std::vector<std::string> f;
            for (size_t s = 0;;) {
                size_t c = v.find(':', s);
                f.push_back(c == std::string::npos ? v.substr(s) : v.substr(s, c - s));
                if (c == std::string::npos) break;
                s = c + 1;
            }
            ChSpec cs;
            cs.dial = atof(f[0].c_str());
            if (f.size() > 1) cs.path = f[1];
            if (f.size() > 2 && !f[2].empty()) cs.mode = (int)parse_mode(f[2]);
            if (f.size() > 3 && !f[3].empty()) cs.outrate = atoi(f[3].c_str());
            chspec.push_back(cs);
        }
        else if (a == "-h" || a == "--help") usage();
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(); }
    }
    if (chspec.empty()) { std::fprintf(stderr, "need at least one -ch\n"); usage(); }
    // Resolve each channel's mode/outrate against the globals, then size the SDR rate
    // unit as the lcm of all channel outrates: fs must be a multiple of every one.
    int rate_unit = cfg.outrate;
    for (auto &cs : chspec) {
        if (cs.mode < 0) cs.mode = (int)cfg.mode;
        if (cs.outrate < 0) cs.outrate = cfg.outrate;
        cfg.dials.push_back(cs.dial);
        rate_unit = std::lcm(rate_unit, cs.outrate);
    }
    cfg.outrate = rate_unit;

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
        if (cs.path == "-") {
            fd = STDOUT_FILENO;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        } else {
            std::string path = cs.path;
            if (path.empty()) {                          // auto: deterministic per-dial FIFO
                char buf[64];
                std::snprintf(buf, sizeof buf, SDRFANOUT_TMPDIR "/%ld.fifo", (long)cs.dial);
                path = buf;
                mkdir(SDRFANOUT_TMPDIR, 0755);           // ignore EEXIST
                std::fprintf(stderr, "sdrfanout: ch %.0f -> %s\n", cs.dial, path.c_str());
            }
            mkfifo(path.c_str(), 0644);                  // create the FIFO for any path form; ignore EEXIST
            fd = open(path.c_str(), O_RDWR | O_NONBLOCK);   // no O_CREAT: never silently make a regular file
            if (fd < 0) { perror(path.c_str()); delete cap; return 1; }
            struct stat st;
            if (fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode)) {
                std::fprintf(stderr, "sdrfanout: %s exists but is not a FIFO\n", path.c_str());
                delete cap; return 1;
            }
        }
        chans.push_back({cs.dial, fd, (Mode)cs.mode, cfg.format, cs.outrate, 0});
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    int rc = run_producer(cap, chans, buffer_sec, &g_stop, cfg.threads);

    for (auto &c : chans)
        if (c.dropped) std::fprintf(stderr, "sdrfanout: ch %.0f dropped %ld frames\n", c.dial, c.dropped);
    delete cap;
    return rc;
}
