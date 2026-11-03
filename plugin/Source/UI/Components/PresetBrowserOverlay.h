// PresetBrowserOverlay: In-editor auditionable preset browser: sound-purpose filters, a single-line preset list under
//   group labels, and one description strip for the hovered or loaded preset. Also the home for every preset action.

#pragma once
#include <JuceHeader.h>
#include <functional>
#include <vector>

/**
 * PresetBrowserOverlay - the presets-first on-ramp AND preset manager.
 *
 * The top row is the search field. The left column filters by audible
 * purpose, with All Presets and User shortcuts; search intersects that
 * selection. The list shows one line per preset (long names wrap) under
 * group labels. A fixed-height strip above the actions shows the complete
 * description of the hovered preset, else the loaded one with its LOADED /
 * MODIFIED tag, so the loaded identity survives a filter that hides its row.
 *
 * Selecting a preset LOADS it immediately (loads are undoable) and the
 * browser stays open so presets can be auditioned in quick succession. The
 * loaded row carries a left bar and a LOADED tag, which reads MODIFIED when
 * the live state deviates from the loaded preset (Callbacks::isDirty).
 *
 * One action row: INIT and SAVE AS on the left, a LIBRARY menu for
 * save/import/export/reveal on the right.
 * Hovering a User preset row reveals inline REN / DEL affordances. All
 * actions are wired in by PluginEditor via setCallbacks(), so the overlay
 * stays decoupled from the preset system.
 *
 * GL coexistence: the editor passes setPanelPlacementArea() a region that
 * never intersects the GL head view, so the head stays attached and LIVE
 * while presets are auditioned (the GL context punches through the dim
 * backdrop; the panel itself must never sit under it).
 *
 * Keys: type = filter, Space = pass through to the host transport, Up/Down =
 * step + load (visible results), Left/Right = change group, Return = close,
 * Escape = clear filter then close. Click outside the panel closes.
 */
class PresetBrowserOverlay : public juce::Component, private juce::Timer, private juce::ScrollBar::Listener
{
public:
    struct Item
    {
        juce::String name;
        juce::String description;  // complete intent shown in the description strip
        std::function<void()> load;
        bool userEditable = false;  // User presets: expose inline rename/delete
        juce::String searchTerms;   // musical uses, sound qualities, inspirations
    };

    struct Category
    {
        juce::String label;
        std::vector<Item> items;
        // The INIT action's category: its first item is loaded by the INIT
        // button, and it is never listed, filtered or browsed.
        bool isInit = false;
    };

    // Action callbacks wired by PluginEditor. onRename/onDelete receive the
    // target preset name. canSave/isDirty are polled at paint time so they
    // stay correct while auditioning keeps the browser open. onDismiss fires
    // when the overlay closes (GL attachment bookkeeping).
    struct Callbacks
    {
        std::function<void()> onSave, onSaveAs, onImport, onExport, onReveal, onDismiss;
        std::function<void()> onLibraryMenuOpened, onLibraryMenuClosed;
        std::function<void(const juce::String&)> onRename, onDelete;
        std::function<bool()> canSave;  // a User preset is loaded (SAVE overwrites in place)
        std::function<bool()> isDirty;  // live state deviates from the loaded preset
    };

    PresetBrowserOverlay();
    ~PresetBrowserOverlay() override;

    void setCallbacks(Callbacks cb) { callbacks = std::move(cb); }

    // Region (editor coordinates == local coordinates, the overlay covers the
    // editor) the panel is centred in. The editor guarantees it never
    // intersects the GL head view. Empty => centred-in-editor fallback.
    void setPanelPlacementArea(juce::Rectangle<int> area)
    {
        placementArea = area;
        resized();
    }

    // Opens the browser with a fresh model; currentName selects the entry
    // highlighted as the loaded preset (matched across all categories).
    void show(std::vector<Category> model, const juce::String& currentName);
    void dismiss();
    void dismissLibraryMenu();

    juce::Rectangle<int> getWalkthroughPanelScreenBounds() const { return localAreaToGlobal(panelBounds()); }
    juce::Rectangle<int> getWalkthroughCategoryScreenBounds() const { return localAreaToGlobal(categoryArea()); }
    juce::Rectangle<int> getWalkthroughPresetListScreenBounds() const { return localAreaToGlobal(listArea()); }
    juce::Rectangle<int> getWalkthroughIntentScreenBounds() const { return localAreaToGlobal(intentArea()); }
    juce::Rectangle<int> getWalkthroughFilterScreenBounds() const { return localAreaToGlobal(filterArea()); }
    juce::Rectangle<int> getWalkthroughActionScreenBounds() const { return localAreaToGlobal(footerArea()); }
    juce::Rectangle<int> getWalkthroughUserPresetScreenBounds() const;
    void revealWalkthroughLoadedPreset();
    void revealWalkthroughUserPresets();
    juce::String getWalkthroughFilterText() const { return filterText; }
    bool isShowing() const { return isVisible(); }

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    void setWalkthroughFilterTextForTesting(juce::String text) { filterText = std::move(text); }
#endif

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    enum class SemanticKind
    {
        category,
        preset,
        rename,
        remove,
        footer,
        close
    };
    class SemanticControl;
    class FilterEditor;
    void rebuildSemanticControls();
    void layoutSemanticControls();
    void focusSelectedPreset();
    bool updatingSemanticControls = false;
    class PresetAccessGroup final : public juce::Component
    {
    public:
        explicit PresetAccessGroup(PresetBrowserOverlay& o)
            : owner(o)
        {
        }
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    private:
        PresetBrowserOverlay& owner;
    };
    PresetAccessGroup presetAccessGroup{*this};
    std::unique_ptr<FilterEditor> filterEditor;
    juce::ScrollBar listScrollBar{true};
    juce::Component::SafePointer<juce::Component> libraryMenu;
    int libraryMenuGeneration = 0;
    std::vector<std::unique_ptr<SemanticControl>> semanticControls;
    enum class BrowseKind
    {
        all,
        category,
        user
    };
    struct BrowseEntry
    {
        juce::String label;
        BrowseKind kind = BrowseKind::all;
        int category = -1;
    };
    // Text layouts and height are measured together, so wrapping and hit
    // testing use the same geometry at every editor size. A row that starts a
    // new group has its group label painted in the band just above `top`.
    struct Row
    {
        int category = 0;
        int item = 0;
        int top = 0;
        int height = 0;
        bool startsGroup = false;
        juce::TextLayout nameLayout, nameLayoutBright;
    };

