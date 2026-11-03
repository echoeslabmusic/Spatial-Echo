// InteractiveWalkthroughTests: First-run walkthrough state, targeting, and persistence contracts.

#include "SystemTests.h"
#include "../Common/NativeEventLoop.h"
#include "../../PluginEditor.h"
#include "../../Presets/FactoryPresetLibrary.h"
#include "../../Presets/FactoryPresetDescriptions.h"
#include "../../Presets/FactoryPresetBrowseGroups.h"
#include "../../UI/UIConstants.h"
#include "../../UI/Components/DelayPanel.h"
#include "../../UI/Components/InteractiveWalkthrough.h"
#include "../../UI/Components/SpatialHeadView.h"
#include "../../UI/Components/EditorPreferences.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

namespace SpatialEchoTests::System
{
namespace
{
template <typename ComponentType>
ComponentType* findDescendant(juce::Component& parent)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* match = dynamic_cast<ComponentType*>(child))
            return match;
        if (auto* match = findDescendant<ComponentType>(*child))
            return match;
    }
    return nullptr;
}

juce::Component* findChildWithTitle(juce::Component& parent, const juce::String& title)
{
    for (auto* child : parent.getChildren())
    {
        if (child->getTitle() == title)
            return child;
        if (auto* match = findChildWithTitle(*child, title))
            return match;
    }
    return nullptr;
}

class ScopedTestDirectory
{
public:
    ScopedTestDirectory()
        : directory(SpatialEchoTests::artifactDirectory().getNonexistentChildFile("walkthrough", {}))
    {
        directory.createDirectory();
    }

    ~ScopedTestDirectory() { directory.deleteRecursively(); }

    const juce::File& getFile() const { return directory; }

private:
    juce::File directory;
};

class RepaintRecorder final : public juce::CachedComponentImage
{
public:
    explicit RepaintRecorder(juce::Component& ownerIn)
        : owner(ownerIn)
    {
    }

    void paint(juce::Graphics&) override {}

    bool invalidateAll() override
    {
        invalidatedBounds = invalidatedBounds.getUnion(owner.getLocalBounds());
        return true;
    }

    bool invalidate(const juce::Rectangle<int>& area) override
    {
        invalidatedBounds = invalidatedBounds.getUnion(area);
        return true;
    }

    void releaseResources() override {}

    void clear() { invalidatedBounds = {}; }

    bool covers(const juce::Rectangle<int>& area) const { return invalidatedBounds.contains(area); }

private:
    juce::Component& owner;
    juce::Rectangle<int> invalidatedBounds;
};

juce::Image renderWalkthroughCard(InteractiveWalkthrough& walkthrough)
{
    const auto card = walkthrough.getCardBoundsForTesting();
    juce::Image image(juce::Image::ARGB, card.getWidth(), card.getHeight(), true);
    juce::Graphics graphics(image);
    graphics.setOrigin(-card.getX(), -card.getY());
    walkthrough.paint(graphics);
    return image;
}

bool imagesMatch(const juce::Image& first, const juce::Image& second)
{
    if (first.getBounds() != second.getBounds())
        return false;

    for (int y = 0; y < first.getHeight(); ++y)
        for (int x = 0; x < first.getWidth(); ++x)
            if (first.getPixelAt(x, y) != second.getPixelAt(x, y))
                return false;

    return true;
}

class PresetBrowserKeyboardHost final : public juce::Component
{
public:
    PresetBrowserKeyboardHost()
    {
        setBounds(-2000, -2000, 900, 650);
        setWantsKeyboardFocus(true);
        addAndMakeVisible(opener);
        addChildComponent(browser);
        addChildComponent(walkthrough);
        opener.setBounds(0, 0, 120, 28);
        opener.setWantsKeyboardFocus(true);
        browser.setBounds(getLocalBounds());
        walkthrough.setBounds(getLocalBounds());
    }

    void open()
    {
        opener.grabKeyboardFocus();
        walkthrough.onStepChanged = [this](InteractiveWalkthrough::Target,
                                           InteractiveWalkthrough::ExpandedTarget expandedTarget,
                                           InteractiveWalkthrough::SpotlightDetail)
        {
            if (expandedTarget != InteractiveWalkthrough::ExpandedTarget::presetBrowser || browser.isVisible())
                return;

            PresetBrowserOverlay::Category factoryCategory;
            factoryCategory.label = "Factory";
            factoryCategory.items.push_back(
                {"Factory One", "first sound", [this] { loadedPreset = "Factory One"; }, false, {}});
            factoryCategory.items.push_back(
                {"Factory Two", "second sound", [this] { loadedPreset = "Factory Two"; }, false, {}});
            walkthrough.setSpotlightFocusComponent(&browser);
            browser.show({factoryCategory}, "Factory One");
            focusedOnOpen = browser.hasKeyboardFocus(false);
        };

        walkthrough.start();
        walkthrough.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        walkthrough.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        focusedAfterStepTransition = browser.hasKeyboardFocus(false);
        walkthrough.toFront(false);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (walkthrough.isVisible() && walkthrough.keyPressed(key))
            return true;
        if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'Z')
        {
            ++undoCommands;
            return true;
        }
        return false;
    }

    juce::TextButton opener{"Presets"};
    PresetBrowserOverlay browser;
    InteractiveWalkthrough walkthrough;
    juce::String loadedPreset;
    bool focusedOnOpen = false;
    bool focusedAfterStepTransition = false;
    int undoCommands = 0;
};
}  // namespace

