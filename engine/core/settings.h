#pragma once

// Every parameter, listed once. The settings structs, the editor, the saved
// project state and the cache keys are all generated from these lists.
//
// Each stage's list says how much work a change costs: a brain setting
// rebuilds the brain, a target setting re-analyses the clip, and a render
// setting only re-renders.

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace sb {

struct IntParam {
  using value_type = int;
  int min, max, default_value;
};

struct FloatParam {
  using value_type = float;
  float min, max, default_value;
};

struct ChoiceParam {
  using value_type = int;
  std::span<const std::string_view> labels;
  int default_value;
};

struct BoolParam {
  using value_type = bool;
  bool default_value;
};

using ParamKind = std::variant<IntParam, FloatParam, ChoiceParam, BoolParam>;

enum class Stage { Brain, Target, Render };

struct ParamInfo {
  std::string_view id;
  std::string_view label;
  Stage stage;
  ParamKind kind;
};

// In upstream's window::type order.
inline constexpr std::string_view window_labels[] = {
    "dodgy",    "bartlett", "blackman", "flat top",
    "gaussian", "hamming",  "hann",     "rectangle"};

// In upstream's renderer::search_algo order.
inline constexpr std::string_view algorithm_labels[] = {
    "basic", "reversed", "synaptic", "slide"};

// clang-format off
#define SB_BRAIN_SETTINGS(X)                                                   \
  X(block_size, "Block size", (IntParam{10, 99999, 1000}))                     \
  X(overlap,    "Overlap",    (FloatParam{0.f, 0.99f, 0.75f}))                 \
  X(window,     "Window",     (ChoiceParam{window_labels, 0}))

#define SB_TARGET_SETTINGS(X)                                                  \
  X(block_size, "Target block size", (IntParam{10, 99999, 1000}))              \
  X(overlap,    "Target overlap",    (FloatParam{0.f, 0.99f, 0.75f}))          \
  X(window,     "Target window",     (ChoiceParam{window_labels, 0}))          \
  X(stereo,     "Stereo",            (BoolParam{false}))

// Volume is scaled like the app's dial, so 0.5 plays at unity gain.
#define SB_RENDER_SETTINGS(X)                                                  \
  X(algorithm,   "Search algorithm", (ChoiceParam{algorithm_labels, 0}))       \
  X(ratio,       "FFT/MFCC ratio",   (FloatParam{0.f, 1.f, 0.5f}))             \
  X(n_ratio,     "Normalised ratio", (FloatParam{0.f, 1.f, 0.f}))              \
  X(fft1_start,  "FFT start",        (IntParam{0, 99, 0}))                     \
  X(fft1_end,    "FFT end",          (IntParam{0, 99, 99}))                    \
  X(novelty,     "Novelty",          (FloatParam{0.f, 1.f, 0.f}))              \
  X(boredom,     "Boredom",          (FloatParam{0.f, 1.f, 0.f}))              \
  X(stickyness,  "Stickyness",       (FloatParam{0.f, 1.f, 0.f}))              \
  X(synapses,    "Synapses",         (IntParam{0, 1000, 20}))                  \
  X(stretch,     "Search stretch",   (IntParam{1, 50, 1}))                     \
  X(slide_error, "Slide error",      (IntParam{0, 10000, 1000}))               \
  X(autotune,    "Autotune",         (FloatParam{0.f, 1.f, 0.f}))              \
  X(n_mix,       "Normalised mix",   (FloatParam{0.f, 1.f, 0.f}))              \
  X(target_mix,  "Target mix",       (FloatParam{0.f, 1.f, 0.f}))              \
  X(volume,      "Volume",           (FloatParam{0.f, 1.f, 0.5f}))             \
  X(seed,        "Seed",             (IntParam{0, 99999, 0}))
// clang-format on

#define SB_FIELD(name, label, kind)                                            \
  decltype(kind)::value_type name = (kind).default_value;

// operator== asks whether a setting was changed, so exact float comparison
// is intended.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"

struct BrainSettings {
  SB_BRAIN_SETTINGS(SB_FIELD)
  bool operator==(const BrainSettings &) const = default;
};

struct TargetSettings {
  SB_TARGET_SETTINGS(SB_FIELD)
  bool operator==(const TargetSettings &) const = default;
};

struct RenderSettings {
  SB_RENDER_SETTINGS(SB_FIELD)
  // One per brain sound. These belong to the brain rather than the clip,
  // but switching one only needs a re-render.
  std::vector<bool> sound_enabled;
  bool operator==(const RenderSettings &) const = default;
};

#pragma clang diagnostic pop

#undef SB_FIELD

#define SB_BRAIN_INFO(name, label, kind)                                       \
  ParamInfo{#name, label, Stage::Brain, kind},
#define SB_TARGET_INFO(name, label, kind)                                      \
  ParamInfo{#name, label, Stage::Target, kind},
#define SB_RENDER_INFO(name, label, kind)                                      \
  ParamInfo{#name, label, Stage::Render, kind},

inline constexpr ParamInfo brain_params[] = {SB_BRAIN_SETTINGS(SB_BRAIN_INFO)};
inline constexpr ParamInfo target_params[] = {
    SB_TARGET_SETTINGS(SB_TARGET_INFO)};
inline constexpr ParamInfo render_params[] = {
    SB_RENDER_SETTINGS(SB_RENDER_INFO)};

#undef SB_BRAIN_INFO
#undef SB_TARGET_INFO
#undef SB_RENDER_INFO

// Calls f(info, field) for each listed field of a settings struct, in list
// order. The field is const when the settings are.
#define SB_VISIT(name, label, kind) f(params[i++], settings.name);
#define SB_DEFINE_VISIT(Settings, LIST, table)                                 \
  template <class S, class F>                                                  \
    requires std::same_as<std::remove_const_t<S>, Settings>                    \
  constexpr void visit_settings(S &settings, F &&f) {                          \
    const auto &params = table;                                                \
    std::size_t i = 0;                                                         \
    LIST(SB_VISIT)                                                             \
  }

SB_DEFINE_VISIT(BrainSettings, SB_BRAIN_SETTINGS, brain_params)
SB_DEFINE_VISIT(TargetSettings, SB_TARGET_SETTINGS, target_params)
SB_DEFINE_VISIT(RenderSettings, SB_RENDER_SETTINGS, render_params)

#undef SB_VISIT
#undef SB_DEFINE_VISIT

template <class S>
concept Settings = requires(S &s) {
  visit_settings(s, [](const ParamInfo &, auto &) {});
};

// A field only ever meets its own kind of parameter. Other pairings are
// never reached and leave the value alone.
template <class T> T clamp_to(const ParamKind &kind, T value) {
  return std::visit(
      [&](const auto &k) -> T {
        using K = std::remove_cvref_t<decltype(k)>;
        if constexpr (!std::is_same_v<typename K::value_type, T>)
          return value;
        else if constexpr (std::is_same_v<K, ChoiceParam>)
          return std::clamp<int>(value, 0, int(k.labels.size()) - 1);
        else if constexpr (std::is_same_v<K, BoolParam>)
          return value;
        else
          return std::clamp<T>(value, k.min, k.max);
      },
      kind);
}

// Pulls every field into its listed range.
template <Settings S> S clamped(S settings) {
  visit_settings(settings, [](const ParamInfo &info, auto &field) {
    field = clamp_to(info.kind, field);
  });
  return settings;
}

} // namespace sb
