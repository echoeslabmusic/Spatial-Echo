// PresetBrowserOverlay: Auditionable preset browser + preset manager implementation. Purpose filters, single-line rows,
//   one description strip.

#include "PresetBrowserOverlay.h"
#include "CRTLookAndFeel.h"
#include "MenuChrome.h"
#include "../UIConstants.h"
#include <algorithm>
#include <cmath>

// These child controls share the painted surface's geometry and actions.
// Pointer handling stays with the existing browser; native accessibility and
// keyboard focus use the same model, including offscreen preset rows.
class PresetBrowserOverlay::SemanticControl final : public juce::Component
{
public:
    SemanticControl(PresetBrowserOverlay& o, SemanticKind k, int i, const juce::String& title,
                    const juce::String& description = {})
        : owner(o)
        , kind(k)
        , index(i)
    {
        setTitle(title);
        setDescription(description);
        setWantsKeyboardFocus(true);
        setInterceptsMouseClicks(false, false);
    }
    void activate()
    {
        if (!isEnabled())
            return;
        if (kind == SemanticKind::category)
            owner.selectBrowseEntry(index);
        else if (kind == SemanticKind::preset)
            owner.selectAndLoadRow(index);
        else if (kind == SemanticKind::footer)
            owner.triggerAction(static_cast<Action>(index));
        else if (kind == SemanticKind::close)
            owner.dismiss();
        else if (const auto* item = owner.rowItem(index))
        {
            const auto name = item->name;
            const auto callback = kind == SemanticKind::rename ? owner.callbacks.onRename : owner.callbacks.onDelete;
            owner.dismiss();
            if (callback)
                callback(name);
        }
    }
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown())
            return false;
        if (key.getKeyCode() == juce::KeyPress::returnKey)
        {
            activate();
            return true;
        }
        const bool step = kind == SemanticKind::preset &&
                          (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey);
        if (step)
        {
            owner.stepAndLoad(key.getKeyCode() == juce::KeyPress::downKey ? 1 : -1, index);
            owner.focusSelectedPreset();
            return true;
        }
        return owner.keyPressed(key);
    }
    void focusGained(FocusChangeType) override
    {
        if (owner.updatingSemanticControls)
            return;
        if (kind == SemanticKind::preset || kind == SemanticKind::rename || kind == SemanticKind::remove)
        {
            owner.hoveredRow = index;
            owner.ensureRowVisible(index, false);
        }
        owner.repaintPanel();
        repaint();
    }
    void focusLost(FocusChangeType) override
    {
        if (owner.updatingSemanticControls)
            return;
        owner.hoveredRow = -1;
        owner.repaintPanel();
        repaint();
    }
    void paint(juce::Graphics& g) override
    {
        if (hasKeyboardFocus(true))
        {
            g.setColour(CRTLookAndFeel::getTerminalGreen());
            g.drawRect(getLocalBounds().reduced(1), 2);
        }
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        class Handler final : public juce::AccessibilityHandler
        {
        public:
            explicit Handler(SemanticControl& c)
                : AccessibilityHandler(
                      c,
                      c.kind == SemanticKind::preset     ? juce::AccessibilityRole::listItem
                      : c.kind == SemanticKind::category ? juce::AccessibilityRole::radioButton
                                                         : juce::AccessibilityRole::button,
                      juce::AccessibilityActions()
                          .addAction(juce::AccessibilityActionType::press, [&c] { c.activate(); })
                          .addAction(juce::AccessibilityActionType::toggle,
                                     [&c]
                                     {
                                         if (c.kind == SemanticKind::preset && c.owner.selectedRow != c.index)
                                             c.activate();
                                         else if (c.kind == SemanticKind::category)
                                             c.activate();
                                     })
                          .addAction(juce::AccessibilityActionType::focus, [&c] { c.grabKeyboardFocus(); }))
                , control(c)
            {
            }
            juce::AccessibleState getCurrentState() const override
            {
                auto state = AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
                if (control.kind == SemanticKind::preset)
                {
                    state = state.withSelectable();
                    if (control.owner.selectedRow == control.index)
                        state = state.withSelected();
                }
                else if (control.kind == SemanticKind::category)
                {
                    state = state.withCheckable();
                    if (control.owner.selectedBrowseEntry == control.index)
                        state = state.withChecked();
                }
                return state;
            }

        private:
            SemanticControl& control;
        };
        return std::make_unique<Handler>(*this);
    }
    PresetBrowserOverlay& owner;
    SemanticKind kind;
    int index;
};

std::unique_ptr<juce::AccessibilityHandler> PresetBrowserOverlay::PresetAccessGroup::createAccessibilityHandler()
{
    class Table final : public juce::AccessibilityTableInterface
    {
    public:
        explicit Table(PresetBrowserOverlay& o)
            : owner(o)
        {
        }
        int getNumRows() const override
        {
            int count = 0;
            for (const auto& c : owner.semanticControls)
                if (c->kind == SemanticKind::preset)
                    ++count;
            return count;
        }
        int getNumColumns() const override { return 1; }
        const juce::AccessibilityHandler* getHeaderHandler() const override { return nullptr; }
        const juce::AccessibilityHandler* getCellHandler(int, int) const override { return nullptr; }
        const juce::AccessibilityHandler* getRowHandler(int row) const override
        {
            if (row < 0)
                return nullptr;
            for (const auto& c : owner.semanticControls)
                if (c->kind == SemanticKind::preset && row-- == 0)
                    return c->getAccessibilityHandler();
            return nullptr;
        }
        juce::Optional<Span> getRowSpan(const juce::AccessibilityHandler& handler) const override
        {
            int row = 0;
            for (const auto& c : owner.semanticControls)
                if (c->kind == SemanticKind::preset)
                {
                    if (&handler.getComponent() == c.get())
                        return Span{row, 1};
                    ++row;
                }
            return juce::nullopt;
        }
        juce::Optional<Span> getColumnSpan(const juce::AccessibilityHandler& handler) const override
        {
            return getRowSpan(handler) ? juce::makeOptional(Span{0, 1}) : juce::nullopt;
        }
        void showCell(const juce::AccessibilityHandler& handler) const override
        {
            for (const auto& c : owner.semanticControls)
                if (&handler.getComponent() == c.get())
                {
                    owner.ensureRowVisible(c->index, false);
                    return;
                }
        }

    private:
        PresetBrowserOverlay& owner;
    };
    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::list, juce::AccessibilityActions{},
        juce::AccessibilityHandler::Interfaces{std::make_unique<Table>(owner)});
}

