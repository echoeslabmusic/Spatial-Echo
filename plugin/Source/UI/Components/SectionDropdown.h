// SectionDropdown: Clickable section header that opens a floating drawer; shared by the ECHO ENGINE and REVERB
//   headers, plus the drawer chrome both drawers draw.

#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>

/**
 * SectionDropdown - a section title framed as a button. Left-click (or
 * Return/Space) opens and closes the section's drawer; right-click or the
 * drawer's PIN toggle keeps it open. An unpinned drawer closes on an outside
 * click or after the pointer has stayed away from both the header and the
 * drawer for about a second.
 *
 * Subclasses build the drawer content in createDropdownWindow() and hand it
 * to openDropdownWindow(). The static helpers give every drawer the same
 * frame, title strip, PIN toggle and step arrows.
 */
class SectionDropdown : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    ~SectionDropdown() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

    void showDropdown();
    void hideDropdown();
    bool isDropdownVisible() const { return dropdownVisible; }
    // Screen bounds of the open drawer window, or empty while it is closed.
    juce::Rectangle<int> getOpenDrawerScreenBounds() const
    {
        return dropdownWindow != nullptr && dropdownWindow->isShowing() ? dropdownWindow->getScreenBounds()
                                                                        : juce::Rectangle<int>{};
    }
    void setWalkthroughNavigationComponent(juce::Component* component);

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    juce::Component* getDropdownWindowForTesting() const { return dropdownWindow.get(); }
#endif

    // ---- Drawer chrome shared by every section drawer ----
    static constexpr int kDrawerHeaderHeight = 48;
    static constexpr int kDrawerHeaderGap = 10;
    static constexpr int kDrawerPadding = 12;
    static constexpr int kStepButtonWidth = 26;
    static constexpr int kPresetRowHeight = 30;

    // Frame-family surface, tinted title strip and separator; the title is
    // centred in the strip left of the PIN toggle.
    static void paintDrawerChrome(juce::Graphics& g, juce::Rectangle<float> bounds, const juce::String& title);
    // PIN toggle slot in the drawer's title strip.
    static juce::Rectangle<int> pinToggleBounds(juce::Rectangle<int> drawerBounds);
    static void configurePinToggle(juce::ToggleButton& toggle, std::function<void()> onClick);
    static void showPinnedState(juce::ToggleButton& toggle, bool pinned);
    // Transparent "<" / ">" preset step arrow.
    static void configureStepButton(juce::TextButton& button, const juce::String& text, std::function<void()> onClick);

protected:
    // keepTimerRunning: the subclass needs timerTick() even while the drawer
    // is closed (a header status light); otherwise the timer runs only while
    // the drawer is open.
    SectionDropdown(const juce::String& title, bool keepTimerRunning);

    // Builds the drawer content and opens it with openDropdownWindow().
    virtual void createDropdownWindow() = 0;
    // The pinned state changed; the open drawer's PIN toggle should follow.
    virtual void pinnedStateChanged(bool pinned) { juce::ignoreUnused(pinned); }
    // Called on every timer tick, before the outside-hover close check.
    virtual void timerTick() {}

    // Hosts `content` in a desktop window at `screenBounds` above the editor.
    void openDropdownWindow(std::unique_ptr<juce::Component> content, juce::Rectangle<int> screenBounds);
    juce::Component* getDropdownWindow() const noexcept { return dropdownWindow.get(); }
    bool isPinnedOpen() const noexcept { return pinnedOpen; }
    void togglePinned() { setPinnedOpen(!pinnedOpen); }
    void setTitleColour(juce::Colour colour);

private:
    static constexpr int kTimerHz = 30;
    static constexpr int kCloseDwellTicks = 30;  // ~1000 ms at 30 Hz

    void timerCallback() override;
    void setPinnedOpen(bool shouldBePinned);

    juce::Label titleLabel;
    std::unique_ptr<juce::Component> dropdownWindow;
    const bool timerAlwaysRunning;
    bool dropdownVisible = false;
    bool pinnedOpen = false;
    bool isHovered = false;
    int outsideHoverTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SectionDropdown)
};
