#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/core.h"
#include "fixtures.h"
#include "io/audio_file.h"

using namespace sb;
using namespace sb::test;

namespace {

std::size_t blocks_for(std::size_t length, int block_size, float overlap) {
  const auto block = std::size_t(block_size);
  return expected_blocks(length, block, block - std::size_t(block * overlap));
}

} // namespace

TEST_CASE("a written wav reads back unchanged") {
  const Mono audio = synth(3, 5000, 48000);
  const auto path = temp_path("exact.wav");
  REQUIRE(write_wav(path, audio, 48000));
  const auto read = read_audio_file(path, 48000);
  std::filesystem::remove(path);
  REQUIRE(read);
  CHECK(read->channels[0] == audio);
}

TEST_CASE("brain and target analysed at any shared rate") {
  const unsigned rate = GENERATE(8000u, 22050u, 44100u, 48000u, 96000u);

  // Files at 44.1k, read back at the project rate like the plugin does.
  std::vector<Sound> sounds;
  for (const Sound &s : small_sounds()) {
    const auto path = temp_path(s.name + ".wav");
    REQUIRE(write_wav(path, s.audio, 44100));
    auto read = read_audio_file(path, rate);
    std::filesystem::remove(path);
    REQUIRE(read);
    const double expected_length = double(s.audio.size()) * rate / 44100;
    CHECK(std::abs(double(read->channels[0].size()) - expected_length) <= 2);
    sounds.push_back({s.name, read->channels[0]});
  }
  const ClipAudio clip{{sounds[0].audio}};

  const BrainSettings bs = small_brain_settings();
  const TargetSettings ts = small_target_settings();
  const auto brain = analyse_brain(sounds, bs, rate);
  const auto target = analyse_target(clip, ts, rate);
  REQUIRE(brain);
  REQUIRE(target);

  CHECK(brain->num_blocks() ==
        blocks_for(sounds[0].audio.size(), bs.block_size, bs.overlap) +
            blocks_for(sounds[1].audio.size(), bs.block_size, bs.overlap));
  CHECK(target->num_blocks() ==
        blocks_for(clip.channels[0].size(), ts.block_size, ts.overlap));

  const auto out = render(*brain, *target, render_settings(0, 1));
  REQUIRE(out);
  CHECK(out->channels[0].size() == clip.channels[0].size());
}