class PresetBrowserOverlay::FilterEditor final : public juce::TextEditor
{
public:
    explicit FilterEditor(PresetBrowserOverlay& o)
        : owner(o)
    {
    }
    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto code = key.getKeyCode();
        if ((key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()) && (code == 'Z' || code == 'S'))
            return false;
        if (code == juce::KeyPress::spaceKey)
            return false;
        if (code == juce::KeyPress::escapeKey || code == juce::KeyPress::returnKey || code == juce::KeyPress::upKey ||
            code == juce::KeyPress::downKey)
            return owner.keyPressed(key);
        return juce::TextEditor::keyPressed(key);
    }

private:
    PresetBrowserOverlay& owner;
};

PresetBrowserOverlay::PresetBrowserOverlay()
{
    setVisible(false);
    setWantsKeyboardFocus(true);
    setInterceptsMouseClicks(true, true);
    setTitle("Preset browser");
    setFocusContainerType(FocusContainerType::keyboardFocusContainer);
    presetAccessGroup.setTitle("Presets");
    presetAccessGroup.setFocusContainerType(FocusContainerType::focusContainer);
    presetAccessGroup.setInterceptsMouseClicks(false, true);
    addAndMakeVisible(presetAccessGroup);
    filterEditor = std::make_unique<FilterEditor>(*this);
    filterEditor->setTitle("Filter presets");
    filterEditor->setDescription("Search names, categories, descriptions, musical uses, or inspirations. Up and Down "
                                 "audition matches. Escape clears the filter or closes the browser.");
    filterEditor->setMultiLine(false);
    filterEditor->setJustification(juce::Justification::centredLeft);
    filterEditor->setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize));
    filterEditor->setTextToShowWhenEmpty("Search name, sound or use",
                                         CRTLookAndFeel::getTerminalGreen().withAlpha(UIConstants::alphaLabel));
    filterEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    filterEditor->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    filterEditor->setColour(juce::TextEditor::focusedOutlineColourId, CRTLookAndFeel::getTerminalGreen());
    filterEditor->setColour(juce::TextEditor::textColourId, CRTLookAndFeel::getTerminalGreen());
    filterEditor->onTextChange = [this]
    {
        filterText = filterEditor->getText();
        rebuildRows();
        repaintPanel();
    };
    addAndMakeVisible(*filterEditor);
    listScrollBar.setTitle("Scroll presets");
    listScrollBar.setAutoHide(true);
    listScrollBar.addListener(this);
    addAndMakeVisible(listScrollBar);
}

PresetBrowserOverlay::~PresetBrowserOverlay()
{
    ++libraryMenuGeneration;
    if (libraryMenu != nullptr && libraryMenu->isCurrentlyModal(false))
        libraryMenu->exitModalState(0);
    stopTimer();
    listScrollBar.removeListener(this);
    updatingSemanticControls = true;
    // Child focus callbacks must not inspect the model during teardown.
    semanticControls.clear();
    filterEditor->onTextChange = nullptr;
    filterEditor.reset();
}

void PresetBrowserOverlay::resized()
{
    rebuildRowLayouts();
    scrollY = scrollTarget = juce::jlimit(0.0f, maxScrollY(), scrollY);
    ensureRowVisible(selectedRow, false);
    layoutSemanticControls();
}

void PresetBrowserOverlay::rebuildSemanticControls()
{
    {
        const juce::ScopedValueSetter<bool> rebuilding(updatingSemanticControls, true);
        // Retain navigation/action controls: activating a group must not
        // destroy the native control while its accessibility action is running.
        semanticControls.erase(std::remove_if(semanticControls.begin(), semanticControls.end(),
                                              [](const auto& control)
                                              {
                                                  return control->kind == SemanticKind::preset ||
                                                         control->kind == SemanticKind::rename ||
                                                         control->kind == SemanticKind::remove;
                                              }),
                               semanticControls.end());
    }
    auto add = [&](SemanticKind kind, int index, const juce::String& title, const juce::String& description = {})
    {
        auto control = std::make_unique<SemanticControl>(*this, kind, index, title, description);
        const bool row = kind == SemanticKind::preset || kind == SemanticKind::rename || kind == SemanticKind::remove;
        if (row)
            presetAccessGroup.addAndMakeVisible(*control);
        else
            addAndMakeVisible(*control);
        semanticControls.push_back(std::move(control));
    };
    if (semanticControls.empty())
    {
        for (int c = 0; c < static_cast<int>(browseEntries.size()); ++c)
            add(SemanticKind::category, c, browseEntries[static_cast<size_t>(c)].label,
                "Filter presets without changing the loaded sound.");
        for (const auto& button : footerButtons())
            add(SemanticKind::footer, static_cast<int>(button.action), button.label);
        add(SemanticKind::close, 0, "Close preset browser");
    }
    for (int r = 0; r < static_cast<int>(rows.size()); ++r)
        if (const auto* item = rowItem(r))
        {
            add(SemanticKind::preset, r, item->name, item->description);
            if (item->userEditable)
            {
                add(SemanticKind::rename, r, "Rename " + item->name);
                add(SemanticKind::remove, r, "Delete " + item->name);
            }
        }
    layoutSemanticControls();
    if (auto* handler = presetAccessGroup.getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::structureChanged);
}

void PresetBrowserOverlay::layoutSemanticControls()
{
    if (!filterEditor || updatingSemanticControls)
        return;
    auto inner = filterArea();
    inner.removeFromLeft(38);  // search icon
    const int hintWidth = juce::roundToInt(std::ceil(juce::GlyphArrangement::getStringWidth(
        CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize), headerHintText())));
    inner.removeFromRight(hintWidth + 20);
    filterEditor->setBounds(inner.reduced(0, 9));
    if (filterEditor->getText() != filterText)
        filterEditor->setText(filterText, false);
    presetAccessGroup.setBounds(listArea());
    listScrollBar.setBounds(listArea().withX(listArea().getRight() - 10).withWidth(10));
    listScrollBar.setRangeLimits(0.0,
                                 juce::jmax(static_cast<double>(listArea().getHeight()),
                                            static_cast<double>(maxScrollY()) + listArea().getHeight()),
                                 juce::dontSendNotification);
    listScrollBar.setCurrentRange(static_cast<double>(scrollY), static_cast<double>(listArea().getHeight()),
                                  juce::dontSendNotification);
    const auto buttons = footerButtons();
    for (auto& control : semanticControls)
    {
        switch (control->kind)
        {
            case SemanticKind::category:
                control->setBounds(categoryRowBounds(control->index).reduced(6, 1));
                break;
            case SemanticKind::preset:
            case SemanticKind::rename:
            case SemanticKind::remove:
            {
                auto bounds = control->kind == SemanticKind::preset   ? rowScreenBounds(control->index).reduced(6, 1)
                              : control->kind == SemanticKind::rename ? renameGlyphBounds(control->index)
                                                                      : deleteGlyphBounds(control->index);
                control->setBounds(bounds.translated(-listArea().getX(), -listArea().getY()));
                break;
            }
            case SemanticKind::footer:
                for (const auto& button : buttons)
                    if (static_cast<int>(button.action) == control->index)
                    {
                        control->setBounds(button.bounds);
                        control->setEnabled(button.enabled);
                    }
                break;
            case SemanticKind::close:
                control->setBounds(closeButtonBounds());
                break;
        }
    }
}

