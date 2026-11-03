// SpatialRoutingDrawer (implementation): Collapsible BLEND drawer (signal chain, wet, width, crossover, render
//   quality).

#include "SpatialRoutingDrawer.h"
#include "../../SpatialParamIDs.h"

namespace
{
constexpr int kHeaderHeight = 24;
constexpr int kRowHeight = 24;  // 24px minimum hit target
constexpr int kRowGap = 8;
constexpr int kRowCount = 5;  // QUALITY, CHAIN, WET, WIDTH, XOVER
constexpr int kLabelWidth = 64;
constexpr int kHorizontalInset = 4;
}  // namespace

SpatialRoutingDrawer::SpatialRoutingDrawer(juce::AudioProcessorValueTreeState& apvts)
    : valueTreeState(apvts)
{
    styleRowLabel(qualityLabel, "QUALITY");
    styleRowLabel(chainLabel, "CHAIN");
    styleRowLabel(wetLabel, "WET");
    styleRowLabel(widthLabel, "WIDTH");
    styleRowLabel(crossoverLabel, "XOVER");

    if (auto* quality =
            dynamic_cast<juce::AudioParameterChoice*>(valueTreeState.getParameter(SpatialIDs::binaural::kQuality)))
    {
        int id = 1;
        for (const auto& choice : quality->choices)
            qualityCombo.addItem(choice, id++);
    }
    qualityCombo.setTooltip("Spatial rendering quality.");
    addAndMakeVisible(qualityCombo);
    qualityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        valueTreeState, SpatialIDs::binaural::kQuality, qualityCombo);

    if (auto* routing = dynamic_cast<juce::AudioParameterChoice*>(
            valueTreeState.getParameter(SpatialIDs::routing::kSpatialRouting)))
    {
        int id = 1;
        for (const auto& choice : routing->choices)
            chainCombo.addItem(choice, id++);
    }
    chainCombo.setTooltip("Where spatial processing sits: after or before the delay/reverb.");
    addAndMakeVisible(chainCombo);
    chainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        valueTreeState, SpatialIDs::routing::kSpatialRouting, chainCombo);

    auto setupSlider = [this](ParameterValueSlider& slider, const juce::String& tooltip)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        // 66px fits the widest value strings ("Natural", "180 Hz").
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 66, kRowHeight - 2);
        slider.setTooltip(tooltip);
        addAndMakeVisible(slider);
    };
    setupSlider(wetSlider, "Blend between unrendered and spatially rendered signal.");
    setupSlider(widthSlider, "Spatial source width: Focused, Natural, or Wide.");
    setupSlider(crossoverSlider, "Transition between low-band spatial anchoring and full spatial rendering.");

    wetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, SpatialIDs::binaural::kWetMix, wetSlider);
    widthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, SpatialIDs::binaural::kWidth, widthSlider);
    crossoverAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, SpatialIDs::binaural::kCrossover, crossoverSlider);

    setTitle("blend");
    chainCombo.setTitle("Signal chain");
    wetSlider.setTitle("Spatial Mix");
    widthSlider.setTitle("Source Width");
    crossoverSlider.setTitle("Spatial Crossover");
    qualityCombo.setTitle("Spatial Quality");

    setExpanded(false);
    startTimerHz(2);
    timerCallback();
}

void SpatialRoutingDrawer::timerCallback()
{
    // Deviation dot in the header while collapsed state hides non-defaults.
    auto deviates = [this](const char* paramId)
    {
        if (auto* p = valueTreeState.getParameter(paramId))
            return std::abs(p->getValue() - p->getDefaultValue()) > 0.001f;
        return false;
    };
    const bool dirty = deviates(SpatialIDs::routing::kSpatialRouting) || deviates(SpatialIDs::binaural::kWetMix) ||
                       deviates(SpatialIDs::binaural::kWidth) || deviates(SpatialIDs::binaural::kCrossover) ||
                       deviates(SpatialIDs::binaural::kQuality);
    if (dirty != contentsDirty)
    {
        contentsDirty = dirty;
        repaint(headerBounds);
    }
}

void SpatialRoutingDrawer::styleRowLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
    label.setColour(juce::Label::textColourId, CRTLookAndFeel::getTerminalGreen().withAlpha(UIConstants::alphaLabel));
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
}

int SpatialRoutingDrawer::getPreferredHeight() const
{
    return expanded ? kHeaderHeight + kRowCount * (kRowHeight + kRowGap) : kHeaderHeight;
}

juce::Rectangle<int> SpatialRoutingDrawer::getWalkthroughQualityScreenBounds() const
{
    return qualityLabel.getScreenBounds().getUnion(qualityCombo.getScreenBounds());
}

juce::Rectangle<int> SpatialRoutingDrawer::getWalkthroughChainScreenBounds() const
{
    return chainLabel.getScreenBounds().getUnion(chainCombo.getScreenBounds());
}

juce::Rectangle<int> SpatialRoutingDrawer::getWalkthroughWetScreenBounds() const
{
    return wetLabel.getScreenBounds().getUnion(wetSlider.getScreenBounds());
}

juce::Rectangle<int> SpatialRoutingDrawer::getWalkthroughWidthScreenBounds() const
{
    return widthLabel.getScreenBounds().getUnion(widthSlider.getScreenBounds());
}

juce::Rectangle<int> SpatialRoutingDrawer::getWalkthroughCrossoverScreenBounds() const
{
    return crossoverLabel.getScreenBounds().getUnion(crossoverSlider.getScreenBounds());
}

void SpatialRoutingDrawer::setExpanded(bool shouldExpand)
{
    expanded = shouldExpand;
    for (auto* c : std::initializer_list<juce::Component*>{&qualityLabel, &qualityCombo, &chainLabel, &chainCombo,
                                                           &wetLabel, &wetSlider, &widthLabel, &widthSlider,
                                                           &crossoverLabel, &crossoverSlider})
        c->setVisible(expanded);
    if (onLayoutChanged)
        onLayoutChanged();
    repaint();
}

void SpatialRoutingDrawer::mouseUp(const juce::MouseEvent& event)
{
    if (headerBounds.contains(event.getPosition()))
        setExpanded(!expanded);
}

void SpatialRoutingDrawer::resized()
{
    auto area = getLocalBounds();
    headerBounds = area.removeFromTop(kHeaderHeight);

    auto placeRow = [&](juce::Label& label, juce::Component& control)
    {
        area.removeFromTop(kRowGap);
        auto row = area.removeFromTop(kRowHeight);
        row.removeFromLeft(kHorizontalInset);
        row.removeFromRight(kHorizontalInset);
        label.setBounds(row.removeFromLeft(kLabelWidth));
        control.setBounds(row);
    };
    placeRow(qualityLabel, qualityCombo);
    placeRow(chainLabel, chainCombo);
    placeRow(wetLabel, wetSlider);
    placeRow(widthLabel, widthSlider);
    placeRow(crossoverLabel, crossoverSlider);
}

void SpatialRoutingDrawer::paint(juce::Graphics& g)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
    g.setColour(green.withAlpha(UIConstants::alphaLabel));
    const juce::String arrow = expanded ? juce::String::charToString(0x25BE) : juce::String::charToString(0x25B8);
    juce::String header = arrow + " BLEND";
    if (contentsDirty && !expanded)
        header << " " << juce::String::charToString(0x25CF);
    g.drawText(header, headerBounds, juce::Justification::centredLeft);
}
