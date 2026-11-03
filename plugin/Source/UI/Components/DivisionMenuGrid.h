// DivisionMenuGrid: Shared 3-column grid for tempo-synced division menus.

#pragma once
#include <JuceHeader.h>

/**
 * DivisionMenuGrid - Custom 3-column grid for division menus.
 * Renders STRAIGHT | TRIPLET | DOTTED columns with even gutters.
 * Maintains 24px row height and CRT chrome styling.
 * Delay menus use indices 0-16. The Spatial Motion layout appends straight,
 * triplet, and dotted long divisions while keeping those existing parameter
 * indices stable.
 *
 * Dismissal contract: the grid only reports; the owner closes it.
 *  - onItemSelected(index): item clicked (owner applies + dismisses)
 *  - Arrow keys navigate the grid; Return/Space selects the focused item.
 *  - onDismissRequested(): ESC pressed (needs keyboard focus - the owner
 *    must grabKeyboardFocus() after showing). Outside clicks are the host
 *    window's job (Host::inputAttemptWhenModal), not this component's:
 *    a component never receives mouseDown for clicks outside itself.
 */
class DivisionMenuGrid : public juce::Component
{
public:
    enum class Layout
    {
        Delay,
        SpatialMotion
    };

    std::function<void(int)> onItemSelected;
    std::function<void()> onDismissRequested;

    explicit DivisionMenuGrid(Layout layout = Layout::Delay);

    // Marks the head's active division so the open menu shows where you are.
    void setCurrentIndex(int index);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;

    int getPreferredWidth() const;
    int getPreferredHeight() const;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    juce::String getLabelForMenuIndexForTesting(int index) const;
#endif

private:
    struct Item
    {
        juce::String label;
        int menuIndex = -1;
        int navigationRow = 0;
        bool separator = false;
    };

    struct Column
    {
        juce::String header;
        juce::Array<Item> items;
    };

    Column columns[3];
    int currentIndex = -1;
    int hoveredIndex = -1;
    int keyboardIndex = -1;
    static constexpr int kRowHeight = 24;
    static constexpr int kColumnWidth = 90;
    static constexpr int kGutter = 8;
    static constexpr int kPadding = 6;

    int getItemIndexAtPoint(juce::Point<int> p) const;
    int getFirstSelectableIndex() const;
    int getItemIndexForCell(int column, int row) const;
    int getNearestItemIndexInColumn(int column, int navigationRow) const;
    int getVerticalItemIndex(int column, int row, int direction) const;
    int getMaximumRowCount() const;
    int getNavigationRowForIndex(int index) const;
    bool getCellForIndex(int index, int& column, int& row) const;
    bool isSelectableIndex(int index) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DivisionMenuGrid)
};
