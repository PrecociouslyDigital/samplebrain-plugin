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

// Writes 32-bit float WAV, so reading it back gives the same samples.
std::expected<void, AudioFileError>
write_wav(const std::filesystem::path &, const Mono &, unsigned rate);

} // namespace sb
