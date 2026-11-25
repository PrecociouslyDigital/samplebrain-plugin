#pragma once

// What the project saves: the project's brains and each clip's settings.

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

#include "core/keys.h"
#include "core/settings.h"

namespace sb::plugin {

struct BrainSample {
  juce::String path;
  bool enabled = true;
  bool operator==(const BrainSample &) const = default;
};

struct BrainDefinition {
  juce::String id = juce::Uuid().toString();
  juce::String name = "Brain";
  std::vector<BrainSample> samples;
  // The settings' block_size comes from block, through at().
  BrainSettings settings;
  BlockLength block;

  // The definition the engine sees at a rate and tempo.
  BrainDefinition at(unsigned rate, double bpm) const {
    BrainDefinition resolved = *this;
    resolved.settings.block_size = block_samples(block, rate, bpm);
    resolved.settings = clamped(resolved.settings);
    return resolved;
  }

  std::vector<juce::String> paths() const {
    std::vector<juce::String> paths;
    for (const BrainSample &s : samples) paths.push_back(s.path);
    return paths;
  }
};

// The brain a clip plays and how it plays it. The render settings'
// sound_enabled comes from the brain when rendering.
struct ClipSettings {
  juce::String brain_id;
  // The target's block_size comes from target_block, through target_at().
  TargetSettings target;
  BlockLength target_block;
  RenderSettings render;

  TargetSettings target_at(unsigned rate, double bpm) const {
    TargetSettings resolved = target;
    resolved.block_size = block_samples(target_block, rate, bpm);
    return clamped(resolved);
  }
};

template <Settings S> juce::var to_var(const S &settings) {
  auto *object = new juce::DynamicObject();
  visit_settings(settings, [&](const ParamInfo &info, const auto &field) {
    object->setProperty(juce::String(info.id.data(), info.id.size()), field);
  });
  return object;
}

// Fields that are missing keep their defaults, and every field is clamped
// into its range.
template <Settings S> S settings_from_var(const juce::var &v) {
  S settings;
  visit_settings(settings, [&](const ParamInfo &info, auto &field) {
    const juce::var &value = v[juce::Identifier(juce::String(info.id.data(), info.id.size()))];
    if (!value.isVoid()) field = static_cast<std::remove_cvref_t<decltype(field)>>(value);
  });
  return clamped(settings);
}

juce::var to_var(const BrainDefinition &);
BrainDefinition brain_from_var(const juce::var &);
juce::var to_var(const ClipSettings &);
ClipSettings clip_from_var(const juce::var &);

// Reads what Key::hex() wrote.
template <class K> std::optional<K> key_from_string(const juce::String &s) {
  if (s.length() != 32 || !s.containsOnly("0123456789abcdef")) return std::nullopt;
  return K{std::uint64_t(s.substring(0, 16).getHexValue64()),
           std::uint64_t(s.substring(16).getHexValue64())};
}

} // namespace sb::plugin
