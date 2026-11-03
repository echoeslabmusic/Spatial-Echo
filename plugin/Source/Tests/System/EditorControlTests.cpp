// EditorControlTests: Editor contracts (knob reset targets, OUT/MIX text parsing, free-running head times, combo box
//   lists, drawer look in hosts, window zoom, dropdown windows, embedded brand art) and the EditorSnapshot capture
//   used for the README screenshot.

#include "SystemTests.h"
#include "../Common/NativeEventLoop.h"
#include "../../PluginProcessor.h"
#include "../../PluginEditor.h"
#include "../../UI/Components/EditorPreferences.h"
#include "../../UI/Components/CRTLookAndFeel.h"
#include "../../UI/Components/DelayPanel.h"
#include "../../UI/Components/DropdownWindowUtils.h"
#include "../../UI/Components/EchoEngineDropdown.h"
#include "../../UI/Components/MotionCardStrip.h"
#include "../../UI/Components/ReverbDropdown.h"
#include "../../UI/Components/SpatialPanel.h"
#include "../../UI/Components/SpatialHeadView.h"
#include "../../DSP/HeadRatioConstants.h"
#include "../Common/TestUtilities.h"
#include <array>
#include <functional>
#include <memory>
#include <cmath>
#include <vector>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

namespace SpatialEchoTests::System
{
namespace
{
juce::Slider* findSliderNamed(juce::Component& parent, const juce::String& name)
{
    for (auto* child : parent.getChildren())
    {
        if (child->getName() == name)
            if (auto* slider = dynamic_cast<juce::Slider*>(child))
                return slider;
        if (auto* match = findSliderNamed(*child, name))
            return match;
    }
    return nullptr;
}

void collectShowingComboBoxes(juce::Component& parent, std::vector<juce::ComboBox*>& comboBoxes)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* comboBox = dynamic_cast<juce::ComboBox*>(child))
            if (comboBox->isShowing())
                comboBoxes.push_back(comboBox);
        collectShowingComboBoxes(*child, comboBoxes);
    }
}

void collectShowingFocusTargets(juce::Component& parent, std::vector<juce::Component*>& targets)
{
    for (auto* child : parent.getChildren())
    {
        // Text fields take typed spaces by design.
        if (child->isShowing() && child->isEnabled() && child->getWantsKeyboardFocus() &&
            dynamic_cast<juce::TextEditor*>(child) == nullptr)
            targets.push_back(child);
        collectShowingFocusTargets(*child, targets);
    }
}

int countDesktopWindowsExceptTooltips()
{
    auto& desktop = juce::Desktop::getInstance();
    int windows = 0;
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (dynamic_cast<juce::TooltipWindow*>(desktop.getComponent(i)) == nullptr)
            ++windows;
    return windows;
}

juce::Component* findOpenPopupMenuWindow()
{
    for (int i = 0; i < juce::Component::getNumCurrentlyModalComponents(); ++i)
        if (auto* modal = juce::Component::getCurrentlyModalComponent(i))
            if (modal->getName() == "menu")
                return modal;
    return nullptr;
}

void pumpUntil(const std::function<bool()>& done, double timeoutMs = 300.0)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (!done() && juce::Time::getMillisecondCounterHiRes() < deadline)
        NativeEventLoop::pumpBriefly();
}

template <typename ComponentType>
ComponentType* findChildOfType(juce::Component& parent, const juce::String& name = {})
{
    for (auto* child : parent.getChildren())
    {
        if (auto* match = dynamic_cast<ComponentType*>(child))
            if (name.isEmpty() || child->getName() == name)
                return match;
        if (auto* match = findChildOfType<ComponentType>(*child, name))
            return match;
    }
    return nullptr;
}
}  // namespace

