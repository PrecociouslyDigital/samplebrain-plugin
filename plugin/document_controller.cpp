#include "document_controller.h"

#include <thread>

#include <ARA_Library/Utilities/ARATimelineConversion.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "playback_renderer.h"

namespace sb::plugin {

namespace {

constexpr int timer_ms = 50;
// Re-rendering waits this long after a change, so dragging a slider doesn't
// start a render for every step. A clip's first render skips the wait:
// REAPER takes back sample access about half a second after creating it.
const juce::RelativeTime render_pause = juce::RelativeTime::milliseconds(300);

// For clips the host gives no tempo for.
constexpr double default_tempo = 120.0;

// Where a clip starts on the timeline, if it's on it.
const juce::ARAPlaybackRegion *first_region(const SamplebrainAudioModification &m) {
  const juce::ARAPlaybackRegion *first = nullptr;
  for (auto *region : m.getPlaybackRegions<juce::ARAPlaybackRegion>())
    if (!first || region->getStartInPlaybackTime() < first->getStartInPlaybackTime())
      first = region;
  return first;
}

// Quarter notes per minute where the region starts.
double tempo_at(const juce::ARAPlaybackRegion &region) {
  using Reader = ARA::PlugIn::HostContentReader<ARA::kARAContentTypeTempoEntries>;
  const Reader reader(region.getRegionSequence()->getMusicalContext());
  if (!reader || reader.getEventCount() < 2) return default_tempo;
  const ARA::TempoConverter<Reader> converter(reader);
  const double start = region.getStartInPlaybackTime();
  constexpr double step = 0.001;
  return 60.0 * (converter.getQuarterForTime(start + step) - converter.getQuarterForTime(start)) /
         step;
}

double clip_tempo(const SamplebrainAudioModification &m) {
  const auto *region = first_region(m);
  return region ? tempo_at(*region) : default_tempo;
}

// Runs on the message thread if the target is still alive.
template <class T, class F> void post(juce::WeakReference<T> target, F &&f) {
  juce::MessageManager::callAsync([target, f = std::forward<F>(f)]() mutable {
    if (auto *t = target.get()) f(*t);
  });
}

} // namespace

// ---------------------------------------------------------------------------

SamplebrainAudioModification::SamplebrainAudioModification(
    juce::ARAAudioSource *source, ARA::ARAAudioModificationHostRef ref,
    const juce::ARAAudioModification *clone, ReaderRegistry &readers)
    : juce::ARAAudioModification(source, ref, clone), render(readers) {
  if (auto *original = static_cast<const SamplebrainAudioModification *>(clone))
    settings = original->settings;
}

void SamplebrainAudioModification::want(const std::variant<Waiting, RenderInputs> &want,
                                        juce::Time now) {
  if (const auto *waiting = std::get_if<Waiting>(&want)) {
    plan_ = *waiting;
  } else {
    const auto &inputs = std::get<RenderInputs>(want);
    const auto *scheduled = std::get_if<Scheduled>(&plan_);
    if (last_ && last_->inputs == inputs) plan_ = Settled{};
    else if (!scheduled || scheduled->inputs != inputs)
      plan_ = Scheduled{inputs, last_ ? now + render_pause : now};
  }
  update_pending();
}

std::optional<RenderInputs> SamplebrainAudioModification::due(juce::Time now) const {
  const auto *scheduled = std::get_if<Scheduled>(&plan_);
  if (!scheduled || scheduled->due > now) return std::nullopt;
  return scheduled->inputs;
}

void SamplebrainAudioModification::started(const RenderInputs &inputs,
                                           std::shared_ptr<JobProgress> progress) {
  if (last_)
    if (auto *r = std::get_if<Rendering>(&last_->outcome)) r->progress->cancel();
  last_ = Render{inputs, Rendering{std::move(progress)}};
  plan_ = Settled{};
  update_pending();
}

void SamplebrainAudioModification::finished(std::variant<Ready, Failed> outcome) {
  std::visit([&](auto o) { last_->outcome = std::move(o); }, std::move(outcome));
  update_pending();
}

RenderStatus SamplebrainAudioModification::status() const {
  if (const auto *waiting = std::get_if<Waiting>(&plan_)) return *waiting;
  if (!last_) return Idle{};
  return std::visit([](const auto &o) -> RenderStatus { return o; }, last_->outcome);
}

void SamplebrainAudioModification::update_pending() {
  const auto *waiting = std::get_if<Waiting>(&plan_);
  pending_ = scheduled() || std::holds_alternative<Rendering>(status()) ||
             (waiting && waiting->for_brain);
}

bool BrainState::needs_build() const {
  return std::ranges::any_of(current, [&](const auto &entry) {
    auto it = playing.find(entry.first);
    return it == playing.end() || it->second.brain.key != entry.second.key;
  });
}

std::vector<bool> BrainState::sound_enabled(const PlayingBrain &brain) const {
  std::vector<bool> enabled;
  for (const juce::String &path : brain.paths) {
    auto it = std::ranges::find(definition.samples, path, &BrainSample::path);
    enabled.push_back(it == definition.samples.end() || it->enabled);
  }
  return enabled;
}

// ---------------------------------------------------------------------------

// The ARA document is already gone by now, so this doesn't look at it.
SamplebrainDocumentController::~SamplebrainDocumentController() {
  stopTimer();
  for (const auto &job : jobs_)
    if (auto progress = job.lock()) progress->cancel();
  engine_->pool().removeAllJobs(true, 10000);
}

std::shared_ptr<JobProgress> SamplebrainDocumentController::new_job() {
  std::erase_if(jobs_, [](const auto &job) { return job.expired(); });
  auto job = std::make_shared<JobProgress>();
  jobs_.push_back(job);
  return job;
}

SamplebrainDocumentController *
SamplebrainDocumentController::get(ARA::PlugIn::DocumentController *dc) {
  return getSpecialisedDocumentController<SamplebrainDocumentController>(dc);
}

std::vector<SamplebrainAudioModification *>
SamplebrainDocumentController::modifications() {
  std::vector<SamplebrainAudioModification *> all;
  // The host can destroy the document while job callbacks and the timer
  // still run.
  auto *document = getDocumentController()->getDocument<juce::ARADocument>();
  if (!document) return all;
  for (auto *source : document->getAudioSources<juce::ARAAudioSource>())
    for (auto *m : source->getAudioModifications<SamplebrainAudioModification>())
      all.push_back(m);
  return all;
}

BrainState *SamplebrainDocumentController::find_brain(const juce::String &id) {
  auto it = std::ranges::find(brains_, id, [](auto &b) { return b->definition.id; });
  return it == brains_.end() ? nullptr : it->get();
}

BrainState &SamplebrainDocumentController::add_brain() {
  auto &brain = *brains_.emplace_back(std::make_unique<BrainState>());
  brain.definition.name = "Brain " + juce::String(brains_.size());
  for (auto *m : modifications())
    if (!find_brain(m->settings.brain_id)) m->settings.brain_id = brain.definition.id;
  changed();
  return brain;
}

int SamplebrainDocumentController::clips_using(const juce::String &brain_id) {
  return int(std::ranges::count(modifications(), brain_id,
                                [](auto *m) { return m->settings.brain_id; }));
}

std::vector<unsigned>
SamplebrainDocumentController::rates_for(const juce::String &brain_id) {
  std::vector<unsigned> rates;
  for (auto *m : modifications())
    if (m->settings.brain_id == brain_id) {
      const auto rate = unsigned(m->getAudioSource()->getSampleRate());
      if (std::ranges::find(rates, rate) == rates.end()) rates.push_back(rate);
    }
  return rates;
}

double SamplebrainDocumentController::brain_tempo(const juce::String &brain_id) {
  const juce::ARAPlaybackRegion *first = nullptr;
  for (auto *m : modifications())
    if (m->settings.brain_id == brain_id)
      if (auto *region = first_region(*m);
          region && (!first || region->getStartInPlaybackTime() < first->getStartInPlaybackTime()))
        first = region;
  return first ? tempo_at(*first) : default_tempo;
}

void SamplebrainDocumentController::edit_brain(
    const juce::String &id, const std::function<void(BrainDefinition &)> &edit) {
  if (auto *brain = find_brain(id)) {
    edit(brain->definition);
    brain_changed(*brain);
  }
}

void SamplebrainDocumentController::brain_changed(BrainState &brain) {
  ++brain.revision;
  refresh_keys(brain);
  changed();
}

template <class Work, class Apply>
void SamplebrainDocumentController::load(BrainState &brain, Work work, Apply apply) {
  ++brain.loading;
  engine_->pool().addJob(
      [self = juce::WeakReference(this), id = brain.definition.id, work, apply] {
        std::optional<decltype(work())> result;
        try {
          result = work();
        } catch (...) {
        }
        post(self, [=](SamplebrainDocumentController &dc) {
          BrainState *b = dc.find_brain(id);
          if (!b) return;
          --b->loading;
          if (result) apply(*b, *result);
          dc.changed();
        });
      });
}

namespace {
// What refresh_keys finds: the sounds, and the brain if it's already built.
struct KeyLookup {
  std::optional<LoadedSounds> sounds;
  std::optional<LoadedBrain> built;
  std::string error;
};
} // namespace

// Works out the key the brain's samples and settings give now, and loads
// that brain straight away if it's already built.
void SamplebrainDocumentController::refresh_keys(BrainState &brain) {
  brain.tempo = brain_tempo(brain.definition.id);
  for (unsigned rate : rates_for(brain.definition.id))
    load(
        brain,
        [engine = engine_, definition = brain.definition.at(rate, brain.tempo), rate] {
          KeyLookup found;
          try {
            auto sounds = engine->load_sounds(definition, rate);
            if (!sounds) {
              found.error = sounds.error();
              return found;
            }
            found.sounds = *sounds;
            auto built = engine->brain(definition, *sounds, rate, false);
            if (built) found.built = *built;
            else found.error = built.error();
          } catch (const std::exception &e) {
            found.error = e.what();
          }
          return found;
        },
        [revision = brain.revision, rate](BrainState &b, const KeyLookup &found) {
          if (b.revision != revision) return;
          b.error = found.error;
          if (found.sounds) b.current.insert_or_assign(rate, *found.sounds);
          if (found.built)
            b.playing.insert_or_assign(rate, PlayingBrain{*found.built, b.definition.paths()});
        });
}

void SamplebrainDocumentController::build_brain(const juce::String &id) {
  BrainState *brain = find_brain(id);
  if (!brain || brain->build) return;
  std::vector<unsigned> rates = rates_for(id);
  if (rates.empty()) rates.push_back(44100);

  auto progress = new_job();
  brain->build = progress;
  brain->error.clear();
  changed();
  engine_->pool().addJob([engine = engine_, self = juce::WeakReference(this),
                          definition = brain->definition, revision = brain->revision,
                          bpm = brain_tempo(id), rates, progress] {
    ProgressScope scope(progress.get());
    std::map<unsigned, LoadedSounds> sounds;
    std::map<unsigned, LoadedBrain> built;
    std::string error;
    try {
      for (unsigned rate : rates) {
        progress->report("reading samples");
        const BrainDefinition at = definition.at(rate, bpm);
        auto s = engine->load_sounds(at, rate);
        if (!s) {
          error = s.error();
          break;
        }
        auto b = engine->brain(at, *s, rate, true);
        if (!b) {
          error = b.error();
          break;
        }
        sounds.emplace(rate, *s);
        built.emplace(rate, **b);
      }
    } catch (const Cancelled &) {
      error = "cancelled";
    } catch (const std::exception &e) {
      error = e.what();
    }
    post(self, [=](SamplebrainDocumentController &dc) {
      BrainState *b = dc.find_brain(definition.id);
      if (!b) return;
      b->build.reset();
      b->error = error;
      const std::vector<juce::String> paths = definition.paths();
      for (const auto &[rate, brain] : built) {
        b->playing.insert_or_assign(rate, PlayingBrain{brain, paths});
        if (b->revision == revision) b->current.insert_or_assign(rate, sounds.at(rate));
      }
      dc.changed();
    });
  });
}

void SamplebrainDocumentController::cancel_build(const juce::String &id) {
  if (BrainState *brain = find_brain(id); brain && brain->build) brain->build->cancel();
}

void SamplebrainDocumentController::export_brain(const juce::String &id,
                                                 const juce::File &file) {
  BrainState *brain = find_brain(id);
  if (!brain || brain->playing.empty()) return;
  const LoadedBrain loaded = brain->playing.begin()->second.brain;
  engine_->pool().addJob([engine = engine_, self = juce::WeakReference(this), id,
                          loaded, file] {
    auto result = engine->export_brain(loaded, file);
    post(self, [=](SamplebrainDocumentController &dc) {
      if (BrainState *b = dc.find_brain(id)) b->error = result ? "" : result.error();
      dc.changed();
    });
  });
}

void SamplebrainDocumentController::import_brain(
    const juce::File &file, std::function<void(const juce::String &)> done) {
  if (import_) return;
  import_ = new_job();
  import_error_.clear();
  changed();
  engine_->pool().addJob([engine = engine_, self = juce::WeakReference(this), file,
                          progress = import_, done = std::move(done)] {
    ProgressScope scope(progress.get());
    std::expected<ImportedBrain, std::string> imported = std::unexpected("cancelled");
    try {
      imported = engine->import_brain(file);
    } catch (const Cancelled &) {
    } catch (const std::exception &e) {
      imported = std::unexpected(e.what());
    }
    post(self, [=, imported = std::move(imported)](SamplebrainDocumentController &dc) {
      dc.import_.reset();
      if (!imported) {
        dc.import_error_ = imported.error();
        dc.changed();
        return;
      }
      auto &brain = *dc.brains_.emplace_back(std::make_unique<BrainState>());
      brain.definition = imported->definition;
      const std::vector<juce::String> paths = brain.definition.paths();
      brain.playing.emplace(imported->rate, PlayingBrain{imported->brain, paths});
      brain.current.emplace(imported->rate, imported->sounds);
      dc.changed();
      done(brain.definition.id);
    });
  });
}

void SamplebrainDocumentController::edit_clip(
    SamplebrainAudioModification &m, const std::function<void(ClipSettings &)> &edit) {
  const juce::String old_brain = m.settings.brain_id;
  edit(m.settings);
  if (m.settings.brain_id != old_brain)
    if (BrainState *brain = find_brain(m.settings.brain_id)) refresh_keys(*brain);
  changed();
}

void SamplebrainDocumentController::changed() {
  ++version_;
  for (auto &brain : brains_) {
    const double bpm = brain_tempo(brain->definition.id);
    const auto &block = brain->definition.block;
    if (std::ranges::any_of(rates_for(brain->definition.id), [&](unsigned rate) {
          return block_samples(block, rate, bpm) != block_samples(block, rate, brain->tempo);
        })) {
      ++brain->revision;
      refresh_keys(*brain);
    }
  }
  const auto now = juce::Time::getCurrentTime();
  bool scheduled = false;
  for (auto *m : modifications()) {
    m->want(wanted(*m), now);
    scheduled |= m->scheduled();
  }
  if (scheduled && !isTimerRunning()) startTimer(timer_ms);
}

std::variant<Waiting, RenderInputs>
SamplebrainDocumentController::wanted(SamplebrainAudioModification &m) {
  auto *source = m.getAudioSource();
  if (!source->isSampleAccessEnabled())
    return Waiting{"waiting for the host to share the clip's audio"};
  BrainState *brain = find_brain(m.settings.brain_id);
  if (!brain) return Waiting{"choose a brain"};
  const auto rate = unsigned(source->getSampleRate());
  auto playing = brain->playing.find(rate);
  if (playing == brain->playing.end())
    return brain->busy() ? Waiting{"waiting for the brain", true} : Waiting{"build the brain"};

  RenderSettings render = m.settings.render;
  render.sound_enabled = brain->sound_enabled(playing->second);
  return RenderInputs{playing->second.brain, m.settings.target_at(rate, clip_tempo(m)), render,
                      rate, m.content};
}

void SamplebrainDocumentController::start_render(SamplebrainAudioModification &m,
                                                 const RenderInputs &inputs) {
  auto progress = new_job();
  m.started(inputs, progress);
  const std::uint64_t generation = ++*m.generation;
  auto reader = std::make_shared<juce::ARAAudioSourceReader>(m.getAudioSource());
  changed();

  engine_->pool().addJob([engine = engine_, self = juce::WeakReference(this),
                          mod = juce::WeakReference(&m), reader, brain = inputs.brain,
                          target_settings = inputs.target, render = inputs.render,
                          rate = inputs.rate, progress, generation,
                          current = m.generation]() mutable {
    ProgressScope scope(progress.get());
    std::expected<std::shared_ptr<const RenderCache>, std::string> result =
        std::unexpected("cancelled");
    try {
      progress->report("reading clip");
      const auto channels = std::min(2, int(reader->numChannels));
      const auto length = int(reader->lengthInSamples);
      juce::AudioBuffer<float> buffer(channels, length);
      constexpr int chunk = 1 << 16;
      for (int pos = 0; pos < length; pos += chunk) {
        if (*current != generation) throw Cancelled{};
        const int n = std::min(chunk, length - pos);
        if (!reader->read(&buffer, pos, n, pos, true, channels > 1))
          throw std::runtime_error("couldn't read the clip's audio");
      }
      ClipAudio clip;
      for (int c = 0; c < channels; ++c)
        clip.channels.emplace_back(buffer.getReadPointer(c), buffer.getReadPointer(c) + length);
      const auto target = engine->target(clip, target_settings, rate);
      if (!target) throw std::runtime_error(target.error());
      result = engine->render(brain, *target, render);
    } catch (const Cancelled &) {
    } catch (const std::exception &e) {
      result = std::unexpected(e.what());
    }

    // The reader listens to the audio source, so it's released on the
    // message thread.
    post(self, [=, reader = std::move(reader)](SamplebrainDocumentController &dc) mutable {
      reader.reset();
      auto *m = mod.get();
      if (!m || *m->generation != generation) return;
      if (result) {
        m->render.publish(*result);
        m->finished(Ready{});
        m->notifyContentChanged(juce::ARAContentUpdateScopes::samplesAreAffected(), true);
      } else if (result.error() != "cancelled") {
        m->finished(Failed{result.error()});
      }
      dc.changed();
    });
  });
}

void SamplebrainDocumentController::timerCallback() {
  const auto now = juce::Time::getCurrentTime();
  bool waiting = false;
  for (auto *m : modifications()) {
    m->render.collect();
    if (!editing_)
      if (auto inputs = m->due(now)) start_render(*m, *inputs);
    waiting |= m->scheduled() || m->render.has_retired();
  }
  if (!waiting) stopTimer();
}

// ---------------------------------------------------------------------------
// ARA model callbacks

void SamplebrainDocumentController::willBeginEditing(juce::ARADocument *) {
  editing_ = true;
  // Let any playback callback that started before the flag finish.
  const auto reading = readers_.readers_reading();
  while (!readers_.finished(reading)) std::this_thread::yield();
}

void SamplebrainDocumentController::didEndEditing(juce::ARADocument *) {
  editing_ = false;
#ifdef SB_TEST_HOOKS
  if (brains_.empty()) add_test_brain();
#endif
  changed();
}

#ifdef SB_TEST_HOOKS
// For testing in a host without its GUI: SAMPLEBRAIN_TEST_SAMPLES lists
// sample files, separated by colons, to build a brain from.
void SamplebrainDocumentController::add_test_brain() {
  const juce::String samples =
      juce::SystemStats::getEnvironmentVariable("SAMPLEBRAIN_TEST_SAMPLES", {});
  if (samples.isEmpty() || modifications().empty()) return;
  BrainState &brain = add_brain();
  for (const auto &path : juce::StringArray::fromTokens(samples, ":", ""))
    brain.definition.samples.push_back({path, true});
  brain_changed(brain);
  build_brain(brain.definition.id);
}
#endif

juce::ARAAudioModification *SamplebrainDocumentController::doCreateAudioModification(
    juce::ARAAudioSource *source, ARA::ARAAudioModificationHostRef ref,
    const juce::ARAAudioModification *clone) noexcept {
  auto *m = new SamplebrainAudioModification(source, ref, clone, readers_);
  if (!clone && !brains_.empty()) m->settings.brain_id = brains_.front()->definition.id;
  return m;
}

juce::ARAPlaybackRenderer *SamplebrainDocumentController::doCreatePlaybackRenderer() noexcept {
  return new SamplebrainPlaybackRenderer(getDocumentController(), *this);
}

void SamplebrainDocumentController::didEnableAudioSourceSamplesAccess(
    juce::ARAAudioSource *, bool) {
  changed();
}

void SamplebrainDocumentController::doUpdateMusicalContextContent(
    juce::ARAMusicalContext *, juce::ARAContentUpdateScopes) {
  changed();
}

void SamplebrainDocumentController::doUpdateAudioSourceContent(
    juce::ARAAudioSource *source, juce::ARAContentUpdateScopes scopes) {
  if (!scopes.affectSamples()) return;
  for (auto *m : source->getAudioModifications<SamplebrainAudioModification>()) ++m->content;
  changed();
}

// ---------------------------------------------------------------------------
// Saving

namespace {
constexpr int archive_version = 1;
}

bool SamplebrainDocumentController::doStoreObjectsToStream(
    juce::ARAOutputStream &output, const juce::ARAStoreObjectsFilter *filter) noexcept {
  juce::Array<juce::var> brains;
  for (const auto &b : brains_) {
    juce::var brain = to_var(b->definition);
    auto *playing = new juce::DynamicObject();
    for (const auto &[rate, p] : b->playing) {
      auto *entry = new juce::DynamicObject();
      entry->setProperty("key", juce::String(p.brain.key.hex()));
      juce::Array<juce::var> paths;
      for (const auto &path : p.paths) paths.add(path);
      entry->setProperty("paths", paths);
      playing->setProperty(juce::String(rate), entry);
    }
    brain.getDynamicObject()->setProperty("playing", playing);
    brains.add(brain);
  }

  auto *clips = new juce::DynamicObject();
  for (auto *m : filter->getAudioModificationsToStore<SamplebrainAudioModification>())
    clips->setProperty(juce::String(m->getPersistentID()), to_var(m->settings));

  auto *archive = new juce::DynamicObject();
  archive->setProperty("version", archive_version);
  archive->setProperty("brains", brains);
  archive->setProperty("clips", clips);
  return output.writeString(juce::JSON::toString(juce::var(archive), true));
}

bool SamplebrainDocumentController::doRestoreObjectsFromStream(
    juce::ARAInputStream &input, const juce::ARARestoreObjectsFilter *filter) noexcept {
  const juce::var archive = juce::JSON::parse(input.readString());
  if (input.failed() || int(archive["version"]) != archive_version) return false;

  if (const auto *brains = archive["brains"].getArray())
    for (const juce::var &v : *brains) {
      BrainDefinition definition = brain_from_var(v);
      BrainState *brain = find_brain(definition.id);
      if (!brain) {
        brain = brains_.emplace_back(std::make_unique<BrainState>()).get();
      }
      brain->definition = std::move(definition);
      ++brain->revision;

      // Load the brains clips were playing by key, so they play even if
      // their sample files have moved.
      if (const auto *playing = v["playing"].getDynamicObject())
        for (const auto &entry : playing->getProperties()) {
          const auto rate = unsigned(entry.name.toString().getIntValue());
          const auto key = key_from_string<BrainKey>(entry.value["key"].toString());
          if (!key) continue;
          std::vector<juce::String> paths;
          if (const auto *p = entry.value["paths"].getArray())
            for (const auto &path : *p) paths.push_back(path.toString());
          load(
              *brain, [engine = engine_, key = *key] { return engine->cached_brain(key); },
              [rate, paths](BrainState &b, const std::optional<LoadedBrain> &loaded) {
                if (loaded) b.playing.try_emplace(rate, PlayingBrain{*loaded, paths});
              });
        }
    }

  if (const auto *clips = archive["clips"].getDynamicObject())
    for (const auto &entry : clips->getProperties()) {
      auto *m = filter->getAudioModificationToRestoreStateWithID<SamplebrainAudioModification>(
          entry.name.toString().toRawUTF8());
      if (!m) continue;
      m->settings = clip_from_var(entry.value);
    }

  for (auto &brain : brains_) refresh_keys(*brain);
  changed();
  return true;
}

} // namespace sb::plugin
