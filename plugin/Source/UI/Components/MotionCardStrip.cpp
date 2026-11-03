// MotionCardStrip (implementation): Pattern-first motion selector: radio-group cards attached to MotionPattern, with
//   micro-previews via the shared evaluator.

#include "MotionCardStrip.h"
#include "CRTLookAndFeel.h"
#include "../../DSP/SpatialMotionTrajectory.h"
#include "../../SpatialParamIDs.h"

namespace
{
constexpr int kCardWidth = 82;
constexpr int kCardGap = 6;
constexpr float kPreviewRateCps = 0.45f;  // preview animation speed, cycles/sec
constexpr int kAnimHz = 30;
}  // namespace

// --- CardComponent ---------------------------------------------------------

MotionCardStrip::CardComponent::CardComponent(MotionCardStrip& ownerStrip, int cardIndex)
    : owner(ownerStrip)
    , index(cardIndex)
{
    if (index < 0 || index >= (int)MotionCardStrip::kPatterns.size())
        return;

    const auto& def = kPatterns[(size_t)index];
    setTitle(juce::String(def.name).toLowerCase());
    setDescription(def.hint);
    setTooltip(juce::String(def.hint));
    setWantsKeyboardFocus(true);
}

std::unique_ptr<juce::AccessibilityHandler> MotionCardStrip::CardComponent::createAccessibilityHandler()
{
    class Handler final : public juce::AccessibilityHandler
    {
    public:
        explicit Handler(CardComponent& cardIn)
            : AccessibilityHandler(cardIn, juce::AccessibilityRole::radioButton,
                                   juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press,
                                                                          [&cardIn]
                                                                          { cardIn.owner.selectCard(cardIn.index); }))
            , card(cardIn)
        {
        }
        juce::AccessibleState getCurrentState() const override
        {
            auto state = AccessibilityHandler::getCurrentState().withSelectable().withCheckable();
            return card.owner.getSelectedCardIndex() == card.index ? state.withSelected().withChecked() : state;
        }

    private:
        CardComponent& card;
    };
    return std::make_unique<Handler>(*this);
}

bool MotionCardStrip::CardComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown())
        return false;
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::returnKey || code == juce::KeyPress::spaceKey)
    {
        owner.selectCard(index);
        return true;
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey ||
        code == juce::KeyPress::downKey)
    {
        const int direction = (code == juce::KeyPress::leftKey || code == juce::KeyPress::upKey) ? -1 : 1;
        const int next = (index + direction + static_cast<int>(owner.cardComponents.size())) %
                         static_cast<int>(owner.cardComponents.size());
        owner.selectCard(next);
        owner.cardComponents[static_cast<size_t>(next)]->grabKeyboardFocus();
        return true;
    }
    return false;
}

void MotionCardStrip::CardComponent::mouseEnter(const juce::MouseEvent&)
{
    hovered = true;
    repaint();
}
void MotionCardStrip::CardComponent::mouseExit(const juce::MouseEvent&)
{
    hovered = false;
    repaint();
}

void MotionCardStrip::CardComponent::mouseUp(const juce::MouseEvent& event)
{
    if (!event.mouseWasDraggedSinceMouseDown() && contains(event.getPosition()))
    {
        owner.selectCard(index);
    }
}

