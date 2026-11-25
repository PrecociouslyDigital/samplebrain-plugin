#include "render_file.h"

#include <array>
#include <cstdint>
#include <fstream>

namespace sb {

namespace {
constexpr std::array<char, 4> magic = {'S', 'B', 'R', '1'};

struct Header {
  std::array<char, 4> magic;
  std::uint32_t channels;
  std::uint64_t frames;
};
} // namespace

bool save_render(const RenderedAudio &audio, const std::filesystem::path &path) {
  std::ofstream file(path, std::ios::binary);
  const Header header{magic, std::uint32_t(audio.channels.size()),
                      audio.channels.empty() ? 0 : audio.channels[0].size()};
  file.write(reinterpret_cast<const char *>(&header), sizeof header);
  for (const Mono &channel : audio.channels)
    file.write(reinterpret_cast<const char *>(channel.data()),
               std::streamsize(channel.size() * sizeof(float)));
  return bool(file);
}

std::expected<RenderedAudio, RenderFileError>
load_render(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  Header header{};
  file.read(reinterpret_cast<char *>(&header), sizeof header);
  if (!file || header.magic != magic || header.channels < 1 ||
      header.channels > 2)
    return std::unexpected(RenderFileError{});
  std::error_code ec;
  const auto expected_size =
      sizeof header + header.channels * header.frames * sizeof(float);
  if (std::filesystem::file_size(path, ec) != expected_size)
    return std::unexpected(RenderFileError{});

  RenderedAudio audio;
  audio.channels.assign(header.channels, Mono(header.frames));
  for (Mono &channel : audio.channels)
    file.read(reinterpret_cast<char *>(channel.data()),
              std::streamsize(channel.size() * sizeof(float)));
  if (!file) return std::unexpected(RenderFileError{});
  return audio;
}

} // namespace sb
