#include "framer.h"
#include "sdrf.h"
#include <unistd.h>
#include <cstddef>
#include <cstring>
#include <utility>

Framer::Framer(int fd, int outrate, int frame_samples, double buffer_sec,
               int sample_bytes, bool framed, bool f32)
    : fd_(fd), outrate_(outrate), frame_(frame_samples), sample_bytes_(sample_bytes),
      framed_(framed), f32_(f32), t_newest_(0), dropped_(0) {
    frame_bytes_ = (size_t)frame_ * sample_bytes_;
    double fps = (double)outrate_ / frame_;              // frames per second
    max_frames_ = (size_t)(buffer_sec * fps + 0.5);
    if (max_frames_ < 1) max_frames_ = 1;
}

void Framer::add(const uint8_t *bytes, size_t nbytes, double t_newest_sec) {
    buf_.insert(buf_.end(), bytes, bytes + nbytes);
    t_newest_ = t_newest_sec;
    while (buf_.size() >= frame_bytes_) emit();
    drain();
}

void Framer::emit() {
    // wall time of the frame's first sample = newest - (count-1)/outrate
    size_t samples = buf_.size() / sample_bytes_;
    double t_first = t_newest_ - (double)(samples - 1) / outrate_;

    std::vector<uint8_t> f;
    if (framed_) {
        sdrf_hdr h;
        h.magic = SDRF_MAGIC;
        h.version = SDRF_VERSION;
        h.flags = f32_ ? SDRF_FLAG_F32 : 0;
        h.rate_hz = (uint32_t)outrate_;
        h.nsamples = (uint32_t)frame_;
        h.t_usec = (uint64_t)(t_first * 1e6 + 0.5);
        f.resize(sizeof h + frame_bytes_);
        std::memcpy(f.data(), &h, sizeof h);
        std::memcpy(f.data() + sizeof h, buf_.data(), frame_bytes_);
    } else {                                             // IQ: raw headerless bytes
        f.assign(buf_.begin(), buf_.begin() + frame_bytes_);
    }
    queue_.push_back(std::move(f));

    buf_.erase(buf_.begin(), buf_.begin() + frame_bytes_);
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
