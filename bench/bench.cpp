// Times each engine stage on real audio, to check the caching choices.
//
//   bench <target file> <brain files...>
//
// Prints a Markdown table. Uses the default settings for every stage.

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "core/core.h"
#include "core/progress.h"
#include "io/audio_file.h"
#include "io/render_file.h"

using namespace sb;

namespace {

constexpr unsigned rate = 44100;

class StderrProgress : public ProgressSink {
public:
  void report(std::string_view text) override {
    if (text != last_) std::cerr << "\r" << text << "        " << std::flush;
    last_ = text;
  }

private:
  std::string last_;
};

template <class F> auto timed(const char *what, F &&f) {
  const auto start = std::chrono::steady_clock::now();
  auto result = f();
  const std::chrono::duration<double> took =
      std::chrono::steady_clock::now() - start;
  std::cerr << "\r";
  std::cout << std::format("| {} | {:.2f} s |\n", what, took.count());
  return result;
}

ClipAudio first_seconds(const ClipAudio &clip, double seconds) {
  ClipAudio out;
  const auto frames =
      std::min(clip.channels[0].size(), std::size_t(seconds * rate));
  for (const Mono &c : clip.channels)
    out.channels.emplace_back(c.begin(), c.begin() + std::ptrdiff_t(frames));
  return out;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: bench <target file> <brain files...>\n";
    return 2;
  }
  StderrProgress progress;
  ProgressScope scope(&progress);

  std::vector<Sound> sounds;
  double brain_seconds = 0;
  for (int i = 2; i < argc; ++i) {
    auto clip = read_audio_file(argv[i], rate);
    if (!clip) {
      std::cerr << clip.error().message << "\n";
      return 1;
    }
    sounds.push_back({argv[i], mix_to_mono(*clip)});
    brain_seconds += double(sounds.back().audio.size()) / rate;
  }
  auto target_file = read_audio_file(argv[1], rate);
  if (!target_file) {
    std::cerr << target_file.error().message << "\n";
    return 1;
  }

  const BrainSettings brain_settings;
  std::cout << std::format(
      "Brain: {} sounds, {:.1f} min. Block size {}, overlap {}, {} Hz.\n\n",
      sounds.size(), brain_seconds / 60, brain_settings.block_size,
      brain_settings.overlap, rate);
  std::cout << "| Step | Time |\n|---|---|\n";

  const Brain brain = timed("analyse_brain (blocks and synapses)", [&] {
    return *analyse_brain(sounds, brain_settings, rate);
  });
  std::cout << std::format("| brain blocks | {} |\n", brain.num_blocks());

  const auto brain_path =
      std::filesystem::temp_directory_path() / "samplebrain-bench.brain";
  timed("save_brain", [&] { return save_brain(brain, brain_path.string()); });
  const Brain loaded = timed(
      "load_brain", [&] { return *load_brain(brain_path.string(), rate); });
  std::cout << std::format("| .brain file size | {:.0f} MB |\n",
                           double(std::filesystem::file_size(brain_path)) / 1e6);
  std::filesystem::remove(brain_path);

  RenderSettings render_settings;
  render_settings.sound_enabled.assign(brain.num_sounds(), true);

  for (double seconds : {10.0, 180.0}) {
    const ClipAudio clip = first_seconds(*target_file, seconds);
    const double clip_seconds = double(clip.channels[0].size()) / rate;
    const Target target =
        timed(std::format("analyse_target, {:.0f} s clip", clip_seconds).c_str(),
              [&] { return *analyse_target(clip, TargetSettings{}, rate); });

    for (int algorithm : {0, 2}) {
      render_settings.algorithm = algorithm;
      const RenderedAudio out = timed(
          std::format("render {}, {:.0f} s clip",
                      algorithm_labels[std::size_t(algorithm)], clip_seconds)
              .c_str(),
          [&] { return *render(loaded, target, render_settings); });

      const auto render_path =
          std::filesystem::temp_directory_path() / "samplebrain-bench.render";
      save_render(out, render_path);
      timed(std::format("load cached render, {:.0f} s clip", clip_seconds).c_str(),
            [&] { return load_render(render_path); });
      std::filesystem::remove(render_path);
    }
  }
}
