#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "core/settings.h"

namespace sb::plugin {

class SamplebrainProcessor;
class SamplebrainDocumentController;
class SamplebrainAudioModification;
struct BrainState;

// One control for one entry in settings.h.
class ParamControl : public juce::Component {
public:
  ParamControl(const ParamInfo &, std::function<void(double)> on_change);
  void set_value(double);
  void resized() override;

private:
  juce::Label label_;
  std::unique_ptr<juce::Component> control_;
  std::function<void(double)> on_change_;
};

// Edits the project's brains and the selected clip's settings.
class SamplebrainEditor : public juce::AudioProcessorEditor,
                          public juce::AudioProcessorEditorARAExtension,
                          public juce::FileDragAndDropTarget,
                          private juce::ListBoxModel,
                          private juce::Timer {
public:
  explicit SamplebrainEditor(SamplebrainProcessor &);
  ~SamplebrainEditor() override;

  void paint(juce::Graphics &) override;
  void resized() override;

  bool isInterestedInFileDrag(const juce::StringArray &) override { return brain() != nullptr; }
  void filesDropped(const juce::StringArray &, int, int) override;

private:
  SamplebrainDocumentController *controller() const;
  BrainState *brain() const;
  std::vector<SamplebrainAudioModification *> clips() const;
  SamplebrainAudioModification *clip() const;

  void add_files(const juce::StringArray &);
  void refresh();
  void refresh_progress();
  void timerCallback() override;

  int getNumRows() override;
  void paintListBoxItem(int row, juce::Graphics &, int width, int height, bool selected) override;
  void listBoxItemClicked(int row, const juce::MouseEvent &) override;

  juce::ComboBox brain_box_;
  juce::TextButton new_brain_{"+ New"}, export_brain_{"Export .brain"};
  juce::Label brain_users_;
  juce::ListBox samples_{"samples", this};
  juce::TextButton add_samples_{"Add..."}, remove_sample_{"Remove"};
  std::vector<std::unique_ptr<ParamControl>> brain_controls_;
  juce::TextButton build_{"Build brain"}, cancel_{"Cancel"};
  juce::Label brain_status_;

  juce::ComboBox clip_box_;
  juce::Label clip_status_;
  juce::Component clip_panel_;
  juce::Viewport clip_view_;
  std::vector<std::unique_ptr<ParamControl>> clip_controls_;

  std::unique_ptr<juce::FileChooser> chooser_;
  juce::String brain_id_;
  std::uint64_t seen_version_ = ~0ull;
};

} // namespace sb::plugin
