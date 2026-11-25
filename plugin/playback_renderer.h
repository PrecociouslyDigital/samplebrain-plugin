#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "realtime/published.h"

namespace sb::plugin {

class SamplebrainDocumentController;

// Plays each region's cached render. Never locks or allocates while
// processing.
class SamplebrainPlaybackRenderer : public juce::ARAPlaybackRenderer {
public:
  SamplebrainPlaybackRenderer(ARA::PlugIn::DocumentController *,
                              SamplebrainDocumentController &);
  ~SamplebrainPlaybackRenderer() override;

  void prepareToPlay(double sampleRate, int maximumSamplesPerBlock, int numChannels,
                     juce::AudioProcessor::ProcessingPrecision,
                     AlwaysNonRealtime) override;
  void releaseResources() override;

  bool processBlock(juce::AudioBuffer<float> &, juce::AudioProcessor::Realtime,
                    const juce::AudioPlayHead::PositionInfo &) noexcept override;
  using juce::ARAPlaybackRenderer::processBlock;

private:
  void wait_for_renders();
  bool render(juce::AudioBuffer<float> &, const juce::AudioPlayHead::PositionInfo &)
      noexcept [[clang::nonblocking]];

  SamplebrainDocumentController &controller_;
  ReaderEpoch epoch_;
  double rate_ = 44100;
  bool gave_up_waiting_ = false;
  // Each region renders here, then adds into the output so overlaps sum.
  juce::AudioBuffer<float> mix_;
};

} // namespace sb::plugin
