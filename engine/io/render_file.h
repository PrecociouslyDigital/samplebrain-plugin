#pragma once

#include <expected>
#include <filesystem>

#include "core/core.h"

namespace sb {

// Our own format for cached renders: a header, then each channel's floats.
bool save_render(const RenderedAudio &, const std::filesystem::path &);

struct RenderFileError {};
std::expected<RenderedAudio, RenderFileError>
load_render(const std::filesystem::path &);

} // namespace sb