void PresetBrowserOverlay::focusSelectedPreset()
{
    for (auto& control : semanticControls)
        if (control->kind == SemanticKind::preset && control->index == selectedRow)
        {
            control->grabKeyboardFocus();
            return;
        }
}

//==============================================================================
// Show / dismiss
//==============================================================================
void PresetBrowserOverlay::show(std::vector<Category> model, const juce::String& currentPresetName)
{
    {
        const juce::ScopedValueSetter<bool> rebuilding(updatingSemanticControls, true);
        semanticControls.clear();
    }
    categories = std::move(model);
    browseEntries.clear();
    browseEntries.push_back({"All Presets", BrowseKind::all, -1});
    for (int c = 0; c < static_cast<int>(categories.size()); ++c)
        if (!categories[static_cast<size_t>(c)].isInit &&
            !categories[static_cast<size_t>(c)].label.equalsIgnoreCase("User"))
            browseEntries.push_back({categories[static_cast<size_t>(c)].label, BrowseKind::category, c});
    browseEntries.push_back({"User", BrowseKind::user, -1});
    selectedBrowseEntry = 0;
    filterText.clear();
    currentName = currentPresetName;
    selectedRow = -1;
    hoveredRow = hoveredCategory = hoveredFooter = -1;
    hoveredGlyph = 0;
    hoveredClose = false;
    stopTimer();
    scrollY = scrollTarget = 0.0f;

    rebuildRows();

    setVisible(true);
    toFront(true);
    if (getPeer() != nullptr && isShowing())
        grabKeyboardFocus();
    ensureRowVisible(selectedRow, /*animated*/ false);
    repaint();
}

void PresetBrowserOverlay::dismiss()
{
    const juce::Component::SafePointer<PresetBrowserOverlay> safeThis(this);
    dismissLibraryMenu();
    if (safeThis == nullptr)
        return;
    stopTimer();
    setVisible(false);
    if (callbacks.onDismiss)
        callbacks.onDismiss();
}

void PresetBrowserOverlay::dismissLibraryMenu()
{
    auto menu = libraryMenu;
    libraryMenu = nullptr;
    // A cancelled menu must not deliver its delayed completion after another
    // walkthrough popup has acquired the modal key listener.
    ++libraryMenuGeneration;
    if (menu == nullptr)
        return;
    if (menu->isCurrentlyModal(false))
        menu->exitModalState(0);
    if (callbacks.onLibraryMenuClosed)
        callbacks.onLibraryMenuClosed();
}

//==============================================================================
// View model: selected purpose/shortcut intersected with the search query.
//==============================================================================
void PresetBrowserOverlay::rebuildRows()
{
    rows.clear();
    hoveredRow = -1;
    if (browseEntries.empty())
        return;
    const auto& selection = browseEntries[static_cast<size_t>(selectedBrowseEntry)];
    for (int c = 0; c < (int)categories.size(); ++c)
    {
        const auto& cat = categories[(size_t)c];

        for (int i = 0; i < (int)cat.items.size(); ++i)
        {
            const auto& item = cat.items[(size_t)i];
            if (matchesBrowseEntry(selection, c) && matchesQuery(cat, item))
            {
                Row row;
                row.category = c;
                row.item = i;
                rows.push_back(std::move(row));
            }
        }
    }
    rebuildRowLayouts();

    // Re-locate the loaded preset within the fresh row list.
    selectedRow = -1;
    for (int r = 0; r < (int)rows.size(); ++r)
    {
        if (const auto* item = rowItem(r); item != nullptr && item->name == currentName)
        {
            selectedRow = r;
            break;
        }
    }

    stopTimer();
    scrollY = scrollTarget = 0.0f;
    ensureRowVisible(selectedRow, /*animated*/ false);
    rebuildSemanticControls();
}

bool PresetBrowserOverlay::matchesQuery(const Category& category, const Item& item) const
{
    const auto words = juce::StringArray::fromTokens(filterText.trim(), false);
    return std::all_of(words.begin(), words.end(),
                       [&](const juce::String& word)
                       {
                           return item.name.containsIgnoreCase(word) || category.label.containsIgnoreCase(word) ||
                                  item.description.containsIgnoreCase(word) ||
                                  item.searchTerms.containsIgnoreCase(word);
                       });
}

bool PresetBrowserOverlay::matchesBrowseEntry(const BrowseEntry& entry, int category) const
{
    const auto& candidate = categories[static_cast<size_t>(category)];
    if (candidate.isInit)
        return false;
    switch (entry.kind)
    {
        case BrowseKind::all:
            return true;
        case BrowseKind::category:
            return category == entry.category;
        case BrowseKind::user:
            return candidate.label.equalsIgnoreCase("User");
    }
    return false;
}

int PresetBrowserOverlay::browseEntryCount(int index) const
{
    if (index < 0 || index >= static_cast<int>(browseEntries.size()))
        return 0;
    int count = 0;
    for (int c = 0; c < static_cast<int>(categories.size()); ++c)
        for (const auto& item : categories[static_cast<size_t>(c)].items)
            if (matchesBrowseEntry(browseEntries[static_cast<size_t>(index)], c) &&
                matchesQuery(categories[static_cast<size_t>(c)], item))
                ++count;
    return count;
}

void PresetBrowserOverlay::rebuildRowLayouts()
{
    // The description strip is sized for the tallest description in the whole
    // catalog at this width, so hovering never resizes the list.
    {
        const float stripWidth = static_cast<float>(juce::jmax(20, panelBounds().getWidth() - 2 - kRowInsetLeft * 2));
        int tallest = 0;
        for (const auto& category : categories)
            for (const auto& item : category.items)
                tallest = juce::jmax(
                    tallest, juce::roundToInt(std::ceil(intentLayout(item, "MODIFIED", stripWidth).getHeight())));
        intentHeight = juce::jmax(40, tallest + kIntentPaddingY * 2);
    }

    const auto nameFont = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize);
    const auto green = CRTLookAndFeel::getTerminalGreen();
    int top = 0;
    int previousCategory = -1;
    for (auto& row : rows)
    {
        const auto& item = categories[static_cast<size_t>(row.category)].items[static_cast<size_t>(row.item)];
        // Width for the tag and (User rows) the edit glyphs is reserved on every
        // row, so names never re-wrap when the selection or hover changes.
        const int nameWidth = juce::jmax(20, listArea().getWidth() - kRowInsetLeft - kRowInsetRight - kRowTagWidth -
                                                 (item.userEditable ? kGlyphWidth * 2 + 6 : 0));
        for (auto* layout : {&row.nameLayout, &row.nameLayoutBright})
        {
            juce::AttributedString name;
            name.setWordWrap(juce::AttributedString::WordWrap::byChar);
            name.append(item.name, nameFont,
                        green.withAlpha(layout == &row.nameLayout ? 0.78f : UIConstants::alphaBright));
            layout->createLayout(name, static_cast<float>(nameWidth));
        }
        row.startsGroup = row.category != previousCategory;
        previousCategory = row.category;
        if (row.startsGroup)
            top += kGroupLabelHeight;
        row.top = top;
        row.height =
            juce::jmax(kItemRowHeight, juce::roundToInt(std::ceil(row.nameLayout.getHeight())) + kItemPaddingY * 2);
        top += row.height;
    }
}

