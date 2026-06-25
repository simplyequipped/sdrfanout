// framer: accumulate 12 kHz int16 audio, frame it as [sdrf_hdr][PCM], and write
// to fd. Holds up to `buffer_sec` of frames in a userspace queue, drained to the
// fd opportunistically (non-blocking). When the queue is full it drops the OLDEST
// frame (drop-not-stall) so a slow/stalled consumer can't back-pressure the shared
// SDR read and overflow IQ for every channel.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class Framer {
public:
    Framer(int fd, int rate, int frame_samples, double buffer_sec);

    // Append n audio samples whose NEWEST (a[n-1]) was captured at wall time
    // t_newest_sec (CLOCK_REALTIME seconds). Emits/queues whole frames as they fill.
    void add(const int16_t *a, size_t n, double t_newest_sec);

    long dropped() const { return dropped_; }

private:
    int fd_, rate_, frame_;
    size_t max_frames_;                          // queue cap = buffer_sec worth
    double t_newest_;
    long dropped_;
    std::vector<int16_t> buf_;                   // audio accumulator
    std::deque<std::vector<uint8_t>> queue_;     // pending [hdr|pcm] frames
    void emit();    // build one frame from buf_, push to the queue
    void drain();   // flush queued frames to fd, drop oldest beyond capacity
};
