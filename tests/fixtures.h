#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <unistd.h>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "core/core.h"

namespace sb::test {

// A few seconds of sines and noise that vary over time, so blocks differ.
inline Mono synth(std::uint32_t seed, std::size_t length, unsigned rate) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> freq(80.f, 2000.f), amp(0.1f, 0.5f),
      noise(-0.05f, 0.05f);
  Mono out(length);
  const std::size_t segment = rate / 8;
  float f = freq(rng), a = amp(rng);
  for (std::size_t i = 0; i < length; ++i) {
    if (i % segment == 0) {
      f = freq(rng);
      a = amp(rng);
    }
    const float t = float(i) / float(rate);
    out[i] = a * std::sin(2 * std::numbers::pi_v<float> * f * t) + noise(rng);
  }
  return out;
}

// Small enough that synapses build in well under a second.
inline BrainSettings small_brain_settings() {
  BrainSettings s;
  s.block_size = 512;
  s.overlap = 0.5f;
  return s;
}

inline TargetSettings small_target_settings() {
  TargetSettings s;
  s.block_size = 512;
  s.overlap = 0.5f;
  return s;
}

inline std::vector<Sound> small_sounds(unsigned rate = 44100) {
  return {{"a", synth(1, rate * 2, rate)}, {"b", synth(2, rate * 3 / 2, rate)}};
}

inline ClipAudio small_clip(unsigned rate = 44100, std::size_t channels = 1) {
  ClipAudio clip;
  for (std::size_t c = 0; c < channels; ++c)
    clip.channels.push_back(synth(100 + std::uint32_t(c), rate * 2, rate));
  return clip;
}

inline RenderSettings render_settings(int algorithm, int seed,
                                      std::size_t sounds = 2) {
  RenderSettings s;
  s.algorithm = algorithm;
  s.seed = seed;
  s.novelty = 0.3f;
  s.boredom = 0.5f;
  s.stickyness = 0.2f;
  s.sound_enabled.assign(sounds, true);
  return s;
}

// Blocks upstream's chop_and_add makes from a sound.
inline std::size_t expected_blocks(std::size_t length, std::size_t block,
                                   std::size_t shift) {
  return length >= block ? (length - block) / shift + 1 : 0;
}

inline std::filesystem::path temp_path(const std::string &name) {
  return std::filesystem::temp_directory_path() /
         ("samplebrain-test-" + std::to_string(::getpid()) + "-" + name);
}

} // namespace sb::test
