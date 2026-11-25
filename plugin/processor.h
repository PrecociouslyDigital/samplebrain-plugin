#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace sb::plugin {

class SamplebrainProcessor : public juce::AudioProcessor,
                             public juce::AudioProcessorARAExtension {
public:
  SamplebrainProcessor();

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;
  bool isBusesLayoutSupported(const BusesLayout &) const override;
  void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
  using juce::AudioProcessor::processBlock;

  juce::AudioProcessorEditor *createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override { return "Samplebrain"; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  double getTailLengthSeconds() const override;

  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String &) override {}

  // Everything is saved in the ARA archive.
  void getStateInformation(juce::MemoryBlock &) override {}
  void setStateInformation(const void *, int) override {}

  juce::AudioProcessorARAExtension *getARAClientExtensions() override { return this; }
};

} // namespace sb::plugin
