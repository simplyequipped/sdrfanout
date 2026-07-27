// run: the producer loop. Capture IQ -> per-channel (mix/decimate/USB) -> frame
// out to each channel's fd. The per-channel DSP can fan across worker threads
// (one read thread, a fork-join pool per buffer). Writes are non-blocking
// drop-not-stall so a slow consumer can't back-pressure the capture.
#pragma once
#include "capture.h"
#include <string>
#include <vector>

struct ChannelOut {
    double dial;                   // Hz
    int fd;                        // output fd (FIFO/file/stdout)
    Mode mode = Mode::USB;         // channel output mode
    Format format = Format::I16;   // output sample element type
    int outrate = 12000;           // channel output sample rate (Hz)
    long dropped = 0;              // filled in on return
};

// Runs until *stop, or until capture read() returns <0. Returns 0 on clean stop,
// 1 on capture error. Each channel is framed at 0.1 s of its own output rate.
// `threads` worker threads share the channels (<=0 = auto: one per channel, capped
// at the core count; 1 = the inline single-thread path). `level_interval` > 0 reports
// each channel's signal level that often, in seconds.
int run_producer(Capture *cap, std::vector<ChannelOut> &chans,
                 double buffer_sec, volatile bool *stop, int threads = 1,
                 double level_interval = 0.0);
