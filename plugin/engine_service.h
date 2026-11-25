#pragma once

// The engine behind its caches. Every function here blocks and is safe to
// call from any thread, so jobs on the pool call them directly.

#include <juce_core/juce_core.h>

#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "cache/disk_cache.h"
#include "cache/memo_cache.h"
#include "core/core.h"
#include "core/keys.h"
#include "core/progress.h"
#include "model.h"

namespace sb::plugin {

struct LoadedSound {
  Mono audio;
  AudioHash hash;
};

// A brain's sounds, read and resampled to one rate.
struct LoadedSounds {
  std::vector<std::shared_ptr<const LoadedSound>> sounds;
  BrainKey key;
};

struct LoadedBrain {
  BrainKey key;
  std::shared_ptr<const Brain> brain;
  bool operator==(const LoadedBrain &other) const { return key == other.key; }
};

// A brain read from a .brain file, with its sounds saved as samples.
struct ImportedBrain {
  BrainDefinition definition;
  LoadedSounds sounds;
  LoadedBrain brain;
  unsigned rate;
};

struct LoadedTarget {
  TargetKey key;
  std::shared_ptr<const Target> target;
};

// A clip's render and the rate it was rendered at.
struct RenderCache {
  RenderKey key;
  unsigned rate;
  std::shared_ptr<const RenderedAudio> audio;
};

// Progress of one job, shared between the job and the editor. Cancelling
// makes the job's next progress report throw.
class JobProgress : public ProgressSink {
public:
  void report(std::string_view text) override;
  void sound_state(std::string_view sound, SoundBuildState) override;

  void cancel() { cancelled_ = true; }
  bool cancelled() const { return cancelled_; }
  std::string text() const;
  std::optional<SoundBuildState> state_of(const std::string &sound) const;

private:
  std::atomic<bool> cancelled_ = false;
  mutable std::mutex mutex_;
  std::string text_;
  std::map<std::string, SoundBuildState, std::less<>> sounds_;
};

// Caches sounds, brains, targets and renders in memory, and brains and
// renders on disk too.
class EngineService {
public:
  EngineService();

  // The key it returns is the brain these sounds and settings would build.
  std::expected<LoadedSounds, std::string> load_sounds(const BrainDefinition &,
                                                       unsigned rate);

  // The brain for these sounds, from memory or disk. Builds it only when
  // asked to, and returns nullopt if it isn't built.
  std::expected<std::optional<LoadedBrain>, std::string>
  brain(const BrainDefinition &, const LoadedSounds &, unsigned rate, bool build);

  // A brain by key alone, for when its sounds can't be read.
  std::optional<LoadedBrain> cached_brain(const BrainKey &);

  // Reads a .brain file. Its sounds become WAV files in the user's
  // application data, and the brain is cached under the key they give.
  std::expected<ImportedBrain, std::string> import_brain(const juce::File &);

  // Writes the brain to a user-chosen .brain file.
  std::expected<void, std::string> export_brain(const LoadedBrain &,
                                                const juce::File &);

  // Analyses a clip, for rendering against a brain.
  std::expected<LoadedTarget, std::string> target(const ClipAudio &, const TargetSettings &,
                                                  unsigned rate);

  // The brain's rendition of the clip.
  std::expected<std::shared_ptr<const RenderCache>, std::string>
  render(const LoadedBrain &, const LoadedTarget &, const RenderSettings &);

  // Where jobs that call this service run.
  juce::ThreadPool &pool() { return pool_; }

private:
  std::expected<std::shared_ptr<const LoadedSound>, std::string>
  sound(const juce::File &, unsigned rate);

  DiskCache brain_disk_;
  DiskCache render_disk_;
  MemoCache<std::string, LoadedSound, std::string> sounds_;
  MemoCache<BrainKey, Brain, std::string> brains_;
  MemoCache<TargetKey, Target, std::string> targets_;
  MemoCache<RenderKey, RenderCache, std::string> renders_;
  juce::ThreadPool pool_;
};

} // namespace sb::plugin
