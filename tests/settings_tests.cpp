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
