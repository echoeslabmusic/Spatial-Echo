// ReverbDropdown: Clickable "REVERB" header that opens the reverb fine-tune drawer.

#pragma once
#include <JuceHeader.h>
#include <optional>
#include "ParameterValueComboBox.h"
#include "ParameterValueSlider.h"
#include "SectionDropdown.h"

/**
 * ReverbDropdown - the "REVERB" section header. Clicking it opens a drawer
 * with the full macro schema:
 *   - Engine selection (Spring / Cloud) + per-engine presets
 *   - Size, Decay, Pre-Delay, Width (space)
 *   - Tone, Character (voicing), Mod (motion), Ducking (texture)
 *   - Reset to defaults
 */
class ReverbDropdown : public SectionDropdown
{
public:
    enum class WalkthroughRegion
    {
        engine,
        preset,
        preDelayWidth,
        character,
        mod,
        ducking,
        pin,
        reset
    };

    explicit ReverbDropdown(juce::AudioProcessorValueTreeState& apvts);
    ~ReverbDropdown() override;

    juce::Rectangle<int> getWalkthroughScreenBounds() const
    {
        if (auto* window = getDropdownWindow(); window != nullptr && window->isShowing())
            return window->getScreenBounds();
        return getScreenBounds();
    }
    std::optional<juce::Rectangle<int>> getWalkthroughScreenBounds(WalkthroughRegion region) const;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    bool applyPresetSelectionForTesting(int engine, int presetIndex);
#endif

    // Resync the TYPE combo to the current reverb state after a global preset load.
    // Called by PluginEditor after loadFactoryPreset/loadUserPreset/loadDefaultPreset.
    void resyncTypeComboFromPreset();

private:
    void createDropdownWindow() override;
    void pinnedStateChanged(bool pinned) override;

    juce::AudioProcessorValueTreeState& valueTreeState;

    int lastPresetIndex = 0;
    int lastCloudPresetIndex = 0;

    class DropdownContent;
    std::unique_ptr<DropdownContent> createConfiguredContent();
    juce::Component::SafePointer<DropdownContent> dropdownContent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbDropdown)
};

/**
 * DropdownContent - the drawer with the reverb macros
 */
class ReverbDropdown::DropdownContent : public juce::Component, private juce::Timer, private juce::Slider::Listener
{
public:
    DropdownContent(juce::AudioProcessorValueTreeState& apvts, int initialPresetIndex, int initialCloudPresetIndex);
    ~DropdownContent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    int getPreferredWidth() const;
    int getPreferredHeight() const;
    void setPinnedState(bool pinned);
    juce::Rectangle<int> getWalkthroughScreenBounds(WalkthroughRegion region) const;

    std::function<void(int)> onPresetChanged;
    std::function<void(int)> onCloudPresetChanged;
    std::function<void(int)> onRequestResize;
    std::function<void()> onPinToggle;

public:
    void resyncTypeComboFromPreset();
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    bool applyPresetSelectionForTesting(int engine, int presetIndex);
#endif

private:
    void timerCallback() override;
    void sliderValueChanged(juce::Slider* slider) override;
    void switchToCustomPreset();

    juce::AudioProcessorValueTreeState& valueTreeState;
    bool presetApplyInProgress = false;  // Suppress Custom switch during preset apply

    // Section labels
    juce::Label spaceSectionLabel;
    juce::Label toneSectionLabel;
    juce::Label textureSectionLabel;
    juce::Label motionSectionLabel;

    // Reverb TYPE combo with click-through arrows
    ParameterValueComboBox typeCombo;
    juce::TextButton prevButton, nextButton;
    juce::ToggleButton pinToggleButton;
    void stepPreset(int delta);

    // Engine selector (Spring/Cloud)
    ParameterValueComboBox engineCombo;

    // Cloud preset with click-through arrows
    ParameterValueComboBox cloudPresetCombo;
    juce::TextButton cloudPrevButton, cloudNextButton;
    void stepCloudPreset(int delta);

    // Sliders with attachments (the full 8-macro schema minus Engine)
    ParameterValueSlider sizeSlider;
    ParameterValueSlider decaySlider;
    ParameterValueSlider toneSlider;  // Reverb tone (dark to bright); binds ReverbTone parameter
    ParameterValueSlider characterSlider;
    ParameterValueSlider modSlider;
    ParameterValueSlider widthSlider;
    ParameterValueSlider preDelaySlider;
    ParameterValueSlider duckSlider;

    // Slider labels
    juce::Label sizeLabel;
    juce::Label decayLabel;
    juce::Label toneLabel;
    juce::Label characterLabel;
    juce::Label modLabel;
    juce::Label widthLabel;
    juce::Label preDelayLabel;
    juce::Label duckLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sizeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> decayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> toneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> characterAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> modAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> widthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> preDelayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> duckAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> engineAttachment;

    // Reset button
    juce::TextButton resetButton;

    void setupSectionLabel(juce::Label& label, const juce::String& text);
    void setupParamLabel(juce::Label& label, const juce::String& text);
    void setupSlider(ParameterValueSlider& slider, juce::Label& label, const juce::String& text,
                     const juce::String& suffix);
    void applySpringPreset(int presetIndex);
    void applyCloudPreset(int presetIndex);
    void resetToDefaults();
    void syncEngineVisibility();

    std::atomic<float>* engineValue = nullptr;
    int lastEngineIndex = -1;  // -1=uninitialized, 0=Spring, 1=Cloud

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DropdownContent)
};
