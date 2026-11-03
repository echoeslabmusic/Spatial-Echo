// SpatialMotionSection (implementation): Inline motion control surface: cards + choreography + RATE/DEPTH macros +
//   FINE-TUNE drawer.

#include "SpatialMotionSection.h"
#include "../../ParameterWriteHelpers.h"
#include "../../SpatialParamIDs.h"

namespace
{
constexpr int kCardsHeight = 84;
constexpr int kRowHeight = 22;
constexpr int kRowGap = 5;
constexpr int kDrawerRowHeight = 20;
constexpr int kDrawerRows = 5;
constexpr int kLabelWidth = 46;

juce::StringArray choicesFor(juce::AudioProcessorValueTreeState& apvts, const char* paramId)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(paramId)))
        return choice->choices;
    return {};
}

bool paramDeviatesFromDefault(juce::AudioProcessorValueTreeState& apvts, const char* paramId)
{
    if (auto* p = apvts.getParameter(paramId))
        return std::abs(p->getValue() - p->getDefaultValue()) > 0.001f;
    return false;
}
}  // namespace

SpatialMotionSection::SpatialMotionSection(juce::AudioProcessorValueTreeState& apvts)
    : valueTreeState(apvts)
{
    addAndMakeVisible(cardStrip);
    cardStrip.onCustomRequested = [this]
    {
        fineTuneButton.setToggleState(true, juce::dontSendNotification);
        setFineTuneVisible(true);
    };

    // --- Choreography row -------------------------------------------------
    styleRowLabel(choreographyLabel, "CHOREO");
    for (auto* b : {&unisonButton, &constellationButton, &cascadeButton})
    {
        b->setLookAndFeel(&selectorLookAndFeel);
        b->setClickingTogglesState(false);
        addAndMakeVisible(*b);
    }
    unisonButton.setTooltip("All heads move together (same phase).");
    constellationButton.setTooltip("Heads orbit 120 degrees apart - a staggered formation.");
    cascadeButton.setTooltip("Heads follow each other in time - a ripple through the echoes.");

    if (auto* choreo = valueTreeState.getParameter(SpatialIDs::motion::kChoreo))
    {
        choreoAttachment = std::make_unique<juce::ParameterAttachment>(
            *choreo,
            [this](float value)
            {
                const int mode = juce::roundToInt(value);
                unisonButton.setToggleState(mode == 0, juce::dontSendNotification);
                constellationButton.setToggleState(mode == 1, juce::dontSendNotification);
                cascadeButton.setToggleState(mode == 2, juce::dontSendNotification);
                updateSpreadEnablement();
            });
        choreoAttachment->sendInitialUpdate();

        auto setMode = [this, choreo](int mode)
        {
            choreoAttachment->setValueAsCompleteGesture(
                choreo->convertFrom0to1(choreo->getNormalisableRange().convertTo0to1((float)mode)));
        };
        unisonButton.onClick = [setMode] { setMode(0); };
        constellationButton.onClick = [setMode] { setMode(1); };
        cascadeButton.onClick = [setMode] { setMode(2); };
    }

    styleRowLabel(spreadLabel, "SPREAD");
    setupCompactSlider(spreadSlider, "How far apart the heads' spatial phases sit (Constellation/Cascade).");
    spreadAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, SpatialIDs::motion::kSpread, spreadSlider);

    // --- RATE row ----------------------------------------------------------
    styleRowLabel(rateLabel, "RATE");
    setupCompactSlider(rateSlider, "Spatial movement speed (free-running, in Hz).");
    rateSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, kRowHeight - 2);
    rateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, SpatialIDs::motion::kRate, rateSlider);

    setupCompactCombo(divisionCombo, SpatialIDs::motion::kDivision, "Note value for tempo-synced spatial movement.");
    for (int i = 1; i < SpatialIDs::motion::kLongDivisionCount; ++i)
        divisionCombo.addItem(SpatialIDs::motion::kLongDivisionChoices[i], SpatialIDs::motion::kDivisionCount + i);
    for (int i = 1; i < SpatialIDs::motion::kLongVariantDivisionCount; ++i)
        divisionCombo.addItem(SpatialIDs::motion::kLongVariantDivisionChoices[i],
                              SpatialIDs::motion::kVariantLongMenuStart + i);
    divisionCombo.onPopupRequested = [this] { showDivisionMenu(); };
    divisionCombo.onChange = [this]
    {
        const int selectedId = divisionCombo.getSelectedId();
        if (selectedId > 0)
            setCombinedDivisionIndex(selectedId - 1);
    };
    if (auto* division = valueTreeState.getParameter(SpatialIDs::motion::kDivision))
    {
        divisionAttachment =
            std::make_unique<juce::ParameterAttachment>(*division, [this](float) { updateDivisionSelection(); });
        divisionAttachment->sendInitialUpdate();
    }
    if (auto* longDivision = valueTreeState.getParameter(SpatialIDs::motion::kLongDivision))
    {
        longDivisionAttachment =
            std::make_unique<juce::ParameterAttachment>(*longDivision, [this](float) { updateDivisionSelection(); });
        longDivisionAttachment->sendInitialUpdate();
    }
    if (auto* longVariantDivision = valueTreeState.getParameter(SpatialIDs::motion::kLongVariantDivision))
    {
        longVariantDivisionAttachment = std::make_unique<juce::ParameterAttachment>(*longVariantDivision, [this](float)
                                                                                    { updateDivisionSelection(); });
        longVariantDivisionAttachment->sendInitialUpdate();
    }
    updateDivisionSelection();

    syncButton.setLookAndFeel(&selectorLookAndFeel);
    syncButton.setClickingTogglesState(true);
    syncButton.setTooltip("Lock spatial movement speed to the host tempo.");
    addAndMakeVisible(syncButton);
    syncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        valueTreeState, SpatialIDs::motion::kSync, syncButton);

    if (auto* sync = valueTreeState.getParameter(SpatialIDs::motion::kSync))
    {
        syncVisibilityAttachment = std::make_unique<juce::ParameterAttachment>(
            *sync, [this](float value) { updateSyncVisibility(value >= 0.5f); });
        syncVisibilityAttachment->sendInitialUpdate();
    }

    // --- DEPTH row ---------------------------------------------------------
    styleRowLabel(depthLabel, "DEPTH");
    setupCompactSlider(
        depthSlider,
        "Spatial movement depth (0-100%). Orbit currently keeps a full azimuth circle at any nonzero value.");
    depthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, SpatialIDs::motion::kDepth, depthSlider);

    depthReadout.setJustificationType(juce::Justification::centredRight);
    depthReadout.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize));
    depthReadout.setColour(juce::Label::textColourId,
                           CRTLookAndFeel::getTerminalGreen().withAlpha(UIConstants::alphaDim));
    depthReadout.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(depthReadout);

    fineTuneButton.setLookAndFeel(&selectorLookAndFeel);
    fineTuneButton.setClickingTogglesState(true);
    fineTuneButton.setTooltip("Show spatial choreography and restart mode.");
    fineTuneButton.onClick = [this] { setFineTuneVisible(fineTuneButton.getToggleState()); };
    addAndMakeVisible(fineTuneButton);

    // --- FINE-TUNE drawer: choreography, spread and restart mode ----
    styleRowLabel(retrigLabel, "RETRIG");
    setupCompactCombo(retrigCombo, SpatialIDs::motion::kRetrig, "How the spatial phase runs or restarts.");
    // Note restarts the phase on incoming MIDI notes. Only the standalone app
    // receives MIDI, so a plug-in instance offers the choice greyed out. The
    // parameter keeps all four choices so saved state and automation are
    // unchanged.
    if (valueTreeState.processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
    {
        constexpr int kNoteChoiceId = 3;  // "Note" (index 2) in SpatialIDs::motion::kRetrigChoices
        retrigCombo.setItemEnabled(kNoteChoiceId, false);
        retrigCombo.setTooltip("How the spatial phase runs or restarts. Note follows MIDI notes and is available in "
                               "the standalone app.");
    }
    retrigAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        valueTreeState, SpatialIDs::motion::kRetrig, retrigCombo);

    // VoiceOver titles (labels are not attached, so titles carry the names).
    setTitle("Spatial Motion");
    spreadSlider.setTitle("Spatial Spread");
    rateSlider.setTitle("Spatial Rate");
    divisionCombo.setTitle("Spatial Division");
    depthSlider.setTitle("Spatial Depth");
    retrigCombo.setTitle("Spatial Restart");

    setFineTuneVisible(false);
    startTimerHz(10);
}

