#include "editor.h"

#include "document_controller.h"
#include "playback_renderer.h"
#include "processor.h"

namespace sb::plugin {

namespace {

juce::String to_juce(std::string_view s) { return juce::String(s.data(), s.size()); }

template <Settings S> double get_field(const S &settings, std::string_view id) {
  double value = 0;
  visit_settings(settings, [&](const ParamInfo &info, const auto &field) {
    if (info.id == id) value = double(field);
  });
  return value;
}

template <Settings S> void set_field(S &settings, std::string_view id, double value) {
  visit_settings(settings, [&](const ParamInfo &info, auto &field) {
    using T = std::remove_cvref_t<decltype(field)>;
    if (info.id == id) field = clamp_to(info.kind, std::is_integral_v<T> ? T(std::lround(value)) : T(value));
  });
}

juce::String status_text(const RenderStatus &status) {
  return std::visit(
      [](const auto &s) -> juce::String {
        using S = std::remove_cvref_t<decltype(s)>;
        if constexpr (std::is_same_v<S, Idle>) return "";
        else if constexpr (std::is_same_v<S, Waiting>) return to_juce(s.reason);
        else if constexpr (std::is_same_v<S, Rendering>) return to_juce(s.progress->text());
        else if constexpr (std::is_same_v<S, Ready>) return "rendered";
        else return "failed: " + to_juce(s.message);
      },
      status);
}

constexpr int row_height = 26;
constexpr int gap = 6;

} // namespace

// ---------------------------------------------------------------------------

ParamControl::ParamControl(const ParamInfo &info, std::function<void(double)> on_change)
    : on_change_(std::move(on_change)) {
  label_.setText(to_juce(info.label), juce::dontSendNotification);
  addAndMakeVisible(label_);
  std::visit(
      [&](const auto &kind) {
        using K = std::remove_cvref_t<decltype(kind)>;
        if constexpr (std::is_same_v<K, ChoiceParam>) {
          auto box = std::make_unique<juce::ComboBox>();
          for (std::size_t i = 0; i < kind.labels.size(); ++i)
            box->addItem(to_juce(kind.labels[i]), int(i) + 1);
          box->onChange = [this, b = box.get()] { on_change_(b->getSelectedItemIndex()); };
          control_ = std::move(box);
        } else if constexpr (std::is_same_v<K, BoolParam>) {
          auto toggle = std::make_unique<juce::ToggleButton>();
          toggle->onClick = [this, t = toggle.get()] { on_change_(t->getToggleState()); };
          control_ = std::move(toggle);
        } else {
          auto slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal,
                                                       juce::Slider::TextBoxRight);
          const bool whole = std::is_same_v<K, IntParam>;
          slider->setRange(kind.min, kind.max, whole ? 1.0 : 0.01);
          slider->onValueChange = [this, s = slider.get()] { on_change_(s->getValue()); };
          control_ = std::move(slider);
        }
      },
      info.kind);
  addAndMakeVisible(*control_);
}

void ParamControl::set_value(double value) {
  if (auto *box = dynamic_cast<juce::ComboBox *>(control_.get()))
    box->setSelectedItemIndex(int(value), juce::dontSendNotification);
  else if (auto *toggle = dynamic_cast<juce::ToggleButton *>(control_.get()))
    toggle->setToggleState(value > 0.5, juce::dontSendNotification);
  else if (auto *slider = dynamic_cast<juce::Slider *>(control_.get()))
    slider->setValue(value, juce::dontSendNotification);
}

void ParamControl::resized() {
  auto area = getLocalBounds();
  label_.setBounds(area.removeFromLeft(130));
  control_->setBounds(area);
}

// ---------------------------------------------------------------------------

