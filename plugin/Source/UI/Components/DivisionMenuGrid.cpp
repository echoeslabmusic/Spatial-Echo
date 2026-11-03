// DivisionMenuGrid: Three-column straight/triplet/dotted division grid used by the delay heads and Spatial Motion.

#include "DivisionMenuGrid.h"
#include "CRTLookAndFeel.h"
#include "../../SpatialParamIDs.h"

namespace
{
// Label for a combined menu index: 0-16 are the shared note divisions, 17-20
// the straight long divisions, 21-28 the triplet and dotted long divisions.
// The parameters' own choice lists are the single source for every label.
juce::String labelForMenuIndex(int menuIndex)
{
    using namespace SpatialIDs::motion;
    if (menuIndex < kStraightLongMenuStart)
        return kDivisionChoices[menuIndex];
    if (menuIndex < kVariantLongMenuStart)
        return kLongDivisionChoices[menuIndex - kStraightLongMenuStart + 1];
    return kLongVariantDivisionChoices[menuIndex - kVariantLongMenuStart + 1];
}
}  // namespace

DivisionMenuGrid::DivisionMenuGrid(Layout layout)
{
    columns[0].header = "Straight";
    columns[1].header = "Triplet";
    columns[2].header = "Dotted";

    auto addItem = [](Column& column, int menuIndex, int navigationRow)
    { column.items.add(Item{labelForMenuIndex(menuIndex), menuIndex, navigationRow, false}); };
    auto addSeparator = [](Column& column) { column.items.add(Item{{}, -1, 0, true}); };

    if (layout == Layout::SpatialMotion)
    {
        // Slowest first visually. These are combined menu indices; the owner
        // maps them onto the long-division parameters, leaving the shared
        // 0-16 note-division indices untouched.
        for (int column = 0; column < 3; ++column)
        {
            // Straight: 20..17; triplet: 24..21; dotted: 28..25.
            const int slowest = column == 0 ? 20 : (column == 1 ? 24 : 28);
            for (int row = 0; row < 4; ++row)
                addItem(columns[column], slowest - row, row - 4);
            addSeparator(columns[column]);
        }
    }

    // Straight 1/1..1/64 (0-6), triplet 1/2T..1/32T (7-11), dotted 1/2D..1/32D (12-16).
    for (int index = 0; index <= 6; ++index)
        addItem(columns[0], index, index);
    for (int index = 7; index <= 11; ++index)
        addItem(columns[1], index, index - 7);
    for (int index = 12; index <= 16; ++index)
        addItem(columns[2], index, index - 12);

    setInterceptsMouseClicks(true, false);
    setWantsKeyboardFocus(true);  // ESC dismissal needs real focus
    setRepaintsOnMouseActivity(true);
}

void DivisionMenuGrid::setCurrentIndex(int index)
{
    currentIndex = isSelectableIndex(index) ? index : -1;
    keyboardIndex = currentIndex >= 0 ? currentIndex : getFirstSelectableIndex();
    repaint();
}