void MotionCardStrip::CardComponent::paint(juce::Graphics& g)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();

    if (index < 0 || index >= (int)MotionCardStrip::kPatterns.size())
        return;

    const auto& def = kPatterns[(size_t)index];
    const bool selected = (owner.selectedIndex == index);
    auto bounds = getLocalBounds().toFloat().reduced(1.0f);

    g.setColour(CRTLookAndFeel::getVoidBlack().brighter(selected ? 0.06f : 0.02f));
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(green.withAlpha(hasKeyboardFocus(true) ? 1.0f : (selected ? 0.9f : (hovered ? 0.55f : 0.28f))));
    g.drawRoundedRectangle(bounds, 3.0f, hasKeyboardFocus(true) ? 2.0f : (selected ? 1.4f : 1.0f));

    // Name label along the bottom.
    auto area = getLocalBounds().reduced(4);
    auto nameArea = area.removeFromBottom(14);
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
    g.setColour(green.withAlpha(selected ? 1.0f : (hovered ? 0.85f : 0.6f)));
    g.drawText(def.name, nameArea, juce::Justification::centred);

    // Animated trajectory preview: plan-view polar plot (azimuth = angle,
    // distance = radius) with elevation as vertical lift - calls the shared
    // evaluator, so the card shows exactly what will play.
    const auto pv = area.toFloat().reduced(3.0f);
    const auto cx = pv.getCentreX();
    const auto cy = pv.getCentreY() + 2.0f;
    const float radius = juce::jmin(pv.getWidth(), pv.getHeight()) * 0.30f;

    g.setColour(green.withAlpha(0.14f));
    g.drawEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 0.6f);

    // Still pattern: single dot at center.
    if (index == 0)  // Still
    {
        g.setColour(green.withAlpha(0.8f));
        g.fillEllipse(cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
        return;
    }

    // Draw trajectory using the shared evaluator with a locally advanced phase.
    // depth01 is a fixed preview depth (moderate); the actual depth comes from
    // the processor's MotionDepth parameter.
    const float previewDepth01 = 0.5f;
    const double phase = owner.previewPhase;

    auto pointAt = [&](double cyc) -> juce::Point<float>
    {
        const auto offsets = spatialmotion::evaluateTrajectory((SpatialIDs::motion::Pattern)index, cyc, previewDepth01);

        const float azRad = juce::degreesToRadians(offsets.azimuthDeg);
        const float r = radius * juce::jlimit(0.25f, 1.9f, 1.0f + offsets.distanceMeters * 0.45f);
        return {cx + std::sin(azRad) * r,
                cy + std::cos(azRad) * r * 0.35f - (offsets.elevationDeg / 90.0f) * pv.getHeight() * 0.32f};
    };

    // Pattern-specific preview projections (hand-tuned for legibility).
    // Breathe (index 4) and Wander (index 5) collapse to near-static dots in
    // the default polar projection; they get custom projections that show motion.
    constexpr int kBreatheIndex = 4;
    constexpr int kWanderIndex = 5;

    if (index == kBreatheIndex)
    {
        // Breathe: distance-only sine modulation. Show as vertical oscillation
        // (distance-only => no azimuth change => radial pulse).
        constexpr int segments = 12;
        juce::Path path;
        for (int i = 0; i <= segments; ++i)
        {
            const double cyc = phase + (double)i / (double)segments;
            const auto offsets =
                spatialmotion::evaluateTrajectory((SpatialIDs::motion::Pattern)index, cyc, previewDepth01);
            // Radial: distance modulation only; show as pulsing ring around center.
            const float r = radius * juce::jlimit(0.25f, 1.4f, 1.0f + offsets.distanceMeters * 0.45f);
            const float angle =
                juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(segments);
            const auto p = juce::Point<float>(cx + r * std::sin(angle), cy + r * std::cos(angle));
            if (i == 0)
                path.startNewSubPath(p);
            else
                path.lineTo(p);
        }
        g.setColour(green.withAlpha(selected || hovered ? 0.55f : 0.32f));
        g.strokePath(path, juce::PathStrokeType(1.0f));

        // Draw current distance as a filled ring.
        const auto offsets =
            spatialmotion::evaluateTrajectory((SpatialIDs::motion::Pattern)index, phase, previewDepth01);
        const float r = radius * juce::jlimit(0.25f, 1.4f, 1.0f + offsets.distanceMeters * 0.45f);
        g.setColour(green.withAlpha(0.95f));
        g.drawEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f, 1.2f);
    }
    else if (index == kWanderIndex)
    {
        // Wander: small random drifts in all axes. Show as a scattered trail
        // (multiple points along a smoothed random path).
        constexpr int segments = 24;
        juce::Path path;
        for (int i = 0; i <= segments; ++i)
        {
            const auto p = pointAt(phase + (double)i / (double)segments);
            if (i == 0)
                path.startNewSubPath(p);
            else
                path.lineTo(p);
        }
        g.setColour(green.withAlpha(selected || hovered ? 0.45f : 0.22f));
        g.strokePath(path, juce::PathStrokeType(1.2f));

        // Draw multiple points along the trail to show organic scatter.
        g.setColour(green.withAlpha(selected || hovered ? 0.60f : 0.35f));
        for (int i = 0; i <= segments; i += 4)
        {
            const auto p = pointAt(phase + (double)i / (double)segments);
            g.fillEllipse(p.x - 1.2f, p.y - 1.2f, 2.4f, 2.4f);
        }
    }
    else
    {
        // Default polar plot for other patterns (Orbit, Pendulum, Figure-8).
        constexpr int segments = 36;
        juce::Path path;
        for (int i = 0; i <= segments; ++i)
        {
            const auto p = pointAt(phase + (double)i / (double)segments);
            if (i == 0)
                path.startNewSubPath(p);
            else
                path.lineTo(p);
        }
        g.setColour(green.withAlpha(selected || hovered ? 0.55f : 0.32f));
        g.strokePath(path, juce::PathStrokeType(1.0f));

        const auto now = pointAt(phase);
        g.setColour(green.withAlpha(0.95f));
        g.fillEllipse(now.x - 1.8f, now.y - 1.8f, 3.6f, 3.6f);
    }
}

