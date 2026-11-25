#include "engine_service.h"

#include <format>

#include "io/audio_file.h"
#include "io/render_file.h"

namespace sb::plugin {

void JobProgress::report(std::string_view text) {
  if (cancelled_) throw Cancelled{};
  std::lock_guard lock(mutex_);
  text_ = text;
}

void JobProgress::sound_state(std::string_view sound, SoundBuildState state) {
  if (cancelled_) throw Cancelled{};
  std::lock_guard lock(mutex_);
  sounds_.insert_or_assign(std::string(sound), state);
}

std::string JobProgress::text() const {
  std::lock_guard lock(mutex_);
  return text_;
}

std::optional<SoundBuildState> JobProgress::state_of(const std::string &sound) const {
  std::lock_guard lock(mutex_);
  auto it = sounds_.find(sound);
  if (it == sounds_.end()) return std::nullopt;
  return it->second;
}

namespace {
std::string brain_file(const BrainKey &key) { return key.hex() + ".brain"; }
std::string render_file(const RenderKey &key) { return key.hex() + ".render"; }
} // namespace

EngineService::EngineService()
    : brain_disk_(user_cache_dir() / "brains"),
      render_disk_(user_cache_dir() / "renders"),
      pool_(juce::ThreadPoolOptions{}
                .withThreadName("Samplebrain")
                .withNumberOfThreads(std::max(2, juce::SystemStats::getNumCpus() / 2))) {
  // Brains are much bigger than renders, so they get most of the budget.
  brain_disk_.trim(disk_cache_budget * 4 / 5);
  render_disk_.trim(disk_cache_budget / 5);
}

std::expected<std::shared_ptr<const LoadedSound>, std::string>
EngineService::sound(const juce::File &file, unsigned rate) {
  const std::string key =
      std::format("{}|{}|{}|{}", file.getFullPathName().toStdString(),
                  file.getLastModificationTime().toMilliseconds(), file.getSize(), rate);
  return sounds_.get(key, [&]() -> decltype(sounds_)::Result {
    auto clip = read_audio_file(file.getFullPathName().toStdString(), rate);
    if (!clip) return std::unexpected(clip.error().message);
    LoadedSound sound{mix_to_mono(*clip), {}};
    sound.hash = hash_audio(sound.audio);
    return std::make_shared<const LoadedSound>(std::move(sound));
  });
}

std::expected<LoadedSounds, std::string>
EngineService::load_sounds(const BrainDefinition &definition, unsigned rate) {
  LoadedSounds loaded;
  std::vector<AudioHash> hashes;
  for (const BrainSample &sample : definition.samples) {
    auto s = sound(juce::File(sample.path), rate);
    if (!s) return std::unexpected(s.error());
    hashes.push_back((*s)->hash);
    loaded.sounds.push_back(std::move(*s));
  }
  loaded.key = brain_key(hashes, definition.settings, rate);
  return loaded;
}

std::optional<LoadedBrain> EngineService::cached_brain(const BrainKey &key) {
  if (auto live = brains_.find(key)) return LoadedBrain{key, live};
  if (!brain_disk_.find(brain_file(key))) return std::nullopt;
  auto loaded = brains_.get(key, [&]() -> decltype(brains_)::Result {
    auto path = brain_disk_.find(brain_file(key));
    if (!path) return std::unexpected("brain left the disk cache");
    report_progress("loading brain");
    auto brain = load_brain(path->string());
    if (!brain) return std::unexpected("couldn't read cached brain");
    return std::make_shared<const Brain>(std::move(*brain));
  });
  if (!loaded) return std::nullopt;
  return LoadedBrain{key, *loaded};
}

std::expected<std::optional<LoadedBrain>, std::string>
EngineService::brain(const BrainDefinition &definition, const LoadedSounds &sounds,
                     unsigned rate, bool build) {
  if (auto cached = cached_brain(sounds.key)) return cached;
  if (!build) return std::nullopt;

  auto built = brains_.get(sounds.key, [&]() -> decltype(brains_)::Result {
    std::vector<Sound> input;
    for (std::size_t i = 0; i < sounds.sounds.size(); ++i)
      input.push_back({definition.samples[i].path.toStdString(), sounds.sounds[i]->audio});
    auto brain = analyse_brain(input, definition.settings, rate);
    if (!brain)
      return std::unexpected(brain.error().reason == BrainError::NoSounds
                                 ? "the brain has no samples"
                                 : "every sample is shorter than one block");
    report_progress("saving brain");
    brain_disk_.store(brain_file(sounds.key), [&](const std::filesystem::path &p) {
      return bool(save_brain(*brain, p.string()));
    });
    return std::make_shared<const Brain>(std::move(*brain));
  });
  if (!built) return std::unexpected(built.error());
  return LoadedBrain{sounds.key, *built};
}

std::expected<ImportedBrain, std::string>
EngineService::import_brain(const juce::File &file) {
  report_progress("reading " + file.getFileName().toStdString());
  auto brain = load_brain(file.getFullPathName().toStdString());
  if (!brain) return std::unexpected("couldn't read " + file.getFileName().toStdString());

  const juce::File dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                             .getChildFile("samplebrain-plugin/imported")
                             .getChildFile(file.getFileNameWithoutExtension() + "-" +
                                           juce::Uuid().toString().substring(0, 8));
  if (!dir.createDirectory()) return std::unexpected("couldn't create " + dir.getFullPathName().toStdString());

  ImportedBrain imported{{}, {}, {}, brain->rate()};
  imported.definition.name = file.getFileNameWithoutExtension();
  imported.definition.settings = brain_settings(*brain);
  imported.definition.block.ms =
      float(imported.definition.settings.block_size) * 1000.f / float(imported.rate);
  int index = 0;
  for (const Sound &sound : brain_sounds(*brain)) {
    const juce::String name = juce::File::createLegalFileName(
        juce::String(++index).paddedLeft('0', 3) + " " +
        juce::File(juce::String(sound.name)).getFileNameWithoutExtension() + ".wav");
    const juce::File path = dir.getChildFile(name);
    if (auto written = write_wav(path.getFullPathName().toStdString(), sound.audio, imported.rate); !written)
      return std::unexpected(written.error().message);
    imported.definition.samples.push_back({path.getFullPathName(), true});
  }

  auto sounds = load_sounds(imported.definition, imported.rate);
  if (!sounds) return std::unexpected(sounds.error());
  imported.sounds = *sounds;
  report_progress("saving brain");
  brain_disk_.store(brain_file(sounds->key), [&](const std::filesystem::path &p) {
    return bool(save_brain(*brain, p.string()));
  });
  auto shared = std::make_shared<const Brain>(std::move(*brain));
  auto cached = brains_.get(sounds->key, [&] { return decltype(brains_)::Result(shared); });
  if (!cached) return std::unexpected(cached.error());
  imported.brain = LoadedBrain{sounds->key, *cached};
  return imported;
}

std::expected<void, std::string> EngineService::export_brain(const LoadedBrain &brain,
                                                             const juce::File &file) {
  if (!save_brain(*brain.brain, file.getFullPathName().toStdString()))
    return std::unexpected("couldn't write " + file.getFullPathName().toStdString());
  return {};
}

std::expected<LoadedTarget, std::string>
EngineService::target(const ClipAudio &clip, const TargetSettings &settings, unsigned rate) {
  std::vector<AudioHash> channels;
  for (const Mono &c : clip.channels) channels.push_back(hash_audio(c));
  const TargetKey key = target_key(channels, settings, rate);
  auto target = targets_.get(key, [&]() -> decltype(targets_)::Result {
    report_progress("analysing clip");
    auto target = analyse_target(clip, settings, rate);
    if (!target) return std::unexpected("the clip has no audio channels");
    return std::make_shared<const Target>(std::move(*target));
  });
  if (!target) return std::unexpected(target.error());
  return LoadedTarget{key, *target};
}

std::expected<std::shared_ptr<const RenderCache>, std::string>
EngineService::render(const LoadedBrain &brain, const LoadedTarget &target,
                      const RenderSettings &settings) {
  const RenderKey key = render_key(brain.key, target.key, settings);
  return renders_.get(key, [&]() -> decltype(renders_)::Result {
    const unsigned rate = target.target->rate();
    if (auto path = render_disk_.find(render_file(key)))
      if (auto audio = load_render(*path))
        return std::make_shared<const RenderCache>(
            RenderCache{key, rate, std::make_shared<const RenderedAudio>(std::move(*audio))});

    auto audio = sb::render(*brain.brain, *target.target, settings);
    if (!audio)
      return std::unexpected(audio.error().reason == RenderError::RateMismatch
                                 ? "the brain and clip have different rates"
                                 : "the brain's samples changed");
    render_disk_.store(render_file(key), [&](const std::filesystem::path &p) {
      return save_render(*audio, p);
    });
    return std::make_shared<const RenderCache>(
        RenderCache{key, rate, std::make_shared<const RenderedAudio>(std::move(*audio))});
  });
}

} // namespace sb::plugin