juce::TextLayout PresetBrowserOverlay::intentLayout(const Item& item, const juce::String& tag, float width) const
{
    const auto green = CRTLookAndFeel::getTerminalGreen();
    juce::AttributedString text;
    text.setWordWrap(juce::AttributedString::WordWrap::byWord);
    if (tag.isNotEmpty())
        text.append(tag + "  ", CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize),
                    tag == "MODIFIED" ? CRTLookAndFeel::getWarningAmber() : green.withAlpha(UIConstants::alphaLabel));
    text.append(item.name, CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize),
                green.withAlpha(UIConstants::alphaBright));
    const auto description = item.description.isNotEmpty() ? item.description
                             : item.userEditable           ? juce::String("Your saved preset.")
                                                           : juce::String("Factory preset.");
    text.append("  " + description, CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize),
                green.withAlpha(0.62f));
    juce::TextLayout layout;
    layout.createLayout(text, width);
    return layout;
}

const PresetBrowserOverlay::Item* PresetBrowserOverlay::intentItem(bool& isLoaded) const
{
    if (const auto* hovered = rowItem(hoveredRow))
    {
        isLoaded = hoveredRow == selectedRow;
        return hovered;
    }
    isLoaded = true;
    for (const auto& category : categories)
        for (const auto& item : category.items)
            if (item.name == currentName)
                return &item;
    return nullptr;
}

const PresetBrowserOverlay::Item* PresetBrowserOverlay::rowItem(int rowIndex) const
{
    if (rowIndex < 0 || rowIndex >= (int)rows.size())
        return nullptr;
    const auto& row = rows[(size_t)rowIndex];
    return &categories[(size_t)row.category].items[(size_t)row.item];
}

//==============================================================================
// Layout
//==============================================================================
juce::Rectangle<int> PresetBrowserOverlay::panelBounds() const
{
    // Centre in the editor-supplied placement area (guaranteed clear of the
    // GL head view so it stays live); fall back to centred-in-editor.
    const auto area = placementArea.isEmpty() ? getLocalBounds().reduced(24, 60) : placementArea.reduced(8);
    const int w = juce::jmax(0, juce::jmin(kPanelMaxWidth, area.getWidth()));
    const int h = juce::jmax(0, juce::jmin(kPanelMaxHeight, area.getHeight()));
    return {area.getCentreX() - w / 2, area.getCentreY() - h / 2, w, h};
}

juce::Rectangle<int> PresetBrowserOverlay::headerArea() const
{
    return panelBounds().reduced(1).removeFromTop(kHeaderHeight);
}

// The search row minus the close button.
juce::Rectangle<int> PresetBrowserOverlay::filterArea() const
{
    return headerArea().withTrimmedRight(kCloseWidth);
}

juce::Rectangle<int> PresetBrowserOverlay::closeButtonBounds() const
{
    return headerArea().removeFromRight(kCloseWidth).withSizeKeepingCentre(28, 28);
}

// Keyboard hints share the search row; the match count is kept while
// filtering, the audition hint only where the search field keeps its room.
juce::String PresetBrowserOverlay::headerHintText() const
{
    const auto font = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize);
    const juce::String full = filtering() ? juce::String(static_cast<int>(rows.size())) + " MATCHES   ESC CLEARS"
                                          : juce::String("UP/DOWN AUDITION   ESC CLOSES");
    const int roomForHints = filterArea().getWidth() - 36 - 180;
    if (filtering() || juce::GlyphArrangement::getStringWidth(font, full) <= static_cast<float>(roomForHints))
        return full;
    return "ESC CLOSES";
}

juce::Rectangle<int> PresetBrowserOverlay::footerArea() const
{
    return panelBounds().reduced(1).removeFromBottom(kActionRowHeight);
}

juce::Rectangle<int> PresetBrowserOverlay::intentArea() const
{
    auto panel = panelBounds().reduced(1);
    panel.removeFromBottom(kActionRowHeight);
    return panel.removeFromBottom(intentHeight);
}

int PresetBrowserOverlay::categoryColumnWidth() const
{
    const auto labelFont = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize);
    const auto countFont = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize);
    float widest = 0.0f;
    for (const auto& entry : browseEntries)
        widest = juce::jmax(widest, juce::GlyphArrangement::getStringWidth(labelFont, entry.label));
    const int needed = kRowInsetLeft + juce::roundToInt(std::ceil(widest)) + 12 +
                       juce::roundToInt(std::ceil(juce::GlyphArrangement::getStringWidth(countFont, "000"))) + 14;
    return juce::jlimit(kCategoryColumnMinWidth, juce::jmax(kCategoryColumnMinWidth, panelBounds().getWidth() / 2),
                        needed);
}

juce::Rectangle<int> PresetBrowserOverlay::categoryArea() const
{
    auto panel = panelBounds().reduced(1);
    panel.removeFromTop(kHeaderHeight);
    panel.removeFromBottom(kActionRowHeight + intentHeight);
    return panel.removeFromLeft(juce::jmin(categoryColumnWidth(), panel.getWidth() / 2));
}

juce::Rectangle<int> PresetBrowserOverlay::listArea() const
{
    auto panel = panelBounds().reduced(1);
    panel.removeFromTop(kHeaderHeight);
    panel.removeFromBottom(kActionRowHeight + intentHeight);
    panel.removeFromLeft(categoryArea().getWidth() + 1);
    return panel;
}

juce::Rectangle<int> PresetBrowserOverlay::categoryRowBounds(int index) const
{
    const auto area = categoryArea();
    // Fit every navigation row plus the User divider, even when a long loaded
    // description and the open scope shrink the browser body.
    constexpr int topGap = 8, dividerGap = 17;
    const int rowHeight =
        juce::jmin(kNavigationRowHeight, juce::jmax(1, (area.getHeight() - topGap * 2 - dividerGap) /
                                                           juce::jmax(1, static_cast<int>(browseEntries.size()))));
    const bool shortcut = index >= 0 && index < static_cast<int>(browseEntries.size()) &&
                          browseEntries[static_cast<size_t>(index)].kind == BrowseKind::user;
    return {area.getX(), area.getY() + topGap + index * rowHeight + (shortcut ? dividerGap : 0), area.getWidth(),
            rowHeight};
}

