#include "progress.h"

namespace sb {

namespace {
thread_local ProgressSink *t_sink = nullptr;
}

ProgressScope::ProgressScope(ProgressSink *sink) : previous_(t_sink) {
  t_sink = sink;
}

ProgressScope::~ProgressScope() { t_sink = previous_; }

ProgressSink *current_progress_sink() { return t_sink; }

void report_progress(std::string_view text) {
  if (t_sink) t_sink->report(text);
}

void report_sound_state(std::string_view sound, SoundBuildState state) {
  if (t_sink) t_sink->sound_state(sound, state);
}

} // namespace sb
