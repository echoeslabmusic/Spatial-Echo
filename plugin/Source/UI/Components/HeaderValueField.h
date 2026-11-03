// HeaderValueField: Compact terminal-style parameter field for the header strip (dim label + bordered drag/type value
//   box).

#pragma once
#include <JuceHeader.h>
#include "CRTLookAndFeel.h"
#include "ParameterValueLayout.h"
#include "../UIConstants.h"

/**
 * HeaderValueField - the header strip's master controls (OUT, MIX): a dim
 * label beside a bordered value box with a proportional phosphor fill.
 *
 * Interaction:
 *  - vertical drag  -> value change (shift = fine)
 *  - mouse wheel    -> small steps
 *  - double-click   -> type a value inline; the parameter's own text
 *                      conversion parses it (units optional)
 * Host automation is bracketed with begin/end gestures via ParameterAttachment.
 */
class HeaderValueField : public juce::Component, public juce::SettableTooltipClient
{
public:
    HeaderValueField(juce::RangedAudioParameter& param, juce::String labelText, int boxWidth)
        : parameter(param)
        , attachment(
              param, [this](float) { refreshValueText(); }, nullptr)
        , label(std::move(labelText))
        , boxW(juce::jmax(boxWidth, ParameterValueLayout::requiredParameterEnvelopeWidth(valueFont(), parameter)))
    {
        const auto green = CRTLookAndFeel::getTerminalGreen();
        editorBox.setFont(valueFont());
        editorBox.setJustification(juce::Justification::centred);
        editorBox.setColour(juce::TextEditor::backgroundColourId, CRTLookAndFeel::getVoidBlack());
        editorBox.setColour(juce::TextEditor::textColourId, green);
        editorBox.setColour(juce::TextEditor::highlightColourId, green.withAlpha(0.3f));
        editorBox.setColour(juce::TextEditor::outlineColourId, green.withAlpha(0.5f));
        editorBox.setColour(juce::TextEditor::focusedOutlineColourId, green.withAlpha(0.8f));
        editorBox.setVisible(false);
        editorBox.onReturnKey = [this]() { endEdit(true); };
        editorBox.onEscapeKey = [this]() { endEdit(false); };
        editorBox.onFocusLost = [this]() { endEdit(true); };
        addChildComponent(editorBox);

        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        attachment.sendInitialUpdate();
    }

    // Width needed for the dim label + gap + value box at header font metrics.
    int getPreferredWidth() const
    {
        return juce::GlyphArrangement::getStringWidthInt(labelFont(), label) + kLabelGap + boxW;
    }

    bool hasValidValueLayout() const { return ParameterValueLayout::fits(valueFont(), valueText, boxBounds()); }

    float getValueFontHeight() const { return valueFont().getHeight(); }

    // Dim + block interaction while the associated lock parameter is engaged.
    void setLocked(bool shouldLock)
    {
        if (locked == shouldLock)
            return;
        locked = shouldLock;
        if (locked)
            endEdit(false);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto green = CRTLookAndFeel::getTerminalGreen();
        const float dim = locked ? 0.45f : 1.0f;
        const auto box = boxBounds().toFloat();

        g.setColour(green.withAlpha(0.6f * dim));
        g.setFont(labelFont());
        g.drawText(label, getLocalBounds().withRight(box.toNearestInt().getX() - kLabelGap),
                   juce::Justification::centredRight, false);

        // Value box: same treatment as the SpatialPanel readout boxes.
        g.setColour(green.withAlpha(0.08f));
        g.fillRoundedRectangle(box, 2.0f);

        // Proportional phosphor fill (head-chip idiom): the bar tracks the
        // parameter, which both shows the level at a glance and reads as
        // "draggable" - the affordance the plain box lacked.
        const float norm = parameter.getValue();
        const bool hot = isMouseOverOrDragging() && !locked;
        if (norm > 0.0f)
        {
            g.setColour(green.withAlpha((dragging ? 0.30f : hot ? 0.22f : 0.14f) * dim));
            g.fillRoundedRectangle(box.withWidth(box.getWidth() * juce::jlimit(0.0f, 1.0f, norm)), 2.0f);
        }

        g.setColour(green.withAlpha(dragging ? 0.9f : hot ? 0.55f : 0.35f));
        g.drawRoundedRectangle(box, 2.0f, 1.0f);

        if (!editorBox.isVisible())
        {
            g.setColour(green.withAlpha(((dragging || hot) ? 1.0f : 0.9f) * dim));
            ParameterValueLayout::drawValueText(g, *this, valueFont(), valueText, box.toNearestInt(),
                                                juce::Justification::centred);
        }
    }

    void resized() override { editorBox.setBounds(boxBounds()); }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (locked || editorBox.isVisible())
            return;
        dragStartNorm = parameter.getValue();
        attachment.beginGesture();
        dragging = true;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (!dragging)
            return;
        const float sensitivity = event.mods.isShiftDown() ? 0.0008f : 0.005f;
        const float norm =
            juce::jlimit(0.0f, 1.0f, dragStartNorm - (float)event.getDistanceFromDragStartY() * sensitivity);
        attachment.setValueAsPartOfGesture(parameter.convertFrom0to1(norm));
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (!dragging)
            return;
        dragging = false;
        attachment.endGesture();
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (locked)
            return;
        editorBox.setText(valueText, juce::dontSendNotification);
        editorBox.setVisible(true);
        editorBox.grabKeyboardFocus();
        editorBox.selectAll();
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (locked || editorBox.isVisible())
            return;
        const float norm = juce::jlimit(0.0f, 1.0f, parameter.getValue() + wheel.deltaY * 0.25f);
        attachment.setValueAsCompleteGesture(parameter.convertFrom0to1(norm));
    }

    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }

private:
    static constexpr int kLabelGap = 6;

    static juce::Font valueFont() { return CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize); }
    static juce::Font labelFont() { return CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize); }

    juce::Rectangle<int> boxBounds() const { return getLocalBounds().removeFromRight(boxW); }

    void refreshValueText()
    {
        valueText = parameter.getCurrentValueAsText().toUpperCase();
        repaint();
    }

    void endEdit(bool apply)
    {
        if (!editorBox.isVisible())
            return;
        const auto text = editorBox.getText().trim();
        editorBox.setVisible(false);
        if (apply && text.isNotEmpty() && !locked)
        {
            const float norm = juce::jlimit(0.0f, 1.0f, parameter.getValueForText(text));
            attachment.setValueAsCompleteGesture(parameter.convertFrom0to1(norm));
        }
        repaint();
    }

    juce::RangedAudioParameter& parameter;
    juce::ParameterAttachment attachment;
    juce::String label;
    int boxW = 60;
    juce::String valueText;
    juce::TextEditor editorBox;
    float dragStartNorm = 0.0f;
    bool dragging = false;
    bool locked = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeaderValueField)
};