SpatialMotionSection::~SpatialMotionSection()
{
    stopTimer();
    dismissDivisionMenu(false);
    divisionCombo.onPopupRequested = nullptr;
    divisionCombo.onChange = nullptr;
    for (auto* b : {&unisonButton, &constellationButton, &cascadeButton, &syncButton, &fineTuneButton})
        b->setLookAndFeel(nullptr);
}

void SpatialMotionSection::setFineTuneVisibleForWalkthrough(bool visible)
{
    fineTuneButton.setToggleState(visible, juce::dontSendNotification);
    if (fineTuneVisible != visible)
        setFineTuneVisible(visible);
}

juce::Rectangle<int> SpatialMotionSection::getWalkthroughPatternScreenBounds() const
{
    return cardStrip.getScreenBounds();
}

juce::Rectangle<int> SpatialMotionSection::getWalkthroughRateScreenBounds() const
{
    return rateLabel.getScreenBounds().getUnion(rateSlider.getScreenBounds()).getUnion(divisionCombo.getScreenBounds());
}

juce::Rectangle<int> SpatialMotionSection::getWalkthroughSyncScreenBounds() const
{
    return syncButton.getScreenBounds();
}

juce::Rectangle<int> SpatialMotionSection::getWalkthroughDivisionControlScreenBounds() const
{
    return divisionCombo.getScreenBounds();
}

