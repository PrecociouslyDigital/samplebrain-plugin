#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

#include "core.h"

namespace sb {

// Writes frames [start, start + frames) of a render to each output channel,
// with silence outside the render. A mono render plays on every channel.
inline void play_slice(const RenderedAudio &audio, std::int64_t start,
                       std::span<float *const> out,
                       std::size_t frames) noexcept [[clang::nonblocking]] {
  for (std::size_t c = 0; c < out.size(); ++c) {
    float *dst = out[c];
    std::fill_n(dst, frames, 0.f);
    if (audio.channels.empty()) continue;
    const Mono &src = audio.channels[std::min(c, audio.channels.size() - 1)];
    const std::int64_t length = std::int64_t(src.size());
    const std::int64_t from = std::clamp<std::int64_t>(start, 0, length);
    const std::int64_t to =
        std::clamp<std::int64_t>(start + std::int64_t(frames), 0, length);
    if (from < to)
      std::copy(src.begin() + from, src.begin() + to, dst + (from - start));
  }
}

// Like play_slice, for a render at a different rate from the output. Output
// frame i plays the render at position start + i * step, interpolating
// linearly; step is render rate / output rate.
inline void play_resampled(const RenderedAudio &audio, double start,
                           double step, std::span<float *const> out,
                           std::size_t frames) noexcept [[clang::nonblocking]] {
  for (std::size_t c = 0; c < out.size(); ++c) {
    float *dst = out[c];
    if (audio.channels.empty()) {
      std::fill_n(dst, frames, 0.f);
      continue;
    }
    const Mono &src = audio.channels[std::min(c, audio.channels.size() - 1)];
    const auto sample = [&](std::int64_t i) {
      return i >= 0 && i < std::int64_t(src.size()) ? src[std::size_t(i)] : 0.f;
    };
    for (std::size_t i = 0; i < frames; ++i) {
      const double pos = start + double(i) * step;
      const double whole = std::floor(pos);
      const auto t = float(pos - whole);
      const auto at = std::int64_t(whole);
      dst[i] = sample(at) * (1 - t) + sample(at + 1) * t;
    }
  }
}

} // namespace sb