// --- MotionCardStrip -------------------------------------------------------

MotionCardStrip::MotionCardStrip(juce::AudioProcessorValueTreeState& apvts)
    : valueTreeState(apvts)
{
    setTitle("Spatial Patterns");
    setFocusContainerType(FocusContainerType::focusContainer);

    // Create one card per pattern (Still through Wander).
    for (int i = 0; i < (int)kPatterns.size(); ++i)
    {
        auto card = std::make_unique<CardComponent>(*this, i);
        addAndMakeVisible(*card);
        cardComponents.push_back(std::move(card));
    }

    // Attach to MotionPattern parameter: read from it to update selectedIndex.
    if (auto* patternParam = apvts.getParameter(SpatialIDs::motion::kPattern))
    {
        patternAttachment = std::make_unique<juce::ParameterAttachment>(
            *patternParam,
            [this](float value)
            {
                selectedIndex = juce::roundToInt(value);
                for (auto& card : cardComponents)
                {
                    card->repaint();
                    if (auto* handler = card->getAccessibilityHandler())
                        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
                }
            });
        patternAttachment->sendInitialUpdate();
    }

    startTimerHz(kAnimHz);
}

MotionCardStrip::~MotionCardStrip()
{
    stopTimer();
}

void MotionCardStrip::paint(juce::Graphics& g)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
    g.setColour(green.withAlpha(0.45f));
    g.drawText("MOTION", getLocalBounds().removeFromTop(12), juce::Justification::centredLeft);
}

void MotionCardStrip::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop(12);

    // Cards shrink below kCardWidth if needed so the whole set always fits.
    const int count = (int)cardComponents.size();
    const int cardHeight = juce::jmax(40, area.getHeight());
    const int cardWidth = juce::jmin(kCardWidth, (area.getWidth() - (count - 1) * kCardGap) / juce::jmax(1, count));

    int x = area.getX();
    for (auto& card : cardComponents)
    {
        card->setBounds(x, area.getY(), cardWidth, cardHeight);
        x += cardWidth + kCardGap;
    }
}

void MotionCardStrip::visibilityChanged()
{
    if (isVisible())
        startTimerHz(kAnimHz);
    else
        stopTimer();
}

float MotionCardStrip::readFloat(const char* paramId) const
{
    if (auto* v = valueTreeState.getRawParameterValue(paramId))
        return v->load();
    return 0.0f;
}

int MotionCardStrip::readChoice(const char* paramId) const
{
    return (int)std::lround(readFloat(paramId));
}

int MotionCardStrip::getSelectedCardIndex() const
{
    return readChoice(SpatialIDs::motion::kPattern);
}

void MotionCardStrip::selectCard(int index)
{
    if (index < 0 || index >= (int)kPatterns.size())
        return;

    // Set MotionPattern via the parameter with begin/end gesture.
    if (auto* param = valueTreeState.getParameter(SpatialIDs::motion::kPattern))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost(param->getNormalisableRange().convertTo0to1((float)index));
        param->endChangeGesture();
    }
}

void MotionCardStrip::timerCallback()
{
    // Not on screen (e.g. constructed but not yet added): skip all work.
    if (!isShowing())
        return;

    previewPhase += (double)kPreviewRateCps / (double)kAnimHz;

    // Repaint all cards to animate the preview trajectories.
    for (auto& card : cardComponents)
        card->repaint();
}
