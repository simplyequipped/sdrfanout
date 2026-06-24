#include "framer.h"
#include "sdrf.h"
#include <unistd.h>
#include <cstring>

Framer::Framer(int fd, int rate, int frame_samples, double buffer_sec)
    : fd_(fd), rate_(rate), frame_(frame_samples), t_newest_(0), dropped_(0) {
    double fps = (double)rate_ / frame_;                 // frames per second
    max_frames_ = (size_t)(buffer_sec * fps + 0.5);
    if (max_frames_ < 1) max_frames_ = 1;
}

void Framer::add(const int16_t *a, size_t n, double t_newest_sec) {
    buf_.insert(buf_.end(), a, a + n);
    t_newest_ = t_newest_sec;
    while ((int)buf_.size() >= frame_) emit();
    drain();
}

void Framer::emit() {
    // wall time of buf_[0] (the frame's first sample) = newest − (count−1)/rate
    double t_first = t_newest_ - (double)(buf_.size() - 1) / rate_;

    sdrf_hdr h;
    h.magic = SDRF_MAGIC;
    h.version = SDRF_VERSION;
    h.flags = 0;
    h.rate_hz = (uint32_t)rate_;
    h.nsamples = (uint32_t)frame_;
    h.t_usec = (uint64_t)(t_first * 1e6 + 0.5);

    std::vector<uint8_t> f(sizeof(sdrf_hdr) + (size_t)frame_ * sizeof(int16_t));
    std::memcpy(f.data(), &h, sizeof h);
    std::memcpy(f.data() + sizeof h, buf_.data(), (size_t)frame_ * sizeof(int16_t));
    queue_.push_back(std::move(f));

    buf_.erase(buf_.begin(), buf_.begin() + frame_);
}

void Framer::drain() {
    // Flush as many queued frames as the fd will take without blocking. A write of
    // <= PIPE_BUF bytes is atomic on a pipe, so it's all-or-EAGAIN (no partials).
    while (!queue_.empty()) {
        const std::vector<uint8_t> &f = queue_.front();
        ssize_t w = write(fd_, f.data(), f.size());
        if (w == (ssize_t)f.size()) { queue_.pop_front(); continue; }
        break;  // EAGAIN (or transient error): keep it queued, try next time
    }
    // Cap the backlog: drop the oldest frames beyond buffer_sec worth.
    while (queue_.size() > max_frames_) { queue_.pop_front(); dropped_++; }
}