SamplebrainEditor::SamplebrainEditor(SamplebrainProcessor &p)
    : juce::AudioProcessorEditor(&p), juce::AudioProcessorEditorARAExtension(&p) {
  for (auto *c : std::initializer_list<juce::Component *>{
           &brain_box_, &new_brain_, &import_brain_, &export_brain_, &brain_users_, &samples_, &add_samples_,
           &remove_sample_, &build_, &cancel_, &brain_status_, &clip_box_, &clip_status_,
           &clip_view_})
    addAndMakeVisible(c);
  clip_view_.setViewedComponent(&clip_panel_, false);
  samples_.setRowHeight(22);

  brain_box_.onChange = [this] {
    const int i = brain_box_.getSelectedItemIndex();
    if (auto *dc = controller(); dc && i >= 0 && i < int(dc->brains().size()))
      brain_id_ = dc->brains()[std::size_t(i)]->definition.id;
    // Choosing a brain here also points the selected clip at it.
    if (auto *c = clip(); c && c->settings.brain_id != brain_id_)
      controller()->edit_clip(*c, [&](ClipSettings &s) { s.brain_id = brain_id_; });
    refresh();
  };
  new_brain_.onClick = [this] { new_brain(); };
  import_brain_.onClick = [this] {
    chooser_ = std::make_unique<juce::FileChooser>("Import brain", juce::File(), "*.brain");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                              juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser &fc) {
      if (auto *dc = controller(); dc && fc.getResult() != juce::File())
        dc->import_brain(fc.getResult(), [this, safe = juce::Component::SafePointer(this)](const juce::String &id) {
          if (!safe) return;
          brain_id_ = id;
          if (auto *c = clip())
            controller()->edit_clip(*c, [&](ClipSettings &s) { s.brain_id = id; });
        });
    });
  };
  export_brain_.onClick = [this] {
    chooser_ = std::make_unique<juce::FileChooser>("Export brain", juce::File(), "*.brain");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode, [this](const juce::FileChooser &fc) {
      if (auto *dc = controller(); dc && fc.getResult() != juce::File())
        dc->export_brain(brain_id_, fc.getResult().withFileExtension("brain"));
    });
  };
  add_samples_.onClick = [this] {
    chooser_ = std::make_unique<juce::FileChooser>("Add samples", juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                              juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::canSelectMultipleItems,
                          [this](const juce::FileChooser &fc) {
                            juce::StringArray paths;
                            for (const auto &f : fc.getResults()) paths.add(f.getFullPathName());
                            add_files(paths);
                          });
  };
  remove_sample_.onClick = [this] {
    const int row = samples_.getSelectedRow();
    if (auto *dc = controller(); dc && row >= 0)
      dc->edit_brain(brain_id_, [&](BrainDefinition &d) {
        if (row < int(d.samples.size())) d.samples.erase(d.samples.begin() + row);
      });
  };
  build_.onClick = [this] {
    if (auto *dc = controller()) dc->build_brain(brain_id_);
  };
  cancel_.onClick = [this] {
    if (auto *dc = controller()) dc->cancel_build(brain_id_);
  };
  clip_box_.onChange = [this] { refresh(); };

  for (const ParamInfo &info : brain_params)
    brain_controls_.push_back(std::make_unique<ParamControl>(info, [this, id = info.id](double v) {
      if (auto *dc = controller())
        dc->edit_brain(brain_id_, [&](BrainDefinition &d) { set_field(d.settings, id, v); });
    }));
  for (auto &c : brain_controls_) addAndMakeVisible(*c);

  auto add_clip_control = [this](const ParamInfo &info, auto field_of) {
    clip_controls_.push_back(std::make_unique<ParamControl>(info, [this, id = info.id, field_of](double v) {
      if (auto *c = clip())
        controller()->edit_clip(*c, [&](ClipSettings &s) { set_field(field_of(s), id, v); });
    }));
    clip_panel_.addAndMakeVisible(*clip_controls_.back());
  };
  for (const ParamInfo &info : target_params)
    add_clip_control(info, [](ClipSettings &s) -> TargetSettings & { return s.target; });
  for (const ParamInfo &info : render_params)
    add_clip_control(info, [](ClipSettings &s) -> RenderSettings & { return s.render; });

  setResizable(true, false);
  setSize(760, 720);
  startTimerHz(10);
  refresh();
}

SamplebrainEditor::~SamplebrainEditor() { stopTimer(); }

SamplebrainDocumentController *SamplebrainEditor::controller() const {
  if (auto *view = getARAEditorView())
    return SamplebrainDocumentController::get(view->getDocumentController());
  return nullptr;
}

BrainState *SamplebrainEditor::brain() const {
  auto *dc = controller();
  return dc ? dc->find_brain(brain_id_) : nullptr;
}

std::vector<SamplebrainAudioModification *> SamplebrainEditor::clips() const {
  std::vector<SamplebrainAudioModification *> found;
  auto &p = static_cast<SamplebrainProcessor &>(processor);
  auto add = [&](juce::ARAPlaybackRegion *region) {
    auto *m = region->getAudioModification<SamplebrainAudioModification>();
    if (std::ranges::find(found, m) == found.end()) found.push_back(m);
  };
  if (auto *renderer = p.getPlaybackRenderer<SamplebrainPlaybackRenderer>())
    for (auto *region : renderer->getPlaybackRegions()) add(region);
  // As a track effect, REAPER makes the instance an editor renderer for the
  // whole track instead.
  if (auto *renderer = p.getEditorRenderer<juce::ARAEditorRenderer>()) {
    for (auto *region : renderer->getPlaybackRegions()) add(region);
    for (auto *sequence : renderer->getRegionSequences())
      for (auto *region : sequence->getPlaybackRegions<juce::ARAPlaybackRegion>()) add(region);
  }
  return found;
}

