// run: the producer loop. Capture IQ -> per-channel (mix/decimate/USB) -> frame
// out to each channel's fd. The per-channel DSP can fan across worker threads
// (one read thread, a fork-join pool per buffer). Writes are non-blocking
// drop-not-stall so a slow consumer can't back-pressure the capture.
#pragma once
#include "capture.h"
#include <string>
#include <vector>

struct ChannelOut {
    double dial;       // Hz
    int fd;            // output fd (FIFO/file/stdout)
    long dropped = 0;  // filled in on return
};

// Runs until *stop, or until capture read() returns <0. Returns 0 on clean stop,
// 1 on capture error. `threads` worker threads share the channels (<=0 = auto:
// one per channel, capped at the core count; 1 = the inline single-thread path).
int run_producer(Capture *cap, std::vector<ChannelOut> &chans,
                 int frame_samples, double buffer_sec, volatile bool *stop,
                 int threads = 1);
