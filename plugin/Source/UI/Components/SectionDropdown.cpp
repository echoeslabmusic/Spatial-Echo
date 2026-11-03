// SectionDropdown: Clickable section header that opens a floating drawer (shared header behaviour and drawer chrome).

#include "SectionDropdown.h"
#include "../UIConstants.h"
#include "CRTLookAndFeel.h"
#include "DropdownWindowUtils.h"

namespace
{
constexpr int kPinButtonWidth = 84;
constexpr int kPinButtonHeight = 24;
constexpr int kPinCheckboxSize = 12;
}  // namespace

SectionDropdown::SectionDropdown(const juce::String& title, bool keepTimerRunning)
    : timerAlwaysRunning(keepTimerRunning)
{
    setInterceptsMouseClicks(true, true);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);

    titleLabel.setText(title, juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, CRTLookAndFeel::getTerminalGreen());
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeSectionSize));
    titleLabel.setInterceptsMouseClicks(false, false);  // clicks reach the header
    addAndMakeVisible(titleLabel);

    if (timerAlwaysRunning)
        startTimerHz(kTimerHz);
}

SectionDropdown::~SectionDropdown()
{
    stopTimer();
    dropdownWindow.reset();
}

void SectionDropdown::paint(juce::Graphics& g)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();
    auto bounds = getLocalBounds().toFloat();

    // Background (matches CRTLookAndFeel::drawButtonBackground)
    if (dropdownVisible)
    {
        g.setColour(green.withAlpha(0.2f));
        g.fillRoundedRectangle(bounds, 3.0f);
    }
    else if (isHovered)
    {
        g.setColour(green.withAlpha(0.1f));
        g.fillRoundedRectangle(bounds, 3.0f);
    }

    g.setColour(green.withAlpha(0.6f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);
    g.setColour(green);
    CRTLookAndFeel::drawCornerAccents(g, bounds.reduced(1.0f), 8.0f, 2.0f);

    // Dropdown caret triangle (right side, matches CRTLookAndFeel::drawComboBox arrow)
    {
        const float triW = 6.0f;
        const float triH = 4.0f;
        const float triX = bounds.getRight() - triW - 6.0f;
        const float triY = bounds.getCentreY() - triH * 0.5f;
        juce::Path caret;
        caret.addTriangle(triX, triY, triX + triW * 0.5f, triY + triH, triX + triW, triY);
        g.setColour(green.withAlpha(dropdownVisible ? 1.0f : 0.6f));
        g.fillPath(caret);
    }

    // Pin indicator (right-click toggles pinned-open mode)
    if (pinnedOpen)
    {
        g.setColour(green.withAlpha(0.95f));
        g.fillEllipse(bounds.getX() + 4.0f, bounds.getCentreY() - 2.0f, 4.0f, 4.0f);
    }
}

void SectionDropdown::resized()
{
    titleLabel.setBounds(getLocalBounds());
}

void SectionDropdown::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        setPinnedOpen(!pinnedOpen);
        if (pinnedOpen && !dropdownVisible)
            showDropdown();
        return;
    }

    if (dropdownVisible)
        hideDropdown();
    else
        showDropdown();
}

bool SectionDropdown::keyPressed(const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey && dropdownVisible)
    {
        hideDropdown();
        return true;
    }

    // Space is the host transport shortcut in DAWs, so only Return toggles the drawer.
    if (key.getKeyCode() == juce::KeyPress::returnKey)
    {
        if (dropdownVisible)
            hideDropdown();
        else
            showDropdown();
        return true;
    }

    return false;
}

void SectionDropdown::mouseEnter(const juce::MouseEvent&)
{
    isHovered = true;
    repaint();
}

void SectionDropdown::mouseExit(const juce::MouseEvent&)
{
    isHovered = false;
    repaint();
}

void SectionDropdown::showDropdown()
{
    if (dropdownVisible)
        return;

    createDropdownWindow();
    dropdownVisible = true;
    outsideHoverTicks = 0;
    repaint();

    if (!timerAlwaysRunning)
        startTimerHz(kTimerHz);
}

void SectionDropdown::hideDropdown()
{
    if (!timerAlwaysRunning)
        stopTimer();
    if (dropdownWindow != nullptr)
        SpatialEchoDropdownWindow::setOutsideClickDismissal(*dropdownWindow, false);
    dropdownWindow.reset();
    dropdownVisible = false;
    repaint();
}

void SectionDropdown::setWalkthroughNavigationComponent(juce::Component* component)
{
    if (auto* host = dynamic_cast<SpatialEchoDropdownWindow::Host*>(dropdownWindow.get()))
        host->setModalEventPassthrough(component);
}

void SectionDropdown::setTitleColour(juce::Colour colour)
{
    titleLabel.setColour(juce::Label::textColourId, colour);
    titleLabel.repaint();
}

void SectionDropdown::setPinnedOpen(bool shouldBePinned)
{
    pinnedOpen = shouldBePinned;
    if (pinnedOpen)
        outsideHoverTicks = 0;
    if (dropdownWindow != nullptr)
        SpatialEchoDropdownWindow::setOutsideClickDismissal(*dropdownWindow, !pinnedOpen);
    pinnedStateChanged(pinnedOpen);
    repaint();
}