juce::String runPresetBrowserKeyboardRoutingTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);

    PresetBrowserKeyboardHost host;
    host.addToDesktop(juce::ComponentPeer::windowIsTemporary);
    host.setVisible(true);
    host.toFront(true);
    host.open();

    auto* peer = host.getPeer();
    const bool focused = host.focusedOnOpen;
    const bool printableHandled =
        peer != nullptr && peer->handleKeyPress(juce::KeyPress('f', juce::ModifierKeys(), 'f'));
    const auto filterBeforeNavigation = host.browser.getWalkthroughFilterText();
    const bool downHandled = peer != nullptr && peer->handleKeyPress(juce::KeyPress(juce::KeyPress::downKey));
    const auto filterBeforeSpace = host.browser.getWalkthroughFilterText();
    const bool undoHandled =
        peer != nullptr && peer->handleKeyPress(juce::KeyPress('Z', juce::ModifierKeys::commandModifier, 'z'));
    const bool saveHandled =
        peer != nullptr && peer->handleKeyPress(juce::KeyPress('S', juce::ModifierKeys::commandModifier, 's'));
    const auto filterAfterCommands = host.browser.getWalkthroughFilterText();
    const bool spaceHandled =
        peer != nullptr && peer->handleKeyPress(juce::KeyPress(juce::KeyPress::spaceKey, juce::ModifierKeys(), ' '));
    const auto filterAfterSpace = host.browser.getWalkthroughFilterText();
    const bool browserStillVisible = host.browser.isVisible();
    const bool escapeHandled = peer != nullptr && peer->handleKeyPress(juce::KeyPress(juce::KeyPress::escapeKey));
    const bool rightHandled = peer != nullptr && peer->handleKeyPress(juce::KeyPress(juce::KeyPress::rightKey));

    // Search the authored catalog through the same public key path as auditioning.
    juce::StringArray catalogLoads;
    std::vector<PresetBrowserOverlay::Category> catalogModel(FactoryPresetBrowseGroups::kGroupLabels.size());
    for (size_t i = 0; i < catalogModel.size(); ++i)
        catalogModel[i].label = FactoryPresetBrowseGroups::kGroupLabels[i];
    int assignedPresets = 0;
    for (const auto& category : FactoryPresetLibrary::kFactoryPresetCategories)
    {
        for (const auto& preset : FactoryPresetLibrary::kFactoryPresets)
            if (std::strcmp(preset.category, category.id) == 0)
            {
                const int group = FactoryPresetBrowseGroups::groupIndexFor(preset.name);
                if (group < 0 || group >= static_cast<int>(catalogModel.size()))
                    continue;
                const juce::String name(preset.name);
                catalogModel[static_cast<size_t>(group)].items.push_back(
                    {name, FactoryPresetDescriptions::find(preset.name), [&, name] { catalogLoads.add(name); }, false,
                     juce::String(category.label) + " " + FactoryPresetDescriptions::findSearchTerms(preset.name)});
                ++assignedPresets;
            }
    }
    catalogModel.push_back({"Init", {{"Init", "starting state", [&] { catalogLoads.add("Init"); }, false, {}}}, true});
    catalogModel.push_back({"User", {}});
    PresetBrowserOverlay catalog;
    host.addChildComponent(catalog);
    catalog.setBounds(host.getLocalBounds());
    catalog.show(catalogModel, {});
    juce::String failedQuery;
    const int factoryPresetCount = static_cast<int>(FactoryPresetLibrary::kFactoryPresets.size());
    const auto expectQuery = [&](const juce::String& query, int expectedCount, const juce::String& requiredName)
    {
        catalogLoads.clear();
        bool ok = true;
        if (catalog.getWalkthroughFilterText().isNotEmpty())
            ok &= catalog.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
        for (int i = 0; i < query.length(); ++i)
            ok &= catalog.keyPressed(juce::KeyPress(static_cast<int>(query[i]), {}, query[i]));
        ok &= catalog.isVisible() && catalog.getWalkthroughFilterText() == query && catalogLoads.isEmpty();
        for (size_t i = 0; i <= FactoryPresetLibrary::kFactoryPresets.size(); ++i)
            ok &= catalog.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        juce::StringArray matches;
        for (const auto& name : catalogLoads)
            matches.addIfNotAlreadyThere(name);
        ok &= matches.size() == expectedCount && (requiredName.isEmpty() || matches.contains(requiredName));
        if (!ok && failedQuery.isEmpty())
            failedQuery = "query='" + query + "' matches=" + juce::String(matches.size());
        return ok;
    };
    bool catalogSearchOk = assignedPresets == static_cast<int>(FactoryPresetLibrary::kFactoryPresets.size());
    catalogSearchOk &= expectQuery("FoReSt", 1, "Forest Afterglow");
    catalogSearchOk &= expectQuery("lingering", 1, "Forest Afterglow");
    catalogSearchOk &= expectQuery("woodland", 1, "Forest Afterglow");
    catalogSearchOk &= expectQuery("orbits", 5, "Beat Orbit");
    catalogSearchOk &= expectQuery("showcase", 12, "Point Orbit");
    catalogSearchOk &= expectQuery("does-not-exist", 0, {});
    catalogSearchOk &= expectQuery({}, factoryPresetCount, "Sky Figure Eight");

    // Break caught: choosing a browse group scrolls the whole bank, or searching
    // silently escapes the selected group and changes the loaded sound.
    catalogLoads.clear();
    catalog.show(catalogModel, "Point Orbit");
    const auto findCatalogControl = [&](const juce::String& title) -> juce::Component*
    { return findChildWithTitle(catalog, title); };
    const auto pressCatalogControl = [&](const juce::String& title)
    {
        auto* control = findCatalogControl(title);
        auto* handler = control != nullptr ? control->getAccessibilityHandler() : nullptr;
        return handler != nullptr && handler->getActions().invoke(juce::AccessibilityActionType::press);
    };
    const auto catalogTable = [&]() -> const juce::AccessibilityTableInterface*
    {
        auto* list = findCatalogControl("Presets");
        auto* handler = list != nullptr ? list->getAccessibilityHandler() : nullptr;
        return handler != nullptr ? handler->getTableInterface() : nullptr;
    };
    const auto selectedCatalogControl = [&](const juce::String& title)
    {
        auto* control = findCatalogControl(title);
        auto* handler = control != nullptr ? control->getAccessibilityHandler() : nullptr;
        if (handler == nullptr)
            return false;
        const auto state = handler->getCurrentState();
        return state.isSelected() || state.isChecked();
    };
    const auto catalogRowCount = [&]()
    {
        const auto* table = catalogTable();
        return table != nullptr ? table->getNumRows() : -1;
    };
    const auto setCatalogQuery = [&](const juce::String& query)
    {
        auto* field = dynamic_cast<juce::TextEditor*>(findCatalogControl("Filter presets"));
        if (field == nullptr)
            return false;
        field->setText(query, false);
        field->onTextChange();
        return catalog.getWalkthroughFilterText() == query;
    };
    int browseControlCount = 0;
    for (auto* child : catalog.getChildren())
        if (auto* handler = child->getAccessibilityHandler();
            handler != nullptr && handler->getRole() == juce::AccessibilityRole::radioButton)
            ++browseControlCount;
    bool catalogFiltersOk = browseControlCount == 6 && findCatalogControl("Spatial Showcase") == nullptr &&
                            catalogRowCount() == factoryPresetCount && selectedCatalogControl("All Presets") &&
                            selectedCatalogControl("Point Orbit");
    juce::String failedFilter;
    const auto recordFilterFailure = [&](const char* phase)
    {
        if (!catalogFiltersOk && failedFilter.isEmpty())
            failedFilter = phase;
    };
    recordFilterFailure("initial selection");
    for (size_t group = 0; group < FactoryPresetBrowseGroups::kGroupLabels.size(); ++group)
    {
        const juce::String label(FactoryPresetBrowseGroups::kGroupLabels[group]);
        catalogFiltersOk &= pressCatalogControl(label) && selectedCatalogControl(label) &&
                            !selectedCatalogControl("All Presets") &&
                            catalogRowCount() == static_cast<int>(catalogModel[group].items.size());
        if (const auto* table = catalogTable())
            for (int row = 0; row < table->getNumRows(); ++row)
            {
                const auto* handler = table->getRowHandler(row);
                catalogFiltersOk &=
                    handler != nullptr && FactoryPresetBrowseGroups::groupIndexFor(handler->getTitle().toRawUTF8()) ==
                                              static_cast<int>(group);
            }
        else
            catalogFiltersOk = false;
    }
    recordFilterFailure("group membership or native state");
    catalogFiltersOk &= pressCatalogControl("Echo & Rhythm") && findCatalogControl("Point Orbit") == nullptr &&
                        setCatalogQuery("woodland") && catalogRowCount() == 0;
    catalogFiltersOk &= pressCatalogControl("Reverb & Atmosphere") &&
                        catalog.getWalkthroughFilterText() == "woodland" && catalogRowCount() == 1 &&
                        findCatalogControl("Forest Afterglow") != nullptr;
    recordFilterFailure("group/query intersection");
    catalogFiltersOk &=
        setCatalogQuery({}) && pressCatalogControl("Spatial & Motion") && selectedCatalogControl("Spatial & Motion") &&
        catalogRowCount() == 24 && findCatalogControl("Point Orbit") != nullptr &&
        findCatalogControl("Sky Figure Eight") != nullptr && setCatalogQuery("showcase") && catalogRowCount() == 12;
    if (const auto* table = catalogTable())
        for (int row = 0; row < table->getNumRows(); ++row)
        {
            const auto* handler = table->getRowHandler(row);
            catalogFiltersOk &=
                handler != nullptr && FactoryPresetBrowseGroups::groupIndexFor(handler->getTitle().toRawUTF8()) ==
                                          static_cast<int>(FactoryPresetBrowseGroups::Group::spatialAndMotion);
        }
    else
        catalogFiltersOk = false;
    recordFilterFailure("focused spatial presets remain in Spatial & Motion");
    catalogFiltersOk &=
        setCatalogQuery("approach") && catalogRowCount() == 1 && findCatalogControl("Approach Retreat") != nullptr;
    recordFilterFailure("spatial/query intersection");
    catalogFiltersOk &= pressCatalogControl("User") && selectedCatalogControl("User") && catalogRowCount() == 0 &&
                        setCatalogQuery({}) && catalogRowCount() == 0;
    recordFilterFailure("empty User shortcut");
    catalogFiltersOk &= pressCatalogControl("All Presets") && catalogRowCount() == factoryPresetCount &&
                        selectedCatalogControl("Point Orbit") && catalogLoads.isEmpty();
    recordFilterFailure("loaded row restoration without load");
    // The description strip sits between the list and the actions, inside the
    // panel, even after revealing a loaded row near the bank's end.
    catalog.revealWalkthroughLoadedPreset();
    const auto descriptionTarget = catalog.getWalkthroughIntentScreenBounds();
    catalogFiltersOk &= !descriptionTarget.isEmpty() &&
                        catalog.getWalkthroughPanelScreenBounds().contains(descriptionTarget) &&
                        !catalog.getWalkthroughPresetListScreenBounds().intersects(descriptionTarget) &&
                        descriptionTarget.getBottom() <= catalog.getWalkthroughActionScreenBounds().getY();
    if (const auto* table = catalogTable())
        for (int row = 0; row < table->getNumRows(); ++row)
        {
            const auto* handler = table->getRowHandler(row);
            catalogFiltersOk &= handler != nullptr && handler->getTitle() != "Init";
        }
    recordFilterFailure("description target or Init row exclusion");

    // User rows remain separate from the factory bank; clearing a query never
    // exposes factory sounds while the User shortcut is active.
    catalogModel.back().items.push_back({"My Space", "saved sound", [&] { catalogLoads.add("My Space"); }, true, {}});
    catalog.show(catalogModel, "Point Orbit");
    catalogFiltersOk &= pressCatalogControl("User") && catalogRowCount() == 1 &&
                        findCatalogControl("My Space") != nullptr && findCatalogControl("Point Orbit") == nullptr &&
                        setCatalogQuery("Point Orbit") && catalogRowCount() == 0 && setCatalogQuery({}) &&
                        catalogRowCount() == 1 && catalogLoads.isEmpty();
    recordFilterFailure("populated User/query intersection");
    catalogFiltersOk &= pressCatalogControl("INIT") && catalog.isShowing() && catalogLoads.size() == 1 &&
                        catalogLoads[0] == "Init" && pressCatalogControl("All Presets") && catalogRowCount() == 63;
    if (const auto* table = catalogTable())
        for (int row = 0; row < table->getNumRows(); ++row)
        {
            const auto* handler = table->getRowHandler(row);
            catalogFiltersOk &=
                handler != nullptr && handler->getTitle() != "Init" && !handler->getCurrentState().isSelected();
        }
    recordFilterFailure("Init action and selection");
    catalogFiltersOk &= pressCatalogControl("Close preset browser") && !catalog.isShowing();
    recordFilterFailure("close action");
    if (catalog.isShowing())
        catalog.dismiss();

    // Break caught: long saved names overlap the list, the description strip
    // cannot hold a long description, or adding MODIFIED during a repaint moves
    // the footer away from its native buttons or resizes the list.
    bool browserDirty = false;
    PresetBrowserOverlay::Callbacks layoutCallbacks;
    layoutCallbacks.isDirty = [&] { return browserDirty; };
    catalog.setCallbacks(std::move(layoutCallbacks));
    bool wrappedLayoutOk = true;
    juce::String failedLayout;
    const juce::String namePattern = juce::String::repeatedString("SpatialOrbit ", 12);
    const juce::String wrappedDescription = "A saved sound with a clear movement from the front to either side, "
                                            "followed by a slow return behind the listener. The changing position "
                                            "stays distinct while the echoes leave room for the original sound.";
    const auto editorBrowserPlacement = [](juce::Rectangle<int> editorBounds, bool scopeOpen)
    {
        editorBounds.removeFromTop(scopeOpen ? UIConstants::headerHeight : UIConstants::headerControlStripHeight);
        auto content = editorBounds.reduced(UIConstants::padding);
        return content.removeFromLeft((content.getWidth() - UIConstants::panelGap) / 2);
    };
    for (const auto& editorSize : {juce::Point<int>(1400, 650), juce::Point<int>(1800, 837)})
    {
        host.setSize(editorSize.x, editorSize.y);
        catalog.setBounds(host.getLocalBounds());
        // Match the editor's content split with the scope strip closed.
        catalog.setPanelPlacementArea(editorBrowserPlacement(host.getLocalBounds(), false));
        for (int length : {72, 84, 96, 108, 120})
        {
            const auto longName = namePattern.substring(0, length);
            const std::vector<PresetBrowserOverlay::Category> layoutModel{
                {"Init", {{"Init", {}, [] {}, false, {}}}, true},
                {"User", {{"Short", "saved sound", [] {}, true, {}}, {longName, wrappedDescription, [] {}, true, {}}}}};
            browserDirty = false;
            // The strip is sized for the catalog's longest description: taller
            // than a short-only catalog needs, and the same height whichever
            // preset is loaded so hovering and loading never resize the list.
            auto shortOnlyModel = layoutModel;
            shortOnlyModel.back().items.pop_back();
            catalog.show(shortOnlyModel, "Short");
            const int shortDescriptionHeight = catalog.getWalkthroughIntentScreenBounds().getHeight();
            catalog.show(layoutModel, "Short");
            auto* shortRow = findCatalogControl("Short");
            const int shortRowHeight = shortRow != nullptr ? shortRow->getHeight() : 0;
            const int stripWithShortLoaded = catalog.getWalkthroughIntentScreenBounds().getHeight();
            catalog.show(layoutModel, longName);
            catalog.revealWalkthroughLoadedPreset();
            auto* longRow = findCatalogControl(longName);
            auto* initButton = findCatalogControl("INIT");
            const auto description = catalog.getWalkthroughIntentScreenBounds();
            const auto listBeforeDirty = catalog.getWalkthroughPresetListScreenBounds();
            const auto footerBeforeDirty = catalog.getWalkthroughActionScreenBounds();
            const auto initBeforeDirty = initButton != nullptr ? initButton->getScreenBounds() : juce::Rectangle<int>();
            wrappedLayoutOk &=
                longRow != nullptr && initButton != nullptr && shortRowHeight > 0 &&
                longRow->getHeight() > shortRowHeight && longRow->getDescription() == wrappedDescription &&
                description.getHeight() > shortDescriptionHeight && description.getHeight() == stripWithShortLoaded &&
                listBeforeDirty.contains(longRow->getScreenBounds()) && !listBeforeDirty.intersects(description) &&
                description.getBottom() <= footerBeforeDirty.getY();
            for (bool dirty : {true, false})
            {
                browserDirty = dirty;
                catalog.repaint();
                juce::Image frame(juce::Image::ARGB, editorSize.x, editorSize.y, true);
                juce::Graphics graphics(frame);
                catalog.paint(graphics);
                wrappedLayoutOk &= catalog.getWalkthroughPresetListScreenBounds() == listBeforeDirty &&
                                   catalog.getWalkthroughActionScreenBounds() == footerBeforeDirty &&
                                   initButton != nullptr && initButton->getScreenBounds() == initBeforeDirty &&
                                   catalog.getWalkthroughIntentScreenBounds() == description;
            }
            if (!wrappedLayoutOk && failedLayout.isEmpty())
                failedLayout = juce::String(editorSize.x) + "x" + juce::String(editorSize.y) +
                               " nameLength=" + juce::String(length);
        }
    }

    // Break caught: the complete sidebar's User shortcut enters the footer
    // when a wrapped loaded name and the open scope reduce the browser body.
    bool sidebarLayoutOk = true;
    juce::String failedSidebarLayout;
    juce::StringArray navigationLabels{"All Presets"};
    for (const auto* label : FactoryPresetBrowseGroups::kGroupLabels)
        navigationLabels.add(label);
    navigationLabels.add("User");
    auto scopeOpenModel = catalogModel;
    host.setSize(1400, 650);
    catalog.setBounds(host.getLocalBounds());
    catalog.setPanelPlacementArea(editorBrowserPlacement(host.getLocalBounds(), true));
    catalogLoads.clear();
    for (int length : {72, 84, 96, 108, 120})
    {
        const auto longName = namePattern.substring(0, length);
        scopeOpenModel.back().items = {
            {"Short", "saved sound", [&] { catalogLoads.add("Short"); }, true, {}},
            {longName, wrappedDescription, [&, longName] { catalogLoads.add(longName); }, true, {}}};
        browserDirty = false;
        catalog.show(scopeOpenModel, longName);
        const auto categoryBeforeDirty = catalog.getWalkthroughCategoryScreenBounds();
        const auto listBeforeDirty = catalog.getWalkthroughPresetListScreenBounds();
        const auto footerBeforeDirty = catalog.getWalkthroughActionScreenBounds();
        auto* initButton = findCatalogControl("INIT");
        const auto initBeforeDirty = initButton != nullptr ? initButton->getScreenBounds() : juce::Rectangle<int>();
        const auto expectSidebar = [&](bool condition, const char* phase)
        {
            sidebarLayoutOk &= condition;
            if (!condition && failedSidebarLayout.isEmpty())
                failedSidebarLayout = juce::String(phase) + " 1400x650 scope=open nameLength=" + juce::String(length) +
                                      " categoryHeight=" + juce::String(categoryBeforeDirty.getHeight());
        };
        expectSidebar(navigationLabels.size() == 6 && initButton != nullptr && selectedCatalogControl("All Presets") &&
                          catalogRowCount() == 64,
                      "full catalog setup");
        std::vector<juce::Rectangle<int>> navigationBeforeDirty;
        for (const auto& label : navigationLabels)
        {
            auto* control = findCatalogControl(label);
            const auto bounds = control != nullptr ? control->getScreenBounds() : juce::Rectangle<int>();
            navigationBeforeDirty.push_back(bounds);
            expectSidebar(control != nullptr && !bounds.isEmpty() && categoryBeforeDirty.contains(bounds) &&
                              !footerBeforeDirty.intersects(bounds),
                          "navigation bounds");
        }

        if (auto* userControl = findCatalogControl("User"))
        {
            const auto point = catalog.getLocalPoint(userControl, userControl->getLocalBounds().getCentre()).toFloat();
            const auto now = juce::Time::getCurrentTime();
            catalog.mouseDown(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), point,
                                               juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f,
                                               0.0f, 0.0f, 0.0f, &catalog, &catalog, now, point, now, 1, false));
        }
        expectSidebar(catalog.isShowing() && selectedCatalogControl("User") && catalogRowCount() == 2 &&
                          findCatalogControl(longName) != nullptr && findCatalogControl("Point Orbit") == nullptr &&
                          catalogLoads.isEmpty(),
                      "User midpoint mouse selection");
        expectSidebar(pressCatalogControl("All Presets") && catalogRowCount() == 64 &&
                          !selectedCatalogControl("User") && pressCatalogControl("User") &&
                          selectedCatalogControl("User") && catalogRowCount() == 2 &&
                          selectedCatalogControl(longName) && catalogLoads.isEmpty(),
                      "User native selection");

        for (bool dirty : {true, false})
        {
            browserDirty = dirty;
            catalog.repaint();
            juce::Image frame(juce::Image::ARGB, host.getWidth(), host.getHeight(), true);
            juce::Graphics graphics(frame);
            catalog.paint(graphics);
            expectSidebar(catalog.getWalkthroughCategoryScreenBounds() == categoryBeforeDirty &&
                              catalog.getWalkthroughPresetListScreenBounds() == listBeforeDirty &&
                              catalog.getWalkthroughActionScreenBounds() == footerBeforeDirty &&
                              initButton != nullptr && initButton->getScreenBounds() == initBeforeDirty,
                          "dirty repaint geometry");
            for (int index = 0; index < navigationLabels.size(); ++index)
            {
                auto* control = findCatalogControl(navigationLabels[index]);
                expectSidebar(control != nullptr &&
                                  control->getScreenBounds() == navigationBeforeDirty[static_cast<size_t>(index)],
                              "dirty repaint navigation bounds");
            }
        }
    }
    catalog.dismiss();

    const bool passed = peer != nullptr && catalogSearchOk && catalogFiltersOk && wrappedLayoutOk && sidebarLayoutOk &&
                        focused && host.focusedAfterStepTransition && printableHandled &&
                        filterBeforeNavigation == "f" && downHandled && host.loadedPreset == "Factory Two" &&
                        filterBeforeSpace == "f" && undoHandled && host.undoCommands == 1 && !saveHandled &&
                        filterAfterCommands == filterBeforeSpace && !spaceHandled &&
                        filterAfterSpace == filterBeforeSpace && browserStillVisible && escapeHandled &&
                        host.browser.getWalkthroughFilterText().isEmpty() && rightHandled;

    juce::String result;
    result << "PresetBrowserKeyboardRouting:"
           << " peer=" << (peer != nullptr ? "ready" : "missing") << " focus=" << (focused ? "browser" : "elsewhere")
           << " stepFocus=" << (host.focusedAfterStepTransition ? "browser" : "elsewhere")
           << " printable=" << (printableHandled ? "handled" : "ignored") << " filter='" << filterBeforeNavigation
           << "'"
           << " down=" << (downHandled ? "handled" : "ignored") << " loaded='" << host.loadedPreset << "'"
           << " undo=" << host.undoCommands << " save=" << (saveHandled ? "handled" : "forwarded")
           << " commandFilterUnchanged=" << (filterAfterCommands == filterBeforeSpace ? "yes" : "no")
           << " space=" << (spaceHandled ? "handled" : "forwarded")
           << " unchanged=" << (filterAfterSpace == filterBeforeSpace ? "yes" : "no")
           << " visible=" << (browserStillVisible ? "yes" : "no") << " nav=" << (rightHandled ? "handled" : "ignored")
           << " catalogSearch=" << (catalogSearchOk ? "ok" : failedQuery)
           << " browseFilters=" << (catalogFiltersOk ? "ok" : failedFilter)
           << " wrappedLayout=" << (wrappedLayoutOk ? "ok" : failedLayout)
           << " sidebarLayout=" << (sidebarLayoutOk ? "ok" : failedSidebarLayout) << " "
           << (passed ? "[PASS]" : "[FAIL]");
    host.removeFromDesktop();
    return result;
}

