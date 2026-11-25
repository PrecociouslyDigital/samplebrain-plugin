#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>
#include <memory>
#include <variant>

#include "engine_service.h"
#include "model.h"
#include "realtime/published.h"

namespace sb::plugin {

class SamplebrainDocumentController;

// What a clip's render is doing, for the editor.
struct Idle {};
struct Waiting {
  std::string reason;
  // Whether it's waiting for its brain to load or build, rather than for
  // the user.
  bool for_brain = false;
};
struct Rendering {
  std::shared_ptr<JobProgress> progress;
};
struct Ready {};
struct Failed {
  std::string message;
};
using RenderStatus = std::variant<Idle, Waiting, Rendering, Ready, Failed>;

// Everything a clip's render depends on. A clip renders again when these
// differ from its last render's.
struct RenderInputs {
  LoadedBrain brain;
  TargetSettings target;
  RenderSettings render;
  unsigned rate;
  std::uint64_t content;
  bool operator==(const RenderInputs &) const = default;
};

// One clip: its settings, its render's status, and the render being played.
class SamplebrainAudioModification : public juce::ARAAudioModification {
public:
  SamplebrainAudioModification(juce::ARAAudioSource *, ARA::ARAAudioModificationHostRef,
                               const juce::ARAAudioModification *clone,
                               ReaderRegistry &);

  ClipSettings settings;
  Published<RenderCache> render;
  // Bumped when the host changes the clip's audio.
  std::uint64_t content = 0;

  // Bumped for every new render, so older ones can tell they're stale.
  std::shared_ptr<std::atomic<std::uint64_t>> generation =
      std::make_shared<std::atomic<std::uint64_t>>(0);

  // Message thread. The controller states what the clip should play: either
  // waiting on something, or these inputs, which schedule a render unless
  // the last render already used them.
  void want(const std::variant<Waiting, RenderInputs> &, juce::Time now);
  bool scheduled() const { return std::holds_alternative<Scheduled>(plan_); }
  // The scheduled render's inputs, once it's due.
  std::optional<RenderInputs> due(juce::Time now) const;
  void started(const RenderInputs &, std::shared_ptr<JobProgress>);
  void finished(std::variant<Ready, Failed>);

  RenderStatus status() const;
  // Whether a render is due, running, or waiting on its brain. Offline
  // renders wait for this to clear.
  bool pending() const noexcept { return pending_.load(); }

private:
  struct Settled {};
  struct Scheduled {
    RenderInputs inputs;
    juce::Time due;
  };
  struct Render {
    RenderInputs inputs;
    std::variant<Rendering, Ready, Failed> outcome;
  };

  void update_pending();

  // While settled or scheduled, the clip shows how its last render went.
  std::variant<Settled, Waiting, Scheduled> plan_;
  std::optional<Render> last_;
  std::atomic<bool> pending_ = false;

  JUCE_DECLARE_WEAK_REFERENCEABLE(SamplebrainAudioModification)
};

// A built brain and the sample paths it was built from, which can differ
// from the definition's until the next build.
struct PlayingBrain {
  LoadedBrain brain;
  std::vector<juce::String> paths;
};

// A project brain and what's been built from it, per rate.
struct BrainState {
  BrainDefinition definition;
  // The brain clips at each rate are playing.
  std::map<unsigned, PlayingBrain> playing;
  // The sounds and key the definition gives now, at each rate.
  std::map<unsigned, LoadedSounds> current;
  std::shared_ptr<JobProgress> build;
  std::string error;
  // Sample paths that couldn't be read.
  std::vector<juce::String> missing;
  // Bumped for every definition change, so older key jobs can tell.
  std::uint64_t revision = 0;
  // Jobs looking for or loading a built brain.
  int loading = 0;
  // The tempo its keys were last looked up at.
  double tempo = 0;

  bool busy() const { return loading > 0 || build != nullptr; }