void DivisionMenuGrid::paint(juce::Graphics& g)
{
    // Background
    g.fillAll(CRTLookAndFeel::getVoidBlack());

    // Border
    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.5f));
    g.drawRect(getLocalBounds().toFloat().reduced(0.5f), 1.0f);

    // Corner accents (preset browser frame family idiom)
    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.7f));
    CRTLookAndFeel::drawCornerAccents(g, getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.5f);

    auto font = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize);
    g.setFont(font);

    int x = kPadding;
    for (int col = 0; col < 3; ++col)
    {
        int y = kPadding;

        // Draw header
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.6f));
        g.drawText(columns[col].header.toUpperCase(), x, y, kColumnWidth, kRowHeight, juce::Justification::centredLeft);
        y += kRowHeight;

        // Draw divider line
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.2f));
        g.fillRect(x, y - 1, kColumnWidth, 1);

        // Draw items
        for (int i = 0; i < columns[col].items.size(); ++i)
        {
            const auto& item = columns[col].items.getReference(i);
            const bool isEnabled = item.menuIndex >= 0;

            if (isEnabled)
            {
                const int itemIndex = item.menuIndex;
                const auto rowBounds = juce::Rectangle<int>(x, y, kColumnWidth, kRowHeight);

                if (itemIndex == currentIndex)
                {
                    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.20f));
                    g.fillRoundedRectangle(rowBounds.toFloat().reduced(1.0f), 2.0f);
                }
                else if (itemIndex == hoveredIndex)
                {
                    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.12f));
                    g.fillRoundedRectangle(rowBounds.toFloat().reduced(1.0f), 2.0f);
                }

                if (hasKeyboardFocus(true) && itemIndex == keyboardIndex)
                {
                    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.85f));
                    g.drawRoundedRectangle(rowBounds.toFloat().reduced(1.5f), 2.0f, 1.0f);
                }

                g.setColour(CRTLookAndFeel::getTerminalGreen());
                g.drawText(item.label, x + 8, y, kColumnWidth - 16, kRowHeight, juce::Justification::centredLeft);
            }
            else if (item.separator)
            {
                g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.18f));
                g.fillRect(x + 8, y + kRowHeight / 2, kColumnWidth - 16, 1);
            }

            y += kRowHeight;
        }

        x += kColumnWidth + kGutter;
    }
}

void DivisionMenuGrid::mouseDown(const juce::MouseEvent& e)
{
    // mouseDown only ever fires inside this component; outside clicks are
    // handled by the host window's modal state (inputAttemptWhenModal).
    const int index = getItemIndexAtPoint(e.getPosition());
    if (keyboardIndex != index)
    {
        keyboardIndex = index;
        repaint();
    }
    if (index >= 0 && onItemSelected)
        onItemSelected(index);
}

void DivisionMenuGrid::mouseMove(const juce::MouseEvent& e)
{
    const int index = getItemIndexAtPoint(e.getPosition());
    if (index != hoveredIndex)
    {
        hoveredIndex = index;
        if (index >= 0)
            keyboardIndex = index;
        repaint();
    }
}

void DivisionMenuGrid::mouseExit(const juce::MouseEvent&)
{
    if (hoveredIndex != -1)
    {
        hoveredIndex = -1;
        repaint();
    }
}

bool DivisionMenuGrid::keyPressed(const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        if (onDismissRequested)
            onDismissRequested();
        return true;
    }

    int column = -1;
    int row = -1;
    if (!getCellForIndex(keyboardIndex, column, row))
    {
        keyboardIndex = currentIndex >= 0 ? currentIndex : getFirstSelectableIndex();
        if (!getCellForIndex(keyboardIndex, column, row))
            return false;
    }

    if (key.getKeyCode() == juce::KeyPress::upKey)
        keyboardIndex = getVerticalItemIndex(column, row, -1);
    else if (key.getKeyCode() == juce::KeyPress::downKey)
        keyboardIndex = getVerticalItemIndex(column, row, 1);
    else if (key.getKeyCode() == juce::KeyPress::leftKey)
        keyboardIndex = getNearestItemIndexInColumn(juce::jmax(0, column - 1), getNavigationRowForIndex(keyboardIndex));
    else if (key.getKeyCode() == juce::KeyPress::rightKey)
        keyboardIndex = getNearestItemIndexInColumn(juce::jmin(2, column + 1), getNavigationRowForIndex(keyboardIndex));
    else if (key.getKeyCode() == juce::KeyPress::returnKey || key.getKeyCode() == juce::KeyPress::spaceKey)
    {
        if (onItemSelected)
            onItemSelected(keyboardIndex);
        return true;
    }
    else
    {
        return false;
    }

    hoveredIndex = -1;
    repaint();
    return true;
}

int DivisionMenuGrid::getPreferredWidth() const
{
    return kColumnWidth * 3 + kGutter * 2 + kPadding * 2;
}

int DivisionMenuGrid::getPreferredHeight() const
{
    return (1 + getMaximumRowCount()) * kRowHeight + kPadding * 2;
}

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
juce::String DivisionMenuGrid::getLabelForMenuIndexForTesting(int index) const
{
    for (const auto& column : columns)
        for (const auto& item : column.items)
            if (item.menuIndex == index)
                return item.label;

    return {};
}
#endif

