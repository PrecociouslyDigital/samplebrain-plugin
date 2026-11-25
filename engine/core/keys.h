#pragma once

// Content hashes naming each cached value by the inputs that made it.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "settings.h"

namespace sb {

template <class Tag> struct Key {
  std::uint64_t high = 0, low = 0;

  auto operator<=>(const Key &) const = default;
  std::string hex() const;
};

using AudioHash = Key<struct AudioHashTag>;
using BrainKey = Key<struct BrainKeyTag>;
using TargetKey = Key<struct TargetKeyTag>;
using RenderKey = Key<struct RenderKeyTag>;

AudioHash hash_audio(std::span<const float> samples);

// Sound names and which sounds are switched on don't change the brain.
BrainKey brain_key(std::span<const AudioHash> sounds, const BrainSettings &,
                   unsigned rate);
TargetKey target_key(std::span<const AudioHash> channels,
                     const TargetSettings &, unsigned rate);
RenderKey render_key(const BrainKey &, const TargetKey &,
                     const RenderSettings &);

} // namespace sb

template <class Tag> struct std::hash<sb::Key<Tag>> {
  std::size_t operator()(const sb::Key<Tag> &k) const noexcept {
    return k.low;
  }
};