  bool needs_build() const;
  // Which of the playing brain's sounds are switched on.
  std::vector<bool> sound_enabled(const PlayingBrain &) const;
};

class SamplebrainDocumentController : public juce::ARADocumentControllerSpecialisation,
                                      private juce::Timer {
public:
  using ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;
  ~SamplebrainDocumentController() override;

  static SamplebrainDocumentController *get(ARA::PlugIn::DocumentController *);

  // Message thread. The editor reads and edits the project through these.
  const std::vector<std::unique_ptr<BrainState>> &brains() const { return brains_; }
  BrainState *find_brain(const juce::String &id);
  BrainState &add_brain();
  void edit_brain(const juce::String &id, const std::function<void(BrainDefinition &)> &);
  // Builds the brain for every rate its clips use.
  void build_brain(const juce::String &id);
  void cancel_build(const juce::String &id);
  void export_brain(const juce::String &id, const juce::File &);
  // Adds a brain read from a .brain file, and calls done with its id.
  void import_brain(const juce::File &, std::function<void(const juce::String &)> done);
  // Progress of an import, if one is running.
  std::shared_ptr<JobProgress> import_progress() const { return import_; }
  const std::string &import_error() const { return import_error_; }
  int clips_using(const juce::String &brain_id);

  // Re-renders the clip after a short pause.
  void edit_clip(SamplebrainAudioModification &, const std::function<void(ClipSettings &)> &);

  // Bumped whenever something the editor shows changes.
  std::uint64_t version() const { return version_; }

  // Audio thread: playback renderers register so published renders wait
  // for them, and play silence while the host edits the document.
  ReaderRegistry &readers() { return readers_; }
  bool editing() const noexcept [[clang::nonblocking]] { return editing_.load(); }

protected:
  void willBeginEditing(juce::ARADocument *) override;
  void didEndEditing(juce::ARADocument *) override;

  juce::ARAAudioModification *
  doCreateAudioModification(juce::ARAAudioSource *, ARA::ARAAudioModificationHostRef,
                            const juce::ARAAudioModification *) noexcept override;
  juce::ARAPlaybackRenderer *doCreatePlaybackRenderer() noexcept override;

  bool doRestoreObjectsFromStream(juce::ARAInputStream &,
                                  const juce::ARARestoreObjectsFilter *) noexcept override;
  bool doStoreObjectsToStream(juce::ARAOutputStream &,
                              const juce::ARAStoreObjectsFilter *) noexcept override;

private:
  void didEnableAudioSourceSamplesAccess(juce::ARAAudioSource *, bool enable) override;
  void doUpdateMusicalContextContent(juce::ARAMusicalContext *,
                                     juce::ARAContentUpdateScopes) override;
  void doUpdateAudioSourceContent(juce::ARAAudioSource *,
                                  juce::ARAContentUpdateScopes) override;
  void timerCallback() override;

  std::vector<SamplebrainAudioModification *> modifications();
  std::vector<unsigned> rates_for(const juce::String &brain_id);
  // The tempo where the earliest clip using the brain starts.
  double brain_tempo(const juce::String &brain_id);
  void brain_changed(BrainState &);
  void refresh_keys(BrainState &);
  // Runs work on the pool, counting as one of the brain's loads until it
  // finishes, then hands its result to apply unless it threw.
  template <class Work, class Apply> void load(BrainState &, Work, Apply);
  std::variant<Waiting, RenderInputs> wanted(SamplebrainAudioModification &);
  void start_render(SamplebrainAudioModification &, const RenderInputs &);
  // Call after every change: the editor redraws, and each clip works out
  // what it should play.
  void changed();
#ifdef SB_TEST_HOOKS
  void add_test_brain();
#endif
  std::shared_ptr<JobProgress> new_job();

  std::shared_ptr<EngineService> engine_ = std::make_shared<EngineService>();
  ReaderRegistry readers_;
  std::atomic<bool> editing_ = false;
  std::vector<std::unique_ptr<BrainState>> brains_;
  std::shared_ptr<JobProgress> import_;
  // Every running job's progress, so they can all be cancelled.
  std::vector<std::weak_ptr<JobProgress>> jobs_;
  std::string import_error_;
  std::uint64_t version_ = 0;

  JUCE_DECLARE_WEAK_REFERENCEABLE(SamplebrainDocumentController)
};

} // namespace sb::plugin
