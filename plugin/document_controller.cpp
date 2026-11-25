#include "document_controller.h"

#include <thread>

#include <juce_audio_formats/juce_audio_formats.h>

#include "playback_renderer.h"

namespace sb::plugin {

namespace {

constexpr int timer_ms = 50;
// Rendering waits this long after a settings change, so dragging a slider
// doesn't start a render for every step.
const juce::RelativeTime render_pause = juce::RelativeTime::milliseconds(300);

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

void SamplebrainAudioModification::set_status(RenderStatus status) {
  status_ = std::move(status);
  update_pending();
}

void SamplebrainAudioModification::set_render_due(std::optional<juce::Time> due) {
  render_due_ = due;
  update_pending();
}

void SamplebrainAudioModification::update_pending() {
  const auto *waiting = std::get_if<Waiting>(&status_);
  pending_ = render_due_.has_value() || std::holds_alternative<Rendering>(status_) ||
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
  for (auto *source : getDocumentController()
                          ->getDocument<juce::ARADocument>()
                          ->getAudioSources<juce::ARAAudioSource>())
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
  // Clips keep playing the old brain until a new one is built, but which of
  // its samples are switched on can change straight away.
  render_brain_clips(brain.definition.id);
  changed();
}

// Works out the key the brain's samples and settings give now, and loads
// that brain straight away if it's already built.
void SamplebrainDocumentController::refresh_keys(BrainState &brain) {
  for (unsigned rate : rates_for(brain.definition.id)) {
    ++brain.loading;
    engine_->pool().addJob([engine = engine_, self = juce::WeakReference(this),
                            definition = brain.definition, revision = brain.revision,
                            rate] {
      auto sounds = engine->load_sounds(definition, rate);
      std::optional<LoadedBrain> built;
      std::string error;
      if (sounds) {
        try {
          auto found = engine->brain(definition, *sounds, rate, false);
          if (found) built = *found;
          else error = found.error();
        } catch (const std::exception &e) {
          error = e.what();
        }
      } else {
        error = sounds.error();
      }
      post(self, [=, sounds = std::move(sounds)](SamplebrainDocumentController &dc) {
        BrainState *b = dc.find_brain(definition.id);
        if (!b) return;
        --b->loading;
        if (b->revision == revision) {
          b->error = error;
          if (sounds) b->current.insert_or_assign(rate, *sounds);
          if (built) b->playing.insert_or_assign(rate, PlayingBrain{*built, definition.paths()});
        }
        if (built || !b->busy()) dc.render_brain_clips(definition.id);
        dc.changed();
      });
    });
  }
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
                          rates, progress] {
    ProgressScope scope(progress.get());
    std::map<unsigned, LoadedSounds> sounds;
    std::map<unsigned, LoadedBrain> built;
    std::string error;
    try {
      for (unsigned rate : rates) {
        progress->report("reading samples");
        auto s = engine->load_sounds(definition, rate);
        if (!s) {
          error = s.error();
          break;
        }
        auto b = engine->brain(definition, *s, rate, true);
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
      dc.render_brain_clips(definition.id);
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
  schedule_render(m, render_pause);
  changed();
}

void SamplebrainDocumentController::render_brain_clips(const juce::String &brain_id) {
  for (auto *m : modifications())
    if (m->settings.brain_id == brain_id) schedule_render(*m, render_pause);
}

void SamplebrainDocumentController::schedule_render(SamplebrainAudioModification &m,
                                                    juce::RelativeTime delay) {
  m.set_render_due(juce::Time::getCurrentTime() + delay);
  if (!isTimerRunning()) startTimer(timer_ms);
}

void SamplebrainDocumentController::start_render(SamplebrainAudioModification &m) {
  if (auto *r = std::get_if<Rendering>(&m.status())) r->progress->cancel();

  // The status changes once, straight from one state to the next, so an
  // offline render waiting on pending() never sees it drop in between.
  auto *source = m.getAudioSource();
  BrainState *brain = find_brain(m.settings.brain_id);
  const auto rate = unsigned(source->getSampleRate());
  const PlayingBrain *playing = nullptr;
  if (brain)
    if (auto it = brain->playing.find(rate); it != brain->playing.end()) playing = &it->second;

  std::optional<Waiting> waiting;
  if (!source->isSampleAccessEnabled())
    waiting = Waiting{"waiting for the host to share the clip's audio"};
  else if (!brain)
    waiting = Waiting{"choose a brain"};
  else if (!playing && brain->busy())
    waiting = Waiting{"waiting for the brain", true};
  else if (!playing)
    waiting = Waiting{"build the brain"};
  if (waiting) {
    m.set_status(*waiting);
    m.set_render_due(std::nullopt);
    changed();
    return;
  }

  RenderSettings render = m.settings.render;
  render.sound_enabled = brain->sound_enabled(*playing);
  auto progress = new_job();
  m.set_status(Rendering{progress});
  m.set_render_due(std::nullopt);
  const std::uint64_t generation = ++*m.generation;
  auto reader = std::make_shared<juce::ARAAudioSourceReader>(source);
  changed();

  engine_->pool().addJob([engine = engine_, self = juce::WeakReference(this),
                          mod = juce::WeakReference(&m), reader,
                          brain = playing->brain, target_settings = m.settings.target,
                          render, rate, progress, generation,
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
        m->set_status(Ready{});
        m->notifyContentChanged(juce::ARAContentUpdateScopes::samplesAreAffected(), true);
      } else if (result.error() != "cancelled") {
        m->set_status(Failed{result.error()});
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
    if (m->render_due() && *m->render_due() <= now && !editing_) start_render(*m);
    waiting |= m->render_due().has_value() || m->render.has_retired();
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
  if (brains_.empty()) add_test_brain();
  for (auto *m : modifications())
    if (std::holds_alternative<Idle>(m->status()) && !m->render_due()) schedule_render(*m);
  changed();
}

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
    juce::ARAAudioSource *source, bool enable) {
  if (!enable) return;
  for (auto *m : source->getAudioModifications<SamplebrainAudioModification>())
    schedule_render(*m);
}

void SamplebrainDocumentController::doUpdateAudioSourceContent(
    juce::ARAAudioSource *source, juce::ARAContentUpdateScopes scopes) {
  if (!scopes.affectSamples()) return;
  for (auto *m : source->getAudioModifications<SamplebrainAudioModification>())
    schedule_render(*m);
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
          ++brain->loading;
          engine_->pool().addJob([engine = engine_, self = juce::WeakReference(this),
                                  id = brain->definition.id, rate, key = *key, paths] {
            auto loaded = engine->cached_brain(key);
            post(self, [=](SamplebrainDocumentController &dc) {
              BrainState *b = dc.find_brain(id);
              if (!b) return;
              --b->loading;
              if (loaded) b->playing.try_emplace(rate, PlayingBrain{*loaded, paths});
              if (loaded || !b->busy()) dc.render_brain_clips(id);
              dc.changed();
            });
          });
        }
    }

  if (const auto *clips = archive["clips"].getDynamicObject())
    for (const auto &entry : clips->getProperties()) {
      auto *m = filter->getAudioModificationToRestoreStateWithID<SamplebrainAudioModification>(
          entry.name.toString().toRawUTF8());
      if (!m) continue;
      m->settings = clip_from_var(entry.value);
      schedule_render(*m);
    }

  for (auto &brain : brains_) refresh_keys(*brain);
  changed();
  return true;
}

} // namespace sb::plugin
