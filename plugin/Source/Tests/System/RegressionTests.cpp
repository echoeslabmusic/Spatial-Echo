// Focused regressions: host latency, position publication, accessibility, state schema.
// Realtime-safety: test-only.
#include "RegressionTests.h"
#include "../Common/NativeEventLoop.h"
#include "../Common/TestUtilities.h"
#include "../../PluginProcessor.h"
#include "OutputProtectionStageTestAccess.h"
#include "../../Presets/FactoryPresetLibrary.h"
#include "../../Presets/FactoryPresetDescriptions.h"
#include "../../UI/Components/PresetBrowserOverlay.h"
#include "../../UI/Components/MotionCardStrip.h"
#include <thread>
#include <atomic>
#include <cmath>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
namespace SpatialEchoTests::System
{
namespace
{
bool setPlain(SpatialEchoAudioProcessor& p, const juce::String& id, float value)
{
    auto* parameter = p.getValueTreeState().getParameter(id);
    if (parameter == nullptr)
        return false;
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    return true;
}
}  // namespace

juce::String runOutputLatencyContractTest(SpatialEchoAudioProcessor& processor)
{
    double worstDcDelayError = 0.0;
    float worstDryError = 0.0f, worstBypassError = 0.0f;
    bool ok = true;
    int cases = 0;
    for (double rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    {
        OutputProtectionStage stage;
        stage.prepare(2);
        ok &= std::abs(OutputProtectionStageTestAccess::measuredLatency(stage) - 4.0f) < 0.001f;
        double sum = 0.0, moment = 0.0;
        for (int n = 0; n < 8192; ++n)
        {
            const float input = n == 0 ? 0.01f : 0.0f;
            const float wet = stage.process(0, input, 1.0f);
            const float dry = stage.process(1, input, 0.0f);
            sum += wet;
            moment += n * static_cast<double>(wet);
            worstDryError = std::max(worstDryError, std::abs(dry - (n == 4 ? 0.01f : 0.0f)));
        }
        ok &= std::abs(sum - 0.01) < 0.00001;
        worstDcDelayError = std::max(worstDcDelayError, std::abs(moment / sum - 4.0));

        processor.loadDefaultPreset();
        ok &= setPlain(processor, "Mix", 0.0f) && setPlain(processor, "MixLock", 0.0f) &&
              setPlain(processor, "OutputGain", 0.5f) && setPlain(processor, "OutputProtection", 0.0f) &&
              setPlain(processor, "SpatialEnabled", 0.0f);
        processor.prepareToPlay(rate, 64);
        ok &= processor.getLatencySamples() == 4;
        juce::MidiBuffer midi;
        int cursor = 0;
        // Ragged blocks cross both normal->bypass and bypass->normal boundaries.
        for (int blockSize : {0, 1, 3, 17, 64, 7, 256, 1, 129})
        {
            juce::AudioBuffer<float> buffer(2, blockSize);
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < blockSize; ++n)
                    buffer.setSample(ch, n, 0.1f * std::sin(0.05f * (static_cast<float>(cursor + n))));
            const bool bypassed = cursor >= 21 && cursor < 348;
            if (bypassed)
                processor.processBlockBypassed(buffer, midi);
            else
                processor.processBlock(buffer, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < blockSize; ++n)
                {
                    const int index = cursor + n - 4;
                    const float expected = index < 0 ? 0.0f : 0.1f * std::sin(0.05f * static_cast<float>(index));
                    const float error = std::abs(buffer.getSample(ch, n) - expected);
                    if (bypassed)
                        worstBypassError = std::max(worstBypassError, error);
                    else
                        worstDryError = std::max(worstDryError, error);
                }
            cursor += blockSize;
        }
        setPlain(processor, "OutputProtection", 1.0f);
        ok &= processor.getLatencySamples() == 4;  // no host latency change on toggle
        processor.prepareToPlay(rate, 64);
        sum = moment = 0.0;
        // Measure the actual protected processor at low level as well as the
        // utility stage. The IIR has frequency-dependent phase; DC group delay
        // is the latency contract, not a broadband parallel-null promise.
        for (int offset = 0; offset < 8192; offset += 64)
        {
            juce::AudioBuffer<float> buffer(2, 64);
            buffer.clear();
            if (offset == 0)
                for (int ch = 0; ch < 2; ++ch)
                    buffer.setSample(ch, 0, 0.01f);
            processor.processBlock(buffer, midi);
            for (int n = 0; n < 64; ++n)
            {
                sum += buffer.getSample(0, n);
                moment += (offset + n) * static_cast<double>(buffer.getSample(0, n));
            }
        }
        ok &= std::abs(sum - 0.01) < 0.00001;
        worstDcDelayError = std::max(worstDcDelayError, std::abs(moment / sum - 4.0));
        setPlain(processor, "Power", 0.0f);
        juce::AudioBuffer<float> silent(2, 64);
        silent.clear();
        processor.processBlock(silent, midi);
        ok &= processor.getLatencySamples() == 4;
        processor.processBlock(silent, midi);
        ok &= silent.getMagnitude(0, 64) == 0.0f;
        ++cases;
    }
    ok &= worstDcDelayError < 0.03 && worstDryError < 0.00001f && worstBypassError < 0.00001f;
    return "OutputLatencyContract: rates=" + juce::String(cases) +
           " dcDelayError=" + juce::String(worstDcDelayError, 6) + " dryError=" + juce::String(worstDryError, 8) +
           " bypassError=" + juce::String(worstBypassError, 8) + (ok ? " [PASS]" : " [FAIL]");
}

juce::String runSpatialPositionBatchTest(SpatialEchoAudioProcessor& processor)
{
    auto write = [&](bool second)
    {
        bool ok = setPlain(processor, "HeadPositionMode", second ? 1.0f : 0.0f);
        for (auto prefix :
             {juce::String("Spatial"), juce::String("Head1"), juce::String("Head2"), juce::String("Head3")})
        {
            ok &= setPlain(processor, prefix + "Azimuth", second ? 90.0f : -90.0f);
            std::this_thread::yield();
            ok &= setPlain(processor, prefix + "Elevation", second ? 30.0f : 0.0f);
            ok &= setPlain(processor, prefix + "Distance", second ? 2.0f : 1.0f);
        }
        return ok;
    };
    auto matches = [](const SpatialEchoAudioProcessor::SpatialPositionState& s, bool second)
    {
        const float az = second ? 90.0f : -90.0f, el = second ? 30.0f : 0.0f, d = second ? 2.0f : 1.0f;
        const auto& t = s.taps;
        const auto same = [](float a, float b) { return juce::exactlyEqual(a, b); };
        return t.positionMode == (second ? 1 : 0) && same(s.azimuth, az) && same(s.elevation, el) &&
               same(s.distance, d) && same(t.h1Azimuth, az) && same(t.h2Azimuth, az) && same(t.h3Azimuth, az) &&
               same(t.h1Elevation, el) && same(t.h2Elevation, el) && same(t.h3Elevation, el) && same(t.h1Distance, d) &&
               same(t.h2Distance, d) && same(t.h3Distance, d);
    };
    bool ok = write(false);
    processor.prepareToPlay(48000.0, 64);
    // No read primes the fallback: prepare must publish A before the first
    // callback overlaps the nested B edit below.
    {
        SpatialEchoAudioProcessor::ScopedSpatialParameterBatch outer(processor);
        {
            SpatialEchoAudioProcessor::ScopedSpatialParameterBatch inner(processor);
            ok &= write(true);
        }
        ok &= matches(processor.readSpatialPositionsForTest(), false);  // inner exit cannot publish
    }
    ok &= matches(processor.readSpatialPositionsForTest(), true);

    ok &= write(false);
    processor.releaseResources();
    processor.prepareToPlay(96000.0, 64);
    {
        SpatialEchoAudioProcessor::ScopedSpatialParameterBatch batch(processor);
        ok &= write(true);
        ok &= matches(processor.readSpatialPositionsForTest(), false);
    }

    std::atomic<bool> done{false}, writesOk{true};
    std::thread writer(
        [&]
        {
            for (int i = 0; i < 1000; ++i)
            {
                SpatialEchoAudioProcessor::ScopedSpatialParameterBatch batch(processor);
                if (!write((i & 1) != 0))
                    writesOk = false;
            }
            done.store(true, std::memory_order_release);
        });
    int reads = 0, mixed = 0;
    do
    {
        const auto state = processor.readSpatialPositionsForTest();
        if (!matches(state, false) && !matches(state, true))
            ++mixed;
        ++reads;
    } while (!done.load(std::memory_order_acquire) || reads < 10000);
    writer.join();
    ok &= writesOk && mixed == 0 && matches(processor.readSpatialPositionsForTest(), true);
    // An ordinary independently automated parameter is still consumed next read.
    setPlain(processor, "Head2Azimuth", 12.0f);
    ok &= processor.readSpatialPositionsForTest().taps.h2Azimuth == 12.0f;
    return "SpatialPositionBatch: writes=1000 reads=" + juce::String(reads) + " mixed=" + juce::String(mixed) +
           (ok ? " [PASS]" : " [FAIL]");
}

juce::String runStateSchemaBoundaryTest(SpatialEchoAudioProcessor& processor)
{
    processor.loadDefaultPreset();
    setPlain(processor, "Mix", 0.37f);
    juce::MemoryBlock original;
    processor.getStateInformation(original);
    auto xml = juce::AudioProcessor::getXmlFromBinary(original.getData(), static_cast<int>(original.getSize()));
    if (!xml)
        return "StateSchemaBoundary: missing fixture [ERROR]";
    const auto libraryBefore = processor.getUserPresetNames();
    bool ok = true;
    for (const auto* version : {"2", "0", "-1", "1future", ""})
    {
        xml->setAttribute("stateSchemaVersion", version);
        juce::MemoryBlock future;
        juce::AudioProcessor::copyXmlToBinary(*xml, future);
        processor.setStateInformation(future.getData(), static_cast<int>(future.getSize()));
        juce::MemoryBlock after;
        processor.getStateInformation(after);
        ok &= original == after;
        const auto file = SpatialEchoTests::artifactDirectory().getNonexistentChildFile("FutureSchema", ".xml");
        juce::XmlElement wrapper("SpatialEchoPreset");
        wrapper.setAttribute("name", "Future Schema Rejected");
        wrapper.addChildElement(new juce::XmlElement(*xml));
        if (!file.replaceWithText(wrapper.toString()))
            return "StateSchemaBoundary: fixture write [ERROR]";
        juce::String importedName("unchanged");
        ok &= !processor.importUserPresetFromFile(file, &importedName) && importedName == "unchanged";
        processor.getStateInformation(after);
        ok &= original == after && processor.getUserPresetNames() == libraryBefore;
        file.deleteFile();
    }
    for (bool unversioned : {false, true})
    {
        if (unversioned)
            xml->removeAttribute("stateSchemaVersion");
        else
            xml->setAttribute("stateSchemaVersion", 1);
        juce::MemoryBlock supported;
        juce::AudioProcessor::copyXmlToBinary(*xml, supported);
        setPlain(processor, "Mix", 0.12f);
        processor.setStateInformation(supported.getData(), static_cast<int>(supported.getSize()));
        ok &= std::abs(processor.getValueTreeState().getRawParameterValue("Mix")->load() - 0.37f) < 0.0001f;
    }
    return juce::String("StateSchemaBoundary: rejected=5 accepted=2 host+import unchanged=") +
           (ok ? "yes [PASS]" : "no [FAIL]");
}
juce::String runReleaseAccessibilityTest(SpatialEchoAudioProcessor& processor)
{
    juce::Component host;
    host.setBounds(50, 50, 900, 750);
    PresetBrowserOverlay browser;
    MotionCardStrip motion(processor.getValueTreeState());
    host.addAndMakeVisible(motion);
    motion.setBounds(0, 0, 900, 90);
    host.addChildComponent(browser);
    browser.setBounds(0, 90, 900, 660);
    host.addToDesktop(juce::ComponentPeer::windowIsTemporary);
    host.setVisible(true);
    host.toFront(true);
    bool ok = host.getPeer() != nullptr;
    juce::String firstFailure;
    juce::String libraryFailure;
    const auto checkpoint = [&](const char* phase)
    {
        if (!ok && firstFailure.isEmpty())
            firstFailure = phase;
    };
    checkpoint("native peer");
    int loaded = -1, saves = 0, imports = 0, renames = 0, deletes = 0, libraryClosures = 0;
    PresetBrowserOverlay::Callbacks callbacks;
    callbacks.onSave = [&] { ++saves; };
    callbacks.onImport = [&] { ++imports; };
    callbacks.onLibraryMenuClosed = [&] { ++libraryClosures; };
    callbacks.onRename = [&](const juce::String& name)
    {
        if (name == "User 39")
            ++renames;
    };
    callbacks.onDelete = [&](const juce::String& name)
    {
        if (name == "User 39")
            ++deletes;
    };
    callbacks.canSave = [] { return true; };
    browser.setCallbacks(callbacks);
    PresetBrowserOverlay::Category category;
    category.label = "User";
    for (int i = 0; i < 40; ++i)
        category.items.push_back({"User " + juce::String(i), {}, [&, i] { loaded = i; }, true, {}});
    auto open = [&] { browser.show({category}, "User 0"); };
    auto find = [&](const juce::String& title) -> juce::Component*
    {
        std::function<juce::Component*(juce::Component&)> walk = [&](juce::Component& parent) -> juce::Component*
        {
            for (auto* child : parent.getChildren())
            {
                if (child->getTitle() == title)
                    return child;
                if (auto* result = walk(*child))
                    return result;
            }
            return nullptr;
        };
        return walk(browser);
    };
    auto press = [&](const juce::String& title)
    {
        auto* component = find(title);
        auto* handler = component != nullptr ? component->getAccessibilityHandler() : nullptr;
        return handler != nullptr && handler->getActions().invoke(juce::AccessibilityActionType::press);
    };
    open();
    auto* list = find("Presets");
    auto* listHandler = list != nullptr ? list->getAccessibilityHandler() : nullptr;
    const auto* table = listHandler != nullptr ? listHandler->getTableInterface() : nullptr;
    ok &= listHandler != nullptr && listHandler->getRole() == juce::AccessibilityRole::list && table != nullptr &&
          table->getNumRows() == 40 && table->getNumColumns() == 1;
    auto* last = find("User 39");
    auto* lastHandler = last != nullptr ? last->getAccessibilityHandler() : nullptr;
    ok &= lastHandler != nullptr;
    if (lastHandler != nullptr)
    {
        ok &= lastHandler->getRole() == juce::AccessibilityRole::listItem;
        ok &= lastHandler->getParent() == listHandler && !listHandler->getChildren().empty();
        ok &= lastHandler->getActions().invoke(juce::AccessibilityActionType::focus);
        ok &= last->hasKeyboardFocus(false) &&
              browser.getWalkthroughPresetListScreenBounds().contains(last->getScreenBounds());
        ok &= last->keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && loaded == 38;
        ok &= table != nullptr && table->getRowHandler(39) == lastHandler;
        ok &= lastHandler->getActions().invoke(juce::AccessibilityActionType::toggle);
        ok &= loaded == 39 && lastHandler->getCurrentState().isSelected();
        ok &= last->keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && loaded == 38;
    }
    checkpoint("offscreen row focus and selection");
    // Rebuild while a semantic row owns focus, then exercise the native text field.
    browser.keyPressed(juce::KeyPress('9', {}, '9'));
    auto* filter = dynamic_cast<juce::TextEditor*>(find("Filter presets"));
    ok &= filter != nullptr;
    if (filter != nullptr)
    {
        filter->grabKeyboardFocus();
        filter->setText("User 39");
        filter->onTextChange();
        ok &= browser.getWalkthroughFilterText() == "User 39" && find("User 38") == nullptr;
        ok &= !filter->keyPressed(juce::KeyPress('Z', juce::ModifierKeys::commandModifier, 'z'));
        ok &= !filter->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey, {}, ' '));
        ok &= filter->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
        ok &= browser.getWalkthroughFilterText().isEmpty() && filter->getText().isEmpty();
    }
    checkpoint("native text field");
    // Settle native-window creation and queued text-field events before
    // measuring the menu's asynchronous completion. A readiness marker gives
    // this headless runner the event boundary a host supplies naturally.
    auto nativeReady = std::make_shared<bool>(false);
    juce::MessageManager::callAsync([nativeReady] { *nativeReady = true; });
    const auto readyDeadline = juce::Time::getMillisecondCounterHiRes() + 2000.0;
    while (!*nativeReady && juce::Time::getMillisecondCounterHiRes() < readyDeadline)
        SpatialEchoTests::NativeEventLoop::pumpBriefly();
    ok &= *nativeReady;
    checkpoint("native event readiness");
    // The compact footer keeps management in a real native popup menu.
    // Exercise those menu items.
    const auto pressLibraryItem = [&](const juce::String& title)
    {
        if (!*nativeReady || !juce::MessageManager::getInstance()->isThisTheMessageThread())
            return false;
        const int callbacksBefore = saves + imports;
        const int closuresBefore = libraryClosures;
        if (!press("LIBRARY"))
        {
            libraryFailure = "opener missing";
            return false;
        }
        std::function<juce::AccessibilityHandler*(juce::Component&)> findMenuItem =
            [&](juce::Component& component) -> juce::AccessibilityHandler*
        {
            auto* handler = component.getAccessibilityHandler();
            if (handler != nullptr && handler->getRole() == juce::AccessibilityRole::menuItem &&
                handler->getTitle() == title)
                return handler;
            for (auto* child : component.getChildren())
                if (auto* match = findMenuItem(*child))
                    return match;
            return nullptr;
        };
        juce::AccessibilityHandler* item = nullptr;
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumComponents() && item == nullptr; ++i)
            item = findMenuItem(*desktop.getComponent(i));
        const bool activated = item != nullptr && item->getActions().invoke(juce::AccessibilityActionType::press);
        if (!activated)
            libraryFailure = title + (item == nullptr ? ": menu item missing" : ": native press missing");
        // Plugin builds disable JUCE modal loops. Pump the macOS event loop
        // only in this native-peer regression to deliver the async menu result.
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto deadline = started + 250.0;
        int iterations = 0;
        while (activated && saves + imports == callbacksBefore && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            ++iterations;
            SpatialEchoTests::NativeEventLoop::pumpBriefly();
        }
        if (activated && saves + imports == callbacksBefore)
            libraryFailure = title +
                             ": callback not delivered, closed=" + juce::String(libraryClosures - closuresBefore) +
                             " modal=" + juce::String(juce::Component::getNumCurrentlyModalComponents()) +
                             " iterations=" + juce::String(iterations) +
                             " elapsedMs=" + juce::String(juce::Time::getMillisecondCounterHiRes() - started, 1) +
                             " mainLoop=" + juce::String(SpatialEchoTests::NativeEventLoop::isMainLoop() ? 1 : 0);
        juce::PopupMenu::dismissAllActiveMenus();
        return activated;
    };
    ok &= pressLibraryItem("Save") && saves == 1;
    checkpoint("Library Save");
    open();
    ok &= pressLibraryItem("Import...") && imports == 1;
    checkpoint("Library Import");
    open();
    ok &= press("Rename User 39") && renames == 1;
    open();
    ok &= press("Delete User 39") && deletes == 1;
    browser.dismiss();

    checkpoint("rename and delete");
    // Search the expanded catalog's actual copy through the native filter.
    // A query may match different fields of one preset, never different rows.
    int searchLoads = 0;
    const auto searchItem = [&](const char* name)
    {
        return PresetBrowserOverlay::Item{name, FactoryPresetDescriptions::find(name), [&] { ++searchLoads; }, false,
                                          FactoryPresetDescriptions::findSearchTerms(name)};
    };
    browser.show({{"Reverb Beds", {searchItem("Forest Afterglow")}},
                  {"Transitions & Impacts", {searchItem("Cloud Packets")}},
                  {"User", {{"My Vocal", {}, [&] { ++searchLoads; }, true, {}}}}},
                 "Forest Afterglow");
    auto* search = dynamic_cast<juce::TextEditor*>(find("Filter presets"));
    ok &= search != nullptr;
    const auto expectSearch = [&](const juce::String& query, int count, const juce::String& match)
    {
        if (search == nullptr)
            return false;
        search->setText(query, false);
        search->onTextChange();
        auto* presets = find("Presets");
        auto* handler = presets != nullptr ? presets->getAccessibilityHandler() : nullptr;
        const auto* filteredTable = handler != nullptr ? handler->getTableInterface() : nullptr;
        return filteredTable != nullptr && filteredTable->getNumRows() == count &&
               (match.isEmpty() || find(match) != nullptr);
    };
    ok &= expectSearch(" FoReSt ", 1, "Forest Afterglow");
    ok &= expectSearch("lingering", 1, "Forest Afterglow");
    ok &= expectSearch("acoustic", 1, "Forest Afterglow");
    ok &= expectSearch("woodland", 1, "Forest Afterglow");  // inspiration terms only
    ok &= expectSearch("impacts", 1, "Cloud Packets");
    ok &= expectSearch("acoustic forest", 1, "Forest Afterglow");
    ok &= expectSearch("forest vocal", 0, {});
    ok &= expectSearch("does-not-exist", 0, {});
    ok &= expectSearch("vocal", 1, "My Vocal");
    ok &= searchLoads == 0;  // filtering itself must never change the sound
    ok &= browser.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
    ok &= searchLoads == 1 && browser.isShowing();
    ok &= browser.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    ok &= browser.getWalkthroughFilterText().isEmpty() && find("Forest Afterglow") != nullptr &&
          find("Cloud Packets") != nullptr && find("My Vocal") != nullptr;
    browser.dismiss();

    checkpoint("native catalog search");
    // Genre and source queries must find presets across the whole factory
    // catalog through each preset's search metadata.
    std::vector<PresetBrowserOverlay::Category> factoryCategories;
    for (const auto& definition : FactoryPresetLibrary::kFactoryPresetCategories)
    {
        PresetBrowserOverlay::Category factoryCategory;
        factoryCategory.label = definition.label;
        for (const auto& preset : FactoryPresetLibrary::kFactoryPresets)
            if (std::strcmp(preset.category, definition.id) == 0)
                factoryCategory.items.push_back(searchItem(preset.name));
        factoryCategories.push_back(std::move(factoryCategory));
    }
    browser.show(factoryCategories, "Bass Reply");
    search = dynamic_cast<juce::TextEditor*>(find("Filter presets"));
    const int loadsBeforeGenreSearch = searchLoads;
    ok &= expectSearch("rap trap hats", 1, "Hat Ghost Triplets");
    ok &= expectSearch("rap trap vocal", 1, "Quarter Adlib");
    ok &= expectSearch("festival trap lead", 1, "Lead Gap Cloud");
    ok &= expectSearch("deep dubstep percussion", 1, "Halfstep Shadow");
    ok &= expectSearch("riddim room", 1, "Riddim Pocket Room");
    ok &= expectSearch("auditory rabbit", 1, "Spatial Pull");
    ok &= expectSearch("headphones showcase", 12, "Point Orbit");
    ok &= expectSearch("showcase figure eight", 1, "Sky Figure Eight");
    ok &= expectSearch("showcase turn taking", 1, "Side Relay");
    ok &= expectSearch("showcase approach", 1, "Approach Retreat");
    ok &= expectSearch("trap woodland", 0, {});
    ok &= searchLoads == loadsBeforeGenreSearch;
    ok &= expectSearch({}, static_cast<int>(FactoryPresetLibrary::kFactoryPresets.size()), "Sky Figure Eight");
    browser.dismiss();

    checkpoint("genre and source search");
    auto* firstCard = motion.getChildComponent(0);
    auto* secondCard = motion.getChildComponent(1);
    auto* firstHandler = firstCard != nullptr ? firstCard->getAccessibilityHandler() : nullptr;
    auto* secondHandler = secondCard != nullptr ? secondCard->getAccessibilityHandler() : nullptr;
    ok &= firstHandler != nullptr && secondHandler != nullptr;
    if (firstHandler != nullptr && secondHandler != nullptr)
    {
        ok &= firstHandler->getRole() == juce::AccessibilityRole::radioButton;
        ok &= firstHandler->getActions().invoke(juce::AccessibilityActionType::press);
        ok &= firstHandler->getCurrentState().isChecked() && !secondHandler->getCurrentState().isChecked();
        firstCard->grabKeyboardFocus();
        ok &= firstCard->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
        ok &= secondCard->hasKeyboardFocus(false) && secondHandler->getCurrentState().isChecked() &&
              !firstHandler->getCurrentState().isChecked();
        ok &= secondCard->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        ok &= !secondCard->keyPressed(juce::KeyPress('Z', juce::ModifierKeys::commandModifier, 'z'));
    }
    host.removeFromDesktop();
    checkpoint("motion controls");
    return juce::String("ReleaseAccessibility: firstFailure=") + firstFailure + " library=" + libraryFailure +
           " native peer, offscreen focus, selection, catalog search, actions, motion=" +
           (ok ? "verified [PASS]" : "failed [FAIL]");
}

}  // namespace SpatialEchoTests::System
#endif
