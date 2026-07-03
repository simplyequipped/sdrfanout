// framer: accumulate a channel's output bytes, emit whole frames, and write to fd.
// In an audio mode (USB/LSB) a frame is [sdrf_hdr][samples]; in IQ mode it is raw
// headerless interleaved samples. Holds up to `buffer_sec` of frames in a userspace
// queue, drained to the fd opportunistically (non-blocking). When the queue is full
// it drops the OLDEST frame (drop-not-stall) so a slow/stalled consumer can't
// back-pressure the shared SDR read and overflow IQ for every channel.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class Framer {
public:
    // outrate       : channel output sample rate (header field and backlog cap)
    // frame_samples : output time-steps per frame
    // sample_bytes  : bytes per time-step (element size * 1 for real, * 2 for IQ)
    // framed        : prepend an sdrf_hdr per frame (audio), else write raw bytes (IQ)
    // f32           : set SDRF_FLAG_F32 in the header
    Framer(int fd, int outrate, int frame_samples, double buffer_sec,
           int sample_bytes, bool framed, bool f32);

    // Append nbytes of output (a whole number of time-steps) whose NEWEST sample was
    // captured at wall time t_newest_sec. Emits/queues whole frames as they fill.
    void add(const uint8_t *bytes, size_t nbytes, double t_newest_sec);

    long dropped() const { return dropped_; }

private:
    int fd_, outrate_, frame_, sample_bytes_;
    bool framed_, f32_;
    size_t frame_bytes_;                         // frame_ * sample_bytes_
    size_t max_frames_;                          // queue cap = buffer_sec worth
    double t_newest_;
    long dropped_;
    std::vector<uint8_t> buf_;                   // output byte accumulator
    std::deque<std::vector<uint8_t>> queue_;     // pending frames
    void emit();    // build one frame from buf_, push to the queue
    void drain();   // flush queued frames to fd, drop oldest beyond capacity
};
