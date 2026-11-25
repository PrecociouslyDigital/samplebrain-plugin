#include <random>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>

#include "core/playback.h"

using namespace sb;

TEST_CASE("playback equals the matching slice of the render") {
  const auto seed = GENERATE(take(200, random(0u, 1u << 30)));
  std::mt19937 rng(seed);
  auto between = [&](std::int64_t lo, std::int64_t hi) {
    return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
  };

  RenderedAudio render;
  const auto length = std::size_t(between(0, 5000));
  render.channels.resize(std::size_t(between(1, 2)));
  for (Mono &c : render.channels) {
    c.resize(length);
    for (float &s : c) s = float(between(-1000, 1000));
  }

  const std::int64_t start = between(-6000, 6000);
  const auto frames = std::size_t(between(0, 2048));
  std::vector<Mono> out(std::size_t(between(1, 2)), Mono(frames, 123.f));
  std::vector<float *> pointers;
  for (Mono &c : out) pointers.push_back(c.data());

  play_slice(render, start, pointers, frames);

  for (std::size_t c = 0; c < out.size(); ++c) {
    const Mono &src = render.channels[std::min(c, render.channels.size() - 1)];
    for (std::size_t i = 0; i < frames; ++i) {
      const std::int64_t pos = start + std::int64_t(i);
      const float expected =
          pos >= 0 && pos < std::int64_t(length) ? src[std::size_t(pos)] : 0.f;
      REQUIRE(out[c][i] == expected);
    }
  }
}

TEST_CASE("resampled playback at the render's own rate equals the slice") {
  const auto seed = GENERATE(take(100, random(0u, 1u << 30)));
  std::mt19937 rng(seed);
  auto between = [&](std::int64_t lo, std::int64_t hi) {
    return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
  };

  RenderedAudio render;
  render.channels.resize(std::size_t(between(1, 2)));
  const auto length = std::size_t(between(0, 3000));
  for (Mono &c : render.channels) {
    c.resize(length);
    for (float &s : c) s = float(between(-1000, 1000));
  }
  const std::int64_t start = between(-4000, 4000);
  const auto frames = std::size_t(between(0, 1024));
  Mono sliced(frames), resampled(frames);
  float *a[] = {sliced.data()}, *b[] = {resampled.data()};

  play_slice(render, start, a, frames);
  play_resampled(render, double(start), 1.0, b, frames);
  CHECK(sliced == resampled);
}