int DivisionMenuGrid::getItemIndexAtPoint(juce::Point<int> p) const
{
    int contentX = p.x - kPadding;
    int contentY = p.y - kPadding;

    if (contentX < 0 || contentY < 0)
        return -1;

    // Determine column
    int col = -1;
    for (int c = 0; c < 3; ++c)
    {
        int colX = c * (kColumnWidth + kGutter);
        if (contentX >= colX && contentX < colX + kColumnWidth)
        {
            col = c;
            break;
        }
    }
    if (col < 0)
        return -1;

    // Determine row (skip header at row 0)
    int row = contentY / kRowHeight;
    if (row == 0)
        return -1;  // Clicked on header

    row -= 1;  // Adjust for header row

    if (row >= columns[col].items.size())
        return -1;

    return getItemIndexForCell(col, row);
}

int DivisionMenuGrid::getFirstSelectableIndex() const
{
    for (const auto& column : columns)
        for (const auto& item : column.items)
            if (item.menuIndex >= 0)
                return item.menuIndex;

    return -1;
}

int DivisionMenuGrid::getItemIndexForCell(int column, int row) const
{
    if (!juce::isPositiveAndBelow(column, 3) || !juce::isPositiveAndBelow(row, columns[column].items.size()))
        return -1;

    return columns[column].items.getReference(row).menuIndex;
}

int DivisionMenuGrid::getNearestItemIndexInColumn(int column, int navigationRow) const
{
    const int safeColumn = juce::jlimit(0, 2, column);
    const auto findAtNavigationRow = [this, safeColumn](int targetRow)
    {
        for (const auto& item : columns[safeColumn].items)
            if (item.menuIndex >= 0 && item.navigationRow == targetRow)
                return item.menuIndex;
        return -1;
    };

    if (const int exact = findAtNavigationRow(navigationRow); exact >= 0)
        return exact;

    const int maxDistance = getMaximumRowCount() + std::abs(navigationRow) + 1;
    for (int distance = 1; distance <= maxDistance; ++distance)
    {
        if (const int below = findAtNavigationRow(navigationRow + distance); below >= 0)
            return below;
        if (const int above = findAtNavigationRow(navigationRow - distance); above >= 0)
            return above;
    }

    return keyboardIndex;
}

int DivisionMenuGrid::getVerticalItemIndex(int column, int row, int direction) const
{
    const int safeDirection = direction < 0 ? -1 : 1;
    for (int candidateRow = row + safeDirection; candidateRow >= 0 && candidateRow < columns[column].items.size();
         candidateRow += safeDirection)
    {
        if (const int candidate = getItemIndexForCell(column, candidateRow); candidate >= 0)
            return candidate;
    }

    return keyboardIndex;
}

int DivisionMenuGrid::getMaximumRowCount() const
{
    int maximum = 0;
    for (const auto& column : columns)
        maximum = juce::jmax(maximum, column.items.size());
    return maximum;
}

int DivisionMenuGrid::getNavigationRowForIndex(int index) const
{
    for (const auto& column : columns)
        for (const auto& item : column.items)
            if (item.menuIndex == index)
                return item.navigationRow;

    return 0;
}

bool DivisionMenuGrid::getCellForIndex(int index, int& column, int& row) const
{
    if (index < 0)
        return false;

    for (int candidateColumn = 0; candidateColumn < 3; ++candidateColumn)
    {
        for (int candidateRow = 0; candidateRow < columns[candidateColumn].items.size(); ++candidateRow)
        {
            if (columns[candidateColumn].items.getReference(candidateRow).menuIndex == index)
            {
                column = candidateColumn;
                row = candidateRow;
                return true;
            }
        }
    }

    return false;
}

bool DivisionMenuGrid::isSelectableIndex(int index) const
{
    int column = -1;
    int row = -1;
    return getCellForIndex(index, column, row);
}