juce::Rectangle<int> PresetBrowserOverlay::rowScreenBounds(int rowIndex) const
{
    const auto area = listArea();
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows.size()))
        return {};
    const auto& row = rows[static_cast<size_t>(rowIndex)];
    return {area.getX(), area.getY() + row.top - juce::roundToInt(scrollY), area.getWidth() - 10, row.height};
}

juce::Rectangle<int> PresetBrowserOverlay::getWalkthroughUserPresetScreenBounds() const
{
    int userCategory = -1;
    for (int c = 0; c < static_cast<int>(categories.size()); ++c)
    {
        if (categories[static_cast<size_t>(c)].label.equalsIgnoreCase("User"))
        {
            userCategory = c;
            break;
        }
    }

    const auto saveAsTarget = [this]
    {
        for (const auto& button : footerButtons())
            if (button.action == Action::SaveAs)
                return localAreaToGlobal(button.bounds);
        return localAreaToGlobal(footerArea());
    };

    if (userCategory < 0)
        return saveAsTarget();

    juce::Rectangle<int> focus;
    for (int r = 0; r < static_cast<int>(rows.size()); ++r)
    {
        if (rows[static_cast<size_t>(r)].category != userCategory)
            continue;

        const auto visibleRow = rowScreenBounds(r).getIntersection(listArea());
        if (!visibleRow.isEmpty())
            focus = focus.isEmpty() ? visibleRow : focus.getUnion(visibleRow);
    }

    return focus.isEmpty() ? saveAsTarget() : localAreaToGlobal(focus);
}

void PresetBrowserOverlay::revealWalkthroughLoadedPreset()
{
    if (selectedRow < 0)
    {
        filterText.clear();
        selectedBrowseEntry = 0;
        rebuildRows();
    }
    ensureRowVisible(selectedRow, false);
}

void PresetBrowserOverlay::revealWalkthroughUserPresets()
{
    filterText.clear();
    for (int i = 0; i < static_cast<int>(browseEntries.size()); ++i)
        if (browseEntries[static_cast<size_t>(i)].kind == BrowseKind::user)
        {
            selectedBrowseEntry = i;
            rebuildRows();
            repaintPanel();
            return;
        }
}

// REN / DEL sit just left of the reserved tag column, on the name's first line.
juce::Rectangle<int> PresetBrowserOverlay::deleteGlyphBounds(int rowIndex) const
{
    auto row = rowScreenBounds(rowIndex).withHeight(kItemRowHeight).withTrimmedRight(kRowInsetRight - 10);
    row.removeFromRight(kRowTagWidth);
    return row.removeFromRight(kGlyphWidth).reduced(0, 3);
}

juce::Rectangle<int> PresetBrowserOverlay::renameGlyphBounds(int rowIndex) const
{
    auto row = rowScreenBounds(rowIndex).withHeight(kItemRowHeight).withTrimmedRight(kRowInsetRight - 10);
    row.removeFromRight(kRowTagWidth + kGlyphWidth + 2);  // step past the delete tag
    return row.removeFromRight(kGlyphWidth).reduced(0, 3);
}

float PresetBrowserOverlay::maxScrollY() const
{
    const float contentHeight = rows.empty() ? 0.0f : static_cast<float>(rows.back().top + rows.back().height + 8);
    return juce::jmax(0.0f, contentHeight - (float)listArea().getHeight());
}

int PresetBrowserOverlay::categoryIndexAt(juce::Point<int> pos) const
{
    if (!categoryArea().contains(pos))
        return -1;
    for (int index = 0; index < static_cast<int>(browseEntries.size()); ++index)
        if (categoryRowBounds(index).contains(pos))
            return index;
    return -1;
}

int PresetBrowserOverlay::rowIndexAt(juce::Point<int> pos) const
{
    if (!listArea().contains(pos))
        return -1;
    const float contentY = static_cast<float>(pos.y - listArea().getY()) + scrollY;
    if (contentY < 0.0f)
        return -1;
    for (int index = 0; index < static_cast<int>(rows.size()); ++index)
        if (contentY >= static_cast<float>(rows[static_cast<size_t>(index)].top) &&
            contentY <
                static_cast<float>(rows[static_cast<size_t>(index)].top + rows[static_cast<size_t>(index)].height))
            return index;
    return -1;
}

std::vector<PresetBrowserOverlay::FooterButton> PresetBrowserOverlay::footerButtons() const
{
    const auto actions = footerArea().reduced(kRowInsetLeft - 4, 9);
    const bool hasInit = std::any_of(categories.begin(), categories.end(),
                                     [](const auto& category) { return category.isInit && !category.items.empty(); });
    // One action row: start over and save on the left, library on the right.
    const auto init = actions.withWidth(64);
    return {{Action::Init, "INIT", init, hasInit},
            {Action::SaveAs, "SAVE AS", actions.withX(init.getRight() + 8).withWidth(96), true},
            {Action::Library, "LIBRARY", actions.withX(actions.getRight() - 106).withWidth(106), true}};
}

void PresetBrowserOverlay::showLibraryMenu()
{
    if (libraryMenu != nullptr && libraryMenu->isCurrentlyModal(false))
        return;
    const int generation = ++libraryMenuGeneration;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());
    menu.addItem(static_cast<int>(Action::Save) + 1, "Save", callbacks.canSave && callbacks.canSave());
    menu.addSeparator();
    menu.addItem(static_cast<int>(Action::Import) + 1, "Import...");
    menu.addItem(static_cast<int>(Action::Export) + 1, "Export...");
    menu.addItem(static_cast<int>(Action::Reveal) + 1, "Reveal preset folder");
    juce::Rectangle<int> anchor;
    for (const auto& button : footerButtons())
        if (button.action == Action::Library)
            anchor = button.bounds;
    const juce::Component::SafePointer<PresetBrowserOverlay> safeThis(this);
    menu.showMenuAsync(MenuChrome::popupOptions(*this, localAreaToGlobal(anchor)),
                       [safeThis, generation](int result)
                       {
                           if (safeThis == nullptr || generation != safeThis->libraryMenuGeneration)
                               return;
                           safeThis->libraryMenu = nullptr;
                           if (safeThis != nullptr && safeThis->callbacks.onLibraryMenuClosed)
                               safeThis->callbacks.onLibraryMenuClosed();
                           if (safeThis != nullptr && result > 0)
                               safeThis->triggerAction(static_cast<Action>(result - 1));
                       });
    libraryMenu = juce::Component::getCurrentlyModalComponent();
    if (callbacks.onLibraryMenuOpened)
        callbacks.onLibraryMenuOpened();
}

