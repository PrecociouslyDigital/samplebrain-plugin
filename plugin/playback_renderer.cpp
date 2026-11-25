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
  gave_up_waiting_ = false;
}

void SamplebrainPlaybackRenderer::releaseResources() { mix_.setSize(0, 0); }

bool SamplebrainPlaybackRenderer::processBlock(
    juce::AudioBuffer<float> &buffer, juce::AudioProcessor::Realtime realtime,
    const juce::AudioPlayHead::PositionInfo &position) noexcept {
  if (realtime == juce::AudioProcessor::Realtime::no) wait_for_renders();
  return render(buffer, position);
}

// An offline render can block, so it waits for clips whose renders haven't
// arrived yet, like straight after a project loads. Renders are published
// on the message thread, so it can't wait there.
void SamplebrainPlaybackRenderer::wait_for_renders() {
  if (gave_up_waiting_ || juce::MessageManager::existsAndIsCurrentThread()) return;
  const auto give_up = juce::Time::getMillisecondCounter() + 10 * 60 * 1000;
  for (auto *region : getPlaybackRegions())
    while (region->getAudioModification<SamplebrainAudioModification>()->pending()) {
      // Once is enough: a render that never arrives shouldn't stall every block.
      if (juce::Time::getMillisecondCounter() > give_up) {
        gave_up_waiting_ = true;
        return;
      }
      juce::Thread::sleep(20);
    }
}

bool SamplebrainPlaybackRenderer::render(juce::AudioBuffer<float> &buffer,
                                         const juce::AudioPlayHead::PositionInfo &position)
    noexcept [[clang::nonblocking]] {
  // Raw pointers rather than AudioBuffer's helpers, which clang can't check
  // are nonblocking.
  const int frames = buffer.getNumSamples();
  const std::span<float *const> output(buffer.getArrayOfWritePointers(),
                                       std::size_t(buffer.getNumChannels()));
  for (float *channel : output) std::fill_n(channel, frames, 0.f);
  if (!position.getIsPlaying() || frames > mix_.getNumSamples()) return true;

  const ReaderEpoch::Reading reading(epoch_);
  if (controller_.editing()) return true;

  const std::int64_t block_start = position.getTimeInSamples().orFallback(0);
  // Renders are at most stereo.
  const auto render_channels =
      std::min<std::size_t>({output.size(), std::size_t(mix_.getNumChannels()), 2});
  const std::span<float *const> mix(mix_.getArrayOfWritePointers(), render_channels);

  for (auto *region : getPlaybackRegions()) {
    // Where the region sits on the timeline, in output samples.
    const std::int64_t region_start =
        ARA::samplePositionAtTime(region->getStartInPlaybackTime(), rate_);
    const std::int64_t region_end =
        ARA::samplePositionAtTime(region->getEndInPlaybackTime(), rate_);
    const std::int64_t from = std::max(region_start, block_start);
    const std::int64_t to = std::min(region_end, block_start + frames);
    if (from >= to) continue;

    const auto *modification = region->getAudioModification<SamplebrainAudioModification>();
    const RenderCache *cache = modification->render.read(reading);
    if (!cache) continue;

    // The same moment in the render, which is at the clip's own rate.
    const auto offset = std::size_t(from - block_start);
    const auto count = std::size_t(to - from);
    const double seconds_into_region = double(from - region_start) / rate_;
    const double render_start =
        (region->getStartInAudioModificationTime() + seconds_into_region) * cache->rate;

    float *shifted[2] = {};
    for (std::size_t c = 0; c < mix.size(); ++c) shifted[c] = mix[c] + offset;
    const std::span<float *const> dst(shifted, mix.size());
    if (cache->rate == unsigned(std::lround(rate_)))
      play_slice(*cache->audio, std::llround(render_start), dst, count);
    else
      play_resampled(*cache->audio, render_start, cache->rate / rate_, dst, count);

    // Regions add, so overlapping clips both play.
    for (std::size_t c = 0; c < output.size(); ++c) {
      const float *src = dst[std::min(c, dst.size() - 1)];
      float *out = output[c] + offset;
      for (std::size_t i = 0; i < count; ++i) out[i] += src[i];
    }
  }
  return true;
}

} // namespace sb::plugin