juce::String runEditorKnobResetDefaultsTest(SpatialEchoAudioProcessor& processor)
{
    // Double-click (and the knob menu's "Reset to default") must return each
    // control to its own parameter default, not a shared mid-range value.
    juce::TemporaryFile settings(SpatialEchoTests::artifactDirectory().getChildFile("editor.settings"));
    auto editor = std::make_unique<SpatialEchoAudioProcessorEditor>(
        processor, std::make_unique<EditorPreferences>(settings.getFile()));
    auto& vts = processor.getValueTreeState();

    struct Control
    {
        const char* sliderName;
        const char* parameterId;
    };
    constexpr Control controls[] = {
        {"modeSlider", "DelaySetting"},   {"repeatRateSlider", "RepeatRate"},   {"intensitySlider", "Intensity"},
        {"echoLevelSlider", "EchoLevel"}, {"sizeSlider", "ReverbSize"},         {"decaySlider", "ReverbDecay"},
        {"toneSlider", "ReverbTone"},     {"reverbLevelSlider", "ReverbLevel"}, {"tapeDriveSlider", "TapeDrive"},
    };

    int failures = 0;
    int offCentreDefaults = 0;
    juce::String firstFailure;
    auto fail = [&](const juce::String& message)
    {
        if (firstFailure.isEmpty())
            firstFailure = message;
        ++failures;
    };

    for (const auto& control : controls)
    {
        auto* slider = findSliderNamed(*editor, control.sliderName);
        auto* parameter = vts.getParameter(control.parameterId);
        if (slider == nullptr || parameter == nullptr)
        {
            fail(juce::String(control.sliderName) + " missing");
            continue;
        }

        const double expected = parameter->convertFrom0to1(parameter->getDefaultValue());
        if (!slider->isDoubleClickReturnEnabled())
        {
            fail(juce::String(control.sliderName) + " has no double-click reset");
            continue;
        }

        const double target = slider->getDoubleClickReturnValue();
        if (std::abs(target - expected) > 1.0e-4)
            fail(juce::String(control.sliderName) + " resets to " + juce::String(target, 4) + ", default is " +
                 juce::String(expected, 4));
        if (std::abs(expected - 0.5) > 1.0e-4)
            ++offCentreDefaults;

        // Applying the reset value lands the parameter on its default.
        const float previous = parameter->getValue();
        parameter->setValueNotifyingHost(parameter->getDefaultValue() < 0.5f ? 0.9f : 0.1f);
        slider->setValue(target, juce::sendNotificationSync);
        if (std::abs(parameter->getValue() - parameter->getDefaultValue()) > 1.0e-3f)
            fail(juce::String(control.parameterId) + " did not return to its default");
        parameter->setValueNotifyingHost(previous);
    }

    // Feedback, Size, Decay and Drive have defaults away from 0.5, so a shared
    // hard-coded reset value cannot pass by coincidence.
    const bool coversOffCentre = offCentreDefaults >= 4;
    const bool passed = failures == 0 && coversOffCentre;

    juce::String result;
    result << "EditorKnobResetDefaults: checked=" << static_cast<int>(std::size(controls)) << " failures=" << failures
           << " offCentreDefaults=" << offCentreDefaults;
    if (firstFailure.isNotEmpty())
        result << " first=" << firstFailure;
    result << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runOutputMixTextParsingTest(SpatialEchoAudioProcessor& processor)
{
    // OUT and MIX take typed text with or without units; the header fields
    // commit through the parameters' own text conversion.
    auto& vts = processor.getValueTreeState();
    auto* output = vts.getParameter("OutputGain");
    auto* mix = vts.getParameter("Mix");
    if (output == nullptr || mix == nullptr)
        return "OutputMixTextParsing: OutputGain or Mix missing [FAIL]";

    struct Case
    {
        juce::AudioProcessorParameter* parameter;
        const char* text;
        float expectedNormalised;
    };
    const Case cases[] = {
        {output, "-3 dB", 0.375f}, {output, "-3", 0.375f}, {output, "+6 dB", 0.75f}, {output, "0 dB", 0.5f},
        {output, "-40 dB", 0.0f},  {output, "40", 1.0f},   {mix, "50%", 0.5f},       {mix, "50", 0.5f},
        {mix, "100%", 1.0f},       {mix, "0", 0.0f},       {mix, "150%", 1.0f},      {mix, "37 %", 0.37f},
    };

    int failures = 0;
    juce::String firstFailure;
    for (const auto& testCase : cases)
    {
        const float parsed = testCase.parameter->getValueForText(testCase.text);
        if (std::abs(parsed - testCase.expectedNormalised) > 1.0e-4f)
        {
            ++failures;
            if (firstFailure.isEmpty())
                firstFailure = testCase.parameter->getName(32) + " \"" + testCase.text + "\" -> " +
                               juce::String(parsed, 4) + " expected " + juce::String(testCase.expectedNormalised, 4);
        }
    }

    // Displayed text parses back to the value it shows.
    for (auto* parameter : {output, mix})
    {
        const float value = parameter == output ? 0.375f : 0.42f;
        const auto shown = parameter->getText(value, 32);
        if (std::abs(parameter->getValueForText(shown) - value) > 1.0e-3f)
        {
            ++failures;
            if (firstFailure.isEmpty())
                firstFailure = "round trip of \"" + shown + "\"";
        }
    }

    juce::String result;
    result << "OutputMixTextParsing: failures=" << failures;
    if (firstFailure.isNotEmpty())
        result << " first=" << firstFailure;
    result << " " << (failures == 0 ? "[PASS]" : "[FAIL]");
    return result;
}
juce::String runEditorWindowZoomTest(SpatialEchoAudioProcessor& processor)
{
    // Window Size zoom must scale the whole editor (layout untouched), persist
    // per user, and compose with host display scaling (Windows 150%).
    juce::TemporaryFile settings(artifactDirectory().getChildFile("editor.settings"));
    auto editor = std::make_unique<SpatialEchoAudioProcessorEditor>(
        processor, std::make_unique<EditorPreferences>(settings.getFile()));
    juce::Component host;
    host.addAndMakeVisible(*editor);

    auto hostArea = [&] { return host.getLocalArea(editor.get(), editor->getLocalBounds().toFloat()); };
    auto near = [](float a, float b) { return std::abs(a - b) < 0.6f; };
    auto areaIs = [&](float w, float h)
    {
        const auto a = hostArea();
        return near(a.getWidth(), w) && near(a.getHeight(), h);
    };

    const bool defaultFull = editor->getWindowZoom() == 1.0f && areaIs(1400.0f, 650.0f) && editor->getWidth() == 1400 &&
                             editor->getHeight() == 650;

    editor->setWindowZoom(0.75f);
    const bool zoomedSmall = areaIs(1050.0f, 487.5f) && editor->getWidth() == 1400;
    const bool persisted = EditorPreferences(settings.getFile()).getWindowZoom() == 0.75f;

    editor->setScaleFactor(1.5f);  // host display scaling
    const bool composesWithHost = areaIs(1575.0f, 731.25f);
    editor->setWindowZoom(1.0f);
    const bool hostScaleKept = areaIs(2100.0f, 975.0f);
    editor->setScaleFactor(1.0f);
    editor->setWindowZoom(0.3f);
    const bool clamped =
        editor->getWindowZoom() == 0.75f && EditorPreferences(settings.getFile()).getWindowZoom() == 0.75f;

    const bool passed = defaultFull && zoomedSmall && persisted && composesWithHost && hostScaleKept && clamped;
    juce::String result;
    result << "EditorWindowZoom:"
           << " default100=" << (defaultFull ? "ok" : "BAD") << " zoom75=" << (zoomedSmall ? "ok" : "BAD")
           << " persisted=" << (persisted ? "ok" : "BAD") << " withHost150=" << (composesWithHost ? "ok" : "BAD")
           << " hostKept=" << (hostScaleKept ? "ok" : "BAD") << " clamped=" << (clamped ? "ok" : "BAD") << " "
           << (passed ? "[PASS]" : "[FAIL]");
    host.removeChildComponent(editor.get());
    return result;
}

juce::String runEditorCreationSmokeTest(SpatialEchoAudioProcessor& processor)
{
    if (!processor.hasEditor())
        return "EditorCreationSmoke: processor reports no editor [FAIL]";

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    if (editor == nullptr)
        return "EditorCreationSmoke: createEditor returned null [FAIL]";

    const int width = editor->getWidth();
    const int height = editor->getHeight();
    const bool passed = width > 0 && height > 0;

    juce::String result;
    result << "EditorCreationSmoke: size=" << width << "x" << height << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runDropdownWindowPlacementBoundsTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const juce::Rectangle<int> displayArea(0, 0, 2000, 1200);
    const juce::Rectangle<int> editorArea(40, 50, 1400, 650);
    const auto usableEditorArea = editorArea.reduced(8);
    const juce::Rectangle<int> abletonDisplayArea(0, 0, 1512, 980);
    const juce::Rectangle<int> abletonEditorArea(322, 64, 1400, 669);
    const auto abletonVisibleEditorLocal = abletonDisplayArea.getIntersection(abletonEditorArea)
                                               .reduced(8)
                                               .translated(-abletonEditorArea.getX(), -abletonEditorArea.getY());

    const juce::Rectangle<int> rightEdgeButton(1250, 160, 120, 32);
    const auto rightEdgePlacement =
        SpatialEchoDropdownWindow::calculateConstrainedBounds(rightEdgeButton, {420, 500}, displayArea, editorArea);

    const juce::Rectangle<int> lowButton(540, 640, 120, 32);
    const auto tallPlacement =
        SpatialEchoDropdownWindow::calculateConstrainedBounds(lowButton, {520, 900}, displayArea, editorArea);

    const juce::Rectangle<int> abletonSpatialButton(1294, 86, 120, 32);
    const auto abletonPlacement = SpatialEchoDropdownWindow::calculateConstrainedBounds(
        abletonSpatialButton, {420, 500}, abletonDisplayArea, abletonEditorArea);
    const auto abletonNativeChildBounds =
        SpatialEchoDropdownWindow::convertScreenBoundsForNativeParent(abletonPlacement.bounds, abletonEditorArea, true);
    const auto unparentedBounds = SpatialEchoDropdownWindow::convertScreenBoundsForNativeParent(
        abletonPlacement.bounds, abletonEditorArea, false);

    const bool rightEdgeFits = usableEditorArea.contains(rightEdgePlacement.bounds);
    const bool tallFits = usableEditorArea.contains(tallPlacement.bounds);
    const bool tallWasConstrained = tallPlacement.bounds.getHeight() < 900;
    const bool abletonNativeChildFits = abletonVisibleEditorLocal.contains(abletonNativeChildBounds);
    const bool abletonWasConverted =
        abletonNativeChildBounds.getPosition() ==
        abletonPlacement.bounds.getPosition().translated(-abletonEditorArea.getX(), -abletonEditorArea.getY());
    const bool unparentedPreserved = unparentedBounds == abletonPlacement.bounds;
    const bool passed = rightEdgeFits && tallFits && tallWasConstrained && abletonNativeChildFits &&
                        abletonWasConverted && unparentedPreserved;

    juce::String result;
    result << "DropdownWindowPlacementBounds: rightEdge=" << rightEdgePlacement.bounds.toString()
           << " tall=" << tallPlacement.bounds.toString() << " abletonScreen=" << abletonPlacement.bounds.toString()
           << " abletonNative=" << abletonNativeChildBounds.toString() << " editor=" << usableEditorArea.toString()
           << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runDropdownWindowHostOwnsChildrenTest(SpatialEchoAudioProcessor& /*processor*/)
{
    struct CountingComponent final : public juce::Component
    {
        explicit CountingComponent(int& counterIn)
            : counter(counterIn)
        {
        }
        ~CountingComponent() override { ++counter; }

        int& counter;
    };

    struct CountingViewport final : public juce::Viewport
    {
        explicit CountingViewport(int& counterIn)
            : counter(counterIn)
        {
        }
        ~CountingViewport() override { ++counter; }

        int& counter;
    };

    int directChildDestroyed = 0;
    int viewportDestroyed = 0;
    int viewedContentDestroyed = 0;

    {
        auto host = std::make_unique<SpatialEchoDropdownWindow::Host>();
        host->addAndMakeVisible(new CountingComponent(directChildDestroyed));
    }

    {
        auto host = std::make_unique<SpatialEchoDropdownWindow::Host>();
        auto* viewport = new CountingViewport(viewportDestroyed);
        viewport->setViewedComponent(new CountingComponent(viewedContentDestroyed), true);
        host->addAndMakeVisible(viewport);
    }

    const bool passed = directChildDestroyed == 1 && viewportDestroyed == 1 && viewedContentDestroyed == 1;

    juce::String result;
    result << "DropdownWindowHostOwnsChildren: direct=" << directChildDestroyed << " viewport=" << viewportDestroyed
           << " viewed=" << viewedContentDestroyed << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runBrandSealEmbeddedTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // The About dialog draws the seal from BinaryData; installed bundles have
    // no source tree to fall back on, so the embedded SVG must parse and paint.
    constexpr const char* expectedFile = "echoes-lab-seal-green.svg";
    constexpr const char* expectedResource = "echoeslabsealgreen_svg";

    int resourceSize = 0;
    const char* resourceData = BinaryData::getNamedResource(expectedResource, resourceSize);
    const bool binaryOk =
        (resourceData != nullptr) && (resourceSize > 0) &&
        (juce::String(BinaryData::getNamedResourceOriginalFilename(expectedResource)) == expectedFile);

    std::unique_ptr<juce::Drawable> seal;
    if (binaryOk)
        if (auto svgXml = juce::XmlDocument::parse(juce::String::fromUTF8(resourceData, resourceSize)))
            seal = juce::Drawable::createFromSVG(*svgXml);

    const auto bounds = seal != nullptr ? seal->getDrawableBounds() : juce::Rectangle<float>();
    const bool parseOk = seal != nullptr && !bounds.isEmpty();

    int inkPixels = 0;
    if (parseOk)
    {
        juce::Image image(juce::Image::ARGB, 256, 256, true);
        {
            juce::Graphics g(image);
            seal->drawWithin(g, image.getBounds().toFloat(), juce::RectanglePlacement::centred, 1.0f);
        }
        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
                if (image.getPixelAt(x, y).getAlpha() > 0)
                    ++inkPixels;
    }
    const bool renderOk = inkPixels > 0;

    const bool passed = binaryOk && parseOk && renderOk;

    juce::String result;
    result << "BrandSealEmbedded:"
           << " binary=" << (binaryOk ? "ok" : "fail") << " bytes=" << resourceSize
           << " parse=" << (parseOk ? "ok" : "fail") << " bounds=" << bounds.getWidth() << "x" << bounds.getHeight()
           << " inkPixels=" << inkPixels << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

// Renders the editor offscreen at its default size and writes a 2x PNG (the
// README screenshot). The processor is created as the standalone app creates
// it, so it opens on the first factory preset like a fresh instance. The
// output path comes from SPATIAL_ECHO_SNAPSHOT_PATH, or spatial-echo.png in
// the artifact directory. The editor is never put on screen, so the head view
// keeps its juce::Graphics renderer and the capture needs no OpenGL. The run
// fails if the head view region comes out blank.
juce::String runEditorSnapshot()
{
    // Only the wrapper-hosted path auto-loads the first factory preset.
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
    SpatialEchoAudioProcessor processor;
    juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
    processor.prepareToPlay(48000.0, 512);

    // A fresh preference file with the tour already seen, so the capture shows
    // the plain interface.
    const auto settingsFile = artifactDirectory().getChildFile("snapshot.settings");
    settingsFile.deleteFile();
    EditorPreferences(settingsFile).markWalkthroughSeen();

    auto editor =
        std::make_unique<SpatialEchoAudioProcessorEditor>(processor, std::make_unique<EditorPreferences>(settingsFile));

    // Let timers and deferred layout run for half a second.
    const auto settleUntil = juce::Time::getMillisecondCounterHiRes() + 500.0;
    while (juce::Time::getMillisecondCounterHiRes() < settleUntil)
        NativeEventLoop::pumpBriefly();

    constexpr float kScale = 2.0f;
    const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, kScale);

    std::function<SpatialHeadView*(juce::Component&)> findHeadView = [&](juce::Component& component) -> SpatialHeadView*
    {
        if (auto* view = dynamic_cast<SpatialHeadView*>(&component))
            return view;
        for (auto* child : component.getChildren())
            if (auto* view = findHeadView(*child))
                return view;
        return nullptr;
    };

    // The head view draws a thin green wireframe on a black background; a
    // blank capture has no bright green pixels in its area.
    int greenPixels = 0;
    int headPixels = 0;
    if (auto* headView = findHeadView(*editor))
    {
        const auto area = (editor->getLocalArea(headView, headView->getLocalBounds()).toFloat() * kScale)
                              .getSmallestIntegerContainer()
                              .getIntersection(image.getBounds());
        for (int y = area.getY(); y < area.getBottom(); y += 2)
            for (int x = area.getX(); x < area.getRight(); x += 2)
            {
                const auto pixel = image.getPixelAt(x, y);
                ++headPixels;
                if (pixel.getGreen() >= 96 && pixel.getGreen() > pixel.getRed() + 24)
                    ++greenPixels;
            }
    }
    const bool headViewDrawn = headPixels > 0 && greenPixels >= 50;

    editor.reset();
    processor.releaseResources();
    settingsFile.deleteFile();

    const auto requested = juce::SystemStats::getEnvironmentVariable("SPATIAL_ECHO_SNAPSHOT_PATH", {}).trim();
    const auto output = requested.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile(requested)
                                               : artifactDirectory().getChildFile("spatial-echo.png");
    output.getParentDirectory().createDirectory();
    output.deleteFile();
    bool written = false;
    if (auto stream = output.createOutputStream())
        written = juce::PNGImageFormat().writeImageToStream(image, *stream);

    const bool passed = image.isValid() && written && headViewDrawn;
    juce::String result;
    result << "EditorSnapshot: " << image.getWidth() << "x" << image.getHeight() << " headView green " << greenPixels
           << "/" << headPixels << " file=" << output.getFullPathName() << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runFreeHeadTimeReadoutTest(SpatialEchoAudioProcessor& processor)
{
    // Free-running head readouts: H1 shows the same number as the Rate
    // readout, and every head is within half a millisecond of its true time
    // (175..70 ms base on a 0.6 power curve, times the head ratio).
    juce::TemporaryFile settings(SpatialEchoTests::artifactDirectory().getChildFile("editor.settings"));
    auto editor = std::make_unique<SpatialEchoAudioProcessorEditor>(
        processor, std::make_unique<EditorPreferences>(settings.getFile()));
    auto& vts = processor.getValueTreeState();
    auto* rate = vts.getParameter("RepeatRate");
    auto* ratioPreset = vts.getParameter("HeadRatioPreset");
    auto* panel = findChildOfType<DelayPanel>(*editor);
    std::array<juce::Button*, 3> heads{};
    for (size_t head = 0; head < heads.size() && panel != nullptr; ++head)
        heads[head] = findChildOfType<juce::Button>(*panel, "head" + juce::String(head + 1) + "ValueButton");
    if (rate == nullptr || ratioPreset == nullptr || panel == nullptr || heads[0] == nullptr || heads[1] == nullptr ||
        heads[2] == nullptr)
        return "FreeHeadTimeReadout: RepeatRate, HeadRatioPreset, delay panel or head readouts missing [FAIL]";

    constexpr std::array<std::array<float, 3>, 4> ratios{{
        {1.0f, HeadRatios::CLASSIC_RATIO_2, HeadRatios::CLASSIC_RATIO_3},
        {1.0f, HeadRatios::IDEALIZED_RATIO_2, HeadRatios::IDEALIZED_RATIO_3},
        {1.0f, HeadRatios::GOLDEN_RATIO_2, HeadRatios::GOLDEN_RATIO_3},
        {1.0f, HeadRatios::LINEAR_RATIO_2, HeadRatios::LINEAR_RATIO_3},
    }};
    constexpr int rateSteps = 200;
    const float previousRate = rate->getValue();
    const float previousPreset = ratioPreset->getValue();

    int checked = 0;
    int failures = 0;
    juce::String firstFailure;
    for (size_t preset = 0; preset < ratios.size(); ++preset)
    {
        ratioPreset->setValueNotifyingHost(ratioPreset->convertTo0to1(static_cast<float>(preset)));
        for (int step = 0; step <= rateSteps; ++step)
        {
            rate->setValueNotifyingHost(static_cast<float>(step) / static_cast<float>(rateSteps));
            panel->updateHeadDisplays(false, 10);

            const float baseMs = 175.0f - std::pow(rate->convertFrom0to1(rate->getValue()), 0.6f) * 105.0f;
            juce::String mismatch;
            if (heads[0]->getButtonText() != rate->getCurrentValueAsText())
                mismatch << "H1 " << heads[0]->getButtonText() << " vs Rate " << rate->getCurrentValueAsText();
            for (size_t head = 0; head < heads.size() && mismatch.isEmpty(); ++head)
            {
                const float shownMs = heads[head]->getButtonText().getFloatValue();
                const float trueMs = baseMs * ratios[preset][head];
                if (std::abs(shownMs - trueMs) > 0.5f)
                    mismatch << "H" << static_cast<int>(head + 1) << " " << heads[head]->getButtonText() << " vs "
                             << juce::String(trueMs, 2) << " ms";
            }
            ++checked;
            if (mismatch.isNotEmpty())
            {
                if (firstFailure.isEmpty())
                    firstFailure = "preset " + juce::String(static_cast<int>(preset)) + " step " + juce::String(step) +
                                   ": " + mismatch;
                ++failures;
            }
        }
    }
    rate->setValueNotifyingHost(previousRate);
    ratioPreset->setValueNotifyingHost(previousPreset);

    juce::String result;
    result << "FreeHeadTimeReadout: checked=" << checked << " failures=" << failures;
    if (firstFailure.isNotEmpty())
        result << " first=" << firstFailure;
    result << " " << (failures == 0 ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runComboBoxPopupsFitTest(SpatialEchoAudioProcessor& processor)
{
    // Every combo box uses the plug-in's look-and-feel (drawers are their own
    // desktop windows, and hosts keep JUCE's stock default) and opens its list
    // with every item visible: none trimmed to a scrolling stub at a window's
    // edge (RETRIG sits on the editor's bottom row while fine-tune is open).
    juce::TemporaryFile settings(SpatialEchoTests::artifactDirectory().getChildFile("editor.settings"));
    auto editor = std::make_unique<SpatialEchoAudioProcessorEditor>(
        processor, std::make_unique<EditorPreferences>(settings.getFile()));
    auto& vts = processor.getValueTreeState();
    const auto stateBefore = vts.copyState();
    editor->setTopLeftPosition(40, 60);
    editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
    editor->setVisible(true);

    int checked = 0;
    int failures = 0;
    bool retrigChecked = false;
    juce::String firstFailure;
    auto fail = [&](const juce::String& message)
    {
        firstFailure << (firstFailure.isEmpty() ? "" : "; ") << message;
        ++failures;
    };

    auto checkComboBoxes = [&](juce::Component& root, const juce::String& where)
    {
        std::vector<juce::ComboBox*> comboBoxes;
        collectShowingComboBoxes(root, comboBoxes);
        for (auto* comboBox : comboBoxes)
        {
            const auto name = where + " " + comboBox->getTitle();
            if (dynamic_cast<CRTLookAndFeel*>(&comboBox->getLookAndFeel()) == nullptr)
                fail(name + " uses JUCE's stock look-and-feel");
            comboBox->showPopup();
            const juce::Component::SafePointer<juce::Component> menu = findOpenPopupMenuWindow();
            if (menu == nullptr)
            {
                fail(name + " opened no list");
                continue;
            }

            ++checked;
            retrigChecked = retrigChecked || comboBox->getTitle() == "Spatial Restart";
            int items = 0;
            int hidden = 0;
            for (auto* item : menu->getChildren())
            {
                ++items;
                if (!menu->getLocalBounds().contains(item->getBounds()))
                    ++hidden;
            }
            const auto* parent = menu->getParentComponent();
            const bool inside = parent == nullptr || parent->getLocalBounds().contains(menu->getBounds());
            if (items < comboBox->getNumItems() || hidden > 0 || !inside)
                fail(name + ": " + juce::String(hidden) + " of " + juce::String(items) + " items hidden, combo has " +
                     juce::String(comboBox->getNumItems()) + (inside ? "" : ", list outside its window"));

            juce::PopupMenu::dismissAllActiveMenus();
            pumpUntil([&] { return menu == nullptr || !menu->isVisible(); });
        }
    };

    if (auto* spatial = findChildOfType<SpatialPanel>(*editor))
    {
        spatial->setWalkthroughMotionFineTuneVisible(true);
        checkComboBoxes(*editor, "fine-tune");
        spatial->setWalkthroughMotionFineTuneVisible(false);
        spatial->setWalkthroughBlendExpanded(true);
        checkComboBoxes(*editor, "routing");
        spatial->setWalkthroughBlendExpanded(false);
    }
    else
    {
        fail("spatial panel missing");
    }

    if (auto* echoEngine = findChildOfType<EchoEngineDropdown>(*editor))
    {
        echoEngine->showDropdown();
        if (auto* window = echoEngine->getDropdownWindowForTesting())
            checkComboBoxes(*window, "echo engine");
        echoEngine->hideDropdown();
    }

    auto* reverb = findChildOfType<ReverbDropdown>(*editor);
    auto* reverbEngine = vts.getParameter("ReverbEngine");
    if (reverb != nullptr && reverbEngine != nullptr)
    {
        const float previousEngine = reverbEngine->getValue();
        for (const float engine : {0.0f, 1.0f})
        {
            reverbEngine->setValueNotifyingHost(engine);
            reverb->showDropdown();
            if (auto* window = reverb->getDropdownWindowForTesting())
                checkComboBoxes(*window, engine < 0.5f ? "spring reverb" : "cloud reverb");
            reverb->hideDropdown();
        }
        reverbEngine->setValueNotifyingHost(previousEngine);
    }

    editor->removeFromDesktop();
    const bool stateKept = stateBefore.isEquivalentTo(vts.copyState());
    const bool passed = failures == 0 && checked >= 7 && retrigChecked && stateKept;
    juce::String result;
    result << "ComboBoxPopupsFit: checked=" << checked << " failures=" << failures
           << " retrig=" << (retrigChecked ? "yes" : "no") << " stateKept=" << (stateKept ? "yes" : "no");
    if (firstFailure.isNotEmpty())
        result << " first=" << firstFailure;
    result << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runDrawerLookMatchesStandaloneTest(SpatialEchoAudioProcessor& processor)
{
    // The drawers are their own desktop windows. The Standalone makes the
    // plug-in's look the app default; hosts keep JUCE's. Each drawer must
    // render identically either way, every control included.
    auto renderDrawers = [&](bool pluginLookIsDefault)
    {
        CRTLookAndFeel appDefault;
        if (pluginLookIsDefault)
            juce::LookAndFeel::setDefaultLookAndFeel(&appDefault);
        std::vector<juce::Image> images;
        {
            juce::TemporaryFile settings(SpatialEchoTests::artifactDirectory().getChildFile("editor.settings"));
            SpatialEchoAudioProcessorEditor editor(processor, std::make_unique<EditorPreferences>(settings.getFile()));
            editor.setTopLeftPosition(40, 60);
            editor.addToDesktop(juce::ComponentPeer::windowIsTemporary);
            editor.setVisible(true);
            for (auto* drawer : std::initializer_list<SectionDropdown*>{findChildOfType<ReverbDropdown>(editor),
                                                                        findChildOfType<EchoEngineDropdown>(editor)})
            {
                if (drawer == nullptr)
                    continue;
                drawer->showDropdown();
                if (auto* window = drawer->getDropdownWindowForTesting())
                    images.push_back(window->createComponentSnapshot(window->getLocalBounds(), true, 1.0f));
                drawer->hideDropdown();
            }
            editor.removeFromDesktop();
        }
        if (pluginLookIsDefault)
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        return images;
    };

    const auto host = renderDrawers(false);
    const auto standalone = renderDrawers(true);
    int differingPixels = 0;
    bool sameShape = host.size() == 2 && standalone.size() == 2;
    for (size_t i = 0; sameShape && i < host.size(); ++i)
    {
        sameShape = host[i].getBounds() == standalone[i].getBounds();
        for (int y = 0; sameShape && y < host[i].getHeight(); ++y)
            for (int x = 0; x < host[i].getWidth(); ++x)
                if (host[i].getPixelAt(x, y) != standalone[i].getPixelAt(x, y))
                    ++differingPixels;
    }

    const bool passed = sameShape && differingPixels == 0;
    juce::String result;
    result << "DrawerLookMatchesStandalone: drawers=" << static_cast<int>(host.size()) << "/"
           << static_cast<int>(standalone.size()) << " sameSize=" << (sameShape ? "yes" : "no")
           << " differingPixels=" << differingPixels << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpaceReachesHostTest(SpatialEchoAudioProcessor& processor)
{
    // Space starts and stops the host transport. Whichever control has focus,
    // in the editor, in a drawer or under the tour, Space stays unhandled: no
    // window or menu opens or closes and no parameter changes.
    juce::TemporaryFile settings(SpatialEchoTests::artifactDirectory().getChildFile("editor.settings"));
    EditorPreferences(settings.getFile()).markWalkthroughSeen();  // the tour gets its own check below
    auto editor = std::make_unique<SpatialEchoAudioProcessorEditor>(
        processor, std::make_unique<EditorPreferences>(settings.getFile()));
    auto& vts = processor.getValueTreeState();
    editor->setTopLeftPosition(40, 60);
    editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
    editor->setVisible(true);
    editor->toFront(true);

    auto* echoEngine = findChildOfType<EchoEngineDropdown>(*editor);
    auto* reverb = findChildOfType<ReverbDropdown>(*editor);
    const juce::KeyPress space(juce::KeyPress::spaceKey, juce::ModifierKeys(), ' ');
    int pressed = 0;
    int drawerHeaders = 0;
    int motionCards = 0;
    int failures = 0;
    juce::String firstFailure;
    auto fail = [&](const juce::String& message)
    {
        firstFailure << (firstFailure.isEmpty() ? "" : "; ") << message;
        ++failures;
    };

    auto pressSpaceOnEach = [&](juce::Component& root, const juce::String& where)
    {
        std::vector<juce::Component*> targets;
        collectShowingFocusTargets(root, targets);
        const int windowsBefore = countDesktopWindowsExceptTooltips();
        for (auto* target : targets)
        {
            target->grabKeyboardFocus();
            auto* peer = target->getPeer();
            if (peer == nullptr || !target->hasKeyboardFocus(false))
                continue;

            ++pressed;
            drawerHeaders += (target == echoEngine || target == reverb) ? 1 : 0;
            motionCards += dynamic_cast<MotionCardStrip*>(target->getParentComponent()) != nullptr ? 1 : 0;
            const auto name = where + " " + (target->getTitle().isNotEmpty() ? target->getTitle() : target->getName());
            const auto stateBefore = vts.copyState();
            if (peer->handleKeyPress(space))
                fail(name + " handled Space");
            pumpUntil([] { return false; }, 30.0);  // give a deferred menu time to appear
            if (countDesktopWindowsExceptTooltips() != windowsBefore || findOpenPopupMenuWindow() != nullptr)
                fail(name + " opened or closed a window");
            if (!stateBefore.isEquivalentTo(vts.copyState()))
                fail(name + " changed the plug-in state");

            juce::PopupMenu::dismissAllActiveMenus();
            if (&root == editor.get())
                for (auto* drawer : {static_cast<SectionDropdown*>(echoEngine), static_cast<SectionDropdown*>(reverb)})
                    if (drawer != nullptr && drawer->isDropdownVisible())
                        drawer->hideDropdown();
        }
    };

    pressSpaceOnEach(*editor, "editor");

    for (const auto& [drawer, where] : {std::pair<SectionDropdown*, const char*>{echoEngine, "echo engine drawer"},
                                        std::pair<SectionDropdown*, const char*>{reverb, "reverb drawer"}})
    {
        if (drawer == nullptr)
        {
            fail(juce::String(where) + " missing");
            continue;
        }
        drawer->showDropdown();
        if (auto* window = drawer->getDropdownWindowForTesting())
            pressSpaceOnEach(*window, where);
        else
            fail(juce::String(where) + " opened no window");
        drawer->hideDropdown();
    }

    // The tour's coach layer sits above the editor and sees keys first.
    editor->startWalkthroughForTesting();
    bool tourChecked = false;
    if (auto* peer = editor->getPeer())
    {
        tourChecked = true;
        ++pressed;
        if (peer->handleKeyPress(space))
            fail("tour handled Space");
    }
    editor->exitWalkthroughForTesting();

    editor->removeFromDesktop();
    const bool passed = failures == 0 && drawerHeaders == 2 && motionCards > 0 && tourChecked;
    juce::String result;
    result << "SpaceReachesHost: pressed=" << pressed << " drawerHeaders=" << drawerHeaders
           << " motionCards=" << motionCards << " tour=" << (tourChecked ? "yes" : "no") << " failures=" << failures;
    if (firstFailure.isNotEmpty())
        result << " first=" << firstFailure;
    result << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::System

#endif
