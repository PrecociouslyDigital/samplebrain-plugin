#include <algorithm>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "core/core.h"
#include "fixtures.h"

using namespace sb;
using namespace sb::test;

namespace {

const Brain &shared_brain() {
  static const Brain brain =
      *analyse_brain(small_sounds(), small_brain_settings(), 44100);
  return brain;
}

const Target &shared_target(std::size_t channels = 1) {
  static const Target mono =
      *analyse_target(small_clip(), small_target_settings(), 44100);
  static const Target stereo = [] {
    TargetSettings s = small_target_settings();
    s.stereo = true;
    return *analyse_target(small_clip(44100, 2), s, 44100);
  }();
  return channels == 1 ? mono : stereo;
}

} // namespace

TEST_CASE("render gives the same output for the same inputs") {
  const int algorithm = GENERATE(range(0, 4));
  const int seed = GENERATE(take(3, random(0, 99999)));
  const RenderSettings settings = render_settings(algorithm, seed);

  const auto first = render(shared_brain(), shared_target(), settings);
  REQUIRE(first);

  SECTION("called twice in a row") {
    CHECK(*render(shared_brain(), shared_target(), settings) == *first);
  }

  SECTION("called on two threads sharing one brain") {
    std::expected<RenderedAudio, RenderError> a, b;
    {
      std::jthread ta([&] { a = render(shared_brain(), shared_target(), settings); });
      std::jthread tb([&] { b = render(shared_brain(), shared_target(), settings); });
    }
    CHECK(*a == *first);
    CHECK(*b == *first);
  }
}

TEST_CASE("the renderer's chunk size only changes the end of the output") {
  const int algorithm = GENERATE(range(0, 4));
  const unsigned chunk_a = GENERATE(64u, 333u, 512u);
  const unsigned chunk_b = GENERATE(1u, 1000u, 4096u);
  RenderSettings settings = render_settings(algorithm, 7);
  settings.stretch = GENERATE(1, 3);

  const auto a = testing::render_in_chunks(shared_brain(), shared_target(),
                                           settings, chunk_a);
  const auto b = testing::render_in_chunks(shared_brain(), shared_target(),
                                           settings, chunk_b);
  REQUIRE(a);
  REQUIRE(b);

  // The renderer stops once a chunk would reach past the target's last
  // block. It searches a chunk ahead of what it plays, so up to one block
  // plus two chunks at the end can differ.
  const std::size_t tail = std::size_t(small_target_settings().block_size) +
                           2 * std::max(chunk_a, chunk_b);
  const Mono &x = a->channels[0], &y = b->channels[0];
  REQUIRE(x.size() == y.size());
  REQUIRE(x.size() > tail);
  const auto [diff, _] = std::mismatch(x.begin(), x.end(), y.begin());
  INFO("algorithm " << algorithm << ", chunks " << chunk_a << " and " << chunk_b
                    << ", stretch " << settings.stretch << ": first difference at "
                    << (diff - x.begin()) << " of " << x.size());
  CHECK(diff - x.begin() >= std::ptrdiff_t(x.size() - tail));
}

TEST_CASE("stereo renders each side separately") {
  const RenderSettings settings = render_settings(0, 3);
  const auto mono = render(shared_brain(), shared_target(1), settings);
  const auto stereo = render(shared_brain(), shared_target(2), settings);
  REQUIRE(mono->channels.size() == 1);
  REQUIRE(stereo->channels.size() == 2);
  CHECK(stereo->channels[0] != stereo->channels[1]);
}

TEST_CASE("a loaded brain renders like the brain that was saved") {
  const auto path = temp_path("roundtrip.brain");
  REQUIRE(save_brain(shared_brain(), path.string()));
  const auto loaded = load_brain(path.string(), 44100);
  std::filesystem::remove(path);
  REQUIRE(loaded);
  CHECK(loaded->num_blocks() == shared_brain().num_blocks());

  const int algorithm = GENERATE(range(0, 4));
  const RenderSettings settings = render_settings(algorithm, 11);
  CHECK(*render(*loaded, shared_target(), settings) ==
        *render(shared_brain(), shared_target(), settings));
}

TEST_CASE("switching every sound off doesn't crash") {
  RenderSettings settings = render_settings(GENERATE(range(0, 4)), 5);
  settings.sound_enabled.assign(2, false);
  CHECK(render(shared_brain(), shared_target(), settings));
}

TEST_CASE("render refuses mismatched inputs") {
  const Target other_rate =
      *analyse_target(small_clip(48000), small_target_settings(), 48000);
  CHECK(render(shared_brain(), other_rate, render_settings(0, 0)).error().reason ==
        RenderError::RateMismatch);
  CHECK(render(shared_brain(), shared_target(), render_settings(0, 0, 3))
            .error()
            .reason == RenderError::SoundCountMismatch);
}

TEST_CASE("a sound shorter than one block adds no blocks") {
  std::vector<Sound> sounds = {{"tiny", synth(9, 100, 44100)},
                               {"a", synth(1, 44100, 44100)}};
  const auto brain = analyse_brain(sounds, small_brain_settings(), 44100);
  REQUIRE(brain);
  CHECK(brain->num_blocks() == expected_blocks(44100, 512, 256));
  CHECK(render(*brain, shared_target(), render_settings(0, 0)));
}