void PresetBrowserOverlay::triggerAction(Action a)
{
    if (a == Action::Library)
    {
        showLibraryMenu();
        return;
    }
    if (a == Action::Init)
    {
        for (const auto& category : categories)
            if (category.isInit && !category.items.empty())
            {
                currentName = category.items.front().name;
                selectedRow = -1;
                if (category.items.front().load)
                    category.items.front().load();
                if (auto* handler = presetAccessGroup.getAccessibilityHandler())
                    handler->notifyAccessibilityEvent(juce::AccessibilityEvent::rowSelectionChanged);
                repaintPanel();
                return;
            }
        return;
    }
    // Actions open dialogs / mutate the library, so close first and let the
    // editor drive; the browser is reopened with a fresh model next time.
    dismiss();
    switch (a)
    {
        case Action::Save:
            if (callbacks.onSave)
                callbacks.onSave();
            break;
        case Action::SaveAs:
            if (callbacks.onSaveAs)
                callbacks.onSaveAs();
            break;
        case Action::Import:
            if (callbacks.onImport)
                callbacks.onImport();
            break;
        case Action::Export:
            if (callbacks.onExport)
                callbacks.onExport();
            break;
        case Action::Reveal:
            if (callbacks.onReveal)
                callbacks.onReveal();
            break;
        case Action::Library:
        case Action::Init:
            break;
    }
}

//==============================================================================
// Painting
//==============================================================================
void PresetBrowserOverlay::paint(juce::Graphics& g)
{
    // Three brightness levels: bright = loaded / active, mid = items,
    // dim = labels, counts, and hints. Structure is hairlines, not boxes.
    const auto green = CRTLookAndFeel::getTerminalGreen();
    const auto hairline = green.withAlpha(0.16f);
    const float itemAlpha = 0.78f;

    // Dim the editor behind the panel. The GL head view intentionally punches
    // through this backdrop and stays live while presets are auditioned.
    g.fillAll(juce::Colours::black.withAlpha(0.55f));

    const auto panel = panelBounds();
    g.setColour(CRTLookAndFeel::getVoidBlack());
    g.fillRect(panel);
    g.setColour(green.withAlpha(0.35f));
    g.drawRect(panel, 1);

    // ---- Search row -------------------------------------------------------
    const auto header = headerArea();
    const auto field = filterArea();
    g.setColour(green.withAlpha(filtering() ? UIConstants::alphaBright : UIConstants::alphaLabel));
    const auto icon = juce::Rectangle<float>(static_cast<float>(field.getX() + kRowInsetLeft),
                                             static_cast<float>(field.getCentreY() - 7), 14.0f, 14.0f);
    g.drawEllipse(icon.withSize(10.0f, 10.0f), 1.3f);
    g.drawLine(icon.getX() + 8.5f, icon.getY() + 8.5f, icon.getRight(), icon.getBottom(), 1.3f);
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
    g.setColour(green.withAlpha(UIConstants::alphaDim));
    g.drawText(headerHintText(), field.withTrimmedRight(8), juce::Justification::centredRight, false);
    const auto close = closeButtonBounds().toFloat().reduced(8);
    g.setColour(green.withAlpha(hoveredClose ? 1.0f : UIConstants::alphaLabel));
    g.drawLine(close.getX(), close.getY(), close.getRight(), close.getBottom(), 1.5f);
    g.drawLine(close.getRight(), close.getY(), close.getX(), close.getBottom(), 1.5f);
    g.setColour(hairline);
    g.drawHorizontalLine(header.getBottom(), static_cast<float>(panel.getX() + 1),
                         static_cast<float>(panel.getRight() - 1));

    // ---- Group column -----------------------------------------------------
    const auto body = categoryArea();
    g.setColour(hairline);
    g.drawVerticalLine(body.getRight(), static_cast<float>(body.getY()), static_cast<float>(body.getBottom()));
    for (int c = 0; c < static_cast<int>(browseEntries.size()); ++c)
    {
        const auto bounds = categoryRowBounds(c);
        const bool active = c == selectedBrowseEntry;
        const bool hovered = c == hoveredCategory;
        if (browseEntries[static_cast<size_t>(c)].kind == BrowseKind::user)
        {
            g.setColour(hairline);
            g.drawHorizontalLine(bounds.getY() - 9, static_cast<float>(body.getX() + kRowInsetLeft),
                                 static_cast<float>(body.getRight() - 14));
        }
        if (active || hovered)
        {
            g.setColour(green.withAlpha(active ? 0.10f : 0.05f));
            g.fillRect(bounds);
        }
        if (active)
        {
            g.setColour(green);
            g.fillRect(bounds.withWidth(2));
        }
        auto text = bounds.withTrimmedLeft(kRowInsetLeft).withTrimmedRight(14);
        auto count = text.removeFromRight(28);
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize));
        g.setColour(green.withAlpha(active ? UIConstants::alphaBright : hovered ? 0.9f : itemAlpha));
        g.drawText(browseEntries[static_cast<size_t>(c)].label, text, juce::Justification::centredLeft, false);
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
        g.setColour(green.withAlpha(UIConstants::alphaDim));
        g.drawText(juce::String(browseEntryCount(c)), count, juce::Justification::centredRight, false);
    }

    // ---- Preset list ------------------------------------------------------
    const auto items = listArea();
    if (rows.empty())
    {
        juce::AttributedString message;
        message.append(filtering() ? "No matches in this group.\nTry another search or choose All Presets."
                       : !browseEntries.empty() &&
                               browseEntries[static_cast<size_t>(selectedBrowseEntry)].kind == BrowseKind::user
                           ? "No saved presets yet.\nUse Save As to keep your sound here."
                           : "No presets in this group.",
                       CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize),
                       green.withAlpha(UIConstants::alphaLabel));
        juce::TextLayout layout;
        layout.createLayout(message, static_cast<float>(juce::jmax(20, items.getWidth() - 40)));
        layout.draw(g, items.reduced(kRowInsetLeft, 16).toFloat());
    }

    const bool dirty = callbacks.isDirty && callbacks.isDirty();
    g.saveState();
    g.reduceClipRegion(items);
    for (int index = 0; index < static_cast<int>(rows.size()); ++index)
    {
        const auto bounds = rowScreenBounds(index);
        const auto& row = rows[static_cast<size_t>(index)];
        if (bounds.getBottom() <= items.getY())
            continue;
        if (bounds.getY() - kGroupLabelHeight >= items.getBottom())
            break;
        const auto& item = categories[static_cast<size_t>(row.category)].items[static_cast<size_t>(row.item)];

        if (row.startsGroup)
        {
            g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
            g.setColour(green.withAlpha(UIConstants::alphaDim));
            g.drawText(categories[static_cast<size_t>(row.category)].label.toUpperCase(),
                       bounds.withY(bounds.getY() - kGroupLabelHeight)
                           .withHeight(kGroupLabelHeight)
                           .withTrimmedLeft(kRowInsetLeft)
                           .withTrimmedBottom(4),
                       juce::Justification::bottomLeft, false);
        }

        const bool loaded = index == selectedRow;
        const bool hovered = index == hoveredRow;
        if (loaded || hovered)
        {
            g.setColour(green.withAlpha(loaded ? 0.10f : 0.05f));
            g.fillRect(bounds);
        }
        if (loaded)
        {
            g.setColour(green);
            g.fillRect(bounds.withWidth(2));
        }

        const auto& name = (loaded || hovered) ? row.nameLayoutBright : row.nameLayout;
        name.draw(g, bounds.withTrimmedLeft(kRowInsetLeft)
                         .withTrimmedTop(kItemPaddingY)
                         .toFloat()
                         .withHeight(name.getHeight())
                         .withWidth(name.getWidth()));

        auto firstLine = bounds.withHeight(kItemRowHeight).withTrimmedRight(kRowInsetRight - 10);
        const auto tag = firstLine.removeFromRight(kRowTagWidth);
        if (loaded)
        {
            g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
            g.setColour(dirty ? CRTLookAndFeel::getWarningAmber() : green.withAlpha(UIConstants::alphaLabel));
            g.drawText(dirty ? "MODIFIED" : "LOADED", tag, juce::Justification::centredRight, false);
        }

        const bool editFocus =
            std::any_of(semanticControls.begin(), semanticControls.end(),
                        [index](const auto& control)
                        {
                            return control->index == index &&
                                   (control->kind == SemanticKind::rename || control->kind == SemanticKind::remove) &&
                                   control->hasKeyboardFocus(false);
                        });
        if (item.userEditable && (hovered || editFocus))
        {
            g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
            g.setColour(green.withAlpha(hoveredGlyph == 1 ? 1.0f : UIConstants::alphaLabel));
            g.drawText("REN", renameGlyphBounds(index), juce::Justification::centred, false);
            g.setColour(green.withAlpha(hoveredGlyph == 2 ? 1.0f : UIConstants::alphaLabel));
            g.drawText("DEL", deleteGlyphBounds(index), juce::Justification::centred, false);
        }
    }
    g.restoreState();

    // ---- Description strip -------------------------------------------------
    const auto strip = intentArea();
    g.setColour(hairline);
    g.drawHorizontalLine(strip.getY(), static_cast<float>(panel.getX() + 1), static_cast<float>(panel.getRight() - 1));
    {
        bool isLoaded = false;
        const auto text = strip.reduced(kRowInsetLeft, kIntentPaddingY);
        if (const auto* item = intentItem(isLoaded))
        {
            const auto layout = intentLayout(*item, isLoaded ? (dirty ? "MODIFIED" : "LOADED") : juce::String(),
                                             static_cast<float>(text.getWidth()));
            layout.draw(g, text.toFloat());
        }
        else
        {
            g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
            g.setColour(green.withAlpha(UIConstants::alphaDim));
            g.drawText("Hover a preset to read what it adds.", text, juce::Justification::topLeft, false);
        }
    }

    // ---- Action row --------------------------------------------------------
    const auto footer = footerArea();
    g.setColour(hairline);
    g.drawHorizontalLine(footer.getY(), static_cast<float>(panel.getX() + 1), static_cast<float>(panel.getRight() - 1));
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
    const auto buttons = footerButtons();
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i)
    {
        const auto& button = buttons[static_cast<size_t>(i)];
        const bool hovered = i == hoveredFooter && button.enabled;
        if (hovered)
        {
            g.setColour(green.withAlpha(0.10f));
            g.fillRect(button.bounds);
        }
        g.setColour(green.withAlpha(button.enabled ? (hovered ? 0.6f : 0.32f) : 0.15f));
        g.drawRect(button.bounds, 1);
        g.setColour(green.withAlpha(button.enabled ? (hovered ? 1.0f : 0.85f) : UIConstants::alphaFaint));
        auto label = button.bounds;
        if (button.action == Action::Library)
        {
            const auto chevron =
                label.removeFromRight(20).toFloat().withSizeKeepingCentre(7.0f, 4.0f).translated(-4.0f, 0.0f);
            juce::Path path;
            path.startNewSubPath(chevron.getX(), chevron.getY());
            path.lineTo(chevron.getCentreX(), chevron.getBottom());
            path.lineTo(chevron.getRight(), chevron.getY());
            g.strokePath(path, juce::PathStrokeType(1.3f));
            label.removeFromLeft(6);
        }
        g.drawText(button.label, label, juce::Justification::centred, false);
    }
}

