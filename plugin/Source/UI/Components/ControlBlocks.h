// KnobBlock: Rotary knob with a painted title and a right-click menu (reset, copy, minimum, maximum).

#pragma once
#include <JuceHeader.h>
#include "../UIConstants.h"
#include "CRTLookAndFeel.h"
#include "MenuChrome.h"
#include "ParameterValueSlider.h"

/**
 * KnobBlock - titled rotary knob.
 *
 * Double-click and the context menu's "Reset to default" both return to the
 * attached parameter's default: the APVTS SliderAttachment installs that value
 * as the slider's double-click return value.
 */
class KnobBlock : public juce::Component
{
public:
    explicit KnobBlock(const juce::String& labelText = {})
        : titleText(labelText)
    {
        knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, UIConstants::textBoxWidth, UIConstants::textBoxHeight);
        knob.setWantsKeyboardFocus(true);
        knob.onContextMenu = [this]() { showContextMenu(); };
        addAndMakeVisible(knob);
    }

    ParameterValueSlider& getSlider() { return knob; }

    void setLabelText(const juce::String& text)
    {
        if (titleText == text)
            return;
        titleText = text;
        repaint();
    }

    // Title opacity, so a control handed over to another mode can read as dimmed.
    void setTitleAlpha(float alpha)
    {
        if (juce::approximatelyEqual(titleAlpha, alpha))
            return;
        titleAlpha = alpha;
        repaint();
    }

    void setSliderName(const juce::String& sliderName) { knob.setName(sliderName); }

    void setTitleMetrics(int titleHeightPx, float titleFontHeightPx)
    {
        titleHeight = titleHeightPx;
        titleFontHeight = titleFontHeightPx;
        resized();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto textArea = getLocalBounds().removeFromTop(titleHeight);
        g.setColour(findColour(juce::Label::textColourId).withMultipliedAlpha(titleAlpha));
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(titleFontHeight));
        g.drawText(titleText.toUpperCase(), textArea, juce::Justification::centred);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        area.removeFromTop(titleHeight + UIConstants::smallPadding);
        knob.setBounds(area);
    }

private:
    // Slider subclass that intercepts right-click for the context menu.
    class ContextMenuSlider : public ParameterValueSlider
    {
    public:
        std::function<void()> onContextMenu;
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && onContextMenu)
                onContextMenu();
            else
                ParameterValueSlider::mouseDown(e);
        }
    };

    ContextMenuSlider knob;
    juce::String titleText;
    int titleHeight = UIConstants::labelHeight;
    float titleFontHeight = UIConstants::typeBodySize;
    float titleAlpha = 1.0f;

    void showContextMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Reset to default", knob.isDoubleClickReturnEnabled());
        menu.addItem(2, "Copy value");
        menu.addSeparator();
        menu.addItem(3, "Set to minimum");
        menu.addItem(4, "Set to maximum");

        juce::Component::SafePointer<KnobBlock> safeThis(this);
        menu.showMenuAsync(MenuChrome::popupOptions(knob),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr)
                                   return;

                               switch (result)
                               {
                                   case 1:  // Reset to default
                                       safeThis->knob.setValue(safeThis->knob.getDoubleClickReturnValue(),
                                                               juce::sendNotificationSync);
                                       break;
                                   case 2:  // Copy value
                                       juce::SystemClipboard::copyTextToClipboard(
                                           safeThis->knob.getTextFromValue(safeThis->knob.getValue()));
                                       break;
                                   case 3:  // Set to minimum
                                       safeThis->knob.setValue(safeThis->knob.getMinimum(), juce::sendNotificationSync);
                                       break;
                                   case 4:  // Set to maximum
                                       safeThis->knob.setValue(safeThis->knob.getMaximum(), juce::sendNotificationSync);
                                       break;
                                   default:
                                       break;
                               }
                           });
    }
};
