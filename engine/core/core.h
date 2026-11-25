#pragma once

// The engine: three functions whose results depend only on their
// arguments. Everything else in the plugin caches or schedules them.
//
//   analyse_brain (sounds, settings, rate) -> Brain
//   analyse_target(clip,   settings, rate) -> Target
//   render        (brain, target, settings) -> RenderedAudio
//
// A brain and the target matched against it must share a rate. Progress
// goes to the ProgressSink installed on the calling thread, which can
// cancel by throwing.

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "settings.h"

namespace spiralcore {
class brain;
}

namespace sb {

using Mono = std::vector<float>;

struct Sound {
  std::string name;
  Mono audio;
};

// One or two channels of equal length.
struct ClipAudio {
  std::vector<Mono> channels;
};

// What the renderer produced, cut to the target's length. A single
// channel plays on both sides.
struct RenderedAudio {
  std::vector<Mono> channels;
  bool operator==(const RenderedAudio &) const = default;
};

struct BrainError {
  enum Reason { NoSounds, NoBlocks, UnreadableFile } reason;
};

struct TargetError {
  enum Reason { NoChannels } reason;
};

struct RenderError {
  enum Reason { RateMismatch, SoundCountMismatch } reason;
};

struct CoreAccess;

// Built once and never changed afterwards.
class Brain {
public:
  Brain(Brain &&) noexcept;
  Brain &operator=(Brain &&) noexcept;
  ~Brain();

  unsigned rate() const { return rate_; }
  std::size_t num_blocks() const;
  std::size_t num_sounds() const;

private:
  Brain(std::unique_ptr<spiralcore::brain>, unsigned rate);
  std::unique_ptr<spiralcore::brain> brain_;
  unsigned rate_;
  friend CoreAccess;
};

class Target {
public:
  Target(Target &&) noexcept;
  Target &operator=(Target &&) noexcept;
  ~Target();

  unsigned rate() const { return rate_; }
  std::size_t length() const { return length_; }
  // One analysis per rendered channel: one unless stereo is on.
  std::size_t num_channels() const { return channels_.size(); }
  std::size_t num_blocks() const;

private:
  Target(std::vector<std::unique_ptr<spiralcore::brain>>, unsigned rate,
         std::size_t length);
  std::vector<std::unique_ptr<spiralcore::brain>> channels_;
  unsigned rate_;
  std::size_t length_;
  friend CoreAccess;
};

// The average of the channels.
Mono mix_to_mono(const ClipAudio &);

std::expected<Brain, BrainError> analyse_brain(std::span<const Sound> sounds,
                                               const BrainSettings &,
                                               unsigned rate);

std::expected<Target, TargetError>
analyse_target(const ClipAudio &, const TargetSettings &, unsigned rate);

std::expected<RenderedAudio, RenderError>
render(const Brain &, const Target &, const RenderSettings &);

namespace testing {
// render() with a chosen chunk size for upstream's renderer, which should
// only change the output near the end of the target.
std::expected<RenderedAudio, RenderError>
render_in_chunks(const Brain &, const Target &, const RenderSettings &,
                 unsigned chunk_size);
} // namespace testing

// Upstream's .brain format, shared with the app. A brain doesn't record
// the rate it was built at, so loading needs to be told.
std::expected<void, BrainError> save_brain(const Brain &,
                                           const std::string &path);
std::expected<Brain, BrainError> load_brain(const std::string &path,
                                            unsigned rate);

} // namespace sb