SamplebrainAudioModification *SamplebrainEditor::clip() const {
  const auto all = clips();
  const int i = clip_box_.getSelectedItemIndex();
  return i >= 0 && i < int(all.size()) ? all[std::size_t(i)] : nullptr;
}

SamplebrainEditor::Shown SamplebrainEditor::shown() const {
  auto *dc = controller();
  return {dc, dc ? dc->version() : 0, clips()};
}

void SamplebrainEditor::new_brain() {
  if (auto *dc = controller()) {
    brain_id_ = dc->add_brain().definition.id;
    if (auto *c = clip())
      dc->edit_clip(*c, [&](ClipSettings &s) { s.brain_id = brain_id_; });
  }
}

void SamplebrainEditor::add_files(const juce::StringArray &paths) {
  if (!brain()) new_brain();
  if (auto *dc = controller())
    dc->edit_brain(brain_id_, [&](BrainDefinition &d) {
      for (const auto &p : paths)
        if (std::ranges::find(d.samples, p, &BrainSample::path) == d.samples.end())
          d.samples.push_back({p, true});
    });
}

void SamplebrainEditor::filesDropped(const juce::StringArray &files, int, int) {
  add_files(files);
}

void SamplebrainEditor::timerCallback() {
  if (shown() != shown_) refresh();
  refresh_progress();
}

void SamplebrainEditor::refresh() {
  auto *dc = controller();
  shown_ = shown();
  for (auto *child : getChildren()) child->setVisible(dc != nullptr);
  repaint();
  if (!dc) return;

  // The clips this instance plays.
  const auto all = clips();
  const int clip_index = std::max(0, clip_box_.getSelectedItemIndex());
  clip_box_.clear(juce::dontSendNotification);
  for (std::size_t i = 0; i < all.size(); ++i) {
    const char *name = all[i]->getName();
    clip_box_.addItem(name ? juce::String::fromUTF8(name) : "Clip " + juce::String(i + 1), int(i) + 1);
  }
  if (!all.empty())
    clip_box_.setSelectedItemIndex(std::min(clip_index, int(all.size()) - 1),
                                   juce::dontSendNotification);
  auto *c = clip();
  if (c && dc->find_brain(c->settings.brain_id)) brain_id_ = c->settings.brain_id;
  if (!brain() && !dc->brains().empty()) brain_id_ = dc->brains().front()->definition.id;

  brain_box_.clear(juce::dontSendNotification);
  for (std::size_t i = 0; i < dc->brains().size(); ++i) {
    const auto &b = dc->brains()[i];
    brain_box_.addItem(b->definition.name, int(i) + 1);
    if (b->definition.id == brain_id_)
      brain_box_.setSelectedItemIndex(int(i), juce::dontSendNotification);
  }

  BrainState *b = brain();
  for (auto *comp : std::initializer_list<juce::Component *>{
           &samples_, &remove_sample_, &build_, &export_brain_})
    comp->setEnabled(b != nullptr);
  for (auto &control : brain_controls_) control->setEnabled(b != nullptr);
  if (b) {
    const int users = dc->clips_using(brain_id_);
    brain_users_.setText(juce::String(users) + (users == 1 ? " clip" : " clips"),
                         juce::dontSendNotification);
    for (std::size_t i = 0; i < brain_controls_.size(); ++i)
      brain_controls_[i]->set_value(get_field(b->definition.settings, brain_params[i].id));
    build_.setButtonText(b->needs_build() ? "Build brain *" : "Build brain");
    export_brain_.setEnabled(!b->playing.empty());
  }
  samples_.updateContent();
  samples_.repaint();

  clip_view_.setEnabled(c != nullptr);
  if (c) {
    std::size_t i = 0;
    for (const ParamInfo &info : target_params)
      clip_controls_[i++]->set_value(get_field(c->settings.target, info.id));
    for (const ParamInfo &info : render_params)
      clip_controls_[i++]->set_value(get_field(c->settings.render, info.id));
  }
  refresh_progress();
}

void SamplebrainEditor::refresh_progress() {
  BrainState *b = brain();
  juce::String text;
  auto *dc = controller();
  if (dc && dc->import_progress()) text = to_juce(dc->import_progress()->text());
  else if (dc && !dc->import_error().empty()) text = to_juce(dc->import_error());
  else if (b && b->build) text = to_juce(b->build->text());
  else if (b && !b->error.empty()) text = to_juce(b->error);
  brain_status_.setText(text, juce::dontSendNotification);
  cancel_.setEnabled(b && b->build);
  build_.setEnabled(b && !b->build);
  if (b && b->build) samples_.repaint();

  auto *c = clip();
  clip_status_.setText(c ? status_text(c->status()) : juce::String(),
                       juce::dontSendNotification);
}

