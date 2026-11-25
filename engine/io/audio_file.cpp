#include "audio_file.h"

#include <format>
#include <memory>

#include <samplerate.h>
#include <sndfile.h>

namespace sb {

std::expected<ClipAudio, AudioFileError>
read_audio_file(const std::filesystem::path &path, unsigned rate) {
  SF_INFO info{};
  std::unique_ptr<SNDFILE, decltype(&sf_close)> file(
      sf_open(path.c_str(), SFM_READ, &info), &sf_close);
  if (!file)
    return std::unexpected(AudioFileError{
        std::format("{}: {}", path.string(), sf_strerror(nullptr))});

  const auto channels = std::size_t(info.channels);
  std::vector<float> interleaved(std::size_t(info.frames) * channels);
  const sf_count_t read =
      sf_readf_float(file.get(), interleaved.data(), info.frames);
  interleaved.resize(std::size_t(read) * channels);

  if (unsigned(info.samplerate) != rate) {
    const double ratio = double(rate) / info.samplerate;
    std::vector<float> resampled(
        std::size_t(double(read) * ratio + 1) * channels);
    SRC_DATA data{};
    data.data_in = interleaved.data();
    data.input_frames = long(read);
    data.data_out = resampled.data();
    data.output_frames = long(resampled.size() / channels);
    data.src_ratio = ratio;
    if (int err = src_simple(&data, SRC_SINC_BEST_QUALITY, int(channels)))
      return std::unexpected(AudioFileError{
          std::format("{}: {}", path.string(), src_strerror(err))});
    resampled.resize(std::size_t(data.output_frames_gen) * channels);
    interleaved = std::move(resampled);
  }

  ClipAudio clip;
  const std::size_t frames = interleaved.size() / channels;
  clip.channels.assign(channels, Mono(frames));
  for (std::size_t i = 0; i < frames; ++i)
    for (std::size_t c = 0; c < channels; ++c)
      clip.channels[c][i] = interleaved[i * channels + c];
  return clip;
}

std::expected<void, AudioFileError>
write_wav(const std::filesystem::path &path, const Mono &audio, unsigned rate) {
  SF_INFO info{};
  info.samplerate = int(rate);
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  std::unique_ptr<SNDFILE, decltype(&sf_close)> file(
      sf_open(path.c_str(), SFM_WRITE, &info), &sf_close);
  if (!file ||
      sf_writef_float(file.get(), audio.data(), sf_count_t(audio.size())) !=
          sf_count_t(audio.size()))
    return std::unexpected(AudioFileError{
        std::format("{}: {}", path.string(), sf_strerror(file.get()))});
  return {};
}

} // namespace sb