juce::Rectangle<int> SpatialMotionSection::getWalkthroughDepthScreenBounds() const
{
    return depthLabel.getScreenBounds()
        .getUnion(depthSlider.getScreenBounds())
        .getUnion(depthReadout.getScreenBounds());
}

juce::Rectangle<int> SpatialMotionSection::getWalkthroughFineTuneButtonScreenBounds() const
{
    return fineTuneButton.getScreenBounds();
}

juce::Rectangle<int> SpatialMotionSection::getWalkthroughChoreographyScreenBounds() const
{
    return choreographyLabel.getScreenBounds()
        .getUnion(unisonButton.getScreenBounds())
        .getUnion(constellationButton.getScreenBounds())
        .getUnion(cascadeButton.getScreenBounds());
}

juce::Rectangle<int> SpatialMotionSection::getWalkthroughSpreadScreenBounds() const
{
    return spreadLabel.getScreenBounds().getUnion(spreadSlider.getScreenBounds());
}

juce::Rectangle<int> SpatialMotionSection::getWalkthroughRetriggerScreenBounds() const
{
    return retrigLabel.getScreenBounds().getUnion(retrigCombo.getScreenBounds());
}

juce::Rectangle<int> SpatialMotionSection::getWalkthroughDivisionMenuScreenBounds() const
{
    return divisionMenuWindow != nullptr ? divisionMenuWindow->getScreenBounds() : juce::Rectangle<int>();
}

bool SpatialMotionSection::isWalkthroughDivisionMenuVisible() const
{
    return divisionMenuWindow != nullptr && divisionMenuWindow->isShowing();
}

void SpatialMotionSection::showWalkthroughDivisionMenu()
{
    if (divisionMenuWindow == nullptr)
        showDivisionMenu();
}

void SpatialMotionSection::dismissWalkthroughDivisionMenu()
{
    dismissDivisionMenu(false);
}

void SpatialMotionSection::setWalkthroughNavigationComponent(juce::Component* component)
{
    walkthroughNavigationComponent = component;
    if (divisionMenuWindow != nullptr)
        divisionMenuWindow->setModalEventPassthrough(component);
}

void SpatialMotionSection::styleRowLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
    label.setColour(juce::Label::textColourId, CRTLookAndFeel::getTerminalGreen().withAlpha(UIConstants::alphaLabel));
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
}

void SpatialMotionSection::setupCompactSlider(ParameterValueSlider& slider, const juce::String& tooltip)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 54, kRowHeight - 2);
    slider.setTooltip(tooltip);
    addAndMakeVisible(slider);
}

void SpatialMotionSection::setupCompactCombo(ParameterValueComboBox& combo, const char* paramId,
                                             const juce::String& tooltip)
{
    int id = 1;
    for (const auto& choice : choicesFor(valueTreeState, paramId))
        combo.addItem(choice, id++);
    combo.setTooltip(tooltip);
    addAndMakeVisible(combo);
}

void SpatialMotionSection::updateSyncVisibility(bool synced)
{
    if (!synced)
        dismissDivisionMenu(false);

    rateSlider.setVisible(!synced);
    divisionCombo.setVisible(synced);
}