juce::String runInteractiveWalkthroughTest(SpatialEchoAudioProcessor& processor)
{
    int failures = 0;
    juce::String firstFailure;
    juce::String libraryPumpDiagnostics;
    auto expect = [&](bool condition, const char* message)
    {
        if (condition)
            return;
        ++failures;
        if (firstFailure.isEmpty())
            firstFailure = message;
    };

    // Break caught: two plug-in instances in one host both claim the automatic tour.
    WalkthroughLaunchGate gate;
    expect(gate.tryClaim(false), "first unseen instance did not claim auto-launch");
    expect(!gate.tryClaim(false), "second instance claimed the same auto-launch");
    WalkthroughLaunchGate alreadySeenGate;
    expect(!alreadySeenGate.tryClaim(true), "seen preference still auto-launched");

    const auto defaultSettingsPath = EditorPreferences::getDefaultSettingsFile().getFullPathName();
    expect(defaultSettingsPath.contains("Echoes Lab") && defaultSettingsPath.contains("Spatial Echo"),
           "default walkthrough preference escaped the product support folder");

    // Break caught: suppression is written outside APVTS and does not survive a reopen.
    ScopedTestDirectory temp;
    expect(temp.getFile().isDirectory(), "temporary preference directory was not created");
    const auto settingsFile = temp.getFile().getChildFile("walkthrough.settings");
    {
        EditorPreferences preferences(settingsFile);
        expect(!preferences.hasSeenWalkthrough(), "fresh temporary preference was already seen");
        expect(preferences.markWalkthroughSeen(), "seen preference failed to save");
    }
    {
        EditorPreferences preferences(settingsFile);
        expect(preferences.hasSeenWalkthrough(), "saved seen preference was not reloaded");
    }

    // Break caught: a write failure traps or crashes the dismissal path.
    const auto blockedParent = temp.getFile().getChildFile("not-a-directory");
    blockedParent.replaceWithText("file blocks child creation");
    EditorPreferences unwritable(blockedParent.getChildFile("walkthrough.settings"));
    expect(!unwritable.markWalkthroughSeen(), "invalid settings path reported a successful save");

    SpatialHeadView headView;
    juce::Component walkthroughOverlay;
    headView.setOverlayReferences(nullptr, &walkthroughOverlay);
    walkthroughOverlay.setVisible(true);
    expect(headView.isSoftwareFallbackForcedForTest(),
           "active walkthrough did not force the spatial software fallback");
    walkthroughOverlay.setVisible(false);
    expect(!headView.isSoftwareFallbackForcedForTest(),
           "closed walkthrough did not release the spatial software fallback");
    headView.setOverlayReferences(nullptr, nullptr);

    juce::Component host;
    juce::TextButton spotlightControl{"Target"};
    juce::TextButton dimmedControl{"Dimmed"};
    InteractiveWalkthrough walkthrough;
    const juce::Rectangle<int> targetBounds(110, 120, 220, 90);
    const juce::Rectangle<int> popupBounds(380, 90, 520, 410);
    bool actionComplete = false;
    bool popupOpen = false;
    std::vector<InteractiveWalkthrough::ExpandedTarget> enteredExpandedTargets;
    InteractiveWalkthrough::Dismissal lastDismissal = InteractiveWalkthrough::Dismissal::notNow;
    int dismissalCount = 0;

    host.setBounds(0, 0, 1400, 650);
    spotlightControl.setBounds(targetBounds);
    dimmedControl.setBounds(10, 10, 60, 30);
    host.addAndMakeVisible(spotlightControl);
    host.addAndMakeVisible(dimmedControl);
    host.addAndMakeVisible(walkthrough);

    walkthrough.setTargetResolver(
        [&](InteractiveWalkthrough::Target target, InteractiveWalkthrough::ExpandedTarget,
            InteractiveWalkthrough::SpotlightDetail) -> std::optional<juce::Rectangle<int>>
        {
            if (target == InteractiveWalkthrough::Target::none)
                return std::nullopt;
            return popupOpen ? popupBounds : targetBounds;
        });
    walkthrough.setCompletionResolver([&](InteractiveWalkthrough::CompletionSignal) { return actionComplete; });
    walkthrough.onDismissed = [&](InteractiveWalkthrough::Dismissal dismissal)
    {
        lastDismissal = dismissal;
        ++dismissalCount;
    };
    walkthrough.onStepChanged =
        [&](InteractiveWalkthrough::Target, InteractiveWalkthrough::ExpandedTarget expandedTarget,
            InteractiveWalkthrough::SpotlightDetail) { enteredExpandedTargets.push_back(expandedTarget); };

    walkthrough.setBounds(0, 0, 1400, 650);
    walkthrough.start();
    expect(walkthrough.isVisible(), "start did not show walkthrough");
    expect(walkthrough.getCurrentStepIndexForTesting() == 0, "start did not open welcome");

    // Break caught: the dimming layer makes the surrounding plug-in too dark
    // to understand where the highlighted control sits.
    juce::Image dimmingImage(juce::Image::ARGB, 1400, 650, true);
    juce::Graphics dimmingGraphics(dimmingImage);
    walkthrough.paint(dimmingGraphics);
    expect(dimmingImage.getPixelAt(5, 5).getAlpha() <= 72,
           "walkthrough dimming obscured the surrounding plug-in context");

    auto* nextButton = dynamic_cast<juce::Button*>(walkthrough.findChildWithID("walkthroughNext"));
    auto* backButton = dynamic_cast<juce::Button*>(walkthrough.findChildWithID("walkthroughBack"));
    auto* exitButton = dynamic_cast<juce::Button*>(walkthrough.findChildWithID("walkthroughExit"));
    auto* suppressButton = dynamic_cast<juce::Button*>(walkthrough.findChildWithID("walkthroughSuppress"));
    expect(nextButton != nullptr && backButton != nullptr && exitButton != nullptr && suppressButton != nullptr,
           "coach-card buttons are not discoverable");
    expect(nextButton != nullptr && nextButton->getTitle().isNotEmpty() && backButton != nullptr &&
               backButton->getTitle().isNotEmpty() && exitButton != nullptr && exitButton->getTitle().isNotEmpty() &&
               suppressButton != nullptr && suppressButton->getTitle().isNotEmpty(),
           "coach-card controls are missing accessibility labels");
    expect(suppressButton != nullptr && suppressButton->isVisible(), "welcome did not expose persistent suppression");
    expect(nextButton != nullptr && nextButton->getTitle() == "Start tour" && exitButton != nullptr &&
               exitButton->getTitle() == "Not now",
           "welcome actions did not expose labels matching their visible purpose");
    if (nextButton != nullptr && exitButton != nullptr && suppressButton != nullptr)
    {
        const auto welcomeCard = walkthrough.getCardBoundsForTesting();
        expect(nextButton->getBounds().getCentreX() == welcomeCard.getCentreX(),
               "welcome primary action was not centred in the coach card");
        expect(exitButton->getBounds().getCentreX() == welcomeCard.getCentreX() &&
                   nextButton->getBounds().getBottom() < exitButton->getBounds().getY(),
               "welcome temporary dismissal was not centred beneath the primary action");
        expect(suppressButton->getBounds().getRight() < welcomeCard.getCentreX() &&
                   welcomeCard.getBottom() - suppressButton->getBounds().getBottom() <= 18,
               "persistent suppression did not occupy the lower-left corner");
        expect(suppressButton->getHeight() < exitButton->getHeight(),
               "persistent suppression did not use a smaller tertiary target");
        expect(exitButton->findColour(juce::TextButton::buttonColourId).isTransparent() &&
                   suppressButton->findColour(juce::TextButton::buttonColourId).isTransparent(),
               "welcome dismissal actions were styled as competing primary buttons");
        auto* exitTextButton = dynamic_cast<juce::TextButton*>(exitButton);
        auto* suppressTextButton = dynamic_cast<juce::TextButton*>(suppressButton);
        if (exitTextButton != nullptr && suppressTextButton != nullptr)
        {
            const auto exitFont =
                exitTextButton->getLookAndFeel().getTextButtonFont(*exitTextButton, exitTextButton->getHeight());
            const auto suppressFont = suppressTextButton->getLookAndFeel().getTextButtonFont(
                *suppressTextButton, suppressTextButton->getHeight());
            expect(suppressFont.getHeight() < exitFont.getHeight(),
                   "persistent suppression text was not visually de-emphasised");
        }
    }

    expect(walkthrough.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)),
           "Return did not activate the forward tour action");
    expect(walkthrough.getCurrentStepIndexForTesting() == 1, "start button did not enter first lesson");
    expect(walkthrough.getCurrentExpandedTargetForTesting() == InteractiveWalkthrough::ExpandedTarget::none,
           "first preset lesson opened the browser before introducing the preset control");
    expect(walkthrough.getDescription().containsIgnoreCase("preset name"),
           "first preset lesson did not introduce the closed preset control");
    expect(enteredExpandedTargets.size() == 1 &&
               enteredExpandedTargets.back() == InteractiveWalkthrough::ExpandedTarget::none,
           "Start Tour did not enter the closed preset-control lesson");
    expect(nextButton != nullptr && nextButton->getTitle() == "Next step" && exitButton != nullptr &&
               exitButton->getTitle() == "Exit tour",
           "lesson actions did not expose labels matching their visible purpose");
    expect(suppressButton != nullptr && !suppressButton->isVisible(),
           "persistent suppression remained visible inside the tour");
    expect(walkthrough.getCurrentTargetForTesting() == InteractiveWalkthrough::Target::presets &&
               walkthrough.getDescription().containsIgnoreCase("preset"),
           "current lesson was not exposed to accessibility clients");

    // Break caught: the chapter cue and destination lesson are alpha-blended
    // into the same card frame, making both text layouts overlap.
    const auto chapterFrame = renderWalkthroughCard(walkthrough);
    for (int tick = 0; tick < 20; ++tick)
        walkthrough.refreshActionState();
    const auto chapterMidpointFrame = renderWalkthroughCard(walkthrough);
    expect(imagesMatch(chapterFrame, chapterMidpointFrame),
           "section chapter and lesson content rendered in the same frame");

    expect(walkthrough.isKeyboardFocusContainer(), "walkthrough is not a keyboard focus container");
    expect(walkthrough.isFocusAllowedForTesting(&spotlightControl),
           "spotlight control was excluded from keyboard focus");
    expect(!walkthrough.isFocusAllowedForTesting(&dimmedControl),
           "dimmed control remained in the walkthrough focus scope");

    juce::Component fullEditorPresetBrowser;
    fullEditorPresetBrowser.setBounds(host.getLocalBounds());
    host.addAndMakeVisible(fullEditorPresetBrowser);
    walkthrough.setSpotlightFocusComponent(&fullEditorPresetBrowser);
    expect(walkthrough.isFocusAllowedForTesting(&fullEditorPresetBrowser),
           "active preset browser could not receive filter keyboard focus");
    walkthrough.setSpotlightFocusComponent(nullptr);

    juce::Component popupRoot{"menu"};
    juce::TextButton popupControl{"Popup item"};
    popupRoot.addAndMakeVisible(popupControl);
    host.addAndMakeVisible(popupRoot);
    popupRoot.enterModalState(false);
    walkthrough.setExternalPopupActive(true);
    walkthrough.attachToCurrentPopupMenu();
    expect(walkthrough.isFocusAllowedForTesting(&popupControl),
           "active popup control did not receive temporary keyboard focus access");
    expect(!walkthrough.isFocusAllowedForTesting(&dimmedControl),
           "opening a popup exposed dimmed editor controls to keyboard focus");
    const int stepBeforePopupReturn = walkthrough.getCurrentStepIndexForTesting();
    expect(walkthrough.handlePopupKeyForTesting(juce::KeyPress(juce::KeyPress::returnKey)),
           "Return was swallowed by an automatically opened standard menu");
    expect(walkthrough.getCurrentStepIndexForTesting() == stepBeforePopupReturn + 1,
           "Return did not advance the coach card while a standard menu was open");
    if (backButton != nullptr)
        backButton->onClick();
    walkthrough.detachFromModalComponent();
    walkthrough.setExternalPopupActive(false);
    popupRoot.exitModalState(0);
    expect(!walkthrough.hitTest(targetBounds.getCentreX(), targetBounds.getCentreY()),
           "spotlight blocked the highlighted control");
    expect(walkthrough.hitTest(targetBounds.getX() - 4, targetBounds.getCentreY()),
           "visual spotlight padding exposed an unrelated neighbouring control");
    expect(walkthrough.hitTest(20, 20), "dimmed background allowed stray control input");

    const auto cardAtMinimum = walkthrough.getCardBoundsForTesting();
    expect(walkthrough.getLocalBounds().contains(cardAtMinimum), "coach card escaped minimum editor bounds");
    expect(cardAtMinimum.getWidth() == 420 && cardAtMinimum.getHeight() == 270,
           "coach card lost its intended size at the minimum editor bounds");
    expect(walkthrough.hitTest(cardAtMinimum.getCentreX(), cardAtMinimum.getCentreY()),
           "coach card did not capture its own input");

    SpatialEchoDropdownWindow::Host walkthroughDropdownHost;
    walkthroughDropdownHost.setModalEventPassthrough(&walkthrough);
    expect(nextButton != nullptr && walkthroughDropdownHost.canModalEventBeSentToComponent(nextButton),
           "an automatically opened drawer blocked the coach-card arrows");
    expect(!walkthroughDropdownHost.canModalEventBeSentToComponent(&dimmedControl),
           "drawer modal passthrough exposed an unrelated editor control");
    host.addAndMakeVisible(walkthroughDropdownHost);
    juce::TextButton walkthroughDrawerControl{"Drawer control"};
    walkthroughDropdownHost.addAndMakeVisible(walkthroughDrawerControl);
    walkthroughDrawerControl.setWantsKeyboardFocus(true);
    walkthroughDropdownHost.enterModalState(false);
    walkthrough.attachToCurrentModalComponent();
    expect(walkthrough.getAttachedModalComponentForTesting() == &walkthroughDropdownHost,
           "custom modal root did not delegate keyboard tour navigation");
    const int stepBeforeDrawerReturn = walkthrough.getCurrentStepIndexForTesting();
    expect(!walkthrough.handlePopupKeyForTesting(juce::KeyPress(juce::KeyPress::returnKey)),
           "Return on a live drawer control advanced the coach card");
    expect(walkthrough.getCurrentStepIndexForTesting() == stepBeforeDrawerReturn,
           "live drawer control changed the walkthrough step");
    expect(walkthrough.handlePopupKeyForTesting(juce::KeyPress(juce::KeyPress::tabKey)),
           "Tab in a custom drawer did not enter coach-card navigation");
    walkthrough.detachFromModalComponent();
    walkthroughDropdownHost.exitModalState(0);

    if (nextButton != nullptr)
    {
        nextButton->onClick();

        auto* repaintRecorder = new RepaintRecorder(walkthrough);
        walkthrough.setCachedComponentImage(repaintRecorder);
        repaintRecorder->clear();

        nextButton->onClick();
        expect(repaintRecorder->covers(walkthrough.getCardBoundsForTesting()),
               "same-target Next did not repaint the full coach card");

        if (backButton != nullptr)
        {
            repaintRecorder->clear();
            backButton->onClick();
            expect(repaintRecorder->covers(walkthrough.getCardBoundsForTesting()),
                   "same-target Back did not repaint the full coach card");
            nextButton->onClick();
        }

        walkthrough.setCachedComponentImage(nullptr);
    }
    expect(walkthrough.getCurrentCompletionSignalForTesting() ==
               InteractiveWalkthrough::CompletionSignal::presetAudition,
           "test did not reach an optional preset action");
    expect(!walkthrough.isCurrentActionCompleteForTesting(), "action started complete without interaction");
    actionComplete = true;
    walkthrough.refreshActionState();
    expect(walkthrough.isCurrentActionCompleteForTesting(), "interaction did not complete the suggested action");
    expect(walkthrough.getTitle().containsIgnoreCase("complete") &&
               walkthrough.getDescription().containsIgnoreCase("complete"),
           "completed suggestion was not exposed to accessibility clients");
    expect(walkthrough.getCurrentStepIndexForTesting() == 3, "suggested action auto-advanced the walkthrough");
    actionComplete = false;
    walkthrough.refreshActionState();
    expect(walkthrough.isCurrentActionCompleteForTesting(),
           "completed suggestion was not retained after a popup closed");

    popupOpen = true;
    walkthrough.refreshTarget();
    expect(walkthrough.getSpotlightBoundsForTesting().contains(popupBounds),
           "open popup did not become the spotlight target");
    popupOpen = false;
    walkthrough.refreshTarget();
    expect(walkthrough.getSpotlightBoundsForTesting().contains(targetBounds),
           "closed popup did not fall back to its opener");

    walkthrough.setBounds(0, 0, 1800, 837);
    expect(walkthrough.getLocalBounds().contains(walkthrough.getCardBoundsForTesting()),
           "coach card escaped maximum editor bounds");
    expect(walkthrough.getCardBoundsForTesting().getWidth() == 420 &&
               walkthrough.getCardBoundsForTesting().getHeight() == 270,
           "coach card lost its intended size at the maximum editor bounds");

    // Break caught: a closed popup leaves a click-through hole with no real target.
    walkthrough.setTargetResolver([](InteractiveWalkthrough::Target, InteractiveWalkthrough::ExpandedTarget,
                                     InteractiveWalkthrough::SpotlightDetail) -> std::optional<juce::Rectangle<int>>
                                  { return std::nullopt; });
    walkthrough.refreshTarget();
    expect(walkthrough.getSpotlightBoundsForTesting().isEmpty(), "missing target kept a stale spotlight");
    expect(walkthrough.hitTest(targetBounds.getCentreX(), targetBounds.getCentreY()),
           "missing target left input unblocked");

    walkthrough.start();
    auto* interruptedTransitionRecorder = new RepaintRecorder(walkthrough);
    walkthrough.setCachedComponentImage(interruptedTransitionRecorder);
    if (nextButton != nullptr)
    {
        nextButton->onClick();
        interruptedTransitionRecorder->clear();
        nextButton->onClick();
        expect(interruptedTransitionRecorder->covers(walkthrough.getCardBoundsForTesting()) &&
                   !interruptedTransitionRecorder->covers(walkthrough.getLocalBounds()),
               "same-section Next did not clear the chapter card without repainting the editor");
    }
    if (backButton != nullptr)
    {
        backButton->onClick();
        backButton->onClick();
    }
    walkthrough.setCachedComponentImage(nullptr);
    expect(walkthrough.getCurrentStepIndexForTesting() == 0, "Back did not return to welcome");
    expect(!walkthrough.getSectionTransitionLeadForTesting().contains("SECTION 0"),
           "Back into Welcome displayed an invalid numbered section");
    expect(walkthrough.getSectionTransitionBoundsForTesting() == walkthrough.getCardBoundsForTesting(),
           "Welcome chapter cue escaped the coach card");

    if (exitButton != nullptr)
        exitButton->onClick();
    expect(dismissalCount == 1 && lastDismissal == InteractiveWalkthrough::Dismissal::notNow,
           "Exit did not report Not Now semantics");

    walkthrough.start();
    expect(walkthrough.getCurrentStepIndexForTesting() == 0,
           "manual restart did not ignore prior dismissal and return to Welcome");

    // Break caught: a synced preset disables Rate, so the free-time lesson
    // must lead to Sync first and retarget Rate after the producer turns it off.
    juce::Component delayHost;
    DelayPanel delayPanel(processor.getValueTreeState());
    delayHost.setBounds(0, 0, 700, 430);
    delayHost.addAndMakeVisible(delayPanel);
    delayPanel.setBounds(delayHost.getLocalBounds());
    auto* tempoSync = processor.getValueTreeState().getParameter("TempoSync");
    const auto originalTempoSync = tempoSync != nullptr ? tempoSync->getValue() : 0.0f;
    const auto syncTarget = delayPanel.getWalkthroughScreenBounds(DelayPanel::WalkthroughRegion::sync);
    const auto rateTarget = delayPanel.getWalkthroughScreenBounds(DelayPanel::WalkthroughRegion::rate);
    expect(tempoSync != nullptr, "TempoSync parameter missing from walkthrough test");
    if (tempoSync != nullptr)
    {
        tempoSync->setValueNotifyingHost(1.0f);
        expect(delayPanel.getWalkthroughScreenBounds(DelayPanel::WalkthroughRegion::freeTime) == syncTarget,
               "free-time lesson did not lead a synced preset to the Sync switch");

        tempoSync->setValueNotifyingHost(0.0f);
        expect(delayPanel.getWalkthroughScreenBounds(DelayPanel::WalkthroughRegion::freeTime) == rateTarget,
               "free-time lesson did not retarget Rate after Sync was turned off");
        tempoSync->setValueNotifyingHost(originalTempoSync);
    }

    expect(walkthrough.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)), "Escape did not exit the walkthrough");
    expect(dismissalCount == 2 && lastDismissal == InteractiveWalkthrough::Dismissal::notNow,
           "Escape did not use ordinary Exit semantics");

    walkthrough.start();
    if (suppressButton != nullptr)
        suppressButton->onClick();
    expect(dismissalCount == 3 && lastDismissal == InteractiveWalkthrough::Dismissal::dontShowAgain,
           "Don't Show Again did not report persistent suppression");

    walkthrough.start();
    nextButton = dynamic_cast<juce::Button*>(walkthrough.findChildWithID("walkthroughNext"));
    expect(walkthrough.getStepCountForTesting() == 58,
           "walkthrough did not give Echo, Reverb, Spatial, and Motion their focused producer lessons");
    const std::array expectedTargets{InteractiveWalkthrough::Target::none,
                                     InteractiveWalkthrough::Target::presets,
                                     InteractiveWalkthrough::Target::presets,
                                     InteractiveWalkthrough::Target::presets,
                                     InteractiveWalkthrough::Target::presets,
                                     InteractiveWalkthrough::Target::presets,
                                     InteractiveWalkthrough::Target::presets,
                                     InteractiveWalkthrough::Target::presets,
                                     InteractiveWalkthrough::Target::tape,
                                     InteractiveWalkthrough::Target::tape,
                                     InteractiveWalkthrough::Target::tape,
                                     InteractiveWalkthrough::Target::delay,
                                     InteractiveWalkthrough::Target::delay,
                                     InteractiveWalkthrough::Target::delay,
                                     InteractiveWalkthrough::Target::delay,
                                     InteractiveWalkthrough::Target::delay,
                                     InteractiveWalkthrough::Target::delay,
                                     InteractiveWalkthrough::Target::delay,
                                     InteractiveWalkthrough::Target::delay,
                                     InteractiveWalkthrough::Target::echoEngine,
                                     InteractiveWalkthrough::Target::echoEngine,
                                     InteractiveWalkthrough::Target::echoEngine,
                                     InteractiveWalkthrough::Target::echoEngine,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverb,
                                     InteractiveWalkthrough::Target::reverbRouting,
                                     InteractiveWalkthrough::Target::spatialPosition,
                                     InteractiveWalkthrough::Target::spatialPosition,
                                     InteractiveWalkthrough::Target::spatialPosition,
                                     InteractiveWalkthrough::Target::spatialPosition,
                                     InteractiveWalkthrough::Target::spatialPosition,
                                     InteractiveWalkthrough::Target::spatialPosition,
                                     InteractiveWalkthrough::Target::spatialPosition,
                                     InteractiveWalkthrough::Target::spatialBlend,
                                     InteractiveWalkthrough::Target::spatialBlend,
                                     InteractiveWalkthrough::Target::spatialBlend,
                                     InteractiveWalkthrough::Target::spatialBlend,
                                     InteractiveWalkthrough::Target::spatialBlend,
                                     InteractiveWalkthrough::Target::spatialMotion,
                                     InteractiveWalkthrough::Target::spatialMotion,
                                     InteractiveWalkthrough::Target::spatialMotion,
                                     InteractiveWalkthrough::Target::spatialMotion,
                                     InteractiveWalkthrough::Target::spatialMotion,
                                     InteractiveWalkthrough::Target::spatialMotion,
                                     InteractiveWalkthrough::Target::spatialMotion,
                                     InteractiveWalkthrough::Target::mixOutput,
                                     InteractiveWalkthrough::Target::utilities,
                                     InteractiveWalkthrough::Target::none};
    const std::array expectedExpandedTargets{InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::presetBrowser,
                                             InteractiveWalkthrough::ExpandedTarget::presetBrowser,
                                             InteractiveWalkthrough::ExpandedTarget::presetBrowser,
                                             InteractiveWalkthrough::ExpandedTarget::presetBrowser,
                                             InteractiveWalkthrough::ExpandedTarget::presetBrowser,
                                             InteractiveWalkthrough::ExpandedTarget::presetBrowser,
                                             InteractiveWalkthrough::ExpandedTarget::tapeMenu,
                                             InteractiveWalkthrough::ExpandedTarget::tapeMenu,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::delayDivisionMenu,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::echoEngineDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::echoEngineDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::wordmarkMenu,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::reverbDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::reverbDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::reverbDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::reverbDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::reverbDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::reverbDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::reverbDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::reverbDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::spatialPresetMenu,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::spatialPositionEditor,
                                             InteractiveWalkthrough::ExpandedTarget::spatialBlendDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::spatialBlendDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::spatialBlendDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::spatialBlendDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::spatialBlendDrawer,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::motionDivisionMenu,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::motionFineTune,
                                             InteractiveWalkthrough::ExpandedTarget::motionFineTune,
                                             InteractiveWalkthrough::ExpandedTarget::motionFineTune,
                                             InteractiveWalkthrough::ExpandedTarget::none,
                                             InteractiveWalkthrough::ExpandedTarget::wordmarkMenu,
                                             InteractiveWalkthrough::ExpandedTarget::none};
    const std::array expectedSpotlightDetails{InteractiveWalkthrough::SpotlightDetail::none,
                                              InteractiveWalkthrough::SpotlightDetail::none,
                                              InteractiveWalkthrough::SpotlightDetail::presetCategories,
                                              InteractiveWalkthrough::SpotlightDetail::presetList,
                                              InteractiveWalkthrough::SpotlightDetail::presetIntent,
                                              InteractiveWalkthrough::SpotlightDetail::presetFilter,
                                              InteractiveWalkthrough::SpotlightDetail::presetActions,
                                              InteractiveWalkthrough::SpotlightDetail::presetUserPresets,
                                              InteractiveWalkthrough::SpotlightDetail::tapeFlavors,
                                              InteractiveWalkthrough::SpotlightDetail::tapeNoise,
                                              InteractiveWalkthrough::SpotlightDetail::tapeDrive,
                                              InteractiveWalkthrough::SpotlightDetail::delayMode,
                                              InteractiveWalkthrough::SpotlightDetail::delayRate,
                                              InteractiveWalkthrough::SpotlightDetail::delaySync,
                                              InteractiveWalkthrough::SpotlightDetail::delayHeads,
                                              InteractiveWalkthrough::SpotlightDetail::delayFeedback,
                                              InteractiveWalkthrough::SpotlightDetail::delayPingPong,
                                              InteractiveWalkthrough::SpotlightDetail::delayLevel,
                                              InteractiveWalkthrough::SpotlightDetail::delayDub,
                                              InteractiveWalkthrough::SpotlightDetail::echoDrawer,
                                              InteractiveWalkthrough::SpotlightDetail::echoHeadRatio,
                                              InteractiveWalkthrough::SpotlightDetail::echoStatus,
                                              InteractiveWalkthrough::SpotlightDetail::echoAutoPanic,
                                              InteractiveWalkthrough::SpotlightDetail::reverbOverview,
                                              InteractiveWalkthrough::SpotlightDetail::reverbEngine,
                                              InteractiveWalkthrough::SpotlightDetail::reverbPreset,
                                              InteractiveWalkthrough::SpotlightDetail::reverbSizeDecay,
                                              InteractiveWalkthrough::SpotlightDetail::reverbTone,
                                              InteractiveWalkthrough::SpotlightDetail::reverbLevel,
                                              InteractiveWalkthrough::SpotlightDetail::reverbPreDelayWidth,
                                              InteractiveWalkthrough::SpotlightDetail::reverbCharacter,
                                              InteractiveWalkthrough::SpotlightDetail::reverbMod,
                                              InteractiveWalkthrough::SpotlightDetail::reverbDucking,
                                              InteractiveWalkthrough::SpotlightDetail::reverbPin,
                                              InteractiveWalkthrough::SpotlightDetail::reverbReset,
                                              InteractiveWalkthrough::SpotlightDetail::none,
                                              InteractiveWalkthrough::SpotlightDetail::spatialEnable,
                                              InteractiveWalkthrough::SpotlightDetail::spatialPreset,
                                              InteractiveWalkthrough::SpotlightDetail::spatialMode,
                                              InteractiveWalkthrough::SpotlightDetail::spatialHeads,
                                              InteractiveWalkthrough::SpotlightDetail::spatialView,
                                              InteractiveWalkthrough::SpotlightDetail::spatialDistanceView,
                                              InteractiveWalkthrough::SpotlightDetail::spatialReadout,
                                              InteractiveWalkthrough::SpotlightDetail::spatialWet,
                                              InteractiveWalkthrough::SpotlightDetail::spatialCrossover,
                                              InteractiveWalkthrough::SpotlightDetail::spatialWidth,
                                              InteractiveWalkthrough::SpotlightDetail::spatialChain,
                                              InteractiveWalkthrough::SpotlightDetail::spatialQuality,
                                              InteractiveWalkthrough::SpotlightDetail::motionPatterns,
                                              InteractiveWalkthrough::SpotlightDetail::motionRate,
                                              InteractiveWalkthrough::SpotlightDetail::motionDivision,
                                              InteractiveWalkthrough::SpotlightDetail::motionDepth,
                                              InteractiveWalkthrough::SpotlightDetail::motionChoreo,
                                              InteractiveWalkthrough::SpotlightDetail::motionSpread,
                                              InteractiveWalkthrough::SpotlightDetail::motionRetrig,
                                              InteractiveWalkthrough::SpotlightDetail::none,
                                              InteractiveWalkthrough::SpotlightDetail::none,
                                              InteractiveWalkthrough::SpotlightDetail::none};
    const std::array<const char*, 58> expectedSections{
        "WELCOME", "PRESETS", "PRESETS", "PRESETS", "PRESETS", "PRESETS", "PRESETS",   "PRESETS", "TAPE",    "TAPE",
        "TAPE",    "ECHO",    "ECHO",    "ECHO",    "ECHO",    "ECHO",    "ECHO",      "ECHO",    "ECHO",    "ECHO",
        "ECHO",    "ECHO",    "ECHO",    "REVERB",  "REVERB",  "REVERB",  "REVERB",    "REVERB",  "REVERB",  "REVERB",
        "REVERB",  "REVERB",  "REVERB",  "REVERB",  "REVERB",  "REVERB",  "SPATIAL",   "SPATIAL", "SPATIAL", "SPATIAL",
        "SPATIAL", "SPATIAL", "SPATIAL", "SPATIAL", "SPATIAL", "SPATIAL", "SPATIAL",   "SPATIAL", "MOTION",  "MOTION",
        "MOTION",  "MOTION",  "MOTION",  "MOTION",  "MOTION",  "OUTPUT",  "UTILITIES", "COMPLETE"};
    expect(walkthrough.getStepCountForTesting() == static_cast<int>(expectedTargets.size()),
           "walkthrough did not give each preset-browser surface its own lesson");
    auto* sectionRepaintRecorder = new RepaintRecorder(walkthrough);
    walkthrough.setCachedComponentImage(sectionRepaintRecorder);
    for (int i = 0; nextButton != nullptr && i < walkthrough.getStepCountForTesting(); ++i)
    {
        expect(walkthrough.getCurrentTargetForTesting() == expectedTargets[static_cast<size_t>(i)],
               "walkthrough step order changed");
        expect(walkthrough.getCurrentExpandedTargetForTesting() == expectedExpandedTargets[static_cast<size_t>(i)],
               "arrow navigation did not request the lesson's visible surface");
        expect(walkthrough.getCurrentSpotlightDetailForTesting() == expectedSpotlightDetails[static_cast<size_t>(i)],
               "preset lesson did not point at the browser region named in its copy");
        expect(
            walkthrough.getTitle().startsWithIgnoreCase(juce::String(expectedSections[static_cast<size_t>(i)]) + " //"),
            "walkthrough did not announce the current signal-path section");
        if (i == 1)
        {
            expect(walkthrough.getDescription().containsIgnoreCase("preset name"),
                   "preset introduction did not explain the closed preset control");
            expect(walkthrough.getCurrentCompletionSignalForTesting() == InteractiveWalkthrough::CompletionSignal::none,
                   "preset introduction incorrectly required an interaction");
        }
        if (i == 2)
        {
            expect(walkthrough.getTitle().containsIgnoreCase("result") &&
                       walkthrough.getDescription().containsIgnoreCase("left column") &&
                       walkthrough.getDescription().containsIgnoreCase("Echo & Rhythm") &&
                       walkthrough.getDescription().containsIgnoreCase("Reverb & Atmosphere") &&
                       walkthrough.getDescription().containsIgnoreCase("Spatial & Motion") &&
                       walkthrough.getDescription().containsIgnoreCase("Experimental") &&
                       walkthrough.getDescription().containsIgnoreCase("All Presets") &&
                       walkthrough.getDescription().containsIgnoreCase("User") &&
                       !walkthrough.getDescription().containsIgnoreCase("Spatial Showcase"),
                   "preset category lesson did not explain the visible bank in producer terms");
            expect(walkthrough.getCurrentCompletionSignalForTesting() == InteractiveWalkthrough::CompletionSignal::none,
                   "automatically opened preset browser reported a fake user completion");
        }
        if (i == 3)
        {
            expect(walkthrough.getDescription().containsIgnoreCase("distinct starting points") &&
                       walkthrough.getDescription().containsIgnoreCase("Up/Down"),
                   "preset audition lesson did not explain how the visible choices relate");
            expect(!walkthrough.getDescription().containsIgnoreCase("Hero") &&
                       !walkthrough.getDescription().containsIgnoreCase("Practical") &&
                       !walkthrough.getDescription().containsIgnoreCase("Performance"),
                   "preset audition lesson exposed internal roster language to producers");
            expect(walkthrough.getCurrentCompletionSignalForTesting() ==
                       InteractiveWalkthrough::CompletionSignal::presetAudition,
                   "preset audition lesson did not acknowledge a deliberate preset change");
        }
        if (i == 4)
        {
            expect(walkthrough.getDescription().containsIgnoreCase("strip above the buttons") &&
                       walkthrough.getDescription().containsIgnoreCase("audition"),
                   "preset intent lesson did not explain the visible sound description");
            expect(walkthrough.getCurrentCompletionSignalForTesting() == InteractiveWalkthrough::CompletionSignal::none,
                   "reading the preset intent incorrectly required an interaction");
        }
        if (i == 5)
        {
            expect(walkthrough.getDescription().containsIgnoreCase("filter") &&
                       walkthrough.getDescription().containsIgnoreCase("matches remain") &&
                       walkthrough.getDescription().containsIgnoreCase("musical uses") &&
                       walkthrough.getDescription().containsIgnoreCase("inspirations") &&
                       walkthrough.getDescription().containsIgnoreCase("active group") &&
                       walkthrough.getDescription().containsIgnoreCase("All Presets") &&
                       walkthrough.getDescription().containsIgnoreCase("Escape"),
                   "preset filter lesson did not explain the visible search field");
            expect(walkthrough.getCurrentCompletionSignalForTesting() ==
                       InteractiveWalkthrough::CompletionSignal::presetFilter,
                   "preset filter lesson did not acknowledge text entry");
        }
        if (i == 6)
        {
            expect(walkthrough.getDescription().containsIgnoreCase("Save As") &&
                       walkthrough.getDescription().containsIgnoreCase("Library") &&
                       walkthrough.getDescription().containsIgnoreCase("Import") &&
                       walkthrough.getDescription().containsIgnoreCase("Reveal") &&
                       walkthrough.getDescription().containsIgnoreCase("Init"),
                   "preset action lesson did not explain the visible action bar");
            expect(walkthrough.getCurrentCompletionSignalForTesting() == InteractiveWalkthrough::CompletionSignal::none,
                   "preset action lesson incorrectly required a file or save operation");
        }
        if (i == 7)
        {
            expect(walkthrough.getDescription().containsIgnoreCase("Choose User") &&
                       walkthrough.getDescription().containsIgnoreCase("rename and delete") &&
                       walkthrough.getDescription().containsIgnoreCase("Save As") &&
                       walkthrough.getDescription().containsIgnoreCase("empty"),
                   "user-preset lesson did not explain where saved sounds can be managed");
            expect(walkthrough.getCurrentCompletionSignalForTesting() == InteractiveWalkthrough::CompletionSignal::none,
                   "user-preset lesson incorrectly required a destructive operation");
        }
        if (i == 8)
        {
            expect(walkthrough.getTitle().containsIgnoreCase("flavor") &&
                       walkthrough.getDescription().containsIgnoreCase("Clean") &&
                       walkthrough.getDescription().containsIgnoreCase("Warm") &&
                       walkthrough.getDescription().containsIgnoreCase("Hot") &&
                       walkthrough.getDescription().containsIgnoreCase("Cassette") &&
                       walkthrough.getDescription().containsIgnoreCase("Worn"),
                   "tape Flavor lesson did not explain the five audible choices");
            expect(walkthrough.getCurrentCompletionSignalForTesting() ==
                       InteractiveWalkthrough::CompletionSignal::tapeFlavor,
                   "tape Flavor lesson did not acknowledge a deliberate Flavor choice");
        }
        if (i == 9)
        {
            expect(walkthrough.getTitle().containsIgnoreCase("noise") &&
                       walkthrough.getDescription().containsIgnoreCase("hiss") &&
                       walkthrough.getDescription().containsIgnoreCase("checkmark"),
                   "tape Noise lesson did not explain how or why to switch hiss off");
            expect(walkthrough.getCurrentCompletionSignalForTesting() ==
                       InteractiveWalkthrough::CompletionSignal::tapeNoise,
                   "tape Noise lesson did not acknowledge the Noise switch");
        }
        if (i == 10)
        {
            expect(walkthrough.getTitle().containsIgnoreCase("drive") &&
                       walkthrough.getDescription().containsIgnoreCase("signal hits the tape") &&
                       walkthrough.getDescription().containsIgnoreCase("saturation") &&
                       walkthrough.getDescription().containsIgnoreCase("compression"),
                   "tape Drive lesson did not explain the audible purpose of Drive");
            expect(walkthrough.getCurrentCompletionSignalForTesting() ==
                       InteractiveWalkthrough::CompletionSignal::tapeDrive,
                   "tape Drive lesson did not acknowledge a Drive adjustment");
        }
        if (i == 11)
            expect(walkthrough.getDescription().containsIgnoreCase("E+REV") &&
                       walkthrough.getDescription().containsIgnoreCase("REV ONLY") &&
                       walkthrough.getCurrentSpotlightDetailForTesting() ==
                           InteractiveWalkthrough::SpotlightDetail::delayMode,
                   "Echo workflow did not begin by explaining the audible Mode choices");
        if (i == 18)
            expect(walkthrough.getDescription().containsIgnoreCase("DUB button") &&
                       walkthrough.getDescription().containsIgnoreCase("INPUT OFF") &&
                       walkthrough.getCurrentCompletionSignalForTesting() ==
                           InteractiveWalkthrough::CompletionSignal::delayDub,
                   "Dub lesson did not point at the surface DUB button or its INPUT OFF state");
        if (i == 20)
            expect(walkthrough.getDescription().containsIgnoreCase("Sync is off") &&
                       walkthrough.getDescription().containsIgnoreCase("Golden"),
                   "Head Ratio lesson did not explain when or why its choices apply");
        if (i == 21)
            expect(walkthrough.getDescription().containsIgnoreCase("feedback heat") &&
                       walkthrough.getDescription().containsIgnoreCase("DAW parameter list"),
                   "Echo status lesson did not explain heat and host Power recovery");
        if (i == 22)
            expect(walkthrough.getDescription().containsIgnoreCase("Spatial Echo menu") &&
                       walkthrough.getCurrentCompletionSignalForTesting() ==
                           InteractiveWalkthrough::CompletionSignal::echoAutoPanic,
                   "Auto-Panic lesson did not send the producer to the wordmark menu");
        if (i == 23)
            expect(walkthrough.getDescription().containsIgnoreCase("E+REV") &&
                       walkthrough.getDescription().containsIgnoreCase("main screen"),
                   "Reverb workflow did not establish how to hear and find the controls");
        if (i == 36)
            expect(walkthrough.getDescription().containsIgnoreCase("headphones") &&
                       walkthrough.getCurrentSpotlightDetailForTesting() ==
                           InteractiveWalkthrough::SpotlightDetail::spatialEnable,
                   "Spatial workflow did not begin with enablement and monitoring context");
        if (i == 37)
            expect(walkthrough.getDescription().containsIgnoreCase("all 28") &&
                       walkthrough.getDescription().containsIgnoreCase("head positions stay yours"),
                   "spatial scene lesson overstated what a scene changes");
        if (i == 48)
            expect(walkthrough.getDescription().containsIgnoreCase("Pendulum swings left and right") &&
                       walkthrough.getDescription().containsIgnoreCase("Breathe moves near/far"),
                   "motion pattern lesson did not explain the audible paths");
        if (i == 52)
            expect(walkthrough.getDescription().containsIgnoreCase("PER-HEAD") &&
                       walkthrough.getDescription().containsIgnoreCase("Cascade"),
                   "choreography lesson did not explain its operating mode and choices");
        if (i == 54)
            expect(walkthrough.getDescription().containsIgnoreCase("MIDI note") &&
                       walkthrough.getDescription().containsIgnoreCase("standalone app"),
                   "motion restart lesson did not state the current Note limitation");
        if (i == walkthrough.getStepCountForTesting() - 1)
            expect(nextButton->getTitle() == "Finish tour",
                   "finish action did not announce that it completes the tour");

        sectionRepaintRecorder->clear();
        nextButton->onClick();
        if (i + 1 < walkthrough.getStepCountForTesting())
        {
            const bool sectionChanged =
                juce::String(expectedSections[static_cast<size_t>(i)]) != expectedSections[static_cast<size_t>(i + 1)];
            expect(sectionRepaintRecorder->covers(walkthrough.getLocalBounds()) == false,
                   "section transition repainted over the highlighted interface");
            if (sectionChanged)
            {
                expect(sectionRepaintRecorder->covers(walkthrough.getCardBoundsForTesting()),
                       "section transition did not repaint its chapter card");
                expect(walkthrough.getSectionTransitionBoundsForTesting() == walkthrough.getCardBoundsForTesting(),
                       "section chapter cue escaped the coach card");
                expect(walkthrough.getSectionTransitionLeadForTesting().containsIgnoreCase(
                           i + 1 == walkthrough.getStepCountForTesting() - 1 ? "TOUR COMPLETE" : "NEW SECTION"),
                       "section chapter cue did not clearly announce a transition");
                for (int tick = 0; tick < 40; ++tick)
                    walkthrough.refreshActionState();
                sectionRepaintRecorder->clear();
                walkthrough.refreshActionState();
                expect(!sectionRepaintRecorder->covers(walkthrough.getCardBoundsForTesting()),
                       "section transition did not clear after its brief chapter cue");
            }
        }
    }
    walkthrough.setCachedComponentImage(nullptr);
    expect(dismissalCount == 4 && lastDismissal == InteractiveWalkthrough::Dismissal::completed,
           "finishing the last step did not mark completion");

    // Break caught: editor step entry writes musical state or reads the real
    // Application Support preference instead of the injected temporary file.
    const auto editorSettingsFile = temp.getFile().getChildFile("editor-walkthrough.settings");
    auto injectedPreferences = std::make_unique<EditorPreferences>(editorSettingsFile);
    SpatialEchoAudioProcessorEditor editor(processor, std::move(injectedPreferences));

    // Integration guard: the editor must resolve the Rate lesson through the
    // dynamic free-time region, not bypass it with the always-disabled Rate knob.
    auto* editorTempoSync = processor.getValueTreeState().getParameter("TempoSync");
    const auto originalEditorTempoSync = editorTempoSync != nullptr ? editorTempoSync->getValue() : 0.0f;
    expect(editorTempoSync != nullptr, "TempoSync parameter missing from editor walkthrough test");
    if (editorTempoSync != nullptr)
    {
        editorTempoSync->setValueNotifyingHost(1.0f);
        const auto syncedRateLesson = editor.getResolvedWalkthroughDelayRateTargetForTesting();
        editorTempoSync->setValueNotifyingHost(0.0f);
        const auto freeRateLesson = editor.getResolvedWalkthroughDelayRateTargetForTesting();
        editorTempoSync->setValueNotifyingHost(originalEditorTempoSync);

        expect(syncedRateLesson.has_value() && freeRateLesson.has_value() && *syncedRateLesson != *freeRateLesson,
               "editor Rate lesson did not retarget after Sync was turned off");
    }

    const auto stateBeforeTour = processor.getValueTreeState().copyState();
    editor.startWalkthroughForTesting();
    editor.advanceWalkthroughForTesting();
    expect(!editor.isWalkthroughPresetBrowserVisibleForTesting(),
           "first preset lesson opened the browser before the producer pressed Next");
    editor.advanceWalkthroughForTesting();
    expect(editor.isWalkthroughPresetBrowserVisibleForTesting(),
           "Next did not open the preset browser for the bank explanation");

    const auto categorySpotlight = editor.getWalkthroughSpotlightForTesting();
    editor.advanceWalkthroughForTesting();
    const auto listSpotlight = editor.getWalkthroughSpotlightForTesting();
    editor.advanceWalkthroughForTesting();
    const auto intentSpotlight = editor.getWalkthroughSpotlightForTesting();
    editor.advanceWalkthroughForTesting();
    const auto filterSpotlight = editor.getWalkthroughSpotlightForTesting();
    editor.advanceWalkthroughForTesting();
    const auto actionSpotlight = editor.getWalkthroughSpotlightForTesting();

    // Break caught: the actual Library menu is hosted outside the plug-in
    // window, swallows coach navigation, or outlives the tour after Escape.
    editor.addToDesktop(juce::ComponentPeer::windowIsTemporary);
    editor.setVisible(true);
    editor.toFront(true);
    auto* editorBrowser = findDescendant<PresetBrowserOverlay>(editor);
    auto* editorCoach = findDescendant<InteractiveWalkthrough>(editor);
    expect(editor.getPeer() != nullptr && editorBrowser != nullptr && editorCoach != nullptr,
           "Library walkthrough regression did not obtain an editor peer and browser");
    if (editorBrowser != nullptr && editorCoach != nullptr)
    {
        const int libraryStep = editorCoach->getCurrentStepIndexForTesting();
        expect(libraryStep == 6 && editorCoach->getTitle().containsIgnoreCase("SAVE OR MOVE"),
               "Library regression did not run on the preset-action lesson");
        const auto stateBeforeLibrary = processor.getValueTreeState().copyState();
        const auto preferencesBeforeLibrary = editorSettingsFile.loadFileAsString();
        const auto presetDirectory = processor.getUserPresetDirectoryOnDisk();
        const auto snapshotPresetFiles = [&]
        {
            std::vector<std::pair<juce::String, juce::MemoryBlock>> snapshot;
            juce::Array<juce::File> files;
            presetDirectory.findChildFiles(files, juce::File::findFiles, true);
            for (const auto& file : files)
            {
                juce::MemoryBlock contents;
                expect(file.loadFileAsData(contents), "could not read a preset for Library cancellation");
                snapshot.emplace_back(file.getRelativePathFrom(presetDirectory), std::move(contents));
            }
            std::sort(snapshot.begin(), snapshot.end(),
                      [](const auto& first, const auto& second) { return first.first < second.first; });
            return snapshot;
        };
        const auto filesBeforeLibrary = snapshotPresetFiles();
        const auto pressBrowserControl = [&](const juce::String& title)
        {
            auto* control = findChildWithTitle(*editorBrowser, title);
            auto* handler = control != nullptr ? control->getAccessibilityHandler() : nullptr;
            return handler != nullptr && handler->getActions().invoke(juce::AccessibilityActionType::press);
        };
        const auto openLibrary = [&] { return pressBrowserControl("LIBRARY"); };
        const auto pressCoachControl = [&](const juce::String& id)
        {
            auto* control = editorCoach->findChildWithID(id);
            auto* handler = control != nullptr ? control->getAccessibilityHandler() : nullptr;
            return handler != nullptr && handler->getActions().invoke(juce::AccessibilityActionType::press);
        };
        const auto browseControlChecked = [&](const juce::String& title)
        {
            auto* control = findChildWithTitle(*editorBrowser, title);
            auto* handler = control != nullptr ? control->getAccessibilityHandler() : nullptr;
            return handler != nullptr && handler->getCurrentState().isChecked();
        };
        const auto settleMenuClosure = [&](const juce::Component::SafePointer<juce::Component>& menu, const char* phase)
        {
            // Plugin builds disable JUCE modal loops. Deliver this menu's
            // asynchronous cancellation through the public macOS run loop.
            const auto start = juce::Time::getMillisecondCounterHiRes();
            const auto deadline = start + 250.0;
            const bool hadMenu = menu != nullptr;
            const bool hadAttachment = editorCoach->getAttachedModalComponentForTesting() != nullptr;
            int iterations = 0, handled = 0, timedOut = 0;
            double longestIteration = 0.0;
            while (menu != nullptr && juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                const auto beforeIteration = juce::Time::getMillisecondCounterHiRes();
                const auto result = SpatialEchoTests::NativeEventLoop::pumpBriefly();
                longestIteration =
                    juce::jmax(longestIteration, juce::Time::getMillisecondCounterHiRes() - beforeIteration);
                ++iterations;
                if (result == SpatialEchoTests::NativeEventLoop::PumpResult::handledSource)
                    ++handled;
                if (result == SpatialEchoTests::NativeEventLoop::PumpResult::timedOut)
                    ++timedOut;
            }
            if (menu != nullptr || editorCoach->getAttachedModalComponentForTesting() != nullptr)
                libraryPumpDiagnostics << " {" << phase << " messageThread="
                                       << (juce::MessageManager::getInstance()->isThisTheMessageThread() ? 1 : 0)
                                       << " mainLoop=" << (SpatialEchoTests::NativeEventLoop::isMainLoop() ? 1 : 0)
                                       << " iterations=" << iterations << " handled=" << handled
                                       << " timeouts=" << timedOut << " elapsedMs="
                                       << juce::String(juce::Time::getMillisecondCounterHiRes() - start, 2)
                                       << " longestMs=" << juce::String(longestIteration, 2)
                                       << " menu=" << (hadMenu ? 1 : 0) << "->" << (menu != nullptr ? 1 : 0)
                                       << " attached=" << (hadAttachment ? 1 : 0) << "->"
                                       << (editorCoach->getAttachedModalComponentForTesting() != nullptr ? 1 : 0)
                                       << "}";
        };
        // A cold native peer dispatched one 449 ms source before the menu's
        // 250 ms assertion could reach its callback. Drain to an explicit
        // queue marker first; initialization and menu completion have separate
        // bounds, and a timeout leaves only a shared marker in the queue.
        const auto peerReady = std::make_shared<std::atomic<bool>>(false);
        const bool readinessPosted =
            juce::MessageManager::callAsync([peerReady] { peerReady->store(true, std::memory_order_relaxed); });
        const auto readinessStart = juce::Time::getMillisecondCounterHiRes();
        const auto readinessDeadline = readinessStart + 2000.0;
        int readinessIterations = 0;
        while (readinessPosted && !peerReady->load(std::memory_order_relaxed) &&
               juce::Time::getMillisecondCounterHiRes() < readinessDeadline)
        {
            SpatialEchoTests::NativeEventLoop::pumpBriefly();
            ++readinessIterations;
        }
        const bool nativePeerReady = readinessPosted && peerReady->load(std::memory_order_relaxed);
        expect(nativePeerReady, "native editor queue did not reach readiness before Library");
        expect(editorCoach->getCurrentStepIndexForTesting() == libraryStep,
               "native editor initialization changed the Library walkthrough lesson");
        if (!nativePeerReady)
            libraryPumpDiagnostics << " {peer readiness iterations=" << readinessIterations << " elapsedMs="
                                   << juce::String(juce::Time::getMillisecondCounterHiRes() - readinessStart, 2)
                                   << " messageThread="
                                   << (juce::MessageManager::getInstance()->isThisTheMessageThread() ? 1 : 0)
                                   << " mainLoop=" << (SpatialEchoTests::NativeEventLoop::isMainLoop() ? 1 : 0) << "}";
        expect(openLibrary(), "Library native action did not open its menu during the tour");
        juce::Component::SafePointer<juce::Component> libraryMenu(juce::Component::getCurrentlyModalComponent());
        expect(libraryMenu != nullptr && libraryMenu->getName() == "menu" &&
                   libraryMenu->getParentComponent() == &editor && libraryMenu->getPeer() == editor.getPeer(),
               "Library popup was not parented inside the editor window");
        expect(libraryMenu != nullptr &&
                   editorCoach->getAttachedModalComponentForTesting() == libraryMenu.getComponent(),
               "Library popup was not attached to the active coach card");
        expect(editorCoach->handlePopupKeyForTesting(juce::KeyPress(juce::KeyPress::tabKey)),
               "Tab in Library did not route to coach-card navigation");
        expect(editorCoach->isVisible() && editorCoach->getCurrentStepIndexForTesting() == libraryStep,
               "Tab in Library changed the walkthrough lesson");
        juce::PopupMenu::dismissAllActiveMenus();
        settleMenuClosure(libraryMenu, "Tab cancel");
        expect(editorCoach->getAttachedModalComponentForTesting() == nullptr,
               "cancelled Library popup retained a coach modal attachment");

        // A lesson change must close the live menu before changing focus to
        // the destination surface. Exercise the actual coach button actions.
        expect(openLibrary(), "Library could not reopen before Next");
        libraryMenu = juce::Component::getCurrentlyModalComponent();
        expect(libraryMenu != nullptr && libraryMenu->isCurrentlyModal(false),
               "Next regression did not begin with a live Library menu");
        expect(pressCoachControl("walkthroughNext"), "Next native action did not activate while Library was open");
        settleMenuClosure(libraryMenu, "Next");
        expect(editorCoach->isVisible() && editorBrowser->isShowing() &&
                   editorCoach->getCurrentStepIndexForTesting() == libraryStep + 1 &&
                   editorCoach->getTitle().containsIgnoreCase("MANAGE USER PRESETS") &&
                   editorCoach->getCurrentSpotlightDetailForTesting() ==
                       InteractiveWalkthrough::SpotlightDetail::presetUserPresets,
               "Next from Library did not enter the User lesson in order");
        expect((libraryMenu == nullptr || !libraryMenu->isCurrentlyModal(false)) &&
                   editorCoach->getAttachedModalComponentForTesting() == nullptr,
               "Library remained modal after Next entered the User lesson");
        expect(pressBrowserControl("User") && browseControlChecked("User"),
               "User native action was unavailable after Library closed on Next");

        expect(openLibrary(), "Library could not reopen before Back");
        libraryMenu = juce::Component::getCurrentlyModalComponent();
        expect(libraryMenu != nullptr && libraryMenu->isCurrentlyModal(false),
               "Back regression did not begin with a live Library menu");
        expect(pressCoachControl("walkthroughBack"), "Back native action did not activate while Library was open");
        settleMenuClosure(libraryMenu, "Back");
        expect(editorCoach->isVisible() && editorBrowser->isShowing() &&
                   editorCoach->getCurrentStepIndexForTesting() == libraryStep &&
                   editorCoach->getTitle().containsIgnoreCase("SAVE OR MOVE PRESETS") &&
                   editorCoach->getCurrentSpotlightDetailForTesting() ==
                       InteractiveWalkthrough::SpotlightDetail::presetActions &&
                   editorCoach->getStepCountForTesting() == 58,
               "Back from Library did not restore the preset-action lesson in order");
        expect((libraryMenu == nullptr || !libraryMenu->isCurrentlyModal(false)) &&
                   editorCoach->getAttachedModalComponentForTesting() == nullptr,
               "Library remained modal after Back restored the preset-action lesson");
        expect(pressBrowserControl("All Presets") && browseControlChecked("All Presets"),
               "browser native action was unavailable after Library closed on Back");

        expect(openLibrary(), "Library could not reopen after Next and Back navigation");
        libraryMenu = juce::Component::getCurrentlyModalComponent();
        expect(libraryMenu != nullptr &&
                   editorCoach->getAttachedModalComponentForTesting() == libraryMenu.getComponent(),
               "reopened Library popup did not attach to the coach card");
        expect(editorCoach->handlePopupKeyForTesting(juce::KeyPress(juce::KeyPress::escapeKey)),
               "Escape in Library did not exit the walkthrough");
        expect(!editorCoach->isVisible() && !editorBrowser->isShowing() &&
                   editorCoach->getCurrentStepIndexForTesting() == libraryStep,
               "Escape in Library changed the step or left a tour surface open");
        settleMenuClosure(libraryMenu, "Escape");
        expect((libraryMenu == nullptr || !libraryMenu->isCurrentlyModal(false)) &&
                   editorCoach->getAttachedModalComponentForTesting() == nullptr,
               "Library remained modal after the tour exited");
        juce::PopupMenu::dismissAllActiveMenus();
        settleMenuClosure(libraryMenu, "final cancel");
        expect(stateBeforeLibrary.isEquivalentTo(processor.getValueTreeState().copyState()) &&
                   preferencesBeforeLibrary == editorSettingsFile.loadFileAsString() &&
                   filesBeforeLibrary == snapshotPresetFiles(),
               "Library navigation or cancellation changed sound or saved files");
    }
    editor.removeFromDesktop();
    // Resume at the same action lesson so the original full-tour order and
    // no-parameter-write integration checks continue across the User lesson.
    editor.startWalkthroughForTesting();
    for (int step = 0; step < 6; ++step)
        editor.advanceWalkthroughForTesting();
    expect(editorCoach != nullptr && editorCoach->getCurrentStepIndexForTesting() == 6,
           "Library cancellation did not restore the preset-action lesson");
    editor.advanceWalkthroughForTesting();
    const auto userSpotlight = editor.getWalkthroughSpotlightForTesting();

    expect(!categorySpotlight.isEmpty() && !listSpotlight.isEmpty() && !intentSpotlight.isEmpty() &&
               !filterSpotlight.isEmpty() && !actionSpotlight.isEmpty() && !userSpotlight.isEmpty(),
           "a preset-browser lesson did not expose a visible spotlight");
    expect(categorySpotlight.getCentreX() < listSpotlight.getCentreX(),
           "category and preset lessons did not point to their separate columns");
    expect(filterSpotlight.getCentreY() < listSpotlight.getCentreY() &&
               intentSpotlight.getCentreY() > listSpotlight.getBottom() &&
               intentSpotlight.getCentreY() < actionSpotlight.getCentreY() &&
               actionSpotlight.getCentreY() > listSpotlight.getBottom(),
           "filter, list, description, and action lessons did not follow the browser layout");
    expect(categorySpotlight != listSpotlight && listSpotlight != intentSpotlight &&
               intentSpotlight != filterSpotlight && filterSpotlight != actionSpotlight,
           "preset lessons reused the whole-browser spotlight");

    // Break caught: the User lesson exposes factory rows, or a fresh install
    // points at the empty list instead of the available Save As action.
    PresetBrowserOverlay isolatedBrowser;
    isolatedBrowser.setBounds(editor.getLocalBounds());
    editor.addAndMakeVisible(isolatedBrowser);
    isolatedBrowser.setPanelPlacementArea({0, 0, 900, 650});

    PresetBrowserOverlay::Category factoryCategory;
    factoryCategory.label = "Factory";
    factoryCategory.items.push_back({"Factory One", "factory sound", [] {}, false, {}});
    isolatedBrowser.show({factoryCategory}, "Factory One");
    isolatedBrowser.revealWalkthroughUserPresets();
    const auto emptyUserTarget = editor.getLocalArea(nullptr, isolatedBrowser.getWalkthroughUserPresetScreenBounds());
    const auto emptyUserActions = editor.getLocalArea(nullptr, isolatedBrowser.getWalkthroughActionScreenBounds());
    const auto emptyUserList = editor.getLocalArea(nullptr, isolatedBrowser.getWalkthroughPresetListScreenBounds());
    expect(emptyUserActions.contains(emptyUserTarget) && !emptyUserList.intersects(emptyUserTarget),
           "fresh-install User lesson did not point to Save As");

    PresetBrowserOverlay::Category userCategory;
    userCategory.label = "User";
    userCategory.items.push_back({"My Space", "saved sound", [] {}, true, {}});
    isolatedBrowser.show({factoryCategory, userCategory}, "Factory One");
    isolatedBrowser.revealWalkthroughUserPresets();
    const auto populatedUserTarget =
        editor.getLocalArea(nullptr, isolatedBrowser.getWalkthroughUserPresetScreenBounds());
    const auto populatedUserList = editor.getLocalArea(nullptr, isolatedBrowser.getWalkthroughPresetListScreenBounds());
    const auto populatedCategoryColumn =
        editor.getLocalArea(nullptr, isolatedBrowser.getWalkthroughCategoryScreenBounds());
    expect(populatedUserList.contains(populatedUserTarget) && !populatedCategoryColumn.intersects(populatedUserTarget),
           "User lesson joined separate columns and exposed factory rows between them");
    isolatedBrowser.dismiss();

    editor.advanceWalkthroughForTesting();
    const auto tapeFlavorSpotlight = editor.getWalkthroughSpotlightForTesting();
    editor.advanceWalkthroughForTesting();
    const auto tapeNoiseSpotlight = editor.getWalkthroughSpotlightForTesting();
    editor.advanceWalkthroughForTesting();
    const auto tapeDriveSpotlight = editor.getWalkthroughSpotlightForTesting();
    expect(!tapeFlavorSpotlight.isEmpty() && !tapeNoiseSpotlight.isEmpty() && !tapeDriveSpotlight.isEmpty(),
           "a Tape lesson did not expose a visible spotlight");
    expect(tapeFlavorSpotlight == tapeNoiseSpotlight, "closed Tape menu did not fall back to the Flavor opener");
    expect(tapeDriveSpotlight != tapeFlavorSpotlight,
           "Drive lesson reused the Flavor spotlight instead of isolating Drive");

    for (int i = 11; i < editor.getWalkthroughStepCountForTesting(); ++i)
        editor.advanceWalkthroughForTesting();
    expect(stateBeforeTour.isEquivalentTo(processor.getValueTreeState().copyState()),
           "walkthrough step entry changed processor parameter state");

    // Break caught: retained hidden filter text or a same-name preset makes a
    // deliberate preset-browser action impossible to complete.
    editor.setWalkthroughPresetFilterForTesting("orbit", false);
    editor.prepareWalkthroughPresetCompletionForTesting();
    editor.setWalkthroughPresetFilterForTesting({}, true);
    expect(!editor.isWalkthroughCompletionForTesting(InteractiveWalkthrough::CompletionSignal::presetFilter),
           "reopening the browser completed filtering before text entry");
    editor.setWalkthroughPresetFilterForTesting("orbit", true);
    expect(editor.isWalkthroughCompletionForTesting(InteractiveWalkthrough::CompletionSignal::presetFilter),
           "retyping a retained filter after reopening did not complete the action");
    editor.setWalkthroughPresetFilterForTesting({}, false);

    editor.setWalkthroughPresetIdentityForTesting(1, "Same Name");
    editor.prepareWalkthroughPresetCompletionForTesting();
    editor.setWalkthroughPresetIdentityForTesting(2, "Same Name");
    expect(editor.isWalkthroughCompletionForTesting(InteractiveWalkthrough::CompletionSignal::presetAudition),
           "same-name preset in a different library did not complete auditioning");
    editor.setWalkthroughPresetIdentityForTesting(0, {});

    // Break caught: a named control changes but the card never acknowledges it
    // because its parameter was omitted from the completion baseline.
    const auto expectParameterCompletes = [&](InteractiveWalkthrough::Target target,
                                              InteractiveWalkthrough::CompletionSignal signal, const char* parameterId,
                                              const char* failure)
    {
        auto* parameter = processor.getValueTreeState().getParameter(parameterId);
        expect(parameter != nullptr, "walkthrough completion parameter was missing");
        if (parameter == nullptr)
            return;

        const float previous = parameter->getValue();
        editor.prepareWalkthroughCompletionForTesting(target);
        parameter->setValueNotifyingHost(previous < 0.5f ? 0.75f : 0.25f);
        expect(editor.isWalkthroughCompletionForTesting(signal), failure);
        parameter->setValueNotifyingHost(previous);
    };
    expectParameterCompletes(InteractiveWalkthrough::Target::tape, InteractiveWalkthrough::CompletionSignal::tapeFlavor,
                             "TapeFlavor", "choosing a named Tape Flavor did not complete its suggestion");
    expectParameterCompletes(InteractiveWalkthrough::Target::tape, InteractiveWalkthrough::CompletionSignal::tapeNoise,
                             "TapeNoiseEnabled",
                             "switching the named Tape Noise control did not complete its suggestion");
    expectParameterCompletes(InteractiveWalkthrough::Target::tape, InteractiveWalkthrough::CompletionSignal::tapeDrive,
                             "TapeDrive", "moving the named Tape Drive control did not complete its suggestion");
    expectParameterCompletes(InteractiveWalkthrough::Target::delay,
                             InteractiveWalkthrough::CompletionSignal::delayHeads, "Head3NoteDivision",
                             "changing the named H3 division did not complete the rhythm suggestion");
    expectParameterCompletes(InteractiveWalkthrough::Target::reverb,
                             InteractiveWalkthrough::CompletionSignal::reverbDucking, "ReverbDuckAmount",
                             "changing the named Ducking control did not complete the reverb suggestion");
    expectParameterCompletes(InteractiveWalkthrough::Target::reverb,
                             InteractiveWalkthrough::CompletionSignal::reverbLevel, "ReverbLevel",
                             "changing the named Reverb Level did not complete the reverb suggestion");

    const auto expectParameterDoesNotComplete = [&](InteractiveWalkthrough::Target target,
                                                    InteractiveWalkthrough::CompletionSignal signal,
                                                    const char* parameterId, const char* failure)
    {
        auto* parameter = processor.getValueTreeState().getParameter(parameterId);
        expect(parameter != nullptr, "walkthrough isolation parameter was missing");
        if (parameter == nullptr)
            return;
        const float previous = parameter->getValue();
        editor.prepareWalkthroughCompletionForTesting(target);
        parameter->setValueNotifyingHost(previous < 0.5f ? 0.75f : 0.25f);
        expect(!editor.isWalkthroughCompletionForTesting(signal), failure);
        parameter->setValueNotifyingHost(previous);
    };
    expectParameterDoesNotComplete(InteractiveWalkthrough::Target::delay,
                                   InteractiveWalkthrough::CompletionSignal::delayRate, "Intensity",
                                   "Feedback falsely completed the free Rate lesson");
    expectParameterDoesNotComplete(InteractiveWalkthrough::Target::reverb,
                                   InteractiveWalkthrough::CompletionSignal::reverbMod, "ReverbWidth",
                                   "Width falsely completed the Reverb Mod lesson");
    expectParameterDoesNotComplete(InteractiveWalkthrough::Target::spatialBlend,
                                   InteractiveWalkthrough::CompletionSignal::spatialWet, "SpatialSourceWidth",
                                   "Spatial Width falsely completed the Wet lesson");
    expectParameterDoesNotComplete(InteractiveWalkthrough::Target::spatialMotion,
                                   InteractiveWalkthrough::CompletionSignal::motionDepth, SpatialIDs::motion::kRate,
                                   "Motion Rate falsely completed the Depth lesson");

    auto* mixParameter = processor.getValueTreeState().getParameter("Mix");
    if (mixParameter != nullptr)
    {
        const float previousMix = mixParameter->getValue();
        const float changedMix = previousMix < 0.5f ? 0.75f : 0.25f;
        mixParameter->setValueNotifyingHost(changedMix);
        editor.exitWalkthroughForTesting();
        expect(std::abs(mixParameter->getValue() - changedMix) <= 1.0e-6f,
               "user parameter change was reverted on walkthrough exit");
        mixParameter->setValueNotifyingHost(previousMix);
    }

    juce::String result;
    result << "InteractiveWalkthrough: failures=" << failures;
    if (firstFailure.isNotEmpty())
        result << " first=" << firstFailure;
    if (libraryPumpDiagnostics.isNotEmpty())
        result << " pumps=" << libraryPumpDiagnostics;
    result << " " << (failures == 0 ? "[PASS]" : "[FAIL]");
    return result;
}
juce::String runWalkthroughCardClearsSurfacesTest(SpatialEchoAudioProcessor& processor)
{
    using Target = InteractiveWalkthrough::Target;
    using ExpandedTarget = InteractiveWalkthrough::ExpandedTarget;
    using SpotlightDetail = InteractiveWalkthrough::SpotlightDetail;

    int failures = 0;
    juce::String firstFailure;
    auto expect = [&](bool condition, const juce::String& message)
    {
        if (condition)
            return;
        ++failures;
        if (firstFailure.isEmpty())
            firstFailure = message;
    };

    // Break caught: the card keeps clear of the spotlit control only, so a
    // drawer drawn above the tour around that control hides the card.
    {
        juce::Component host;
        InteractiveWalkthrough walkthrough;
        host.setBounds(0, 0, 1400, 650);
        host.addAndMakeVisible(walkthrough);
        walkthrough.setBounds(host.getLocalBounds());
        const juce::Rectangle<int> control(140, 90, 200, 40);
        const juce::Rectangle<int> drawer(120, 60, 520, 480);
        std::optional<juce::Rectangle<int>> surface;
        walkthrough.setTargetResolver(
            [&](Target target, ExpandedTarget, SpotlightDetail) -> std::optional<juce::Rectangle<int>>
            {
                if (target == Target::none)
                    return std::nullopt;
                return control;
            });
        walkthrough.setCoveringSurfaceResolver([&](ExpandedTarget) { return surface; });
        walkthrough.start();
        walkthrough.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        walkthrough.refreshTarget();
        const auto besideControl = walkthrough.getCardBoundsForTesting();
        surface = drawer;
        walkthrough.refreshTarget();
        const auto card = walkthrough.getCardBoundsForTesting();
        expect(besideControl.intersects(drawer), "the control-only card position did not overlap the test drawer");
        expect(!card.intersects(drawer) && walkthrough.getLocalBounds().contains(card),
               "coach card sat under an open drawer");
        expect(card.getY() < control.getBottom() && card.getBottom() > control.getY(),
               "coach card no longer lines up with the spotlit control");
    }

    // The real tour: on every step, the card is clear of every open drawer,
    // division menu, callout and popup menu.
    juce::TemporaryFile settings(SpatialEchoTests::artifactDirectory().getChildFile("walkthrough-surfaces.settings"));
    SpatialEchoAudioProcessorEditor editor(processor, std::make_unique<EditorPreferences>(settings.getFile()));
    editor.setTopLeftPosition(40, 60);
    editor.addToDesktop(juce::ComponentPeer::windowIsTemporary);
    editor.setVisible(true);
    editor.toFront(true);
    auto* coach = findDescendant<InteractiveWalkthrough>(editor);
    expect(coach != nullptr, "editor has no walkthrough");

    const auto openSurfaces = [&]
    {
        std::vector<juce::Rectangle<int>> surfaces;
        for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
            if (auto* peer = juce::ComponentPeer::getPeer(i))
            {
                auto& component = peer->getComponent();
                if (&component != &editor && component.isVisible() &&
                    dynamic_cast<juce::TooltipWindow*>(&component) == nullptr)
                    surfaces.push_back(component.getScreenBounds());
            }
        for (int i = 0; i < juce::Component::getNumCurrentlyModalComponents(); ++i)
            if (auto* modal = juce::Component::getCurrentlyModalComponent(i))
                if (modal->getName() == "menu" && modal->isShowing() && modal->getTopLevelComponent() == &editor)
                    surfaces.push_back(modal->getScreenBounds());
        return surfaces;
    };

    int steps = 0;
    int coveredSteps = 0;
    int drawerSteps = 0;
    int drawerStepsOpen = 0;
    editor.startWalkthroughForTesting();
    const int stepCount = editor.getWalkthroughStepCountForTesting();
    for (int step = 0; coach != nullptr && coach->isVisible() && step < stepCount; ++step)
    {
        // Drawers and menus open asynchronously after a step begins.
        const auto expanded = coach->getCurrentExpandedTargetForTesting();
        const auto deadline =
            juce::Time::getMillisecondCounterHiRes() + (expanded == ExpandedTarget::none ? 30.0 : 400.0);
        while (juce::Time::getMillisecondCounterHiRes() < deadline &&
               (expanded == ExpandedTarget::none || openSurfaces().empty()))
            NativeEventLoop::pumpBriefly();
        coach->refreshTarget();

        const auto card = coach->localAreaToGlobal(coach->getCardBoundsForTesting());
        const auto surfaces = openSurfaces();
        ++steps;
        if (!surfaces.empty())
            ++coveredSteps;
        if (expanded == ExpandedTarget::echoEngineDrawer || expanded == ExpandedTarget::reverbDrawer)
        {
            ++drawerSteps;
            if (!surfaces.empty())
                ++drawerStepsOpen;
        }
        for (const auto& surface : surfaces)
            expect(!card.intersects(surface), "step " + juce::String(coach->getCurrentStepIndexForTesting() + 1) +
                                                  " (" + coach->getTitle() + "): coach card under an open surface");

        editor.advanceWalkthroughForTesting();
    }
    if (coach != nullptr && coach->isVisible())
        editor.exitWalkthroughForTesting();
    juce::PopupMenu::dismissAllActiveMenus();
    for (int i = 0; i < 10; ++i)
        NativeEventLoop::pumpBriefly();
    editor.removeFromDesktop();

    expect(steps == stepCount, "the tour did not run every step");
    expect(drawerSteps > 0 && drawerStepsOpen == drawerSteps, "a drawer lesson ran without its drawer open");

    juce::String result;
    result << "WalkthroughCardClearsSurfaces: steps=" << steps << " covered=" << coveredSteps
           << " drawerSteps=" << drawerStepsOpen << "/" << drawerSteps << " failures=" << failures;
    if (firstFailure.isNotEmpty())
        result << " first=" << firstFailure;
    result << " " << (failures == 0 ? "[PASS]" : "[FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::System

#endif
