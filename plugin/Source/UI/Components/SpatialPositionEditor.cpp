// SpatialPositionEditor (implementation): Exact-entry popover: az/el/dist typed entry + locks.

#include "SpatialPositionEditor.h"
#include "../../SpatialParamIDs.h"
#include "CRTLookAndFeel.h"
#include "ParameterValueLayout.h"
#include "../UIConstants.h"

namespace
{
constexpr int kRowHeight = 24;
constexpr int kRowGap = 4;
constexpr int kPad = 8;
constexpr int kLabelWidth = 58;
constexpr int kLockWidth = 38;

struct AxisSpec
{
    const char* label;
    const char* lockText;
    const char* lockParamId;
    float lo, hi;
};
constexpr AxisSpec kAxes[3] = {
    {"AZIMUTH", "AZ", SpatialIDs::binaural::kLockAzimuth, -180.0f, 180.0f},
    {"ELEV", "EL", SpatialIDs::binaural::kLockElevation, -90.0f, 90.0f},
    {"DIST", "DIST", SpatialIDs::binaural::kLockDistance, 0.1f, 10.0f},
};

float parseNumericValue(const juce::String& text, float fallback)
{
    juce::String clean;
    clean.preallocateBytes((size_t)text.length());
    for (auto c : text)
    {
        if ((c >= '0' && c <= '9') || c == '-' || c == '.')
            clean << c;
    }
    if (clean.isEmpty())
        return fallback;
    return clean.getFloatValue();
}

juce::String formatAxisValue(int axis, float value)
{
    if (axis == 2)
        return juce::String(value, 1) + " m";
    return juce::String((int)value) + juce::String::charToString(0x00B0);
}
}  // namespace

void SpatialPositionEditor::LockLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                                              bool shouldDrawButtonAsHighlighted,
                                                              bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused(shouldDrawButtonAsDown);
    auto bounds = button.getLocalBounds().toFloat();

    if (shouldDrawButtonAsHighlighted)
    {
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.06f));
        g.fillRoundedRectangle(bounds, 2.0f);
    }

    // Padlock glyph + the axis label (locks are never glyph-only).
    const bool locked = button.getToggleState();
    auto glyphArea = bounds.removeFromLeft(16.0f);
    CRTLookAndFeel::drawPadlockGlyph(g, glyphArea, locked, shouldDrawButtonAsHighlighted);
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(
        locked ? UIConstants::alphaBright
               : (shouldDrawButtonAsHighlighted ? UIConstants::alphaLabel : UIConstants::alphaDim)));
    g.drawText(button.getButtonText(), bounds, juce::Justification::centredLeft);
}

SpatialPositionEditor::SpatialPositionEditor(juce::AudioProcessorValueTreeState& apvts)
    : valueTreeState(apvts)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();

    for (int axis = 0; axis < 3; ++axis)
    {
        const auto& spec = kAxes[(size_t)axis];

        auto& label = axisLabels[(size_t)axis];
        label.setText(spec.label, juce::dontSendNotification);
        label.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
        label.setColour(juce::Label::textColourId, green.withAlpha(UIConstants::alphaLabel));
        label.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(label);

        auto& value = axisValues[(size_t)axis];
        value.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize));
        value.setColour(juce::Label::textColourId, green);
        value.setJustificationType(juce::Justification::centred);
        value.setEditable(true, true, false);
        value.onTextChange = [this, axis] { handleEdit(axis); };
        addAndMakeVisible(value);

        auto& lock = axisLocks[(size_t)axis];
        lock.setButtonText(spec.lockText);
        lock.setClickingTogglesState(true);
        lock.setLookAndFeel(&lockLookAndFeel);
        lock.setTooltip(juce::String("Lock ") + spec.label + " (prevent mouse/keyboard changes)");
        lock.setTitle(juce::String("Lock ") + spec.label);
        addAndMakeVisible(lock);
        lockAttachments[(size_t)axis] = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            valueTreeState, spec.lockParamId, lock);
    }

    axisValues[0].setTooltip("Horizontal angle (-180 to +180 degrees). Click to type.");
    axisValues[1].setTooltip("Vertical angle (-90 to +90 degrees). Click to type.");
    axisValues[2].setTooltip("Distance (0.1 to 10.0 metres). Click to type.");

    setTitle("position");
    setSize(preferredWidth, preferredHeight);
    startTimerHz(10);
}

