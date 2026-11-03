// SpatialPresetRow (implementation): Spatial-preset stepper: prev / name / next + REVERT.

#include "SpatialPresetRow.h"
#include "SpatialPresetCatalog.h"
#include "MenuChrome.h"

SpatialPresetRow::SpatialPresetRow(juce::AudioProcessorValueTreeState& apvts)
    : valueTreeState(apvts)
{
    for (auto* b : {&prevButton, &nextButton, &revertButton})
    {
        b->setLookAndFeel(&selectorLookAndFeel);
        addAndMakeVisible(*b);
    }
    prevButton.setTooltip("Previous spatial preset.");
    nextButton.setTooltip("Next spatial preset.");
    revertButton.setTooltip("Return to the last selected spatial preset.");

    prevButton.onClick = [this] { stepPreset(-1); };
    nextButton.onClick = [this] { stepPreset(1); };
    revertButton.onClick = [this]
    {
        if (lastAppliedIndex >= 0)
            applyPreset(lastAppliedIndex);
    };

    nameLabel.setJustificationType(juce::Justification::centred);
    nameLabel.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
    nameLabel.setColour(juce::Label::textColourId,
                        CRTLookAndFeel::getTerminalGreen().withAlpha(UIConstants::alphaBright));
    nameLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel);
    setTitle("spatial preset");
    setTooltip("Click the name to open the spatial preset list. "
               "Arrows step through; REVERT returns to the last preset.");

    // Seed from whatever state the session opens with.
    lastAppliedIndex =
        SpatialPresetCatalog::findMatchingPresetIndex(SpatialPresetCatalog::readSnapshot(valueTreeState));
    startTimerHz(10);
    timerCallback();
}

SpatialPresetRow::~SpatialPresetRow()
{
    stopTimer();
    for (auto* b : {&prevButton, &nextButton, &revertButton})
        b->setLookAndFeel(nullptr);
}

void SpatialPresetRow::showWalkthroughMenu()
{
    if (!libraryMenuVisible)
        showPresetMenu();
}

void SpatialPresetRow::resized()
{
    // Single header row: < NAME > [REVERT]. The revert slot is always
    // reserved so its show/hide (deviation-gated) never reflows the name,
    // and the left edge mirrors it so the stepper cluster stays centred
    // in the row (the panel centres the row over the head view).
    auto area = getLocalBounds();
    revertButton.setBounds(area.removeFromRight(58).reduced(0, 1));
    area.removeFromRight(6);
    area.removeFromLeft(64);
    prevButton.setBounds(area.removeFromLeft(20));
    nextButton.setBounds(area.removeFromRight(20));
    nameLabel.setBounds(area.reduced(2, 0));
    nameClickBounds = nameLabel.getBounds();
}

void SpatialPresetRow::mouseUp(const juce::MouseEvent& event)
{
    if (!nameClickBounds.contains(event.getPosition()))
        return;
    showPresetMenu();
}

void SpatialPresetRow::showPresetMenu()
{
    if (libraryMenuVisible)
        return;

    const int presetCount = (int)SpatialPresetCatalog::kDefinitions.size();
    const int currentMatch =
        SpatialPresetCatalog::findMatchingPresetIndex(SpatialPresetCatalog::readSnapshot(valueTreeState));

    juce::PopupMenu menu;
    for (int index = 0; index < presetCount; ++index)
    {
        if (index == SpatialPresetCatalog::kMenuItemsPerColumn)
            menu.addColumnBreak();

        const auto& definition = SpatialPresetCatalog::kDefinitions[(size_t)index];
        menu.addItem(index + 1, definition.displayName, true, index == currentMatch);
    }

    libraryMenuVisible = true;
    if (onLibraryMenuVisibility)
        onLibraryMenuVisibility(true);

    menu.showMenuAsync(MenuChrome::popupOptions(*this, localAreaToGlobal(nameClickBounds)),
                       [safe = juce::Component::SafePointer<SpatialPresetRow>(this)](int result)
                       {
                           if (safe == nullptr)
                               return;
                           safe->libraryMenuVisible = false;
                           if (safe->onLibraryMenuVisibility)
                               safe->onLibraryMenuVisibility(false);
                           if (result > 0)
                               safe->applyPreset(result - 1);
                       });
}

void SpatialPresetRow::stepPreset(int delta)
{
    const int count = (int)SpatialPresetCatalog::kDefinitions.size();
    const int current =
        SpatialPresetCatalog::findMatchingPresetIndex(SpatialPresetCatalog::readSnapshot(valueTreeState));
    // From CUSTOM, next goes to the first preset, prev to the last.
    const int base = current >= 0 ? current : (delta > 0 ? -1 : count);
    applyPreset(((base + delta) % count + count) % count);
}

void SpatialPresetRow::applyPreset(int index)
{
    if (SpatialPresetCatalog::applyPresetToParameters(valueTreeState, index, true))
    {
        lastAppliedIndex = index;
        displayedIndex = -2;
        timerCallback();
    }
}

void SpatialPresetRow::timerCallback()
{
    const int match = SpatialPresetCatalog::findMatchingPresetIndex(SpatialPresetCatalog::readSnapshot(valueTreeState));
    if (match >= 0)
        lastAppliedIndex = match;  // external change landed exactly on a preset

    const bool deviates = (match < 0) && (lastAppliedIndex >= 0);
    if (match == displayedIndex && deviates == displayedDeviation)
        return;
    displayedIndex = match;
    displayedDeviation = deviates;

    if (match >= 0)
    {
        const auto& def = SpatialPresetCatalog::kDefinitions[(size_t)match];
        nameLabel.setText(juce::String(def.displayName).toUpperCase(), juce::dontSendNotification);
        nameLabel.setTooltip(def.usage);
    }
    else
    {
        nameLabel.setText("CUSTOM", juce::dontSendNotification);
        nameLabel.setTooltip("Current spatial state matches no spatial preset.");
    }

    // REVERT exists only while there is something to revert.
    revertButton.setVisible(deviates);
}
