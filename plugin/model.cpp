#include "model.h"

namespace sb::plugin {

namespace {
// Old projects only have samples; assume 44.1 kHz.
BlockLength length_from_var(const juce::var &v, int old_samples) {
  if (!v.isVoid()) return settings_from_var<BlockLength>(v);
  BlockLength length;
  length.ms = float(old_samples) * 1000.f / 44100.f;
  return clamped(length);
}
} // namespace

juce::var to_var(const BrainDefinition &brain) {
  juce::Array<juce::var> samples;
  for (const BrainSample &s : brain.samples) {
    auto *sample = new juce::DynamicObject();
    sample->setProperty("path", s.path);
    sample->setProperty("enabled", s.enabled);
    samples.add(sample);
  }
  auto *object = new juce::DynamicObject();
  object->setProperty("id", brain.id);
  object->setProperty("name", brain.name);
  object->setProperty("samples", samples);
  object->setProperty("settings", to_var(brain.settings));
  object->setProperty("block", to_var(brain.block));
  return object;
}

BrainDefinition brain_from_var(const juce::var &v) {
  BrainDefinition brain;
  brain.id = v["id"].toString();
  brain.name = v["name"].toString();
  if (const auto *samples = v["samples"].getArray())
    for (const juce::var &s : *samples)
      brain.samples.push_back({s["path"].toString(), bool(s.getProperty("enabled", true))});
  brain.settings = settings_from_var<BrainSettings>(v["settings"]);
  brain.block = length_from_var(v["block"], brain.settings.block_size);
  return brain;
}

juce::var to_var(const ClipSettings &clip) {
  auto *object = new juce::DynamicObject();
  object->setProperty("brain", clip.brain_id);
  object->setProperty("target", to_var(clip.target));
  object->setProperty("target_block", to_var(clip.target_block));
  object->setProperty("render", to_var(clip.render));
  return object;
}

ClipSettings clip_from_var(const juce::var &v) {
  ClipSettings clip;
  clip.brain_id = v["brain"].toString();
  clip.target = settings_from_var<TargetSettings>(v["target"]);
  clip.target_block = length_from_var(v["target_block"], clip.target.block_size);
  clip.render = settings_from_var<RenderSettings>(v["render"]);
  return clip;
}

} // namespace sb::plugin
