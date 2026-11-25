#include <catch2/catch_test_macros.hpp>

#include "core/keys.h"
#include "core/settings.h"

using namespace sb;

namespace {

// Some other in-range value for a field.
template <class T> T another_value(const ParamKind &kind, T value) {
  return std::visit(
      [&](const auto &k) -> T {
        using K = std::remove_cvref_t<decltype(k)>;
        if constexpr (std::is_same_v<K, BoolParam>)
          return !value;
        else if constexpr (std::is_same_v<K, ChoiceParam>)
          return T((int(value) + 1) % int(k.labels.size()));
        else
          return T(value == k.max ? k.min : k.max);
      },
      kind);
}

template <Settings S, class KeyOf> void each_field_changes_the_key(KeyOf key_of) {
  const S defaults;
  std::size_t fields = 0;
  visit_settings(defaults, [&](const ParamInfo &info, const auto &) {
    S changed = defaults;
    visit_settings(changed, [&](const ParamInfo &other, auto &field) {
      if (other.id == info.id) field = another_value(other.kind, field);
    });
    INFO(info.id);
    CHECK(changed != defaults);
    CHECK(key_of(changed) != key_of(defaults));
    ++fields;
  });
  CHECK(fields > 0);
}

} // namespace

TEST_CASE("defaults are inside their ranges") {
  CHECK(clamped(BrainSettings{}) == BrainSettings{});
  CHECK(clamped(TargetSettings{}) == TargetSettings{});
  CHECK(clamped(RenderSettings{}) == RenderSettings{});
  CHECK(clamped(BlockLength{}) == BlockLength{});
}

TEST_CASE("every setting is part of its stage's key") {
  const AudioHash sound = hash_audio(std::vector<float>{1, 2, 3});
  const AudioHash sounds[] = {sound};

  each_field_changes_the_key<BrainSettings>(
      [&](const BrainSettings &s) { return brain_key(sounds, s, 44100); });
  each_field_changes_the_key<TargetSettings>(
      [&](const TargetSettings &s) { return target_key(sounds, s, 44100); });
  each_field_changes_the_key<RenderSettings>([&](const RenderSettings &s) {
    return render_key(BrainKey{}, TargetKey{}, s);
  });
}

TEST_CASE("the rate and sounds are part of the brain key") {
  const AudioHash a = hash_audio(std::vector<float>{1, 2, 3});
  const AudioHash b = hash_audio(std::vector<float>{1, 2, 4});
  const AudioHash ab[] = {a, b}, ba[] = {b, a};
  const BrainSettings s;
  CHECK(brain_key(ab, s, 44100) != brain_key(ab, s, 48000));
  CHECK(brain_key(ab, s, 44100) != brain_key(ba, s, 44100));
  CHECK(brain_key(ab, s, 44100) == brain_key(ab, s, 44100));
}

TEST_CASE("switching a sound changes the render key") {
  RenderSettings on, off;
  on.sound_enabled = {true, true};
  off.sound_enabled = {true, false};
  CHECK(render_key({}, {}, on) != render_key({}, {}, off));
}

TEST_CASE("a length in ms is that many ms at any rate") {
  BlockLength length;
  for (float ms : {0.5f, 22.7f, 100.f, 2000.f})
    for (unsigned rate : {44100u, 48000u, 96000u}) {
      length.ms = ms;
      CHECK(block_samples(length, rate, 93.0) == std::lround(double(ms) * rate / 1000.0));
    }
}

TEST_CASE("note lengths follow the tempo") {
  BlockLength length;
  length.unit = 1;
  length.note = 0;
  // A quarter note at 120 BPM is half a second.
  CHECK(block_samples(length, 48000, 120.0) == 24000);
  CHECK(block_samples(length, 48000, 60.0) == 48000);

  // Each note down the list is half as long, and the feels scale it.
  for (int note = 1; note < int(std::size(note_labels)); ++note) {
    length.note = note;
    length.feel = 0;
    const double straight = 48000.0 / double(1 << note);
    CHECK(block_samples(length, 48000, 60.0) == std::lround(straight));
    length.feel = 1;
    CHECK(block_samples(length, 48000, 60.0) == std::lround(straight * 2.0 / 3.0));
    length.feel = 2;
    CHECK(block_samples(length, 48000, 60.0) == std::lround(straight * 1.5));
  }
}

TEST_CASE("a length shows the controls for its unit") {
  BlockLength length;
  CHECK(length_param_applies(length, "ms"));
  CHECK(!length_param_applies(length, "note"));
  length.unit = 1;
  CHECK(!length_param_applies(length, "ms"));
  CHECK(length_param_applies(length, "note"));
  CHECK(length_param_applies(length, "feel"));
  CHECK(length_param_applies(length, "unit"));
  CHECK(length_param_applies(length, "project_tempo"));
  CHECK(!length_param_applies(length, "bpm"));
  length.project_tempo = false;
  CHECK(length_param_applies(length, "bpm"));
}

TEST_CASE("a note length with its own tempo ignores the project's") {
  BlockLength length;
  length.unit = 1;
  length.note = 0;
  length.project_tempo = false;
  length.bpm = 60.f;
  for (double project_bpm : {60.0, 90.0, 174.0})
    CHECK(block_samples(length, 48000, project_bpm) == 48000);
}