void SectionDropdown::openDropdownWindow(std::unique_ptr<juce::Component> content, juce::Rectangle<int> screenBounds)
{
    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr)
        return;

    auto host = std::make_unique<SpatialEchoDropdownWindow::Host>();
    juce::Component::SafePointer<SectionDropdown> safeThis(this);
    host->onInputAttempt = [safeThis]()
    {
        juce::MessageManager::callAsync(
            [safeThis]()
            {
                if (safeThis != nullptr && !safeThis->pinnedOpen)
                    safeThis->hideDropdown();
            });
    };
    dropdownWindow = std::move(host);
    dropdownWindow->setOpaque(false);
    dropdownWindow->addAndMakeVisible(content.release());  // the Host deletes its children
    // A desktop window inherits no look-and-feel from the editor, and hosts
    // keep JUCE's stock default (only the Standalone makes the plug-in's look
    // the app default). Set it after the content is added so every control
    // rebuilds its parts (slider value boxes included) in the plug-in's look.
    dropdownWindow->setLookAndFeel(&getLookAndFeel());
    dropdownWindow->setSize(screenBounds.getWidth(), screenBounds.getHeight());
    SpatialEchoDropdownWindow::addToDesktop(*dropdownWindow, topLevel);
    SpatialEchoDropdownWindow::setTopLeftPositionForNativeParent(*dropdownWindow, screenBounds, topLevel);
    dropdownWindow->setVisible(true);
    SpatialEchoDropdownWindow::setOutsideClickDismissal(*dropdownWindow, !pinnedOpen);
    dropdownWindow->toFront(true);
}

void SectionDropdown::timerCallback()
{
    timerTick();

    if (dropdownWindow == nullptr)
        return;

    if (juce::Component::getNumCurrentlyModalComponents() > 0 || pinnedOpen)
    {
        outsideHoverTicks = 0;
        return;
    }

    const auto& mouse = juce::Desktop::getInstance().getMainMouseSource();
    const auto mousePos = mouse.getScreenPosition().toInt();
    const bool pointerOutside =
        !dropdownWindow->getScreenBounds().contains(mousePos) && !getScreenBounds().contains(mousePos);

    if (pointerOutside && !mouse.isDragging())
    {
        if (++outsideHoverTicks >= kCloseDwellTicks)
            hideDropdown();
    }
    else
    {
        outsideHoverTicks = 0;
    }
}

void SectionDropdown::paintDrawerChrome(juce::Graphics& g, juce::Rectangle<float> bounds, const juce::String& title)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();

    // Preset-browser frame family - same surface as every popup menu.
    CRTLookAndFeel::drawFrameFamilySurface(g, bounds);

    auto headerArea = bounds.removeFromTop(static_cast<float>(kDrawerHeaderHeight));
    g.setColour(green.withAlpha(0.12f));
    g.fillRect(headerArea.reduced(2, 2).withTrimmedBottom(2));

    g.setColour(green);
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeSectionSize));
    const auto titleArea = headerArea.reduced(8.0f, 0.0f).withTrimmedRight(static_cast<float>(kPinButtonWidth + 10));
    g.drawText(title, titleArea, juce::Justification::centred);

    g.setColour(green.withAlpha(0.3f));
    g.drawLine(10.0f, headerArea.getBottom(), bounds.getWidth() - 10.0f, headerArea.getBottom(), 1.0f);
}

juce::Rectangle<int> SectionDropdown::pinToggleBounds(juce::Rectangle<int> drawerBounds)
{
    auto pinRow = drawerBounds.removeFromTop(kDrawerHeaderHeight)
                      .withTrimmedLeft(kDrawerPadding)
                      .withTrimmedRight(kDrawerPadding);
    return pinRow.removeFromRight(kPinButtonWidth).withSizeKeepingCentre(kPinButtonWidth - 4, kPinButtonHeight);
}

void SectionDropdown::configurePinToggle(juce::ToggleButton& toggle, std::function<void()> onClick)
{
    toggle.setButtonText("PIN");
    toggle.setClickingTogglesState(true);
    toggle.setColour(juce::ToggleButton::textColourId, CRTLookAndFeel::getTerminalGreen());
    toggle.setTooltip("Keep this dropdown open until unpinned.");
    toggle.getProperties().set(CRTLookAndFeel::kCheckboxSizeProperty, kPinCheckboxSize);
    toggle.onClick = std::move(onClick);
}

void SectionDropdown::showPinnedState(juce::ToggleButton& toggle, bool pinned)
{
    toggle.setToggleState(pinned, juce::dontSendNotification);
    toggle.setButtonText(pinned ? "PINNED" : "PIN");
}

void SectionDropdown::configureStepButton(juce::TextButton& button, const juce::String& text,
                                          std::function<void()> onClick)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();
    button.setButtonText(text);
    button.setColour(juce::TextButton::textColourOffId, green);
    button.setColour(juce::TextButton::textColourOnId, green);
    button.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    button.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    button.onClick = std::move(onClick);
}
