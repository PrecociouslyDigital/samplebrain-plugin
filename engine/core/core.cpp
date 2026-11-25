#include "core.h"

#include <algorithm>
#include <exception>
#include <format>
#include <fstream>
#include <thread>

#include "brain.h"
#include "renderer.h"
#include "search_params.h"

#include "progress.h"

namespace sb {

struct CoreAccess {
  static Brain make_brain(std::unique_ptr<spiralcore::brain> b,
                          unsigned rate) {
    return Brain(std::move(b), rate);
  }
  static const spiralcore::brain &upstream(const Brain &b) {
    return *b.brain_;
  }
  static Target
  make_target(std::vector<std::unique_ptr<spiralcore::brain>> channels,
              unsigned rate, std::size_t length) {
    return Target(std::move(channels), rate, length);
  }
  static const spiralcore::brain &channel(const Target &t, std::size_t i) {
    return *t.channels_[i];
  }
};

Brain::Brain(std::unique_ptr<spiralcore::brain> b, unsigned rate)
    : brain_(std::move(b)), rate_(rate) {}
Brain::Brain(Brain &&) noexcept = default;
Brain &Brain::operator=(Brain &&) noexcept = default;
Brain::~Brain() = default;
std::size_t Brain::num_blocks() const { return brain_->get_num_blocks(); }
std::size_t Brain::num_sounds() const { return brain_->get_samples().size(); }

Target::Target(std::vector<std::unique_ptr<spiralcore::brain>> channels,
               unsigned rate, std::size_t length)
    : channels_(std::move(channels)), rate_(rate), length_(length) {}
Target::Target(Target &&) noexcept = default;
Target &Target::operator=(Target &&) noexcept = default;
Target::~Target() = default;
std::size_t Target::num_blocks() const {
  return channels_.front()->get_num_blocks();
}

namespace {

// Upstream's renderer works in chunks like an audio callback. The chunk
// size only changes where a pass ends: the renderer stops about two chunks
// short of the target's last block.
constexpr unsigned render_chunk = 512;

spiralcore::sample to_sample(std::span<const float> audio) {
  return spiralcore::sample(audio.data(), unsigned(audio.size()));
}

void build(spiralcore::brain &b, int block_size, float overlap, int window,
           unsigned rate) {
  b.init(unsigned(block_size), unsigned(block_size * overlap),
         spiralcore::window::type(window), rate);
}

Mono render_channel(const spiralcore::brain &source,
                    const spiralcore::brain &target, std::size_t length,
                    const RenderSettings &s, unsigned seed, unsigned chunk_size) {
  using spiralcore::renderer;
  renderer r(source, target,
             spiralcore::search_state(source.get_num_blocks(), s.sound_enabled,
                                      s.boredom, seed));
  r.set_search_algo(renderer::search_algo(s.algorithm));
  r.set_volume(s.volume * 10);
  r.set_n_mix(s.n_mix);
  r.set_target_mix(s.target_mix);
  r.set_slide_error(s.slide_error);
  r.set_stretch(unsigned(s.stretch));
  r.set_autotune(s.autotune);
  spiralcore::search_params &p = *r.get_params();
  p.m_ratio = s.ratio;
  p.m_n_ratio = s.n_ratio;
  p.m_fft1_start = unsigned(s.fft1_start);
  p.m_fft1_end = unsigned(s.fft1_end);
  p.m_usage_importance = s.novelty;
  p.m_num_synapses = unsigned(s.synapses);
  p.m_stickyness = s.stickyness;
  r.set_playing(true);

  Mono out;
  out.reserve(length);
  std::vector<float> chunk(chunk_size);
  std::size_t reported_percent = 0;
  while (out.size() < length) {
    std::ranges::fill(chunk, 0.f);
    const bool wrote = r.process(chunk_size, chunk.data());
    if (r.completed_pass()) break;
    if (!wrote) continue; // the first call only sets the renderer up
    out.insert(out.end(), chunk.begin(), chunk.end());
    if (std::size_t percent = out.size() * 100 / length;
        percent != reported_percent) {
      report_progress(std::format("rendering {}%", percent));
      reported_percent = percent;
    }
  }
  out.resize(length, 0.f);
  return out;
}

} // namespace

Mono mix_to_mono(const ClipAudio &clip) {
  Mono mono(clip.channels.front().size(), 0.f);
  const float scale = 1.f / float(clip.channels.size());
  for (const Mono &channel : clip.channels)
    for (std::size_t i = 0; i < mono.size(); ++i) mono[i] += channel[i] * scale;
  return mono;
}

std::expected<Brain, BrainError> analyse_brain(std::span<const Sound> sounds,
                                               const BrainSettings &settings,
                                               unsigned rate) {
  if (sounds.empty()) return std::unexpected(BrainError{BrainError::NoSounds});
  auto b = std::make_unique<spiralcore::brain>();
  for (const Sound &sound : sounds) b->add_sound(sound.name, to_sample(sound.audio));
  build(*b, settings.block_size, settings.overlap, settings.window, rate);
  if (b->get_num_blocks() == 0)
    return std::unexpected(BrainError{BrainError::NoBlocks});
  // The app always builds synapses with these parameters.
  spiralcore::search_params synapse_params(1, 0, 0, 100, 0);
  b->build_synapses_fixed(synapse_params);
  return CoreAccess::make_brain(std::move(b), rate);
}

std::expected<Target, TargetError>
analyse_target(const ClipAudio &clip, const TargetSettings &settings,
               unsigned rate) {
  if (clip.channels.empty())
    return std::unexpected(TargetError{TargetError::NoChannels});

  std::vector<Mono> inputs;
  if (settings.stereo && clip.channels.size() > 1)
    inputs = {clip.channels[0], clip.channels[1]};
  else
    inputs = {mix_to_mono(clip)};

  std::vector<std::unique_ptr<spiralcore::brain>> channels;
  for (const Mono &input : inputs) {
    auto b = std::make_unique<spiralcore::brain>();
    b->add_sound("target", to_sample(input));
    build(*b, settings.block_size, settings.overlap, settings.window, rate);
    channels.push_back(std::move(b));
  }
  return CoreAccess::make_target(std::move(channels), rate,
                                 clip.channels.front().size());
}

std::expected<RenderedAudio, RenderError>
testing::render_in_chunks(const Brain &brain, const Target &target,
                          const RenderSettings &settings, unsigned chunk_size) {
  if (brain.rate() != target.rate())
    return std::unexpected(RenderError{RenderError::RateMismatch});
  if (settings.sound_enabled.size() != brain.num_sounds())
    return std::unexpected(RenderError{RenderError::SoundCountMismatch});

  const spiralcore::brain &source = CoreAccess::upstream(brain);
  auto channel = [&](std::size_t i) {
    // Each channel searches with its own seed, like two separate renders.
    const unsigned seed = unsigned(settings.seed) + unsigned(i) * 0x9e3779b9u;
    return render_channel(source, CoreAccess::channel(target, i),
                          target.length(), settings, seed, chunk_size);
  };

  RenderedAudio out;
  out.channels.resize(target.num_channels());
  if (target.num_channels() == 1) {
    out.channels[0] = channel(0);
    return out;
  }

  // The two sides of a stereo render run in parallel. The second thread
  // reports to the same sink, so it can be cancelled the same way.
  std::exception_ptr right_error;
  {
    ProgressSink *sink = current_progress_sink();
    std::jthread right([&] {
      ProgressScope scope(sink);
      try {
        out.channels[1] = channel(1);
      } catch (...) {
        right_error = std::current_exception();
      }
    });
    out.channels[0] = channel(0);
  }
  if (right_error) std::rethrow_exception(right_error);
  return out;
}

std::expected<RenderedAudio, RenderError>
render(const Brain &brain, const Target &target,
       const RenderSettings &settings) {
  return testing::render_in_chunks(brain, target, settings, render_chunk);
}

std::expected<void, BrainError> save_brain(const Brain &brain,
                                           const std::string &path) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return std::unexpected(BrainError{BrainError::UnreadableFile});
  // Upstream streams in both directions through one non-const function.
  // Writing doesn't change the brain.
  file || const_cast<spiralcore::brain &>(CoreAccess::upstream(brain));
  if (!file) return std::unexpected(BrainError{BrainError::UnreadableFile});
  return {};
}

std::expected<Brain, BrainError> load_brain(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::unexpected(BrainError{BrainError::UnreadableFile});
  auto b = std::make_unique<spiralcore::brain>();
  file || *b;
  if (!file) return std::unexpected(BrainError{BrainError::UnreadableFile});
  if (b->get_num_blocks() == 0)
    return std::unexpected(BrainError{BrainError::NoBlocks});
  const unsigned rate = b->get_block(0).get_rate();
  return CoreAccess::make_brain(std::move(b), rate);
}

std::vector<Sound> brain_sounds(const Brain &brain) {
  std::vector<Sound> sounds;
  for (const auto &s : CoreAccess::upstream(brain).get_samples()) {
    const float *data = s.m_sample.get_buffer();
    sounds.push_back({s.m_filename, Mono(data, data + s.m_sample.get_length())});
  }
  return sounds;
}

BrainSettings brain_settings(const Brain &brain) {
  const spiralcore::brain &b = CoreAccess::upstream(brain);
  BrainSettings settings;
  settings.block_size = int(b.get_block_size());
  settings.overlap = float(b.get_overlap()) / float(b.get_block_size());
  settings.window = int(b.get_window_type());
  return settings;
}

} // namespace sb