SpatialPositionEditor::~SpatialPositionEditor()
{
    stopTimer();
    for (auto& lock : axisLocks)
        lock.setLookAndFeel(nullptr);
}

void SpatialPositionEditor::resized()
{
    auto area = getLocalBounds().reduced(kPad);
    for (int axis = 0; axis < 3; ++axis)
    {
        auto row = area.removeFromTop(kRowHeight);
        area.removeFromTop(kRowGap);
        axisLabels[(size_t)axis].setBounds(row.removeFromLeft(kLabelWidth));
        axisLocks[(size_t)axis].setBounds(row.removeFromRight(kLockWidth));
        row.removeFromRight(4);
        axisValues[(size_t)axis].setBounds(row);
    }
    refreshValues(true);
}

void SpatialPositionEditor::paint(juce::Graphics& g)
{
    // Out-of-range entry feedback: amber ring around the offending readout
    // (the receiver clamps the value itself).
    if (errorFlashAxis >= 0)
    {
        g.setColour(CRTLookAndFeel::getWarningAmber().withAlpha(0.85f));
        g.drawRoundedRectangle(axisValues[(size_t)errorFlashAxis].getBounds().expanded(3, 2).toFloat(), 2.0f, 1.4f);
    }
}

void SpatialPositionEditor::flashRangeError(int axis)
{
    errorFlashAxis = axis;
    repaint();
    juce::Timer::callAfterDelay(700,
                                [safe = juce::Component::SafePointer<SpatialPositionEditor>(this)]
                                {
                                    if (safe != nullptr)
                                    {
                                        safe->errorFlashAxis = -1;
                                        safe->repaint();
                                    }
                                });
}

void SpatialPositionEditor::handleEdit(int axis)
{
    if (suppressCallbacks)
        return;

    const float fallback = getAxisValue ? getAxisValue(axis) : 0.0f;

    // Locked axes refuse typed edits the same way they refuse drags.
    if (axisLocks[(size_t)axis].getToggleState())
    {
        refreshValues(true);
        return;
    }

    const float value = parseNumericValue(axisValues[(size_t)axis].getText(), fallback);
    const auto& spec = kAxes[(size_t)axis];
    if (value < spec.lo || value > spec.hi)
        flashRangeError(axis);

    if (onAxisEdit)
        onAxisEdit(axis, value);
    refreshValues(true);
}

void SpatialPositionEditor::refreshValues(bool force)
{
    if (getAxisValue == nullptr)
        return;
    suppressCallbacks = true;
    for (int axis = 0; axis < 3; ++axis)
    {
        auto& value = axisValues[(size_t)axis];
        if (!force && value.isBeingEdited())
            continue;
        const auto text = formatAxisValue(axis, getAxisValue(axis));
        value.setText(text, juce::dontSendNotification);
        if (!value.getLocalBounds().isEmpty())
            ParameterValueLayout::assertFits(value, CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize),
                                             text, value.getLocalBounds());
    }
    suppressCallbacks = false;
}

void SpatialPositionEditor::timerCallback()
{
    // Mirror drags/automation happening while the popover is open.
    refreshValues(false);
}

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
bool SpatialPositionEditor::hasValidValueLayoutForTesting() const
{
    const auto font = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize);
    for (const auto& value : axisValues)
        if (!ParameterValueLayout::fits(font, value.getText(), value.getLocalBounds()))
            return false;
    return true;
}

float SpatialPositionEditor::getValueFontHeightForTesting() const
{
    return axisValues.front().getFont().getHeight();
}
#endif
