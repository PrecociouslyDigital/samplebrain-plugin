#pragma once

#include <exception>
#include <string_view>

namespace sb {

// Upstream's colours for a sound in the brain's sample list.
enum class SoundBuildState { Waiting, Processing, Built };

struct Cancelled : std::exception {
  const char *what() const noexcept override { return "cancelled"; }
};

// Receives progress from engine work on the threads it's installed on.
// Either method may throw Cancelled to stop that work.
class ProgressSink {
public:
  virtual ~ProgressSink() = default;
  virtual void report(std::string_view text) = 0;
  virtual void sound_state(std::string_view, SoundBuildState) {}
};

// Routes progress on the current thread to a sink while alive.
class ProgressScope {
public:
  explicit ProgressScope(ProgressSink *sink);
  ~ProgressScope();
  ProgressScope(const ProgressScope &) = delete;
  ProgressScope &operator=(const ProgressScope &) = delete;

private:
  ProgressSink *previous_;
};

// The sink installed on this thread, or nullptr.
ProgressSink *current_progress_sink();

void report_progress(std::string_view text);
void report_sound_state(std::string_view sound, SoundBuildState state);

} // namespace sb