//==============================================================================
// Interaction
//==============================================================================
void PresetBrowserOverlay::selectAndLoadRow(int rowIndex)
{
    const auto* item = rowItem(rowIndex);
    if (item == nullptr)
        return;
    selectedRow = rowIndex;
    currentName = item->name;
    ensureRowVisible(rowIndex);
    if (item->load)
        item->load();
    if (auto* handler = presetAccessGroup.getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::rowSelectionChanged);
    repaintPanel();
}

void PresetBrowserOverlay::stepAndLoad(int direction, int originRow)
{
    const int count = (int)rows.size();
    if (count == 0)
        return;

    int index = originRow == -2 ? selectedRow : originRow;
    if (index < 0)
        index = (direction > 0) ? -1 : 0;
    index = (index + direction + count) % count;
    selectAndLoadRow(index);
}

void PresetBrowserOverlay::selectBrowseEntry(int index)
{
    if (index < 0 || index >= static_cast<int>(browseEntries.size()))
        return;
    selectedBrowseEntry = index;
    rebuildRows();
    for (const auto& control : semanticControls)
        if (control->kind == SemanticKind::category)
            if (auto* handler = control->getAccessibilityHandler())
                handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
    repaintPanel();
}

void PresetBrowserOverlay::ensureRowVisible(int rowIndex, bool animated)
{
    if (rowIndex < 0 || rowIndex >= (int)rows.size())
        return;
    const auto& row = rows[static_cast<size_t>(rowIndex)];
    const float rowTop = static_cast<float>(row.top - (row.startsGroup ? kGroupLabelHeight : 0));
    const float rowBottom = static_cast<float>(row.top + row.height + 4);
    const float viewHeight = (float)listArea().getHeight();

    float target = scrollTarget;
    if (rowTop < target)
        target = rowTop;
    else if (rowBottom > target + viewHeight)
        target = rowBottom - viewHeight;
    setScroll(target, animated);
}