    // Layout (computed from the current bounds each time)
    juce::Rectangle<int> panelBounds() const;
    juce::Rectangle<int> headerArea() const;
    juce::Rectangle<int> closeButtonBounds() const;
    juce::Rectangle<int> filterArea() const;
    juce::Rectangle<int> intentArea() const;  // description strip
    juce::Rectangle<int> footerArea() const;  // action row
    juce::Rectangle<int> categoryArea() const;
    juce::Rectangle<int> listArea() const;
    int categoryColumnWidth() const;
    juce::String headerHintText() const;
    juce::Rectangle<int> categoryRowBounds(int index) const;
    juce::Rectangle<int> rowScreenBounds(int rowIndex) const;  // pixel-scrolled
    juce::TextLayout intentLayout(const Item& item, const juce::String& tag, float width) const;
    const Item* intentItem(bool& isLoaded) const;
    juce::Rectangle<int> renameGlyphBounds(int rowIndex) const;
    juce::Rectangle<int> deleteGlyphBounds(int rowIndex) const;
    int categoryIndexAt(juce::Point<int> pos) const;
    int rowIndexAt(juce::Point<int> pos) const;

    // Footer action bar
    enum class Action
    {
        Save,
        SaveAs,
        Import,
        Export,
        Reveal,
        Library,
        Init
    };
    struct FooterButton
    {
        Action action;
        juce::String label;
        juce::Rectangle<int> bounds;
        bool enabled;
    };
    std::vector<FooterButton> footerButtons() const;
    void triggerAction(Action a);
    void showLibraryMenu();

    void selectAndLoadRow(int rowIndex);
    void stepAndLoad(int direction, int originRow = -2);
    void selectBrowseEntry(int index);
    void ensureRowVisible(int rowIndex, bool animated = true);
    void rebuildRows();
    void rebuildRowLayouts();
    bool matchesQuery(const Category& category, const Item& item) const;
    bool matchesBrowseEntry(const BrowseEntry& entry, int category) const;
    int browseEntryCount(int index) const;
    bool filtering() const { return filterText.isNotEmpty(); }
    const Item* rowItem(int rowIndex) const;

    // Pixel-based smooth scrolling. Trackpad (smooth/inertial) deltas write
    // scrollY directly - the OS already supplies momentum; notch wheels and
    // keyboard navigation eases toward scrollTarget on a 60Hz timer.
    void timerCallback() override;
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override;
    float maxScrollY() const;
    void setScroll(float y, bool animated);
    void repaintPanel()
    {
        layoutSemanticControls();
        repaint(panelBounds());
    }

    static constexpr int kHeaderHeight = 44;  // search row
    static constexpr int kCloseWidth = 40;
    static constexpr int kNavigationRowHeight = 30;
    static constexpr int kCategoryColumnMinWidth = 150;
    static constexpr int kItemRowHeight = 28;
    static constexpr int kItemPaddingY = 6;
    static constexpr int kGroupLabelHeight = 26;
    static constexpr int kRowInsetLeft = 16;
    static constexpr int kRowInsetRight = 22;  // clears the scrollbar
    static constexpr int kRowTagWidth = 64;    // LOADED / MODIFIED
    static constexpr int kIntentPaddingY = 8;
    static constexpr int kActionRowHeight = 46;
    static constexpr int kGlyphWidth = 26;  // inline REN / DEL tag width
    static constexpr int kPanelMaxWidth = 720;
    static constexpr int kPanelMaxHeight = 680;

    std::vector<Category> categories;
    std::vector<BrowseEntry> browseEntries;
    std::vector<Row> rows;
    int intentHeight = 40;  // strip height: tallest description at this width
    Callbacks callbacks;
    juce::String filterText;   // type-to-filter query (empty = not filtering)
    juce::String currentName;  // loaded preset, re-highlighted after rebuilds
    juce::Rectangle<int> placementArea;
    int selectedRow = -1;  // row index of the loaded preset (item rows only)
    int selectedBrowseEntry = 0;
    int hoveredRow = -1;
    int hoveredCategory = -1;
    int hoveredFooter = -1;  // index into footerButtons()
    int hoveredGlyph = 0;    // 0 none, 1 rename, 2 delete (on hoveredRow)
    bool hoveredClose = false;
    float scrollY = 0.0f;       // pixel scroll offset into the row list
    float scrollTarget = 0.0f;  // easing destination (== scrollY when idle)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBrowserOverlay)
};
