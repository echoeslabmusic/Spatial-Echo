#pragma once

#include <JuceHeader.h>

//==============================================================================
/**
 * MenuChrome - shared popup hosting recipe for every popup surface.
 *
 * All popup menus (app OPTIONS menu, spatial preset library, knob context
 * menus, ComboBox popups) must be shown with these options so they are
 * hosted identically: anchored to their opener, parented into the plugin
 * window (no desktop-level menu windows, except for a menu opened inside a
 * drawer's own window), and dismissed if the opener is deleted while the menu
 * is open. CRTLookAndFeel supplies the shared visual chrome; this factory
 * supplies the shared behaviour.
 *
 * Usage:
 *   menu.showMenuAsync(MenuChrome::popupOptions(*this), callback);
 *
 * ComboBoxes route through this automatically via
 * CRTLookAndFeel::getOptionsForComboBoxPopupMenu().
 */
class MenuChrome
{
public:
    static constexpr int kMinRowHeight = 24;  // 24px minimum hit target

    /** Standard popup options: target + deletion check = opener, parented
        to the opener's top-level component unless that is a drawer window. */
    static juce::PopupMenu::Options popupOptions(juce::Component& opener);

    /** Same recipe, anchored to an explicit screen rectangle (e.g. a label
        inside the opener) instead of the opener's own bounds. */
    static juce::PopupMenu::Options popupOptions(juce::Component& opener, juce::Rectangle<int> targetScreenArea);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MenuChrome)
};