void PresetBrowserOverlay::scrollBarMoved(juce::ScrollBar*, double newRangeStart)
{
    setScroll(static_cast<float>(newRangeStart), false);
}

void PresetBrowserOverlay::setScroll(float y, bool animated)
{
    scrollTarget = juce::jlimit(0.0f, maxScrollY(), y);
    if (!animated)
    {
        stopTimer();
        if (!juce::approximatelyEqual(scrollY, scrollTarget))
        {
            scrollY = scrollTarget;
            repaintPanel();
        }
        else
        {
            scrollY = scrollTarget;
        }
        return;
    }
    if (!juce::approximatelyEqual(scrollY, scrollTarget) && !isTimerRunning())
        startTimerHz(60);
}

void PresetBrowserOverlay::timerCallback()
{
    // Critically-damped-ish approach: cover 35% of the remaining distance per
    // frame; snap and stop within half a pixel.
    const float remaining = scrollTarget - scrollY;
    if (std::abs(remaining) < 0.5f)
    {
        scrollY = scrollTarget;
        stopTimer();
    }
    else
    {
        scrollY += remaining * 0.35f;
    }
    repaintPanel();
}

void PresetBrowserOverlay::mouseDown(const juce::MouseEvent& event)
{
    const auto pos = event.getPosition();
    if (!panelBounds().contains(pos))
    {
        dismiss();
        return;
    }

    grabKeyboardFocus();
    if (closeButtonBounds().contains(pos))
    {
        dismiss();
        return;
    }

    // Footer action bar first.
    for (const auto& b : footerButtons())
    {
        if (b.enabled && b.bounds.contains(pos))
        {
            triggerAction(b.action);
            return;
        }
    }

    if (const int cat = categoryIndexAt(pos); cat >= 0)
    {
        selectBrowseEntry(cat);
        return;
    }

    if (const int index = rowIndexAt(pos); index >= 0)
    {
        if (const auto* item = rowItem(index); item != nullptr && item->userEditable)
        {
            if (renameGlyphBounds(index).contains(pos))
            {
                const auto name = item->name;
                dismiss();
                if (callbacks.onRename)
                    callbacks.onRename(name);
                return;
            }
            if (deleteGlyphBounds(index).contains(pos))
            {
                const auto name = item->name;
                dismiss();
                if (callbacks.onDelete)
                    callbacks.onDelete(name);
                return;
            }
        }
        selectAndLoadRow(index);
    }
}

void PresetBrowserOverlay::mouseMove(const juce::MouseEvent& event)
{
    const auto pos = event.getPosition();
    const int cat = categoryIndexAt(pos);
    const int index = rowIndexAt(pos);

    int footer = -1;
    const auto buttons = footerButtons();
    for (int i = 0; i < (int)buttons.size(); ++i)
    {
        if (buttons[(size_t)i].enabled && buttons[(size_t)i].bounds.contains(pos))
        {
            footer = i;
            break;
        }
    }

    int glyph = 0;
    if (const auto* item = rowItem(index); item != nullptr && item->userEditable)
    {
        if (renameGlyphBounds(index).contains(pos))
            glyph = 1;
        else if (deleteGlyphBounds(index).contains(pos))
            glyph = 2;
    }

    const bool close = closeButtonBounds().contains(pos);
    if (cat != hoveredCategory || index != hoveredRow || footer != hoveredFooter || glyph != hoveredGlyph ||
        close != hoveredClose)
    {
        hoveredCategory = cat;
        hoveredRow = index;
        hoveredFooter = footer;
        hoveredGlyph = glyph;
        hoveredClose = close;
        const bool interactive = (cat >= 0 || close || rowItem(index) != nullptr || footer >= 0);
        setMouseCursor(interactive ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaintPanel();
    }
}

void PresetBrowserOverlay::mouseExit(const juce::MouseEvent&)
{
    if (hoveredCategory != -1 || hoveredRow != -1 || hoveredFooter != -1 || hoveredGlyph != 0 || hoveredClose)
    {
        hoveredCategory = hoveredRow = hoveredFooter = -1;
        hoveredGlyph = 0;
        hoveredClose = false;
        repaintPanel();
    }
}

void PresetBrowserOverlay::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (!listArea().contains(event.getPosition()))
        return;

    // JUCE wheel deltas arrive sign-correct for content scrolling (Viewport
    // uses them raw; natural scrolling is already folded in by the OS), so no
    // isReversed handling. Trackpad streams (smooth/inertial) carry OS
    // momentum and apply directly - easing on top of momentum feels laggy.
    // Notch wheels get the easing. 260 px/notch tracks Viewport's feel.
    const float amount = wheel.deltaY * 260.0f;
    if (wheel.isSmooth || wheel.isInertial)
    {
        stopTimer();
        const float clamped = juce::jlimit(0.0f, maxScrollY(), scrollY - amount);
        if (!juce::approximatelyEqual(clamped, scrollY))
        {
            scrollY = scrollTarget = clamped;
            repaintPanel();
        }
    }
    else
    {
        setScroll(scrollTarget - amount, /*animated*/ true);
    }
}

bool PresetBrowserOverlay::keyPressed(const juce::KeyPress& key)
{
    const int code = key.getKeyCode();

    // Host/editor commands must bubble up rather than becoming filter text
    // or moving the audition selection (for example Command-Z and Save).
    if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown())
        return false;

    // Space is the host transport shortcut in DAWs. Never turn it into filter
    // text or consume it while producers audition presets.
    if (code == juce::KeyPress::spaceKey)
        return false;

    if (code == juce::KeyPress::escapeKey)
    {
        if (filtering())
        {
            filterText.clear();
            rebuildRows();
            repaintPanel();
            return true;
        }
        dismiss();
        return true;
    }

    if (code == juce::KeyPress::returnKey)
    {
        dismiss();  // current selection is already loaded (audition-on-select)
        return true;
    }

    if (code == juce::KeyPress::backspaceKey)
    {
        if (filtering())
        {
            filterText = filterText.dropLastCharacters(1);
            rebuildRows();
            repaintPanel();
        }
        return true;
    }

    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        stepAndLoad(code == juce::KeyPress::downKey ? 1 : -1);
        return true;
    }

    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const int count = static_cast<int>(browseEntries.size());
        if (count == 0)
            return true;
        const int dir = (code == juce::KeyPress::rightKey) ? 1 : -1;
        selectBrowseEntry((selectedBrowseEntry + dir + count) % count);
        return true;
    }

    // Type-to-filter: any printable character narrows the list.
    const juce::juce_wchar ch = key.getTextCharacter();
    if (ch >= ' ' && ch != 127)
    {
        if (ch == '/' && filterText.isEmpty())
            return true;  // '/' is just the (optional) "start filtering" affordance
        filterText += juce::String::charToString(ch);
        rebuildRows();
        repaintPanel();
        return true;
    }

    return false;
}