int SpatialMotionSection::getCombinedDivisionIndex() const
{
    const auto* standard =
        dynamic_cast<const juce::AudioParameterChoice*>(valueTreeState.getParameter(SpatialIDs::motion::kDivision));
    const auto* longDivision =
        dynamic_cast<const juce::AudioParameterChoice*>(valueTreeState.getParameter(SpatialIDs::motion::kLongDivision));
    const auto* longVariantDivision = dynamic_cast<const juce::AudioParameterChoice*>(
        valueTreeState.getParameter(SpatialIDs::motion::kLongVariantDivision));
    const int standardIndex = standard != nullptr
                                  ? juce::jlimit(0, SpatialIDs::motion::kDivisionCount - 1, standard->getIndex())
                                  : SpatialIDs::motion::kDivisionDefault;
    const int longIndex = longDivision != nullptr
                              ? juce::jlimit(0, SpatialIDs::motion::kLongDivisionCount - 1, longDivision->getIndex())
                              : SpatialIDs::motion::kLongDivisionDefault;
    const int longVariantIndex =
        longVariantDivision != nullptr
            ? juce::jlimit(0, SpatialIDs::motion::kLongVariantDivisionCount - 1, longVariantDivision->getIndex())
            : SpatialIDs::motion::kLongVariantDivisionDefault;

    return SpatialIDs::motion::toDivisionMenuIndex(standardIndex, longIndex, longVariantIndex);
}

void SpatialMotionSection::updateDivisionSelection()
{
    divisionCombo.setSelectedId(getCombinedDivisionIndex() + 1, juce::dontSendNotification);
}

void SpatialMotionSection::setCombinedDivisionIndex(int index)
{
    const int clamped = juce::jlimit(0, SpatialIDs::motion::kDivisionMenuCount - 1, index);
    if (clamped == getCombinedDivisionIndex())
    {
        updateDivisionSelection();
        return;
    }

    if (clamped < SpatialIDs::motion::kDivisionCount)
    {
        // Set the latent standard value before disabling the override so DSP
        // never exposes an intermediate division for one processing block.
        const auto* standard =
            dynamic_cast<const juce::AudioParameterChoice*>(valueTreeState.getParameter(SpatialIDs::motion::kDivision));
        if (standard == nullptr || standard->getIndex() != clamped)
            SpatialEchoParameterWrite::setChoiceById(valueTreeState, SpatialIDs::motion::kDivision, clamped, true);

        const auto* longDivision = dynamic_cast<const juce::AudioParameterChoice*>(
            valueTreeState.getParameter(SpatialIDs::motion::kLongDivision));
        if (longDivision == nullptr || longDivision->getIndex() != SpatialIDs::motion::kLongDivisionDefault)
            SpatialEchoParameterWrite::setChoiceById(valueTreeState, SpatialIDs::motion::kLongDivision,
                                                     SpatialIDs::motion::kLongDivisionDefault, true);

        const auto* longVariantDivision = dynamic_cast<const juce::AudioParameterChoice*>(
            valueTreeState.getParameter(SpatialIDs::motion::kLongVariantDivision));
        if (longVariantDivision == nullptr ||
            longVariantDivision->getIndex() != SpatialIDs::motion::kLongVariantDivisionDefault)
            SpatialEchoParameterWrite::setChoiceById(valueTreeState, SpatialIDs::motion::kLongVariantDivision,
                                                     SpatialIDs::motion::kLongVariantDivisionDefault, true);
        return;
    }

    if (clamped < SpatialIDs::motion::kVariantLongMenuStart)
    {
        const int longIndex = SpatialIDs::motion::longDivisionFromMenuIndex(clamped);
        SpatialEchoParameterWrite::setChoiceById(valueTreeState, SpatialIDs::motion::kLongDivision, longIndex, true);
        SpatialEchoParameterWrite::setChoiceById(valueTreeState, SpatialIDs::motion::kLongVariantDivision,
                                                 SpatialIDs::motion::kLongVariantDivisionDefault, true);
        return;
    }

    const int longVariantIndex = SpatialIDs::motion::longVariantDivisionFromMenuIndex(clamped);
    SpatialEchoParameterWrite::setChoiceById(valueTreeState, SpatialIDs::motion::kLongVariantDivision, longVariantIndex,
                                             true);
    SpatialEchoParameterWrite::setChoiceById(valueTreeState, SpatialIDs::motion::kLongDivision,
                                             SpatialIDs::motion::kLongDivisionDefault, true);
}

