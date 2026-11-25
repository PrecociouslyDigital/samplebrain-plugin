// Upstream reports progress over OSC. This sends it to the progress sink
// of whichever thread is doing the work instead.

#include "status.h"

#include <cstdarg>
#include <cstdio>

#include "core/progress.h"

using namespace spiralcore;

void status::_update(const std::string &msg) { sb::report_progress(msg); }

void status::update(const char *msg, ...) {
  char text[4096];
  va_list args;
  va_start(args, msg);
  vsnprintf(text, sizeof text, msg, args);
  va_end(args);
  sb::report_progress(text);
}

void status::sound_item(const std::string &name, const std::string &colour) {
  using enum sb::SoundBuildState;
  sb::report_sound_state(name, colour == "lightgrey" ? Waiting
                               : colour == "yellow"  ? Processing
                                                     : Built);
}

void status::sound_item_refresh() {}
