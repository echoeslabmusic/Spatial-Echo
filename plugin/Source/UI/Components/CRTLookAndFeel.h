// CRTLookAndFeel: CRT-themed JUCE LookAndFeel (fonts, colours, phosphor styling): void black background (#000000),
//   terminal green phosphor accent (#00D966), white indicators (#FFFFFF), magenta warnings (#E383FA), phosphor glow and
//   scanline texture.

#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"
#include "MenuChrome.h"
#include "ParameterValueLayout.h"
#include "../UIConstants.h"

//==============================================================================
class CRTLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Color palette
    static constexpr uint32_t voidBlack = 0xFF000000;
    static constexpr uint32_t terminalGreen = 0xFF00D966;
    static constexpr uint32_t cleanLight = 0xFFFFFFFF;
    static constexpr uint32_t magentaInterference = 0xFFE383FA;
    // Warning/alert phosphors: the ECHO ENGINE status light ramps
    // terminalGreen -> warningAmber -> alertRed with feedback heat.
    static constexpr uint32_t warningAmber = 0xFFFFB000;
    static constexpr uint32_t alertRed = 0xFFFF3B30;

    // Convenience color accessors
    static juce::Colour getVoidBlack() { return juce::Colour(voidBlack); }
    static juce::Colour getTerminalGreen() { return juce::Colour(terminalGreen); }
    static juce::Colour getCleanLight() { return juce::Colour(cleanLight); }
    static juce::Colour getMagentaInterference() { return juce::Colour(magentaInterference); }
    static juce::Colour getWarningAmber() { return juce::Colour(warningAmber); }
    static juce::Colour getAlertRed() { return juce::Colour(alertRed); }

    //==========================================================================
    CRTLookAndFeel()
    {
        setColour(juce::ResizableWindow::backgroundColourId, getVoidBlack());
        setColour(juce::Label::textColourId, getTerminalGreen());
        setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

        // DocumentWindow & Standalone wrapper chrome
        setColour(juce::DocumentWindow::textColourId, getTerminalGreen());
        setColour(juce::DocumentWindow::backgroundColourId, getVoidBlack());

        // Generic alert windows in dim phosphor green for CRT
        // consistency. The standalone mute banner (NotificationArea) ignores these IDs -
        // its colors are hardcoded in juce_StandaloneFilterWindow.h and stay JUCE-native.
        setColour(juce::AlertWindow::backgroundColourId, getVoidBlack());
        setColour(juce::AlertWindow::textColourId, getTerminalGreen().withAlpha(0.7f));
        setColour(juce::AlertWindow::outlineColourId, getTerminalGreen().withAlpha(0.5f));

        setColour(juce::Slider::textBoxTextColourId, getTerminalGreen());
        setColour(juce::Slider::textBoxBackgroundColourId, getVoidBlack());
        setColour(juce::Slider::textBoxOutlineColourId, getTerminalGreen().withAlpha(0.4f));
        setColour(juce::Slider::thumbColourId, getTerminalGreen());
        setColour(juce::Slider::rotarySliderFillColourId, getTerminalGreen());
        setColour(juce::Slider::rotarySliderOutlineColourId, getTerminalGreen().withAlpha(0.2f));

        setColour(juce::TextButton::buttonColourId, getVoidBlack());
        setColour(juce::TextButton::buttonOnColourId, getTerminalGreen().withAlpha(0.2f));
        setColour(juce::TextButton::textColourOffId, getTerminalGreen());
        setColour(juce::TextButton::textColourOnId, getTerminalGreen());

        setColour(juce::ToggleButton::textColourId, getTerminalGreen());
        setColour(juce::ToggleButton::tickColourId, getTerminalGreen());
        setColour(juce::ToggleButton::tickDisabledColourId, getTerminalGreen().withAlpha(0.3f));

        setColour(juce::ComboBox::backgroundColourId, getVoidBlack());
        setColour(juce::ComboBox::textColourId, getTerminalGreen());
        setColour(juce::ComboBox::outlineColourId, getTerminalGreen().withAlpha(0.5f));
        setColour(juce::ComboBox::arrowColourId, getTerminalGreen());
        setColour(juce::ComboBox::focusedOutlineColourId, juce::Colours::transparentBlack);

        setColour(juce::PopupMenu::backgroundColourId, getVoidBlack());
        setColour(juce::PopupMenu::textColourId, getTerminalGreen());
        setColour(juce::PopupMenu::highlightedBackgroundColourId, getTerminalGreen().withAlpha(0.2f));
        setColour(juce::PopupMenu::highlightedTextColourId, getTerminalGreen());

        setColour(juce::TextEditor::backgroundColourId, getVoidBlack());
        setColour(juce::TextEditor::textColourId, getTerminalGreen());
        setColour(juce::TextEditor::outlineColourId, getTerminalGreen().withAlpha(0.4f));
        setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        setColour(juce::TextEditor::highlightColourId, getTerminalGreen().withAlpha(0.3f));

        setColour(juce::ScrollBar::thumbColourId, getTerminalGreen().withAlpha(0.5f));
        setColour(juce::ScrollBar::trackColourId, getVoidBlack());
    }

    ~CRTLookAndFeel() override = default;

    // Suppress all keyboard/mouse focus outlines globally
    std::unique_ptr<juce::FocusOutline> createFocusOutlineForComponent(juce::Component&) override { return nullptr; }

    //==========================================================================
    // Font Management
    //==========================================================================

    // The embedded IBM VGA face is the only typeface in the UI. It is loaded
    // once per process and shared by every font request.
    static juce::Font getTerminalFontStatic(float height)
    {
        static const juce::Typeface::Ptr typeface = juce::Typeface::createSystemTypefaceFor(
            BinaryData::PxPlus_IBM_VGA_9x16_ttf, BinaryData::PxPlus_IBM_VGA_9x16_ttfSize);
        jassert(typeface != nullptr);
        return juce::Font(juce::FontOptions(typeface).withHeight(height));
    }

    //==========================================================================
    // Shared padlock glyph for lock toggles (header Mix lock + spatial axis
    // locks): closed + bright when locked, open + dim when free.
    //==========================================================================
    static void drawPadlockGlyph(juce::Graphics& g, juce::Rectangle<float> bounds, bool locked, bool highlighted)
    {
        const auto green = getTerminalGreen();
        const float alpha = locked ? 0.95f : (highlighted ? 0.6f : 0.35f);
        const float w = juce::jmin(bounds.getWidth(), bounds.getHeight());
        const float bodyW = w * 0.62f;
        const float bodyH = w * 0.42f;
        const float bodyX = bounds.getCentreX() - bodyW * 0.5f;
        const float bodyY = bounds.getCentreY() + w * 0.04f;
        const float shackleR = bodyW * 0.34f;

        g.setColour(green.withAlpha(alpha));
        g.drawRect(juce::Rectangle<float>(bodyX, bodyY, bodyW, bodyH), 1.2f);

        juce::Path shackle;
        const float cx = bounds.getCentreX() + (locked ? 0.0f : bodyW * 0.28f);
        shackle.addCentredArc(cx, bodyY, shackleR, shackleR, 0.0f, -juce::MathConstants<float>::halfPi,
                              juce::MathConstants<float>::halfPi, true);
        g.strokePath(shackle, juce::PathStrokeType(1.2f));

        if (locked)
            g.fillRect(juce::Rectangle<float>(bounds.getCentreX() - 1.0f, bodyY + bodyH * 0.28f, 2.0f, bodyH * 0.44f));
    }

    juce::Font getLabelFont(juce::Label& label) override { return getTerminalFontStatic(label.getFont().getHeight()); }

    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override
    {
        return getTerminalFontStatic(juce::jmin(UIConstants::typeSectionSize, static_cast<float>(buttonHeight) * 0.6f));
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override { return getTerminalFontStatic(UIConstants::typeValueSize); }

    juce::Font getPopupMenuFont() override { return getTerminalFontStatic(UIConstants::typeValueSize); }

    //==========================================================================
    // Rotary Slider (Knob) - CRT Style
    //==========================================================================

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPosProportional,
                          float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override
    {
        juce::ignoreUnused(slider);
        auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
        auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f;
        auto centreX = bounds.getCentreX();
        auto centreY = bounds.getCentreY();
        auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        // Reduce radius for visual padding
        auto fullRadius = radius;  // keep original for bounds-clamping
        radius *= 0.85f;

        auto arcRadius = radius * 0.82f;
        auto arcThickness = 3.0f;
        auto backgroundArcThickness = 6.0f;

        // Clamp glow stroke so its outer extent (arcRadius + stroke/2) stays
        // within fullRadius with a 1 px safety margin for anti-aliasing.
        auto glowStroke = std::min(arcThickness + 6.0f, std::max(arcThickness, 2.0f * (fullRadius - arcRadius - 1.0f)));

        // Background arc (darker green)
        {
            juce::Path backgroundArc;
            backgroundArc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle,
                                        true);
            g.setColour(getTerminalGreen().withAlpha(0.2f));
            g.strokePath(backgroundArc, juce::PathStrokeType(backgroundArcThickness, juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
        }

        // Glow arc (behind value arc)
        {
            juce::Path glowArc;
            glowArc.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, rotaryStartAngle, angle, true);
            g.setColour(getTerminalGreen().withAlpha(0.25f));
            g.strokePath(glowArc,
                         juce::PathStrokeType(glowStroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Radar trail: stepped-opacity wedges sweeping from rotaryStartAngle to current angle
        // Each wedge is a fraction of the total angle span, with opacity increasing toward the
        // current value (phosphor decay effect: older segments fade, newest segment is full green).
        {
            const auto angleSpan = angle - rotaryStartAngle;

            // Scale trail steps with knob size: larger knobs get more granular trails
            // arcRadius range: ~40-80 pixels typical; map to 6-12 trail steps
            const int trailSteps = juce::jmax(6, (int)(arcRadius * 0.15f));
            const auto stepCount = static_cast<float>(trailSteps);

            for (int i = 0; i < trailSteps; ++i)
            {
                const auto step = static_cast<float>(i);

                // Each wedge spans a fraction of the total angle
                const auto segmentStart = rotaryStartAngle + (angleSpan * step / stepCount);
                const auto segmentEnd = rotaryStartAngle + (angleSpan * (step + 1.0f) / stepCount);

                // Opacity ramps from 0.1f to 1.0f across trail steps (phosphor decay)
                const auto opacity = 0.1f + (0.9f * (step + 1.0f) / stepCount);

                juce::Path trailSegment;
                trailSegment.addCentredArc(centreX, centreY, arcRadius, arcRadius, 0.0f, segmentStart, segmentEnd,
                                           true);
                g.setColour(getTerminalGreen().withAlpha(opacity));
                g.strokePath(trailSegment, juce::PathStrokeType(arcThickness, juce::PathStrokeType::curved,
                                                                juce::PathStrokeType::rounded));
            }
        }

        // Tick marks (clamped to stay within component bounds)
        {
            auto tickRadius = std::min(arcRadius + 8.0f, fullRadius - 2.0f);
            int numTicks = 11;
            g.setColour(getTerminalGreen().withAlpha(0.4f));

            for (int i = 0; i < numTicks; ++i)
            {
                float tickAngle =
                    rotaryStartAngle + (float)i / (float)(numTicks - 1) * (rotaryEndAngle - rotaryStartAngle);
                float tickX = centreX + tickRadius * std::cos(tickAngle - juce::MathConstants<float>::halfPi);
                float tickY = centreY + tickRadius * std::sin(tickAngle - juce::MathConstants<float>::halfPi);
                g.fillEllipse(tickX - 1.5f, tickY - 1.5f, 3.0f, 3.0f);
            }
        }

        // Center dot
        {
            auto dotRadius = 4.0f;
            g.setColour(getTerminalGreen());
            g.fillEllipse(centreX - dotRadius, centreY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
        }

        // Pointer line (white)
        {
            juce::Path pointer;
            auto pointerLength = arcRadius * 0.65f;
            auto pointerThickness = 2.0f;

            pointer.addRectangle(-pointerThickness / 2.0f, -pointerLength, pointerThickness, pointerLength);

            g.setColour(getCleanLight());
            g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centreX, centreY));
        }

        // Focus ring: bounds-aware so it never exceeds the rotary area
        // (Accessibility: WCAG 2.4.7)
        if (slider.hasKeyboardFocus(false))
        {
            auto focusR = fullRadius - 2.0f;
            g.setColour(getTerminalGreen().withAlpha(0.25f));
            g.drawEllipse(centreX - focusR, centreY - focusR, focusR * 2.0f, focusR * 2.0f, 2.0f);
            auto innerFocusR = focusR - 2.5f;
            if (innerFocusR > 0.0f)
            {
                g.setColour(getTerminalGreen().withAlpha(0.8f));
                g.drawEllipse(centreX - innerFocusR, centreY - innerFocusR, innerFocusR * 2.0f, innerFocusR * 2.0f,
                              1.5f);
            }
        }
    }

    //==========================================================================
    // Slider Thumb Radius: return 0 so getSliderLayout doesn't shrink the
    // rotary area.  Our drawRotarySlider already applies radius *= 0.85 for
    // visual padding, so the default 7 px inset is redundant and causes the
    // arc to clip against the reduced bounds.
    //==========================================================================

    int getSliderThumbRadius(juce::Slider&) override { return 0; }

    //==========================================================================
    // Slider TextBox
    //==========================================================================

    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox(slider);
        label->setFont(getTerminalFontStatic(UIConstants::typeValueSize));
        label->setColour(juce::Label::textColourId, getTerminalGreen());
        // Respect the slider's own colour IDs (the class defaults keep the
        // house void-black/dim-outline look) so individual sliders can go
        // transparent on textured backgrounds.
        label->setColour(juce::Label::backgroundColourId, slider.findColour(juce::Slider::textBoxBackgroundColourId));
        label->setColour(juce::Label::outlineColourId, slider.findColour(juce::Slider::textBoxOutlineColourId));
        label->setJustificationType(juce::Justification::centred);
        return label;
    }

    //==========================================================================
    // Toggle Button - CRT Style with checkbox
    //==========================================================================

    // Optional ToggleButton property: fixed checkbox edge in px, so tall rows
    // keep a compact box instead of one that fills the whole height.
    static constexpr const char* kCheckboxSizeProperty = "crtCheckboxSize";

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override
    {
        juce::ignoreUnused(shouldDrawButtonAsDown);
        auto bounds = button.getLocalBounds().toFloat();
        auto isOn = button.getToggleState();

        // Button background with glow when on
        if (isOn)
        {
            // Glow effect
            g.setColour(getTerminalGreen().withAlpha(0.15f));
            g.fillRoundedRectangle(bounds.expanded(2.0f), 4.0f);

            // Main background
            g.setColour(getTerminalGreen().withAlpha(0.1f));
            g.fillRoundedRectangle(bounds, 3.0f);

            // Border
            g.setColour(getTerminalGreen());
            g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.5f);
        }
        else
        {
            // Subtle background on hover
            if (shouldDrawButtonAsHighlighted)
            {
                g.setColour(getTerminalGreen().withAlpha(0.08f));
                g.fillRoundedRectangle(bounds, 3.0f);
            }

            // Border (raised from 0.5 for WCAG 3:1 non-text contrast)
            g.setColour(getTerminalGreen().withAlpha(0.6f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);
        }

        // Checkbox (fills the row height unless the button sets kCheckboxSizeProperty)
        auto checkboxSize =
            static_cast<float>(button.getProperties().getWithDefault(kCheckboxSizeProperty, bounds.getHeight() - 6.0f));
        auto checkboxInset = juce::jmax(4.0f, (bounds.getHeight() - checkboxSize) * 0.5f);
        auto checkboxBounds = juce::Rectangle<float>(
            bounds.getX() + checkboxInset, bounds.getCentreY() - checkboxSize / 2.0f, checkboxSize, checkboxSize);

        // Checkbox border
        g.setColour(isOn ? getTerminalGreen() : getTerminalGreen().withAlpha(0.5f));
        g.drawRect(checkboxBounds, 1.0f);

        // Checkbox fill when on
        if (isOn)
        {
            g.setColour(getTerminalGreen());
            g.fillRect(checkboxBounds.reduced(3.0f));
        }

        // Text
        auto textBounds = bounds.withTrimmedLeft(checkboxInset + checkboxSize + 6.0f);
        g.setColour(isOn ? getTerminalGreen() : getTerminalGreen().withAlpha(0.7f));
        g.setFont(getTerminalFontStatic(UIConstants::typeBodySize));
        g.drawText(button.getButtonText().toUpperCase(), textBounds, juce::Justification::centredLeft);
    }

    //==========================================================================
    // Text Button - CRT Style
    //==========================================================================

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat();

        // If background is fully transparent, draw minimal (no border/accents)
        const bool minimal = (backgroundColour.getAlpha() == 0);

        // Background fill
        if (shouldDrawButtonAsDown)
        {
            g.setColour(getTerminalGreen().withAlpha(0.2f));
            g.fillRoundedRectangle(bounds, 3.0f);
        }
        else if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(getTerminalGreen().withAlpha(0.1f));
            g.fillRoundedRectangle(bounds, 3.0f);
        }
        else if (!minimal && backgroundColour.getAlpha() > 0)
        {
            g.setColour(backgroundColour);
            g.fillRoundedRectangle(bounds, 3.0f);
        }

        if (!minimal)
        {
            // Border with corner accents (WCAG 3:1 non-text contrast)
            g.setColour(getTerminalGreen().withAlpha(0.65f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

            // Corner accents (8px segments)
            g.setColour(getTerminalGreen());
            drawCornerAccents(g, bounds.reduced(1.0f), 8.0f, 2.0f);
        }
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override
    {
        juce::ignoreUnused(shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
        g.setFont(getTerminalFontStatic(UIConstants::typeBodySize));

        auto textColour = button.getToggleState() ? button.findColour(juce::TextButton::textColourOnId)
                                                  : button.findColour(juce::TextButton::textColourOffId);
        g.setColour(textColour.getAlpha() > 0 ? textColour : getTerminalGreen());

        g.drawText(button.getButtonText().toUpperCase(), button.getLocalBounds(), juce::Justification::centred);
    }

    //==========================================================================
    // Linear Slider - CRT Style (tick marks + phosphor fill + glowing thumb)
    //==========================================================================

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                          float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical)
        {
            juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style,
                                                   slider);
            return;
        }

        const auto green = getTerminalGreen();
        const float dim = slider.isEnabled() ? 1.0f : 0.4f;
        const bool hot = slider.isEnabled() && slider.isMouseOverOrDragging();
        const bool horizontal = (style == juce::Slider::LinearHorizontal);

        // Quantised tick marks every 10% (terminal grid feel)
        g.setColour(green.withAlpha(0.15f * dim));
        for (int i = 0; i <= 10; ++i)
        {
            const float t = (float)i / 10.0f;
            if (horizontal)
            {
                const float tx = (float)x + (float)width * t;
                g.drawLine(tx, (float)y + (float)height * 0.5f - 3.0f, tx, (float)y + (float)height * 0.5f + 3.0f,
                           1.0f);
            }
            else
            {
                const float ty = (float)y + (float)height * t;
                g.drawLine((float)x + (float)width * 0.5f - 3.0f, ty, (float)x + (float)width * 0.5f + 3.0f, ty, 1.0f);
            }
        }

        if (horizontal)
        {
            const float cy = (float)y + (float)height * 0.5f;
            g.setColour(green.withAlpha(0.22f * dim));
            g.drawLine((float)x, cy, (float)(x + width), cy, 2.0f);
            g.setColour(green.withAlpha((hot ? 0.75f : 0.55f) * dim));
            g.drawLine((float)x, cy, sliderPos, cy, 2.0f);

            const float th = (float)height * 0.62f;
            if (hot)
            {
                g.setColour(green.withAlpha(0.25f));
                g.fillRoundedRectangle(sliderPos - 4.0f, cy - th * 0.5f - 1.0f, 8.0f, th + 2.0f, 2.0f);
            }
            g.setColour(green.withAlpha((hot ? 1.0f : 0.85f) * dim));
            g.fillRoundedRectangle(sliderPos - 1.5f, cy - th * 0.5f, 3.0f, th, 1.0f);
        }
        else
        {
            const float cx = (float)x + (float)width * 0.5f;
            g.setColour(green.withAlpha(0.22f * dim));
            g.drawLine(cx, (float)y, cx, (float)(y + height), 2.0f);
            g.setColour(green.withAlpha((hot ? 0.75f : 0.55f) * dim));
            g.drawLine(cx, (float)(y + height), cx, sliderPos, 2.0f);

            const float tw = (float)width * 0.62f;
            if (hot)
            {
                g.setColour(green.withAlpha(0.25f));
                g.fillRoundedRectangle(cx - tw * 0.5f - 1.0f, sliderPos - 4.0f, tw + 2.0f, 8.0f, 2.0f);
            }
            g.setColour(green.withAlpha((hot ? 1.0f : 0.85f) * dim));
            g.fillRoundedRectangle(cx - tw * 0.5f, sliderPos - 1.5f, tw, 3.0f, 1.0f);
        }
    }

    //==========================================================================
    // ComboBox - CRT Style
    //==========================================================================

    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                      int buttonW, int buttonH, juce::ComboBox& box) override
    {
        juce::ignoreUnused(isButtonDown, box);
        auto bounds = juce::Rectangle<int>(0, 0, width, height).toFloat();

        // Background
        g.setColour(getVoidBlack());
        g.fillRoundedRectangle(bounds, 3.0f);

        // Border
        g.setColour(getTerminalGreen().withAlpha(0.6f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

        // Arrow
        const auto button = juce::Rectangle<int>(buttonX, buttonY, buttonW, buttonH).toFloat();
        auto arrowBounds =
            juce::Rectangle<float>(button.getX() + button.getWidth() * 0.3f, button.getY() + button.getHeight() * 0.35f,
                                   button.getWidth() * 0.4f, button.getHeight() * 0.3f);
        juce::Path arrow;
        arrow.addTriangle(arrowBounds.getX(), arrowBounds.getY(), arrowBounds.getCentreX(), arrowBounds.getBottom(),
                          arrowBounds.getRight(), arrowBounds.getY());

        g.setColour(getTerminalGreen());
        g.fillPath(arrow);
    }

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(4, 1, box.getWidth() - 20, box.getHeight() - 2);
        const auto valueFont = getTerminalFontStatic(UIConstants::typeValueSize);
        label.setFont(valueFont);
        if (!label.getLocalBounds().isEmpty() && box.getText().isNotEmpty())
            ParameterValueLayout::assertFits(label, valueFont, box.getText(), label.getLocalBounds());
    }

    //==========================================================================
    // PopupMenu - CRT Style
    //==========================================================================

    // Preset-browser frame family surface: void-black
    // fill, rounded phosphor border, corner accent ticks. Shared by popup
    // menus and the floating drawer panels (ECHO ENGINE, REVERB) so every
    // dropdown surface reads as one family.
    static void drawFrameFamilySurface(juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        g.setColour(getVoidBlack());
        g.fillRoundedRectangle(bounds, 3.0f);

        g.setColour(getTerminalGreen().withAlpha(0.5f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

        // Corner accents (~6px segments)
        g.setColour(getTerminalGreen().withAlpha(0.7f));
        drawCornerAccents(g, bounds.reduced(1.0f), 6.0f, 1.5f);
    }

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override
    {
        drawFrameFamilySurface(g, juce::Rectangle<int>(0, 0, width, height).toFloat());
    }

    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                           bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText, const juce::Drawable* icon,
                           const juce::Colour* textColourToUse) override
    {
        juce::ignoreUnused(icon, textColourToUse);

        if (isSeparator)
        {
            auto sepArea = area.reduced(6, 0);
            g.setColour(getTerminalGreen().withAlpha(0.2f));
            const float y = static_cast<float>(area.getCentreY()) + 0.5f;
            g.drawLine(static_cast<float>(sepArea.getX()), y, static_cast<float>(sepArea.getRight()), y);
            return;
        }

        if (isHighlighted && isActive)
        {
            g.setColour(getTerminalGreen().withAlpha(0.15f));
            g.fillRect(area);
        }

        const auto textColour = isActive ? getTerminalGreen() : getTerminalGreen().withAlpha(0.35f);
        g.setColour(textColour);
        const auto font = getTerminalFontStatic(UIConstants::typeValueSize);
        g.setFont(font);

        auto textArea = area.reduced(8, 0);
        if (isTicked)
        {
            auto tickArea = textArea.removeFromLeft(14).reduced(1, 0).toFloat();
            juce::Path tick;
            tick.startNewSubPath(tickArea.getX(), tickArea.getCentreY());
            tick.lineTo(tickArea.getCentreX() - 1.0f, tickArea.getBottom() - 5.0f);
            tick.lineTo(tickArea.getRight(), tickArea.getY() + 5.0f);
            g.strokePath(tick, juce::PathStrokeType(1.4f));
        }
        else
        {
            textArea.removeFromLeft(14);
        }

        if (hasSubMenu)
        {
            auto arrowArea = textArea.removeFromRight(12).toFloat().reduced(1.0f, 5.0f);
            juce::Path arrow;
            arrow.addTriangle(arrowArea.getX(), arrowArea.getY(), arrowArea.getX(), arrowArea.getBottom(),
                              arrowArea.getRight(), arrowArea.getCentreY());
            g.fillPath(arrow);
        }

        if (shortcutKeyText.isNotEmpty())
        {
            // Sized to the measured shortcut: getIdealPopupMenuItemSize() measures
            // the label and shortcut together, so neither side truncates.
            const int shortcutWidth =
                juce::roundToInt(std::ceil(juce::GlyphArrangement::getStringWidth(font, shortcutKeyText)));
            auto shortcutArea = textArea.removeFromRight(shortcutWidth);
            g.setColour(textColour.withAlpha(0.65f));
            g.drawText(shortcutKeyText, shortcutArea, juce::Justification::centredRight);
            g.setColour(textColour);
        }

        g.drawText(text, textArea, juce::Justification::centredLeft);
    }

    void drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area,
                                    const juce::String& sectionName) override
    {
        // Section header: dimmed phosphor text with divider line below
        g.setColour(getTerminalGreen().withAlpha(0.6f));
        g.setFont(getTerminalFontStatic(UIConstants::typeLabelSize));
        g.drawText(sectionName.toUpperCase(), area.reduced(12, 0), juce::Justification::centredLeft);

        // Divider line below section header
        g.setColour(getTerminalGreen().withAlpha(0.2f));
        g.fillRect(area.getX(), area.getBottom() - 1, area.getWidth(), 1);
    }

    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                   int& idealWidth, int& idealHeight) override
    {
        auto font = getTerminalFontStatic(isSeparator ? UIConstants::typeMicroSize : UIConstants::typeValueSize);

        if (isSeparator)
            idealHeight = 4;
        else
            idealHeight = juce::jmax(MenuChrome::kMinRowHeight, standardMenuItemHeight);

        // Width must reserve exactly what drawPopupMenuItem() consumes -
        // 8px margins each side + 14px tick gutter + 12px submenu arrow -
        // or long labels ellipsize (no-truncation rule).
        constexpr int drawInsets = 8 + 8 + 14 + 12;
        juce::GlyphArrangement measured;
        measured.addLineOfText(font, text, 0.0f, 0.0f);
        idealWidth = (int)std::ceil(measured.getBoundingBox(0, -1, true).getWidth()) + drawInsets;

        idealWidth = juce::jmax(idealWidth, 80);
    }

    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox& box, juce::Label& label) override
    {
        // Route ComboBox popups through the shared MenuChrome hosting recipe
        // so every popup surface behaves identically: the list opens below
        // the box, or above it when there is more room there. No
        // withItemThatMustBeVisible(): it lays the list over the box with the
        // selected item on top and, near the window's bottom edge, trims the
        // list to a scrolling stub instead of moving it.
        return MenuChrome::popupOptions(box)
            .withInitiallySelectedItem(box.getSelectedId())
            .withMinimumWidth(box.getWidth())
            .withMaximumNumColumns(1)
            .withStandardItemHeight(juce::jmax(MenuChrome::kMinRowHeight, label.getHeight()));
    }

    //==========================================================================
    // Tooltip - CRT Style
    //==========================================================================

    // Measure and draw share ONE layout: sizing the bubble for wrapped lines
    // but drawing single-line produced tall empty boxes with clipped text.
    static juce::TextLayout layoutTooltipText(const juce::String& text, float maxTextWidth)
    {
        juce::AttributedString s;
        s.setJustification(juce::Justification::topLeft);
        s.append(text, getTerminalFontStatic(UIConstants::typeBodySize), getTerminalGreen());
        juce::TextLayout layout;
        layout.createLayout(s, maxTextWidth);
        return layout;
    }

    static constexpr int kTooltipMaxWidth = 500;
    static constexpr int kTooltipPad = 8;

    juce::Rectangle<int> getTooltipBounds(const juce::String& tipText, juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override
    {
        const auto layout = layoutTooltipText(tipText, (float)(kTooltipMaxWidth - 2 * kTooltipPad));
        const int w = (int)std::ceil(layout.getWidth()) + 2 * kTooltipPad;
        const int h = (int)std::ceil(layout.getHeight()) + 2 * kTooltipPad;

        const int x = screenPos.x > parentArea.getCentreX() ? screenPos.x - w - 12 : screenPos.x + 16;
        const int y = screenPos.y + 20;

        return juce::Rectangle<int>(x, y, w, h).constrainedWithin(parentArea);
    }

    void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        auto bounds = juce::Rectangle<float>(0, 0, static_cast<float>(width), static_cast<float>(height));
        g.setColour(getVoidBlack().withAlpha(0.95f));
        g.fillRoundedRectangle(bounds, 2.0f);
        g.setColour(getTerminalGreen().withAlpha(0.5f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 2.0f, 1.0f);

        // Re-layout at the box's inner width: >= the measured text width, so
        // line breaks land identically to getTooltipBounds.
        const auto layout = layoutTooltipText(text, (float)(width - 2 * kTooltipPad));
        layout.draw(g, bounds.reduced((float)kTooltipPad));
    }

    // Desktop-parented CallOutBox (exact-entry position popover) - stock JUCE
    // grey would break the CRT surface.
    void drawCallOutBoxBackground(juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&) override
    {
        g.setColour(getVoidBlack().withAlpha(0.95f));
        g.fillPath(path);
        g.setColour(getTerminalGreen().withAlpha(0.5f));
        g.strokePath(path, juce::PathStrokeType(1.0f));
    }

    //==========================================================================
    // Label - CRT Style
    //==========================================================================

    void drawLabel(juce::Graphics& g, juce::Label& label) override
    {
        g.fillAll(label.findColour(juce::Label::backgroundColourId));

        if (!label.isBeingEdited())
        {
            auto textColour = label.findColour(juce::Label::textColourId);
            auto text = label.getText();

            // Make section labels uppercase
            if (label.getFont().getHeight() >= UIConstants::typeSectionSize)
                text = text.toUpperCase();

            g.setColour(textColour);
            g.setFont(getTerminalFontStatic(label.getFont().getHeight()));
            g.drawText(text, label.getLocalBounds(), label.getJustificationType(), false);
        }
    }

    //==========================================================================
    // Focus Ring - CRT Style
    //==========================================================================
    //
    // Stock controls draw no keyboard focus outline, by design:
    //   1. createFocusOutlineForComponent() returns nullptr, disabling JUCE's
    //      FocusOutline overlay.
    //   2. ComboBox and TextEditor focusedOutlineColourId are transparent.
    //   3. drawToggleButton(), drawButtonBackground() and drawComboBox() do not
    //      call drawCRTFocusRing().
    // Knobs draw their own focus ring in drawRotarySlider(), the head view and
    // the division grids draw focus cues, and the walkthrough overlay calls
    // drawCRTFocusRing() on its own buttons. Hosts provide their own
    // accessibility layers on top.
    //
    // To enable outlines on the stock controls, remove the
    // createFocusOutlineForComponent() override, set the focusedOutlineColourId
    // colours to getTerminalGreen(), and call drawCRTFocusRing() from the
    // hasKeyboardFocus(false) cases of the three draw functions above.
    //

    static void drawCRTFocusRing(juce::Graphics& g, juce::Rectangle<float> bounds, float cornerRadius = 3.0f)
    {
        // Outer glow (soft phosphor bloom)
        g.setColour(getTerminalGreen().withAlpha(0.25f));
        g.drawRoundedRectangle(bounds.expanded(2.0f), cornerRadius + 1.0f, 2.0f);
        // Inner bright ring
        g.setColour(getTerminalGreen().withAlpha(0.8f));
        g.drawRoundedRectangle(bounds, cornerRadius, 1.5f);
    }

    //==========================================================================
    // Helper Functions for Custom Drawing
    //==========================================================================

    static void drawScanlines(juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        g.setColour(getTerminalGreen().withAlpha(0.02f));
        for (int y = bounds.getY(); y < bounds.getBottom(); y += 3)
        {
            g.fillRect(bounds.getX(), y, bounds.getWidth(), 1);
        }
    }

    // Corner accent ticks: two strokes of `length` running inward from each
    // corner of `bounds`, in the current colour. Every CRT frame (terminal
    // borders, buttons, dropdown headers, popup surfaces) shares this shape.
    static void drawCornerAccents(juce::Graphics& g, juce::Rectangle<float> bounds, float length, float thickness)
    {
        const float x1 = bounds.getX();
        const float y1 = bounds.getY();
        const float x2 = bounds.getRight();
        const float y2 = bounds.getBottom();

        g.drawLine(x1, y1, x1 + length, y1, thickness);
        g.drawLine(x1, y1, x1, y1 + length, thickness);

        g.drawLine(x2 - length, y1, x2, y1, thickness);
        g.drawLine(x2, y1, x2, y1 + length, thickness);

        g.drawLine(x1, y2, x1 + length, y2, thickness);
        g.drawLine(x1, y2 - length, x1, y2, thickness);

        g.drawLine(x2 - length, y2, x2, y2, thickness);
        g.drawLine(x2, y2 - length, x2, y2, thickness);
    }

    static void drawTerminalBorder(juce::Graphics& g, juce::Rectangle<float> bounds, float cornerLength = 8.0f)
    {
        g.setColour(getTerminalGreen().withAlpha(0.6f));
        g.drawRect(bounds, 1.0f);

        g.setColour(getTerminalGreen());
        drawCornerAccents(g, bounds, cornerLength, 2.0f);
    }

    static void drawGradientOverlay(juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        juce::ColourGradient gradient(getTerminalGreen().withAlpha(0.03f), 0.0f, static_cast<float>(bounds.getY()),
                                      juce::Colours::transparentBlack, 0.0f, static_cast<float>(bounds.getBottom()),
                                      false);
        g.setGradientFill(gradient);
        g.fillRect(bounds);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CRTLookAndFeel)
};
