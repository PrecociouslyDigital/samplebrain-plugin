#include "processor.h"

#include "document_controller.h"
#include "editor.h"

namespace sb::plugin {

SamplebrainProcessor::SamplebrainProcessor()
    : juce::AudioProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}

void SamplebrainProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
  prepareToPlayForARA(sampleRate, samplesPerBlock, getMainBusNumOutputChannels(),
                      getProcessingPrecision());
}

void SamplebrainProcessor::releaseResources() { releaseResourcesForARA(); }

bool SamplebrainProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
  const auto out = layouts.getMainOutputChannelSet();
  return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void SamplebrainProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                        juce::MidiBuffer &midi) {
  const juce::ScopedNoDenormals noDenormals;
  if (!processBlockForARA(buffer, isRealtime(), getPlayHead()))
    processBlockBypassed(buffer, midi);
}

double SamplebrainProcessor::getTailLengthSeconds() const {
  double tail = 0;
  getTailLengthSecondsForARA(tail);
  return tail;
}

juce::AudioProcessorEditor *SamplebrainProcessor::createEditor() {
  return new SamplebrainEditor(*this);
}

} // namespace sb::plugin

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new sb::plugin::SamplebrainProcessor();
}

const ARA::ARAFactory *JUCE_CALLTYPE createARAFactory() {
  return juce::ARADocumentControllerSpecialisation::createARAFactory<
      sb::plugin::SamplebrainDocumentController>();
}
