// ParameterValueLayoutTest: Parameter value sizing and no-truncation regression coverage.

#include <JuceHeader.h>

#ifndef SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
#define SPATIAL_ECHO_ENABLE_HEADLESS_TESTS 0
#endif

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "SystemTests.h"
#include "../../PluginEditor.h"
#include "../../PluginProcessor.h"
#include "../../UI/UIConstants.h"
#include "../../UI/Components/CRTLookAndFeel.h"
#include "../../UI/Components/DelayPanel.h"
#include "../../UI/Components/HeaderValueField.h"
#include "../../UI/Components/ParameterValueComboBox.h"
#include "../../UI/Components/ParameterValueLayout.h"
#include "../../UI/Components/ParameterValueSlider.h"
#include "../../UI/Components/SpatialMotionSection.h"
#include "../../UI/Components/SpatialPositionEditor.h"
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace SpatialEchoTests::System
{
namespace
{
bool expect(bool condition, juce::String description, juce::StringArray& failures)
{
    if (!condition)
        failures.addIfNotAlreadyThere(std::move(description));
    return condition;
}

juce::String componentName(const juce::Component& component, const juce::String& path)
{
    if (component.getName().isNotEmpty())
        return path + "/" + component.getName();
    if (component.getTitle().isNotEmpty())
        return path + "/" + component.getTitle();
    return path;
}

void auditVisibleValues(juce::Component& component, const juce::String& path, juce::StringArray& observed,
                        juce::StringArray& failures)
{
    if (!component.isVisible())
        return;

    const auto name = componentName(component, path);
    const auto font = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize);

    if (auto* slider = dynamic_cast<ParameterValueSlider*>(&component))
    {
        const auto text = slider->getTextFromValue(slider->getValue());
        observed.addIfNotAlreadyThere(text);
        expect(slider->hasValidValueLayout(), name + ": Slider parent is narrower than value plus track", failures);
        expect(ParameterValueLayout::fits(font, text, {0, 0, slider->getResolvedTextBoxWidth(), 1}),
               name + ": Slider text box cannot fit '" + text + "'", failures);
        expect(slider->getValueFontHeight() >= UIConstants::typeValueSize,
               name + ": Slider value font is below typeValueSize", failures);
    }

    if (auto* combo = dynamic_cast<ParameterValueComboBox*>(&component))
    {
        observed.addIfNotAlreadyThere(combo->getText());
        expect(combo->hasValidValueLayout(), name + ": ComboBox is narrower than its choice envelope", failures);
        expect(combo->getValueFontHeight() >= UIConstants::typeValueSize,
               name + ": ComboBox value font is below typeValueSize", failures);

        // Widening a combo to fit its longest choice must not push it out
        // of its parent or over a sibling control (e.g. the head-ratio step arrows).
        if (auto* parent = combo->getParentComponent())
        {
            expect(parent->getLocalBounds().contains(combo->getBounds()),
                   name + ": ComboBox extends outside its parent", failures);
            for (auto* sibling : parent->getChildren())
            {
                const bool isControl = dynamic_cast<juce::Button*>(sibling) != nullptr ||
                                       dynamic_cast<juce::ComboBox*>(sibling) != nullptr ||
                                       dynamic_cast<juce::Slider*>(sibling) != nullptr;
                if (sibling != combo && isControl && sibling->isVisible() &&
                    sibling->getBounds().intersects(combo->getBounds()))
                    failures.addIfNotAlreadyThere(name + ": ComboBox overlaps sibling '" + sibling->getName() + "' " +
                                                  sibling->getBounds().toString());
            }
        }
    }

    if (auto* field = dynamic_cast<HeaderValueField*>(&component))
    {
        expect(field->hasValidValueLayout(), name + ": header value does not fit", failures);
        expect(field->getValueFontHeight() >= UIConstants::typeValueSize,
               name + ": header value font is below typeValueSize", failures);
    }

    if (auto* lamp = dynamic_cast<LampButton*>(&component))
    {
        observed.addIfNotAlreadyThere(lamp->getButtonText());
        expect(lamp->getWidth() >= lamp->getRequiredWidth(), name + ": lamp chip cannot fit its engaged label",
               failures);
        if (auto* parent = lamp->getParentComponent())
            expect(parent->getLocalBounds().contains(lamp->getBounds()),
                   name + ": lamp chip extends outside its parent", failures);
    }

    if (auto* head = dynamic_cast<HeadValueButton*>(&component))
    {
        observed.addIfNotAlreadyThere(head->getButtonText());
        expect(head->hasValidValueLayout(), name + ": delay-head value does not fit", failures);
        expect(head->getValueFontHeight() >= UIConstants::typeValueSize,
               name + ": delay-head value font is below typeValueSize", failures);
    }

    if (auto* motion = dynamic_cast<SpatialMotionSection*>(&component))
    {
        expect(motion->hasValidDepthReadoutLayoutForTesting(), name + ": motion depth readout does not fit", failures);
        expect(motion->getDepthReadoutFontHeightForTesting() >= UIConstants::typeValueSize,
               name + ": motion depth font is below typeValueSize", failures);
    }

    for (int i = 0; i < component.getNumChildComponents(); ++i)
    {
        if (auto* child = component.getChildComponent(i))
            auditVisibleValues(*child, name + "/" + juce::String(i), observed, failures);
    }
}

void setAllRangedParameters(SpatialEchoAudioProcessor& processor, int stateIndex)
{
    for (auto* parameter : processor.getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter))
        {
            const float normalized = stateIndex == 0 ? 0.0f : stateIndex == 1 ? ranged->getDefaultValue() : 1.0f;
            ranged->setValueNotifyingHost(normalized);
        }
    }
}