void SpatialMotionSection::showDivisionMenu()
{
    if (divisionMenuWindow != nullptr)
    {
        dismissDivisionMenu();
        return;
    }

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr)
    {
        divisionCombo.hidePopup();
        return;
    }

    auto grid = std::make_unique<DivisionMenuGrid>(DivisionMenuGrid::Layout::SpatialMotion);
    auto* gridComponent = grid.get();
    gridComponent->setCurrentIndex(getCombinedDivisionIndex());

    juce::Component::SafePointer<SpatialMotionSection> safeThis(this);
    auto deferredDismiss = [safeThis]
    {
        juce::MessageManager::callAsync(
            [safeThis]
            {
                if (safeThis != nullptr)
                    safeThis->dismissDivisionMenu();
            });
    };

    gridComponent->onItemSelected = [safeThis, deferredDismiss](int index)
    {
        if (safeThis != nullptr)
            safeThis->setCombinedDivisionIndex(index);
        deferredDismiss();
    };
    gridComponent->onDismissRequested = deferredDismiss;

    const auto anchorBounds = divisionCombo.getScreenBounds();
    const auto placement = SpatialEchoDropdownWindow::calculateConstrainedBounds(
        anchorBounds, {gridComponent->getPreferredWidth(), gridComponent->getPreferredHeight()},
        SpatialEchoDropdownWindow::getDisplayUserAreaFor(anchorBounds), topLevel->getScreenBounds());

    grid->setBounds(0, 0, placement.bounds.getWidth(), placement.bounds.getHeight());

    auto host = std::make_unique<SpatialEchoDropdownWindow::Host>();
    host->onInputAttempt = deferredDismiss;

    divisionMenuWindow = std::move(host);
    divisionMenuWindow->setModalEventPassthrough(walkthroughNavigationComponent.getComponent());
    divisionMenuWindow->setOpaque(false);
    divisionMenuWindow->addAndMakeVisible(gridComponent);
    grid.release();  // SpatialEchoDropdownWindow::Host owns and deletes its children.
    divisionMenuWindow->setSize(placement.bounds.getWidth(), placement.bounds.getHeight());
    SpatialEchoDropdownWindow::addToDesktop(*divisionMenuWindow, topLevel);
    SpatialEchoDropdownWindow::setTopLeftPositionForNativeParent(*divisionMenuWindow, placement.bounds, topLevel);
    divisionMenuWindow->setVisible(true);
    SpatialEchoDropdownWindow::setOutsideClickDismissal(*divisionMenuWindow, true);
    divisionMenuWindow->toFront(true);
    gridComponent->grabKeyboardFocus();
}

void SpatialMotionSection::dismissDivisionMenu(bool restoreFocus)
{
    divisionCombo.hidePopup();

    if (divisionMenuWindow == nullptr)
        return;

    SpatialEchoDropdownWindow::setOutsideClickDismissal(*divisionMenuWindow, false);
    divisionMenuWindow->setVisible(false);
    divisionMenuWindow.reset();

    if (restoreFocus && isShowing() && divisionCombo.isShowing() && divisionCombo.isEnabled())
        divisionCombo.grabKeyboardFocus();
}

void SpatialMotionSection::updateSpreadEnablement()
{
    const bool relevant = !unisonButton.getToggleState();
    spreadSlider.setEnabled(relevant);
    spreadSlider.setAlpha(relevant ? 1.0f : 0.3f);
    spreadLabel.setAlpha(relevant ? 1.0f : 0.55f);
}

void SpatialMotionSection::setFineTuneVisible(bool visible)
{
    fineTuneVisible = visible;
    for (auto* c : std::initializer_list<juce::Component*>{&choreographyLabel, &unisonButton, &constellationButton,
                                                           &cascadeButton, &spreadLabel, &spreadSlider, &retrigLabel,
                                                           &retrigCombo})
        c->setVisible(visible);

    if (onLayoutChanged)
        onLayoutChanged();
}

int SpatialMotionSection::getPreferredHeight() const
{
    int h = kCardsHeight + kRowGap  // cards
            + kRowHeight + kRowGap  // rate
            + kRowHeight;           // depth (+ fine-tune toggle)
    if (fineTuneVisible)
        h += kRowGap + kRowHeight + kRowGap  // choreography + spread
             + kRowHeight;                   // retrig
    return h;
}

