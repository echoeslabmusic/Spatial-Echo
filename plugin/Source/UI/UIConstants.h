// UIConstants: Shared editor dimensions, panel base grids, spacing, and the terminal type scale.

#pragma once

#include <JuceHeader.h>

namespace UIConstants
{
// Editor window. The default size is also the minimum: below it controls drop
// under the 24 px minimum hit target. The aspect ratio is fixed.
constexpr int editorWidth = 1400;
constexpr int editorHeight = 650;
constexpr int editorMaxWidth = 1800;
constexpr int editorMaxHeight = 837;

// Base grids the delay and reverb panels are authored on; GridScaler maps them
// onto the panels' live bounds so the layout keeps its proportions.
constexpr int delayGridWidth = 672;
constexpr int delayGridHeight = 333;
constexpr int reverbGridWidth = 672;
constexpr int reverbGridHeight = 209;

// Text and labels
constexpr int labelHeight = 16;
constexpr int textBoxWidth = 70;
constexpr int textBoxHeight = 24;

// Spacing
constexpr int padding = 8;
constexpr int smallPadding = 4;
constexpr int panelPadding = 8;
constexpr int panelGap = 8;

// Header strip: control row plus the optional oscilloscope below it.
constexpr int headerHeight = 130;
constexpr int headerControlStripHeight = 56;

// Type scale: one terminal face (IBM VGA); hierarchy comes from size and
// brightness tiers only.
constexpr float typeSectionSize = 14.0f;  // section titles, dropdown headers
constexpr float typeValueSize = 13.0f;    // numeric readouts, menu rows
constexpr float typeBodySize = 12.0f;     // button text, tooltips, help rows
constexpr float typeLabelSize = 11.0f;    // control labels
constexpr float typeMicroSize = 10.0f;    // status strips, hints, captions
constexpr float alphaBright = 0.95f;      // primary text, active states
constexpr float alphaLabel = 0.70f;       // labels
constexpr float alphaDim = 0.45f;         // secondary hints
constexpr float alphaFaint = 0.30f;       // structural accents

// Maps rectangles authored on a gridWidth x gridHeight base grid into `area`.
class GridScaler
{
public:
    GridScaler(juce::Rectangle<int> areaToFill, int gridWidth, int gridHeight)
        : area(areaToFill)
        , sx(static_cast<float>(areaToFill.getWidth()) / static_cast<float>(gridWidth))
        , sy(static_cast<float>(areaToFill.getHeight()) / static_cast<float>(gridHeight))
    {
    }

    juce::Rectangle<int> operator()(int x, int y, int w, int h) const
    {
        return {area.getX() + juce::roundToInt(static_cast<float>(x) * sx),
                area.getY() + juce::roundToInt(static_cast<float>(y) * sy),
                juce::roundToInt(static_cast<float>(w) * sx), juce::roundToInt(static_cast<float>(h) * sy)};
    }

    float getScaleY() const noexcept { return sy; }

private:
    juce::Rectangle<int> area;
    float sx = 1.0f;
    float sy = 1.0f;
};
}  // namespace UIConstants