void setNormalized(juce::AudioProcessorValueTreeState& vts, const char* parameterId, float normalized,
                   juce::StringArray& failures)
{
    if (auto* parameter = vts.getParameter(parameterId))
        parameter->setValueNotifyingHost(normalized);
    else
        failures.addIfNotAlreadyThere(juce::String("missing parameter: ") + parameterId);
}

void pumpMessages(int milliseconds = 20)
{
    juce::Thread::sleep(milliseconds);
    juce::Timer::callPendingTimersSynchronously();
}

struct ParameterStateRestorer
{
    explicit ParameterStateRestorer(SpatialEchoAudioProcessor& processor)
    {
        for (auto* parameter : processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter))
                values.emplace_back(ranged, ranged->getValue());
    }

    ~ParameterStateRestorer()
    {
        for (const auto& [parameter, normalized] : values)
            parameter->setValueNotifyingHost(normalized);
    }

    std::vector<std::pair<juce::RangedAudioParameter*, float>> values;
};
}  // namespace

juce::String runParameterValueNoTruncationTest(SpatialEchoAudioProcessor& processor)
{
    juce::StringArray failures;
    const auto font = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize);

    const auto exactWidth = ParameterValueLayout::requiredWidth(font, "Neutral");
    expect(exactWidth == juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(font, "Neutral"))) +
                             ParameterValueLayout::totalHorizontalPadding,
           "required width does not equal shaped text plus padding", failures);

    auto& vts = processor.getValueTreeState();
    auto* tone = vts.getParameter("ReverbTone");
    auto* feedback = vts.getParameter("Intensity");
    expect(tone != nullptr && tone->getText(1.0f, 64) == "Bright", "ReverbTone maximum formatter changed", failures);
    expect(tone != nullptr && tone->getText(0.0f, 64) == "Dark", "ReverbTone minimum formatter changed", failures);
    expect(feedback != nullptr && feedback->getText(1.0f, 64) == "Self-Osc!", "Intensity maximum formatter changed",
           failures);

    ParameterValueSlider slider;
    slider.setBounds(0, 0, 360, 28);
    slider.setRange(0.0, 2.0, 1.0);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 22);
    slider.setValueTextFormatter(
        [](double value)
        {
            if (value == 0.0)
                return juce::String("Dark");
            if (value == 2.0)
                return juce::String("Bright");
            return juce::String("Neutral");
        });
    expect(slider.getResolvedTextBoxWidth() >= exactWidth, "configured width acted as a cap", failures);
    const int stableWidth = slider.getResolvedTextBoxWidth();
    slider.setValue(0.0, juce::sendNotificationSync);
    expect(slider.getResolvedTextBoxWidth() == stableWidth, "precomputed envelope shrank during automation", failures);

    ParameterValueSlider missedSample;
    missedSample.setBounds(0, 0, 420, 28);
    missedSample.setRange(0.0, 1.0, 0.0);
    missedSample.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 22);
    missedSample.setValueTextFormatter(
        [](double value)
        {
            return std::abs(value - 0.5005) < 1.0e-8 ? juce::String("A future formatter branch wider than the sweep")
                                                     : juce::String("Short");
        });
    const int sampledWidth = missedSample.getResolvedTextBoxWidth();
    missedSample.setValue(0.5005, juce::sendNotificationSync);
    expect(missedSample.getResolvedTextBoxWidth() > sampledWidth, "actual-string fallback did not grow synchronously",
           failures);
    const int grownWidth = missedSample.getResolvedTextBoxWidth();
    missedSample.setValue(0.0, juce::sendNotificationSync);
    expect(missedSample.getResolvedTextBoxWidth() == grownWidth, "actual-string fallback later shrank", failures);

    const int undersizedWidth = grownWidth + ParameterValueLayout::minimumHorizontalTrackWidth - 1;
    expect(undersizedWidth < missedSample.getMinimumRequiredComponentWidth(), "undersized parent geometry was accepted",
           failures);

    ParameterStateRestorer restoreParameters(processor);
    juce::StringArray observedStrings;
    const juce::Rectangle<int> editorSizes[] = {
        {0, 0, 1200, 558},
        {0, 0, 1400, 650},
        {0, 0, 1800, 837},
    };
    constexpr const char* stateNames[] = {"minimum", "default", "maximum"};

    for (const auto size : editorSizes)
    {
        setAllRangedParameters(processor, 1);
        std::unique_ptr<juce::AudioProcessorEditor> baseEditor(processor.createEditor());
        auto* editor = dynamic_cast<SpatialEchoAudioProcessorEditor*>(baseEditor.get());
        if (!expect(editor != nullptr, "createEditor did not return SpatialEchoAudioProcessorEditor", failures))
            break;

        editor->setBounds(size);
        editor->setVisible(true);
        editor->resized();
        pumpMessages();

        for (int state = 0; state < 3; ++state)
        {
            const juce::String context =
                juce::String(size.getWidth()) + "x" + juce::String(size.getHeight()) + "/" + stateNames[state];
            setAllRangedParameters(processor, state);
            editor->setValueDisclosuresForTesting(false, false);
            editor->resized();
            pumpMessages();

            for (const float tempoSync : {0.0f, 1.0f})
            {
                setNormalized(vts, "TempoSync", tempoSync, failures);
                pumpMessages();
                editor->resized();
                auditVisibleValues(*editor, context + (tempoSync > 0.5f ? "/sync-on" : "/sync-off"), observedStrings,
                                   failures);
            }

            editor->setValueDisclosuresForTesting(true, true);
            pumpMessages();
            auditVisibleValues(*editor, context + "/disclosures", observedStrings, failures);

            editor->showEchoEngineDropdownForTesting();
            pumpMessages(70);
            if (auto* dropdown = editor->getEchoEngineDropdownWindowForTesting())
                auditVisibleValues(*dropdown, context + "/echo-engine-dropdown", observedStrings, failures);
            else
                failures.addIfNotAlreadyThere(context + ": Echo Engine dropdown missing");

            setNormalized(vts, "ReverbEngine", 0.0f, failures);
            editor->showReverbDropdownForTesting();
            pumpMessages(70);
            if (auto* dropdown = editor->getReverbDropdownWindowForTesting())
                auditVisibleValues(*dropdown, context + "/spring-dropdown", observedStrings, failures);
            else
                failures.addIfNotAlreadyThere(context + ": Reverb dropdown missing");

            setNormalized(vts, "ReverbEngine", 1.0f, failures);
            pumpMessages(70);
            if (auto* dropdown = editor->getReverbDropdownWindowForTesting())
                auditVisibleValues(*dropdown, context + "/cloud-dropdown", observedStrings, failures);
        }
    }

    SpatialPositionEditor positionEditor(vts);
    positionEditor.getAxisValue = [](int axis) { return axis == 0 ? -180.0f : axis == 1 ? 90.0f : 10.0f; };
    positionEditor.setBounds(0, 0, SpatialPositionEditor::preferredWidth, SpatialPositionEditor::preferredHeight);
    positionEditor.resized();
    expect(positionEditor.hasValidValueLayoutForTesting(), "exact-position committed values do not fit", failures);
    expect(positionEditor.getValueFontHeightForTesting() >= UIConstants::typeValueSize,
           "exact-position value font is below typeValueSize", failures);

    // ReverbTone display simplified to Bright/Neutral/Dark in 79ca6c0; "Dark"
    // is the only string unique to that formatter, so it is the real canary.
    expect(observedStrings.contains("Bright"), "geometry audit never observed Bright", failures);
    expect(observedStrings.contains("Dark"), "geometry audit never observed Dark", failures);
    expect(observedStrings.contains("Self-Osc!"), "geometry audit never observed Self-Osc!", failures);
    expect(observedStrings.contains("DUB"), "geometry audit never observed the DUB chip", failures);
    expect(observedStrings.contains("Classic (1:1.9:2.75)"),
           "geometry audit never observed the Echo Engine head-ratio combo", failures);

    juce::String result = "ParameterValueNoTruncation: failures=" + juce::String(failures.size());
    if (!failures.isEmpty())
        result << " [" << failures.joinIntoString(" | ") << "]";
    result << (failures.isEmpty() ? " [PASS]" : " [FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::System

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