int SamplebrainEditor::getNumRows() {
  BrainState *b = brain();
  return b ? int(b->definition.samples.size()) : 0;
}

void SamplebrainEditor::paintListBoxItem(int row, juce::Graphics &g, int width, int height,
                                         bool selected) {
  BrainState *b = brain();
  if (!b || row >= int(b->definition.samples.size())) return;
  const BrainSample &sample = b->definition.samples[std::size_t(row)];
  if (selected) g.fillAll(juce::Colours::white.withAlpha(0.1f));

  // Colours match the original Samplebrain app.
  juce::String state;
  juce::Colour colour = juce::Colours::lightgrey;
  const std::optional<SoundBuildState> build_state =
      b->build ? b->build->state_of(sample.path.toStdString()) : std::nullopt;
  if (!juce::File(sample.path).existsAsFile()) {
    state = "missing";
    colour = juce::Colours::red;
  } else if (build_state == SoundBuildState::Processing) {
    state = "processing";
    colour = juce::Colours::yellow;
  } else if (build_state == SoundBuildState::Built || (!b->build && !b->needs_build() && !b->playing.empty())) {
    state = "built";
    colour = juce::Colours::lightblue;
  } else {
    state = "waiting";
  }

  auto area = juce::Rectangle<int>(width, height).reduced(4, 2);
  g.setColour(juce::Colours::white);
  g.drawRect(area.removeFromLeft(height - 4).reduced(3));
  if (sample.enabled) g.fillRect(juce::Rectangle<int>(4, 2, height - 4, height - 4).reduced(6));
  area.removeFromLeft(6);
  g.setColour(colour);
  g.drawText(state, area.removeFromRight(90), juce::Justification::centredRight);
  g.setColour(juce::Colours::white);
  g.drawText(juce::File(sample.path).getFileName(), area, juce::Justification::centredLeft);
}

void SamplebrainEditor::listBoxItemClicked(int row, const juce::MouseEvent &e) {
  // The box at the left of a row switches the sample on and off.
  if (e.x > samples_.getRowHeight()) return;
  if (auto *dc = controller())
    dc->edit_brain(brain_id_, [&](BrainDefinition &d) {
      if (row < int(d.samples.size())) d.samples[std::size_t(row)].enabled ^= true;
    });
}

void SamplebrainEditor::paint(juce::Graphics &g) {
  g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
  if (!controller()) {
    const auto &p = static_cast<const SamplebrainProcessor &>(processor);
    const juce::String message =
        p.isBoundToARA() ? "The host bound Samplebrain to ARA, but not as an editor."
                         : "Samplebrain only works as an ARA extension on a clip.";
    g.setColour(juce::Colours::white);
    g.drawFittedText(message, getLocalBounds(), juce::Justification::centred, 2);
  }
}

void SamplebrainEditor::resized() {
  auto area = getLocalBounds().reduced(10);
  auto row = [&](int h = row_height) {
    auto r = area.removeFromTop(h);
    area.removeFromTop(gap);
    return r;
  };

  auto top = row();
  brain_box_.setBounds(top.removeFromLeft(220));
  top.removeFromLeft(gap);
  new_brain_.setBounds(top.removeFromLeft(70));
  top.removeFromLeft(gap);
  import_brain_.setBounds(top.removeFromLeft(110));
  top.removeFromLeft(gap);
  export_brain_.setBounds(top.removeFromLeft(110));
  top.removeFromLeft(gap);
  brain_users_.setBounds(top);

  samples_.setBounds(row(150));
  auto sample_buttons = row();
  add_samples_.setBounds(sample_buttons.removeFromLeft(90));
  sample_buttons.removeFromLeft(gap);
  remove_sample_.setBounds(sample_buttons.removeFromLeft(90));

  for (auto &c : brain_controls_) c->setBounds(row());
  auto build_row = row();
  build_.setBounds(build_row.removeFromLeft(130));
  build_row.removeFromLeft(gap);
  cancel_.setBounds(build_row.removeFromLeft(80));
  build_row.removeFromLeft(gap);
  brain_status_.setBounds(build_row);

  area.removeFromTop(gap);
  auto clip_row = row();
  clip_box_.setBounds(clip_row.removeFromLeft(220));
  clip_row.removeFromLeft(gap);
  clip_status_.setBounds(clip_row);

  clip_view_.setBounds(area);
  const int width = area.getWidth() - clip_view_.getScrollBarThickness();
  clip_panel_.setSize(width, int(clip_controls_.size()) * (row_height + gap));
  int y = 0;
  for (auto &c : clip_controls_) {
    c->setBounds(0, y, width, row_height);
    y += row_height + gap;
  }
}

} // namespace sb::plugin
