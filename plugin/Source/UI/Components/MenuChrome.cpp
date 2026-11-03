/*
  ==============================================================================

    MenuChrome.cpp
    Implementation of shared menu chrome helper

  ==============================================================================
*/

#include "MenuChrome.h"
#include "DropdownWindowUtils.h"

juce::PopupMenu::Options MenuChrome::popupOptions(juce::Component& opener)
{
    auto options = juce::PopupMenu::Options().withTargetComponent(&opener).withDeletionCheck(opener);

    // Parent every menu inside the plugin window: identical clipping and
    // scaling for all popup surfaces, and no extra desktop-level windows.
    // A drawer's window is too small to hold a list, so a menu opened inside
    // one gets its own desktop window, like the drawer itself.
    if (auto* top = opener.getTopLevelComponent())
        if (dynamic_cast<SpatialEchoDropdownWindow::Host*>(top) == nullptr)
            options = options.withParentComponent(top);

    return options;
}

juce::PopupMenu::Options MenuChrome::popupOptions(juce::Component& opener, juce::Rectangle<int> targetScreenArea)
{
    return popupOptions(opener).withTargetScreenArea(targetScreenArea);
}
