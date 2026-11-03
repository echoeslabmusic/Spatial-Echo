// DropdownWindowUtils: Helper utilities for modal dropdown windows: placement calculation, focus interception, display
//   bounds.

#pragma once

#include <JuceHeader.h>

namespace SpatialEchoDropdownWindow
{
struct Placement
{
    juce::Rectangle<int> bounds;
    bool widthConstrained = false;
    bool heightConstrained = false;
};

class Host final : public juce::Component
{
public:
    std::function<void()> onInputAttempt;

    void setModalEventPassthrough(juce::Component* component) { modalEventPassthrough = component; }

    bool canModalEventBeSentToComponent(const juce::Component* target) override
    {
        return modalEventPassthrough != nullptr &&
               (target == modalEventPassthrough.getComponent() || modalEventPassthrough->isParentOf(target));
    }

    ~Host() override
    {
        if (isCurrentlyModal())
            exitModalState(0);

        deleteAllChildren();
    }

    void inputAttemptWhenModal() override
    {
        if (onInputAttempt)
            onInputAttempt();
    }

private:
    juce::Component::SafePointer<juce::Component> modalEventPassthrough;
};

inline juce::Rectangle<int> getDisplayUserAreaFor(const juce::Rectangle<int>& screenBounds)
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    if (const auto* display = displays.getDisplayForRect(screenBounds))
        return display->userArea;
    if (const auto* primaryDisplay = displays.getPrimaryDisplay())
        return primaryDisplay->userArea;
    return displays.getTotalBounds(true);
}

inline juce::Rectangle<int> calculateAvailableArea(const juce::Rectangle<int>& displayArea,
                                                   const juce::Rectangle<int>& ownerArea, int margin = 8)
{
    auto available = displayArea;
    if (!ownerArea.isEmpty())
    {
        auto clipped = displayArea.getIntersection(ownerArea);
        if (!clipped.isEmpty())
            available = clipped;
    }

    auto reduced = available.reduced(margin);
    if (reduced.getWidth() > 0 && reduced.getHeight() > 0)
        return reduced;

    return available;
}

inline Placement calculateConstrainedBounds(const juce::Rectangle<int>& anchorBounds, juce::Point<int> preferredSize,
                                            const juce::Rectangle<int>& displayArea,
                                            const juce::Rectangle<int>& ownerArea, int gap = 4, int margin = 8)
{
    const auto available = calculateAvailableArea(displayArea, ownerArea, margin);
    const int width = juce::jmin(preferredSize.x, juce::jmax(1, available.getWidth()));
    const int height = juce::jmin(preferredSize.y, juce::jmax(1, available.getHeight()));

    int x = anchorBounds.getX();
    const int maxX = juce::jmax(available.getX(), available.getRight() - width);
    x = juce::jlimit(available.getX(), maxX, x);

    const int belowY = anchorBounds.getBottom() + gap;
    const int aboveY = anchorBounds.getY() - height - gap;
    int y = belowY;
    if (belowY + height > available.getBottom())
        y = aboveY;

    const int maxY = juce::jmax(available.getY(), available.getBottom() - height);
    y = juce::jlimit(available.getY(), maxY, y);

    return {{x, y, width, height}, width != preferredSize.x, height != preferredSize.y};
}

inline Placement calculateCenteredBounds(const juce::Rectangle<int>& anchorBounds, juce::Point<int> preferredSize,
                                         const juce::Rectangle<int>& displayArea, const juce::Rectangle<int>& ownerArea,
                                         int verticalOffset = 0, int margin = 8)
{
    const auto available = calculateAvailableArea(displayArea, ownerArea, margin);
    const int width = juce::jmin(preferredSize.x, juce::jmax(1, available.getWidth()));
    const int height = juce::jmin(preferredSize.y, juce::jmax(1, available.getHeight()));

    int x = anchorBounds.getX();
    const int maxX = juce::jmax(available.getX(), available.getRight() - width);
    x = juce::jlimit(available.getX(), maxX, x);

    int y = available.getY() + (available.getHeight() - height) / 2 + verticalOffset;
    const int maxY = juce::jmax(available.getY(), available.getBottom() - height);
    y = juce::jlimit(available.getY(), maxY, y);

    return {{x, y, width, height}, width != preferredSize.x, height != preferredSize.y};
}

inline void* getNativeParentHandle(juce::Component* owner)
{
    if (owner == nullptr)
        return nullptr;

    if (auto* ownerPeer = owner->getPeer())
        return ownerPeer->getNativeHandle();

    return nullptr;
}

inline bool usesNativeParent(juce::Component* owner)
{
    return getNativeParentHandle(owner) != nullptr;
}

inline juce::Rectangle<int> convertScreenBoundsForNativeParent(const juce::Rectangle<int>& screenBounds,
                                                               const juce::Rectangle<int>& ownerScreenBounds,
                                                               bool hasNativeParent)
{
    if (!hasNativeParent || ownerScreenBounds.isEmpty())
        return screenBounds;

    return screenBounds.translated(-ownerScreenBounds.getX(), -ownerScreenBounds.getY());
}

inline juce::Rectangle<int> convertScreenBoundsForNativeParent(const juce::Rectangle<int>& screenBounds,
                                                               juce::Component* owner)
{
    return convertScreenBoundsForNativeParent(
        screenBounds, owner != nullptr ? owner->getScreenBounds() : juce::Rectangle<int>(), usesNativeParent(owner));
}

inline void setBoundsForNativeParent(juce::Component& dropdownWindow, const juce::Rectangle<int>& screenBounds,
                                     juce::Component* owner)
{
    dropdownWindow.setBounds(convertScreenBoundsForNativeParent(screenBounds, owner));
}

inline void setTopLeftPositionForNativeParent(juce::Component& dropdownWindow, const juce::Rectangle<int>& screenBounds,
                                              juce::Component* owner)
{
    dropdownWindow.setTopLeftPosition(convertScreenBoundsForNativeParent(screenBounds, owner).getPosition());
}

inline void addToDesktop(juce::Component& dropdownWindow, juce::Component* owner)
{
    dropdownWindow.addToDesktop(juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowHasDropShadow,
                                getNativeParentHandle(owner));
}

inline void setOutsideClickDismissal(juce::Component& dropdownWindow, bool enabled)
{
    if (enabled)
    {
        if (!dropdownWindow.isCurrentlyModal())
            dropdownWindow.enterModalState(false, nullptr, false);
    }
    else if (dropdownWindow.isCurrentlyModal())
    {
        dropdownWindow.exitModalState(0);
    }
}
}  // namespace SpatialEchoDropdownWindow
