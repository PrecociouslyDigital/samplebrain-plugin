#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include "core/core.h"

namespace sb {

struct AudioFileError {
  std::string message;
};

// Reads every channel of a sound file, resampled to the given rate.
std::expected<ClipAudio, AudioFileError>
read_audio_file(const std::filesystem::path &, unsigned rate);

} // namespace sb
