// HelpOverlay: Modal overlay displaying keyboard shortcuts and interaction hints (toggled by '?' key).

#pragma once
#include <JuceHeader.h>
#include "CRTLookAndFeel.h"

// Undo/redo shortcut labels for the current platform, in plain ASCII: the
// embedded IBM VGA font has no command-key or shift-arrow glyphs.
namespace EditorShortcuts
{
inline juce::String undoKeyText()
{
#if JUCE_MAC
    return "Cmd+Z";
#else
    return "Ctrl+Z";
#endif
}

inline juce::String redoKeyText()
{
#if JUCE_MAC
    return "Shift+Cmd+Z";
#else
    return "Ctrl+Shift+Z";
#endif
}
}  // namespace EditorShortcuts

/**
 * HelpOverlay - Toggleable overlay showing keyboard shortcuts and interaction hints.
 *
 * Opened by the '?' key or the Spatial Echo menu's Help entry. Draws a
 * semi-transparent CRT-styled panel listing the shortcuts and gestures; a
 * click, '?' or Escape closes it.
 */
class HelpOverlay : public juce::Component
{
public:
    HelpOverlay()
    {
        setInterceptsMouseClicks(true, false);
        setAlwaysOnTop(true);
        setVisible(false);
        setWantsKeyboardFocus(true);
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        // Semi-transparent backdrop
        g.setColour(CRTLookAndFeel::getVoidBlack().withAlpha(0.88f));
        g.fillRect(bounds);

        // Inner panel
        const float panelW = juce::jmin(540.0f, bounds.getWidth() - 40.0f);
        const float panelH = juce::jmin(450.0f, bounds.getHeight() - 40.0f);
        auto panel = juce::Rectangle<float>(bounds.getCentreX() - panelW / 2.0f, bounds.getCentreY() - panelH / 2.0f,
                                            panelW, panelH);

        g.setColour(CRTLookAndFeel::getVoidBlack().withAlpha(0.95f));
        g.fillRoundedRectangle(panel, 6.0f);
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.6f));
        g.drawRoundedRectangle(panel.reduced(0.5f), 6.0f, 1.5f);

        CRTLookAndFeel::drawGradientOverlay(g, panel.toNearestInt());

        // Title
        auto titleArea = panel.removeFromTop(36.0f);
        g.setColour(CRTLookAndFeel::getTerminalGreen());
        constexpr float kTitleFontSize = 16.0f;
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(kTitleFontSize));
        g.drawText("KEYBOARD SHORTCUTS & HINTS", titleArea, juce::Justification::centred);

        // Separator
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.3f));
        g.drawLine(panel.getX() + 12.0f, panel.getY(), panel.getRight() - 12.0f, panel.getY(), 1.0f);

        panel.removeFromTop(8.0f);

        // Shortcut entries
        struct Entry
        {
            juce::String key;
            const char* desc;
        };
        const Entry entries[] = {
            {EditorShortcuts::undoKeyText(), "Undo last parameter change"},
            {EditorShortcuts::redoKeyText(), "Redo last undone change"},
            {"Double-click", "Reset knob to default value"},
            {"Shift+drag", "Fine-adjust knob or slider"},
            {"Right-click knob", "Context menu: reset, copy, min/max"},
            {"Section drawers", "Open ECHO ENGINE, REVERB, BLEND, or FINE-TUNE"},
            {"AUTO-PANIC", "Optional runaway watchdog (SPATIAL ECHO menu)"},
            {"Arrow keys", "Nudge value of focused knob or slider"},
            {"Tab / Shift+Tab", "Move focus between controls"},
            {"Tooltips", "Show/hide hover tips (SPATIAL ECHO menu)"},
            {"QUALITY", "Spatial render quality (BLEND drawer)"},
            {"Interactive Tour", "Replay from the SPATIAL ECHO menu"},
            {"?", "Toggle this help overlay"},
        };

        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeBodySize));
        const float rowH = 24.0f;
        const float keyColW = 140.0f;
        const float leftPad = panel.getX() + 20.0f;

        for (const auto& e : entries)
        {
            if (panel.getHeight() < rowH)
                break;
            auto row = panel.removeFromTop(rowH);

            g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.9f));
            g.drawText(e.key, juce::Rectangle<float>(leftPad, row.getY(), keyColW, rowH),
                       juce::Justification::centredLeft);

            g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.6f));
            g.drawText(e.desc,
                       juce::Rectangle<float>(leftPad + keyColW, row.getY(), panel.getWidth() - keyColW - 24.0f, rowH),
                       juce::Justification::centredLeft);
        }

        // Footer hint
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.4f));
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
        g.drawText("Press ? or click anywhere to close", getLocalBounds().removeFromBottom(24).toFloat(),
                   juce::Justification::centred);
    }

    void mouseDown(const juce::MouseEvent&) override { setVisible(false); }

    void toggle()
    {
        setVisible(!isVisible());
        if (isVisible())
            toFront(true);
    }

    // The editor wires this to its GL-overlay choke point: ESC-dismissal
    // below changes visibility without going through the editor's toggle
    // sites, and the head view's GL context must reattach on hide.
    std::function<void()> onVisibilityChanged;

    void visibilityChanged() override
    {
        if (isVisible() && isShowing())
            grabKeyboardFocus();
        if (onVisibilityChanged)
            onVisibilityChanged();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            setVisible(false);
            return true;
        }
        return false;
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HelpOverlay)
};