void SpatialMotionSection::resized()
{
    auto area = getLocalBounds();

    cardStrip.setBounds(area.removeFromTop(kCardsHeight));
    area.removeFromTop(kRowGap);

    // RATE row: slider (free) and division combo (synced) share the same slot.
    {
        auto row = area.removeFromTop(kRowHeight);
        rateLabel.setBounds(row.removeFromLeft(kLabelWidth));
        auto syncArea = row.removeFromRight(56);
        syncButton.setBounds(syncArea.reduced(0, 1));
        row.removeFromRight(6);
        rateSlider.setBounds(row);
        const int comboWidth = juce::jmax(divisionCombo.getRequiredValueWidth(), juce::jmin(150, row.getWidth()));
        divisionCombo.setBounds(row.withWidth(comboWidth));
    }
    area.removeFromTop(kRowGap);

    // DEPTH row
    {
        auto row = area.removeFromTop(kRowHeight);
        depthLabel.setBounds(row.removeFromLeft(kLabelWidth));
        auto toggleArea = row.removeFromRight(86);
        fineTuneButton.setBounds(toggleArea.reduced(0, 1));
        row.removeFromRight(6);
        const auto valueFont = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize);
        const int readoutWidth = ParameterValueLayout::requiredWidth(valueFont, "100%");
        depthReadout.setBounds(row.removeFromRight(readoutWidth));
        depthSlider.setBounds(row);
    }

    if (!fineTuneVisible)
        return;

    area.removeFromTop(kRowGap);

    // Choreography row leads the drawer: per-head phase choreography is an
    // advanced control, so it stays out of the top level.
    {
        auto row = area.removeFromTop(kRowHeight);
        choreographyLabel.setBounds(row.removeFromLeft(kLabelWidth + 14));
        unisonButton.setBounds(row.removeFromLeft(64));
        row.removeFromLeft(4);
        constellationButton.setBounds(row.removeFromLeft(110));
        row.removeFromLeft(4);
        cascadeButton.setBounds(row.removeFromLeft(76));
        row.removeFromLeft(12);
        spreadLabel.setBounds(row.removeFromLeft(kLabelWidth));
        spreadSlider.setBounds(row);
    }
    area.removeFromTop(kRowGap);

    // Drawer: one row for RETRIG.
    {
        auto row = area.removeFromTop(kDrawerRowHeight);
        retrigLabel.setBounds(row.removeFromLeft(kLabelWidth));
        const int comboWidth = juce::jmax(retrigCombo.getRequiredValueWidth(), juce::jmin(150, row.getWidth()));
        retrigCombo.setBounds(row.withWidth(comboWidth));
    }
}

void SpatialMotionSection::paint(juce::Graphics& g)
{
    if (!fineTuneVisible)
        return;
    // Hairline above the drawer so the advanced set reads as its own layer.
    const int drawerTop =
        getPreferredHeight() - kDrawerRows * (kDrawerRowHeight + 3) - kRowHeight - kRowGap - kRowGap / 2;
    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(UIConstants::alphaFaint * 0.6f));
    g.drawHorizontalLine(drawerTop, 0.0f, (float)getWidth());
}

void SpatialMotionSection::timerCallback()
{
    // Effective depth readout: show current depth percentage.
    auto rawValue = [this](const char* id)
    {
        if (auto* raw = valueTreeState.getRawParameterValue(id))
            return raw->load();
        return 0.0f;
    };
    const float depth = rawValue(SpatialIDs::motion::kDepth);

    juce::String text;
    text << juce::roundToInt(depth) << "%";
    if (text != lastDepthReadout)
    {
        lastDepthReadout = text;
        if (!depthReadout.getLocalBounds().isEmpty())
            ParameterValueLayout::assertFits(depthReadout,
                                             CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize), text,
                                             depthReadout.getLocalBounds());
        depthReadout.setText(text, juce::dontSendNotification);
    }

    // Deviation dot: signal hidden non-default state in the drawer.
    const bool dirty = paramDeviatesFromDefault(valueTreeState, SpatialIDs::motion::kRetrig);
    if (dirty != fineTuneDirty)
    {
        fineTuneDirty = dirty;
        fineTuneButton.setButtonText(dirty ? "CHOREO " + juce::String::charToString(0x25BA) + " " +
                                                 juce::String::charToString(0x25CF)
                                           : "CHOREO " + juce::String::charToString(0x25BA));
    }
}
