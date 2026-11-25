#pragma once

#include <algorithm>
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

} // namespace sb
