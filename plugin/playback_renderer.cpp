#include "playback_renderer.h"

#include "core/playback.h"
#include "document_controller.h"

namespace sb::plugin {

SamplebrainPlaybackRenderer::SamplebrainPlaybackRenderer(
    ARA::PlugIn::DocumentController *dc, SamplebrainDocumentController &controller)
    : juce::ARAPlaybackRenderer(dc), controller_(controller) {
  controller_.readers().add(&epoch_);
}

SamplebrainPlaybackRenderer::~SamplebrainPlaybackRenderer() {
  controller_.readers().remove(&epoch_);
}

void SamplebrainPlaybackRenderer::prepareToPlay(double sampleRate, int maximumSamplesPerBlock,
                                                int numChannels,
                                                juce::AudioProcessor::ProcessingPrecision,
                                                AlwaysNonRealtime) {
  rate_ = sampleRate;
  mix_.setSize(numChannels, maximumSamplesPerBlock);
}

void SamplebrainPlaybackRenderer::releaseResources() { mix_.setSize(0, 0); }

bool SamplebrainPlaybackRenderer::processBlock(
    juce::AudioBuffer<float> &buffer, juce::AudioProcessor::Realtime,
    const juce::AudioPlayHead::PositionInfo &position) noexcept {
  return render(buffer, position);
}

bool SamplebrainPlaybackRenderer::render(juce::AudioBuffer<float> &buffer,
                                         const juce::AudioPlayHead::PositionInfo &position)
    noexcept [[clang::nonblocking]] {
  buffer.clear();
  const int frames = buffer.getNumSamples();
  if (!position.getIsPlaying() || frames > mix_.getNumSamples()) return true;

  const ReaderEpoch::Reading reading(epoch_);
  if (controller_.editing()) return true;

  const std::int64_t block_start = position.getTimeInSamples().orFallback(0);
  const auto channels = std::size_t(std::min(buffer.getNumChannels(), mix_.getNumChannels()));
  // Renders are at most stereo.
  float *mix[2] = {};
  for (std::size_t c = 0; c < std::min<std::size_t>(channels, 2); ++c)
    mix[c] = mix_.getWritePointer(int(c));
  const std::span<float *const> out(mix, std::min<std::size_t>(channels, 2));

  for (auto *region : getPlaybackRegions()) {
    // Where the region sits on the timeline, in output samples.
    const auto region_range =
        region->getSampleRange(rate_, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
    const auto overlap =
        region_range.getIntersectionWith({block_start, block_start + frames});
    if (overlap.isEmpty()) continue;

    const auto *modification = region->getAudioModification<SamplebrainAudioModification>();
    const RenderCache *cache = modification->render.read(reading);
    if (!cache) continue;

    // The same moment in the render, which is at the clip's own rate.
    const int offset = int(overlap.getStart() - block_start);
    const int count = int(overlap.getLength());
    const double seconds_into_region = double(overlap.getStart() - region_range.getStart()) / rate_;
    const double render_start =
        (region->getStartInAudioModificationTime() + seconds_into_region) * cache->rate;

    float *shifted[2] = {};
    for (std::size_t c = 0; c < out.size(); ++c) shifted[c] = out[c] + offset;
    const std::span<float *const> dst(shifted, out.size());

    if (double(cache->rate) == rate_)
      play_slice(*cache->audio, std::llround(render_start), dst, std::size_t(count));
    else
      play_resampled(*cache->audio, render_start, cache->rate / rate_, dst, std::size_t(count));

    for (std::size_t c = 0; c < channels; ++c)
      buffer.addFrom(int(c), offset, mix_, int(std::min<std::size_t>(c, out.size() - 1)),
                     offset, count);
  }
  return true;
}

} // namespace sb::plugin
