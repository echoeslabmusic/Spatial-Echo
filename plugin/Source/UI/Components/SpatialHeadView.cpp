// SpatialHeadView: Interactive 3D wireframe-head spatial controller implementation.

#include "SpatialHeadView.h"
#include "SpatialHeadGL.h"
#include "CRTLookAndFeel.h"
#include "HeadWireframeData.h"
#include "../UIConstants.h"
#include "../../DSP/SpatialMotionTrajectory.h"
#include "../../SpatialParamIDs.h"
#include <cmath>

namespace
{
constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kDegToRad = kPi / 180.0f;

// Camera orientation: over the shoulder - behind the head and slightly above,
// so FRONT reads at the top (radar 12-o'clock convention) and the head's right
// ear is on screen-right (matches headphones). Negative pitches the nose up/away;
// a positive value would drop the face to the bottom and invert front/back.
constexpr float kCameraPitchDeg = -18.0f;  // slightly above, front toward top
constexpr float kCameraDistance = 4.2f;    // in sphere radii (mild perspective)

constexpr float kHeadScale = 0.42f;  // head size relative to 1 m orbit sphere
constexpr float kMinDistanceM = 0.1f;
constexpr float kMaxDistanceM = 10.0f;
constexpr float kMarkerHitRadiusPx = 15.0f;

// Distance -> drawn radius, with the orbit sphere at 1 m. The near field gets
// the wider visual range (that's where positioning precision matters); the far
// field compresses so 10 m stays inside the viewport.
// 0.1 m -> 0.55 R, 1 m -> 1.0 R, 10 m -> 1.22 R.
float distanceToRadius(float meters)
{
    const float clamped = juce::jlimit(kMinDistanceM, kMaxDistanceM, meters);
    const float lg = std::log10(clamped);
    return 1.0f + (lg < 0.0f ? 0.45f : 0.22f) * lg;
}

float wrapAzimuth(float az)
{
    while (az > 180.0f)
        az -= 360.0f;
    while (az < -180.0f)
        az += 360.0f;
    return az;
}

SpatialHeadView::Target targetForTapIndex(int tapIndex)
{
    switch (tapIndex)
    {
        case 0:
            return SpatialHeadView::Target::H1;
        case 1:
            return SpatialHeadView::Target::H2;
        case 2:
            return SpatialHeadView::Target::H3;
        default:
            return SpatialHeadView::Target::H1;
    }
}

int tapIndexForTarget(SpatialHeadView::Target target)
{
    switch (target)
    {
        case SpatialHeadView::Target::H1:
            return 0;
        case SpatialHeadView::Target::H2:
            return 1;
        case SpatialHeadView::Target::H3:
            return 2;
        case SpatialHeadView::Target::Global:
        case SpatialHeadView::Target::All:
            return -1;
    }
}
}  // namespace

//==============================================================================
SpatialHeadView::SpatialHeadView()
{
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTooltip("Drag a marker to position it (azimuth/elevation) | Shift+drag: single axis | "
               "Drag empty space: orbit the view | Cmd/Ctrl+wheel: zoom | Wheel: distance | "
               "Arrows: fine adjust (shift+up/down: distance) | "
               "Double-click marker: reset position | Double-click space: reset view");
    headLines = buildHeadWireframe();
    buildGLLineData();

    // Configure (do NOT attach here) - updateGLAttachment() attaches the context
    // only when the view is actually shown, so hidden/headless instances never
    // spin up a GL thread. setComponentPaintingEnabled(true) composites paint()
    // (the chrome) over the GL background. Frames render on demand: every
    // repaint() of this component triggers one GL frame. The shaders are GLSL
    // 1.50, so an OpenGL 3.2 core context is enough.
    glContext.setRenderer(this);
    glContext.setOpenGLVersionRequired(juce::OpenGLContext::openGL3_2);
    glContext.setComponentPaintingEnabled(true);
    glContext.setContinuousRepainting(false);
    glContext.setImageCacheSize(2 * 1024 * 1024);

    startTimerHz(timerRateHz);
}

SpatialHeadView::~SpatialHeadView()
{
    stopTimer();
    glContext.detach();  // blocks until the GL thread stops; runs openGLContextClosing
}

//==============================================================================
// 3D core
//==============================================================================
SpatialHeadView::Vec3 SpatialHeadView::sphericalToWorld(float azimuthDeg, float elevationDeg, float radius)
{
    // Matches BinauralProcessor::toUnitVector (y-up): +x right, +y up, +z front.
    const float az = azimuthDeg * kDegToRad;
    const float el = elevationDeg * kDegToRad;
    return {radius * std::cos(el) * std::sin(az), radius * std::sin(el), radius * std::cos(el) * std::cos(az)};
}

float SpatialHeadView::cameraYawRad() const
{
    return camYawDeg * kDegToRad;
}

float SpatialHeadView::cameraPitchRad() const
{
    // Clamp the total pitch so the view can tilt to top-down / slightly-below
    // without flipping over the poles.
    return juce::jlimit(-85.0f, 80.0f, kCameraPitchDeg + camPitchDeg) * kDegToRad;
}

float SpatialHeadView::projScale() const
{
    return viewScale * camZoom;
}

juce::Point<float> SpatialHeadView::project(const Vec3& world, float& depthOut) const
{
    // Orbit camera: yaw about +y, then pitch about +x, then a mild perspective
    // divide. yaw=0/pitch=kCameraPitchDeg/zoom=1 reproduces the original view.
    const float cy = std::cos(cameraYawRad()), sy = std::sin(cameraYawRad());
    const float cp = std::cos(cameraPitchRad()), sp = std::sin(cameraPitchRad());
    const float S = projScale();

    const float xv = world.x * cy + world.z * sy;  // yaw about +y
    const float zw = -world.x * sy + world.z * cy;
    const float y2 = world.y * cp - zw * sp;  // pitch about +x
    const float z2 = world.y * sp + zw * cp;

    const float w = 1.0f + (z2 / kCameraDistance);
    const float invW = 1.0f / juce::jmax(0.25f, w);

    depthOut = juce::jlimit(0.0f, 1.0f, 0.5f + z2 * 0.5f);  // 0 = near, 1 = far
    return {viewCentre.x + xv * S * invW, viewCentre.y - y2 * S * invW};
}

bool SpatialHeadView::screenToSphere(juce::Point<float> screenPos, float radius, bool nearHemisphere, float& azOut,
                                     float& elOut) const
{
    // Invert project() for a point constrained to |p| = radius. The rotation
    // preserves the norm, so x^2 + y2^2 + z2^2 = r^2 gives z2 up to the
    // hemisphere sign; two perspective iterations converge well within a pixel.
    const float cy = std::cos(cameraYawRad()), sy = std::sin(cameraYawRad());
    const float cp = std::cos(cameraPitchRad()), sp = std::sin(cameraPitchRad());
    const float S = projScale();

    float z2 = 0.0f;
    float xv = 0.0f, y2 = 0.0f;
    bool onSphere = true;
    for (int iter = 0; iter < 3; ++iter)
    {
        const float w = juce::jmax(0.25f, 1.0f + z2 / kCameraDistance);
        xv = (screenPos.x - viewCentre.x) / S * w;
        y2 = (viewCentre.y - screenPos.y) / S * w;
        const float planar = xv * xv + y2 * y2;
        if (planar >= radius * radius)
        {
            // Outside the silhouette: clamp to the rim.
            const float scale = radius / std::sqrt(planar) * 0.999f;
            xv *= scale;
            y2 *= scale;
            z2 = 0.0f;
            onSphere = false;
        }
        else
        {
            z2 = std::sqrt(radius * radius - planar) * (nearHemisphere ? -1.0f : 1.0f);
            onSphere = true;
        }
    }

    // Undo the pitch rotation, then the yaw, to recover world coordinates.
    const float yw = y2 * cp + z2 * sp;
    const float zw = -y2 * sp + z2 * cp;
    const float x = xv * cy - zw * sy;
    const float z = xv * sy + zw * cy;

    azOut = std::atan2(x, z) / kDegToRad;
    elOut = std::asin(juce::jlimit(-1.0f, 1.0f, yw / radius)) / kDegToRad;
    return onSphere;
}

std::vector<SpatialHeadView::Polyline> SpatialHeadView::buildHeadWireframe()
{
    std::vector<Polyline> lines;
    constexpr int seg = 24;

    // Scanned head bust (HeadWireframeData: y-up, face +z, extent ~[-1,1]).
    // Scale into sphere units and drop it so the ear axis sits at the origin
    // (ears are at y ~= +0.11 in mesh space).
    constexpr float s = kHeadScale;
    constexpr float earY = 0.11f;
    for (const auto& src : HeadWireframeData::lines())
    {
        Polyline line;
        line.reserve(src.size());
        for (const auto& p : src)
            line.push_back({p[0] * s, (p[1] - earY) * s, p[2] * s});
        lines.push_back(std::move(line));
    }

    // Headphone band: twin arcs from ear to ear over the top, clearing the
    // scalp (head half-width 0.59, top after ear-drop ~0.89 in mesh units).
    for (float bandOffset : {0.0f, 0.045f})
    {
        Polyline band;
        const float rx = (0.66f + bandOffset) * s;
        const float ry = (0.99f + bandOffset) * s;
        for (int i = 0; i <= seg; ++i)
        {
            const float t = kPi * (float)i / (float)seg;  // left ear -> over top -> right ear
            band.push_back({rx * std::cos(t), ry * std::sin(t), 0.05f * s});
        }
        lines.push_back(std::move(band));
    }

    // Headphone cups: chunky twin rings hugging the ears, facing outward.
    for (float side : {-1.0f, 1.0f})
    {
        const float cx = side * 0.65f * s;
        for (float ringScale : {1.0f, 0.6f})
        {
            Polyline cup;
            const float cr = 0.27f * ringScale * s;
            for (int i = 0; i <= seg; ++i)
            {
                const float t = 2.0f * kPi * (float)i / (float)seg;
                cup.push_back({cx + side * (ringScale < 1.0f ? 0.05f : 0.0f) * s, cr * std::sin(t),
                               0.05f * s + cr * std::cos(t)});
            }
            lines.push_back(std::move(cup));
        }
    }

    // The scanned bust is authored facing -z, but the app's convention is
    // face = +z (azimuth 0 = front, where the "F" mark and az-0 marker sit).
    // Flip the whole head front-to-back so the nose points at F. Negating z is a
    // reflection (not a Y-rotation), so x is untouched and the ears + headphone
    // L/R stay exactly where they are.
    for (auto& line : lines)
        for (auto& v : line)
            v.z = -v.z;

    return lines;
}

int SpatialHeadView::primarySlotForTarget() const
{
    switch (editTarget)
    {
        case Target::Global:
            return 0;
        case Target::H1:
            return 1;
        case Target::H2:
            return 2;
        case Target::H3:
            return 3;
        case Target::All:
        {
            const int firstActive = DelayHeadActivation::firstActiveIndex(activeTapMask);
            return firstActive >= 0 ? firstActive + 1 : 1;
        }
    }
    return 0;
}

bool SpatialHeadView::isTapActive(int tapIndex) const
{
    return tapIndex >= 0 && tapIndex < static_cast<int>(activeTapMask.size()) &&
           activeTapMask[static_cast<std::size_t>(tapIndex)];
}

bool SpatialHeadView::canEditCurrentTarget() const
{
    if (!perTapMode)
        return true;
    if (targetIsAll())
        return DelayHeadActivation::anyActive(activeTapMask);
    return isTapActive(tapIndexForTarget(editTarget));
}

//==============================================================================
// Painting
//==============================================================================
void SpatialHeadView::drawPolyline(juce::Graphics& g, const Polyline& line, juce::Colour base, float baseAlpha,
                                   float thickness, bool endpointDots, bool maskBehindHead) const
{
    // Depth-faded, depth-weighted segments: near geometry is brighter AND
    // heavier, far geometry recedes like a dimming phosphor trace.
    for (size_t i = 1; i < line.size(); ++i)
    {
        float d0 = 0.0f, d1 = 0.0f;
        const auto p0 = project(line[i - 1], d0);
        const auto p1 = project(line[i], d1);
        const float depth = 0.5f * (d0 + d1);
        const auto midpoint = (p0 + p1) * 0.5f;
        const float mask = maskBehindHead ? guideVisibilityAt(midpoint) : 1.0f;
        const float alpha = baseAlpha * (1.0f - 0.68f * depth) * mask;
        if (alpha <= 0.01f)
            continue;
        g.setColour(base.withAlpha(alpha));
        g.drawLine({p0, p1}, thickness * (1.35f - 0.6f * depth));
    }

    // Brighter dots where an OPEN polyline terminates (CRT trace endpoints).
    if (endpointDots && line.size() >= 2)
    {
        const auto& a = line.front();
        const auto& b = line.back();
        const bool closed = std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z) < 1.0e-4f;
        if (!closed)
        {
            for (const auto& end : {a, b})
            {
                float depth = 0.0f;
                const auto p = project(end, depth);
                const float alpha = juce::jmin(1.0f, baseAlpha * 1.5f) * (1.0f - 0.68f * depth);
                if (alpha <= 0.01f)
                    continue;
                const float r = thickness * (1.35f - 0.6f * depth) * 0.9f;
                g.setColour(base.withAlpha(alpha));
                g.fillEllipse(p.x - r, p.y - r, r * 2.0f, r * 2.0f);
            }
        }
    }
}

float SpatialHeadView::guideVisibilityAt(juce::Point<float> point) const
{
    // Guide geometry is a backdrop. Fade it through an ellipse covering the
    // head/headphones so rings and trajectories do not cut across the face.
    const float rx = juce::jmax(1.0f, projScale() * 0.34f);
    const float ry = juce::jmax(1.0f, projScale() * 0.52f);
    const float nx = (point.x - viewCentre.x) / rx;
    const float ny = (point.y - viewCentre.y) / ry;
    const float radius = std::sqrt(nx * nx + ny * ny);
    const float t = juce::jlimit(0.0f, 1.0f, (radius - 0.82f) / 0.26f);
    return t * t * (3.0f - 2.0f * t);
}

void SpatialHeadView::drawMaskedGuideLine(juce::Graphics& g, juce::Point<float> from, juce::Point<float> to,
                                          juce::Colour colour, float alpha, float thickness) const
{
    const int segments = juce::jlimit(1, 64, juce::roundToInt(from.getDistanceFrom(to) / 6.0f));
    for (int i = 0; i < segments; ++i)
    {
        const float a = (float)i / (float)segments;
        const float b = (float)(i + 1) / (float)segments;
        const auto p0 = from + (to - from) * a;
        const auto p1 = from + (to - from) * b;
        const float visibility = guideVisibilityAt((p0 + p1) * 0.5f);
        if (visibility <= 0.01f)
            continue;
        g.setColour(colour.withAlpha(alpha * visibility));
        g.drawLine({p0, p1}, thickness);
    }
}

void SpatialHeadView::paint(juce::Graphics& g)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();

    if (glActive.load())
    {
        // The GPU renderer owns the bed + 3D background; overlay only the chrome
        // (the rest stays transparent so the GL render shows through).
        renderChrome(g);
    }
    else
    {
        // Panel bed: void black. No idle animation: constant breathing proved
        // fatiguing. Audio-driven glow appears only while the echo signal is
        // sounding (audioGlow drives renderGlowUnderlay).
        g.fillAll(CRTLookAndFeel::getVoidBlack());
        renderGlowUnderlay(g);
        renderScene(g);
    }

    // Focus ring (keyboard interaction affordance).
    if (hasFocus)
    {
        g.setColour(green.withAlpha(0.5f));
        g.drawRect(getLocalBounds(), 1);
    }
}

void SpatialHeadView::renderScene(juce::Graphics& g)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();
    const float dim = isEnabled ? 1.0f : 0.35f;

    // --- 3D background (head): the GPU renderer owns this in GL mode ---
    // Head + headphones are the spatial anchor; the distance shells and the
    // compass carry the distance and azimuth references.
    // Open contours get endpoint dots (phosphor trace terminations).
    for (const auto& line : headLines)
        drawPolyline(g, line, green, 0.72f * dim, 1.3f, true);

    renderChrome(g);
}

void SpatialHeadView::renderChrome(juce::Graphics& g)
{
    // Always juce::Graphics - composited over whichever 3D background rendered.
    drawDistanceRings(g);
    drawCompassLabels(g);
    drawMotionTrail(g);
    drawDragTrail(g);
    drawSourceMarker(g);
    drawFacingReadout(g);
    drawPositionReadout(g);
}

void SpatialHeadView::renderGlowUnderlay(juce::Graphics& g)
{
    // Downsampled-blur phosphor pass: the scene at quarter resolution,
    // gaussian-blurred, composited under the crisp strokes. Skipped when the
    // view is disabled (dim state should look inert, not radiant).
    if (!isEnabled)
        return;

    const int w = juce::jmax(1, getWidth() / 4);
    const int h = juce::jmax(1, getHeight() / 4);
    if (!glowScene.isValid() || glowScene.getWidth() != w || glowScene.getHeight() != h)
    {
        glowScene = juce::Image(juce::Image::ARGB, w, h, true);
        glowBlur = juce::Image(juce::Image::ARGB, w, h, true);
    }
    else
    {
        glowScene.clear(glowScene.getBounds());
    }

    {
        juce::Graphics gi(glowScene);
        gi.addTransform(juce::AffineTransform::scale(0.25f));
        renderScene(gi);
    }

    juce::ImageConvolutionKernel kernel(5);
    kernel.createGaussianBlur(1.7f);
    kernel.applyToImage(glowBlur, glowScene, glowBlur.getBounds());

    // Audio-reactive lift (CPU fallback mirror of the GL bloom modulation),
    // capped at +20% so the phosphor glow breathes subtly with the signal.
    g.setOpacity(0.55f * (1.0f + 0.20f * audioGlow()));
    g.drawImage(glowBlur, getLocalBounds().toFloat());
    g.setOpacity(1.0f);
}

void SpatialHeadView::drawDragTrail(juce::Graphics& g)
{
    // Phosphor afterimage while the user drags a marker: recent positions
    // fade out behind the glyph like a decaying trace.
    if (!isDragging || dragTrailCount < 2)
        return;

    const auto green = CRTLookAndFeel::getTerminalGreen();
    for (int i = 1; i < dragTrailCount; ++i)
    {
        const float age = (float)i / (float)dragTrailCount;  // 0 old -> 1 new
        g.setColour(green.withAlpha(0.30f * age));
        g.drawLine({dragTrail[(size_t)i - 1], dragTrail[(size_t)i]}, 1.0f + age);
    }
}

void SpatialHeadView::drawDistanceRings(juce::Graphics& g)
{
    // The selected target keeps one persistent live shell. The 0.5/2/5/10 m
    // reference shells appear only while distance is being adjusted, then fade.
    const auto green = CRTLookAndFeel::getTerminalGreen();
    const float dim = isEnabled ? 1.0f : 0.35f;
    const float referenceOpacity = distanceReferenceOpacity();
    static constexpr int seg = 48;

    auto equatorRing = [](float radius)
    {
        Polyline ring;
        ring.reserve(seg + 1);
        for (int i = 0; i <= seg; ++i)
        {
            const float t = 2.0f * kPi * (float)i / (float)seg;
            ring.push_back({radius * std::sin(t), 0.0f, radius * std::cos(t)});
        }
        return ring;
    };

    if (referenceOpacity > 0.01f)
    {
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(juce::jmax(11.0f, viewScale * 0.085f)));
        struct Shell
        {
            float meters;
            const char* label;
        };
        int shellIndex = 0;
        static const float azimuths[] = {45.0f, 135.0f, 225.0f, 315.0f};
        for (const auto& shell : {Shell{0.5f, "0.5"}, Shell{2.0f, "2"}, Shell{5.0f, "5"}, Shell{10.0f, "10M"}})
        {
            const float r = distanceToRadius(shell.meters);
            const float alpha = (shell.meters > 5.0f ? 0.08f : 0.065f) * dim * referenceOpacity;
            drawPolyline(g, equatorRing(r), green, alpha, 1.0f, false, true);

            float depth = 0.0f;
            const auto p = project(sphericalToWorld(azimuths[shellIndex], 0.0f, r), depth);
            g.setColour(green.withAlpha(0.24f * dim * referenceOpacity * (1.0f - 0.5f * depth)));
            g.drawText(shell.label, (int)p.x - 22, (int)p.y - 6, 44, 12, juce::Justification::centred);
            ++shellIndex;
        }
    }

    // Live shell: reads out wheel/slider moves and warms up inside 1 m - the
    // near field literally glows as the source approaches the head.
    if (isEnabled && canEditCurrentTarget())
    {
        const auto& pos = currentPos();
        const float nearBoost = juce::jlimit(0.0f, 1.0f, (1.0f - pos.dist) / 0.9f);
        drawPolyline(g, equatorRing(distanceToRadius(pos.dist)), green, 0.12f + 0.22f * nearBoost,
                     1.0f + 0.5f * nearBoost, false, true);
    }
}

void SpatialHeadView::drawFacingReadout(juce::Graphics& g)
{
    // Plain-words position of the selected target ("where is it, in English"),
    // anchored bottom-left like an instrument status line.
    if (!isEnabled || !canEditCurrentTarget())
        return;

    const auto& pos = currentPos();
    const float az = std::abs(pos.az);
    const bool onLeft = pos.az < 0.0f;
    juce::String facing;
    if (az < 22.5f)
        facing = "FRONT";
    else if (az < 67.5f)
        facing = onLeft ? "FRONT-LEFT" : "FRONT-RIGHT";
    else if (az < 112.5f)
        facing = onLeft ? "LEFT" : "RIGHT";
    else if (az < 157.5f)
        facing = onLeft ? "BEHIND-LEFT" : "BEHIND-RIGHT";
    else
        facing = "BEHIND";
    facing << (pos.el > 15.0f ? " / ABOVE" : pos.el < -15.0f ? " / BELOW" : " / LEVEL");
    if (pos.dist < 0.7f)
        facing << " / NEAR";
    else if (pos.dist > 3.0f)
        facing << " / FAR";

    juce::String prefix;
    if (perTapMode)
        prefix = (targetIsAll() ? juce::String("ALL") : "H" + juce::String(primarySlotForTarget())) + " > ";

    // Single-axis drag cue: while a Shift-constrained drag holds one axis fixed.
    if (isDragging && dragConstraint != DragAxis::Free)
        prefix = juce::String(dragConstraint == DragAxis::Azimuth ? "[AZ] " : "[EL] ") + prefix;
    // Snap confirm cue: while a free drag is magnet-locked to a cardinal anchor.
    if (isDragging && activeSnapLabel.isNotEmpty())
        prefix = "[SNAP " + activeSnapLabel + "] " + prefix;

    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.55f));
    g.drawText(prefix + facing, getLocalBounds().reduced(8), juce::Justification::bottomLeft);

    // First-load discovery cue: nudge toward the PER-HEAD toggle (up-right of
    // this view) until the user tries it once. Bottom-
    // right, because the corner position readout owns the top-right.
    if (discoveryCueVisible && !perTapMode)
    {
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.45f));
        g.drawText("TRY PER-HEAD: PLACE EACH HEAD AROUND YOUR HEAD ^", getLocalBounds().reduced(8),
                   juce::Justification::bottomRight);
    }
}

juce::String SpatialHeadView::positionReadoutText() const
{
    const auto& pos = currentPos();
    const auto degree = juce::String::charToString(0x00B0);
    const auto middot = juce::String::charToString(0x00B7);
    juce::String text;
    text << (int)pos.az << degree << " az " << middot << " " << (int)pos.el << degree << " el " << middot << " "
         << juce::String(pos.dist, 1) << " m";
    return text;
}

juce::Rectangle<int> SpatialHeadView::calculatePositionReadoutBounds() const
{
    if (!isEnabled || !canEditCurrentTarget())
        return {};
    const auto font = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize);
    const int w = juce::GlyphArrangement::getStringWidthInt(font, positionReadoutText()) + 10;
    return getLocalBounds().reduced(8).removeFromTop(16).removeFromRight(w);
}

void SpatialHeadView::drawPositionReadout(juce::Graphics& g)
{
    // One-line az/el/dist readout, top-right; clicking it opens the exact-entry
    // popover.
    const auto bounds = calculatePositionReadoutBounds();
    if (bounds.isEmpty())
        return;

    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
    const auto green = CRTLookAndFeel::getTerminalGreen();
    g.setColour(green.withAlpha(readoutHovered ? UIConstants::alphaBright : UIConstants::alphaLabel));
    g.drawText(positionReadoutText(), bounds, juce::Justification::centredRight);
    if (readoutHovered)
    {
        g.setColour(green.withAlpha(UIConstants::alphaDim));
        g.drawHorizontalLine(bounds.getBottom(), (float)bounds.getX(), (float)bounds.getRight());
    }
}

void SpatialHeadView::drawCompassLabels(juce::Graphics& g)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();
    const auto font = CRTLookAndFeel::getTerminalFontStatic(juce::jmax(11.0f, viewScale * 0.11f));
    g.setFont(font);

    struct Mark
    {
        const char* text;
        float az, el;
    };
    for (const auto& mark :
         {Mark{"F", 0.0f, 0.0f}, Mark{"B", 180.0f, 0.0f}, Mark{"L", -90.0f, 0.0f}, Mark{"R", 90.0f, 0.0f}})
    {
        float depth = 0.0f;
        const auto p = project(sphericalToWorld(mark.az, mark.el, 1.06f), depth);
        g.setColour(green.withAlpha(0.55f * (1.0f - 0.5f * depth)));
        g.drawText(mark.text, (int)p.x - 8, (int)p.y - 8, 16, 16, juce::Justification::centred);
    }
}

SpatialHeadView::SourcePos SpatialHeadView::motionDisplayPos(int slot) const
{
    const SourcePos base = positions[juce::jlimit(0, 3, slot)];
    const auto& mp = motionPreviewState;
    if (!mp.enabled || !isEnabled)
        return base;

    // Pattern offsets are additive around the base position (the engine has no
    // absolute mode). Azimuth wraps like the DSP
    // apply path so Orbit rotates through the rear instead of pinning at ±180.
    auto combine = [&](float mAz, float mEl, float mDist) -> SourcePos
    {
        SourcePos out;
        out.az = std::remainder(base.az + mAz, 360.0f);
        out.el = juce::jlimit(-90.0f, 90.0f, base.el + mEl);
        out.dist = juce::jlimit(kMinDistanceM, kMaxDistanceM, base.dist + mDist);
        return out;
    };

    // Linked (or the global slot): use the DSP's actual smoothed modulation -
    // more truthful than re-evaluating the raw LFO.
    if (!perTapMode || slot == 0)
        return combine(mp.currentModAz, mp.currentModEl, mp.currentModDist);

    // Per-tap: evaluate the trajectory at this tap's phase offset (matches the trail's
    // now-point, since the preview atomics only carry the linked/global value).
    const int tap = slot - 1;
    double offset = 0.0;
    if (mp.tapMode == 1)
        offset = tap * (1.0 / 3.0);  // Constellation
    else if (mp.tapMode == 2)
        offset = tap == 0 ? 0.0 : (tap == 1 ? 0.33 : 0.67);  // Cascade
    offset *= mp.tapSpread;
    const double cyc = mp.phaseCycles + offset;

    const auto offsets = spatialmotion::evaluateTrajectory((SpatialIDs::motion::Pattern)mp.patternId, cyc, mp.depth01);
    return combine(offsets.azimuthDeg, offsets.elevationDeg, offsets.distanceMeters);
}

void SpatialHeadView::drawMotionTrail(juce::Graphics& g)
{
    const auto& mp = motionPreviewState;
    if (!mp.enabled || !isEnabled || mp.depth01 < 0.01f)
        return;

    const auto green = CRTLookAndFeel::getTerminalGreen();
    constexpr int segments = 84;

    // One pattern cycle traced additively around a base position (mirrors the
    // DSP's apply path in PluginProcessor's motion block) - the route each echo
    // travels. The live "now" marker is the emitter glyph itself (drawn at
    // motionDisplayPos), so no separate now-dot is needed.
    auto tracePath = [&](const SourcePos& base, double phaseOffsetCycles, float alpha)
    {
        juce::Point<float> previous;
        bool hasPrevious = false;
        for (int i = 0; i <= segments; ++i)
        {
            const double cyc = mp.phaseCycles + phaseOffsetCycles + (double)i / (double)segments;
            const auto offsets =
                spatialmotion::evaluateTrajectory((SpatialIDs::motion::Pattern)mp.patternId, cyc, mp.depth01);

            float az, el, dist;
            // The trajectory evaluator returns offsets; combine them with the base.
            // Still pattern (id=0) returns zero offsets, so positions stay at base.
            az = std::remainder(base.az + offsets.azimuthDeg, 360.0f);
            el = juce::jlimit(-90.0f, 90.0f, base.el + offsets.elevationDeg);
            dist = juce::jlimit(kMinDistanceM, kMaxDistanceM, base.dist + offsets.distanceMeters);

            float depth = 0.0f;
            const auto p = project(sphericalToWorld(az, el, distanceToRadius(dist)), depth);
            if (hasPrevious)
                drawMaskedGuideLine(g, previous, p, green, alpha, 1.0f);
            previous = p;
            hasPrevious = true;
        }
    };

    if (!perTapMode)
    {
        tracePath(positions[0], 0.0, 0.28f);
    }
    else
    {
        // Per-tap constellation: phase offsets follow the DSP's tap spread.
        for (int tap = 0; tap < 3; ++tap)
        {
            if (!isTapActive(tap))
                continue;
            double offset = 0.0;
            if (mp.tapMode == 1)
                offset = tap * (1.0 / 3.0);  // Constellation
            else if (mp.tapMode == 2)
                offset = tap == 0 ? 0.0 : (tap == 1 ? 0.33 : 0.67);  // Cascade
            offset *= mp.tapSpread;
            const int slot = tap + 1;
            const bool selected = (primarySlotForTarget() == slot) || targetIsAll();
            if (selected)
                tracePath(positions[slot], offset, 0.24f);
        }
    }
}

void SpatialHeadView::drawSourceMarker(juce::Graphics& g)
{
    const auto green = CRTLookAndFeel::getTerminalGreen();

    auto drawDropLines = [&](const SourcePos& pos, float alpha)
    {
        const float r = distanceToRadius(pos.dist);
        float depth = 0.0f;
        const auto p = project(sphericalToWorld(pos.az, pos.el, r), depth);
        float dropDepth = 0.0f;
        const auto pDrop = project(sphericalToWorld(pos.az, 0.0f, r * std::cos(pos.el * kDegToRad)), dropDepth);
        drawMaskedGuideLine(g, p, pDrop, green, alpha, 1.0f);
        float cDepth = 0.0f;
        const auto pCentre = project({0.0f, 0.0f, 0.0f}, cDepth);
        drawMaskedGuideLine(g, pCentre, pDrop, green, alpha * 0.7f, 1.0f);
    };

    // Marker glyph per slot: global = dot, H1 = triangle, H2 = square, H3 = circle.
    // Hovered markers brighten (grab affordance); the grabbed marker's halo
    // swells while dragging.
    auto drawGlyph = [&](int slot, const SourcePos& pos, bool selected, bool active)
    {
        const bool hovered = active && (slot == hoveredSlot);
        const float r = distanceToRadius(pos.dist);
        float depth = 0.0f;
        const auto p = project(sphericalToWorld(pos.az, pos.el, r), depth);
        // Two brightness cues compose: screen-depth fade (behind the head
        // recedes) and listener-distance hierarchy (nearer taps read
        // brighter). Unselected floor raised from 0.38 for legibility.
        // Echoes are the focal point: keep them bright, with only a
        // light back/far dimming so unselected + distant taps still read clearly
        // against the toned-down head/sphere backdrop.
        const float dimNear = 1.0f - 0.16f * depth;
        const float distNorm =
            juce::jlimit(0.0f, 1.0f, std::log(pos.dist / kMinDistanceM) / std::log(kMaxDistanceM / kMinDistanceM));
        const float nearBoost = 1.0f - 0.15f * distNorm;
        const float stateAlpha = active ? 1.0f : 0.22f;
        const float a = (selected ? 1.0f : (hovered ? 0.92f : 0.80f)) * dimNear * nearBoost * stateAlpha;

        // Audio-reactive pulse: each marker breathes with its tap's audio signal
        // on a channel separate from distance (radius/alpha, not the brightness
        // that encodes near/far). Frozen while dragging or orbiting so it never
        // fights precision aiming. Per-tap levels are independent of selection.
        {
            float ag = 0.0f;
            if (active && !isDragging && !isOrbiting)
            {
                // In Linked mode, all markers pulse to global audio; in per-tap mode,
                // each marker pulses to its own tap's envelope level.
                if (perTapMode && slot >= 1 && slot <= 3)
                    ag = getTapAudioGlow(slot - 1);  // slot 1..3 -> tapIndex 0..2
                else if (!perTapMode)
                    ag = audioGlow();  // Linked mode: global glow
            }
            if (ag > 0.02f)
            {
                const float pulseR = (15.0f + 11.0f * ag) * dimNear;
                const float pulseA = 0.22f * ag * dimNear;
                juce::ColourGradient pulse(green.withAlpha(pulseA), p.x, p.y, green.withAlpha(0.0f), p.x + pulseR, p.y,
                                           true);
                g.setGradientFill(pulse);
                g.fillEllipse(p.x - pulseR, p.y - pulseR, pulseR * 2.0f, pulseR * 2.0f);
            }
        }

        if (active && (selected || hovered))
        {
            const bool grabbed = selected && isDragging;
            const float haloR = (grabbed ? 17.0f : hovered ? 15.0f : 13.0f) * dimNear;
            const float haloA = (grabbed ? 0.60f : selected ? 0.45f : 0.28f) * dimNear;
            juce::ColourGradient halo(green.withAlpha(haloA), p.x, p.y, green.withAlpha(0.0f), p.x + haloR, p.y, true);
            g.setGradientFill(halo);
            g.fillEllipse(p.x - haloR, p.y - haloR, haloR * 2.0f, haloR * 2.0f);
        }

        // Luminous emitter node - each echo is a point-source of sound, not a
        // flat glyph. Bright core + soft bloom +
        // a fine reticle ring; tap identity is the index label, not the shape, so
        // all echoes read as one kind of emitter. Selected takes the magenta accent.
        const auto accent = CRTLookAndFeel::getMagentaInterference();
        const juce::Colour ring = selected && active ? accent : green;
        const float coreR = (selected ? 4.0f : 3.4f) * dimNear;
        const float reticleR = (selected ? 8.5f : hovered ? 7.5f : 6.8f) * dimNear;

        {  // emitter bloom - a strong halo so each echo reads as a bright source
            const float bloomR = reticleR * 2.3f;
            juce::ColourGradient bloom(ring.withAlpha(0.55f * a), p.x, p.y, ring.withAlpha(0.0f), p.x + bloomR, p.y,
                                       true);
            g.setGradientFill(bloom);
            g.fillEllipse(p.x - bloomR, p.y - bloomR, bloomR * 2.0f, bloomR * 2.0f);
        }

        g.setColour(ring.withAlpha(a));  // reticle ring (targeting cue)
        g.drawEllipse(p.x - reticleR, p.y - reticleR, reticleR * 2.0f, reticleR * 2.0f, selected ? 1.8f : 1.4f);

        g.setColour(CRTLookAndFeel::getCleanLight().withAlpha(a));  // bright white-hot source core
        g.fillEllipse(p.x - coreR, p.y - coreR, coreR * 2.0f, coreR * 2.0f);
        g.setColour(ring.withAlpha(0.85f * a));
        g.drawEllipse(p.x - coreR - 1.5f, p.y - coreR - 1.5f, (coreR + 1.5f) * 2.0f, (coreR + 1.5f) * 2.0f, 1.3f);

        if (perTapMode)  // identity by label, not shape
        {
            g.setFont(CRTLookAndFeel::getTerminalFontStatic(9.5f));
            const float labelAlpha = active ? (selected ? 0.9f * dimNear : 0.45f * dimNear) : 0.18f * dimNear;
            g.setColour(ring.withAlpha(labelAlpha));
            g.drawText("H" + juce::String(slot), (int)(p.x + reticleR + 3.0f), (int)(p.y - 7.0f), 20, 14,
                       juce::Justification::centredLeft);
        }
    };

    // While Spatial Motion runs, draw each echo at its live modulated position
    // so the markers visibly orbit the head (paused while dragging so a grab
    // still tracks the cursor).
    const bool liveMotion = motionPreviewState.enabled && isEnabled && !isDragging;

    if (!perTapMode)
    {
        const SourcePos disp = liveMotion ? motionDisplayPos(0) : positions[0];
        if (hoveredSlot == 0 || isDragging || distanceReferenceOpacity() > 0.01f)
            drawDropLines(disp, 0.22f);
        drawGlyph(0, disp, true, true);
    }
    else
    {
        const int sel = primarySlotForTarget();
        for (int slot = 1; slot <= 3; ++slot)
        {
            const bool active = isTapActive(slot - 1);
            const bool selected = active && (targetIsAll() || slot == sel);
            const SourcePos disp = liveMotion && active ? motionDisplayPos(slot) : positions[slot];
            const int projectionSlot = hoveredSlot >= 1 ? hoveredSlot : sel;
            if (slot == projectionSlot && (hoveredSlot == slot || isDragging || distanceReferenceOpacity() > 0.01f))
                drawDropLines(disp, 0.18f);
            drawGlyph(slot, disp, selected, active);
        }
    }

    drawGizmo(g);
}

//==============================================================================
// Axis gizmo: click-to-constrain AZ/EL handles around the selected marker
//==============================================================================
juce::Point<float> SpatialHeadView::gizmoAxisDir(const SourcePos& pos, bool azimuth, bool& foreshortened) const
{
    // Screen-space tangent of the azimuth (or elevation) circle at the marker.
    const float r = distanceToRadius(pos.dist);
    const float da = azimuth ? 5.0f : 0.0f;
    const float de = azimuth ? 0.0f : 5.0f;
    float d0 = 0.0f, d1 = 0.0f;
    const auto p0 = project(sphericalToWorld(pos.az - da, juce::jlimit(-90.0f, 90.0f, pos.el - de), r), d0);
    const auto p1 = project(sphericalToWorld(pos.az + da, juce::jlimit(-90.0f, 90.0f, pos.el + de), r), d1);
    auto v = p1 - p0;
    const float len = v.getDistanceFromOrigin();
    foreshortened = len < 9.0f;  // axis nearly edge-on: the handle collapses
    if (len > 0.001f)
        v /= len;
    return v;
}

void SpatialHeadView::drawGizmo(juce::Graphics& g)
{
    // Only for a single selected marker (ALL-mode has no single anchor). Hidden
    // while motion orbits the markers (it anchors the base, which would read as
    // detached from the moving glyph) - pause motion or grab a marker to edit.
    if (!isEnabled || targetIsAll() || !canEditCurrentTarget())
        return;
    if (motionPreviewState.enabled && !isDragging)
        return;

    const auto& pos = currentPos();
    float md = 0.0f;
    const auto p = project(sphericalToWorld(pos.az, pos.el, distanceToRadius(pos.dist)), md);
    const auto green = CRTLookAndFeel::getTerminalGreen();
    const auto accent = CRTLookAndFeel::getMagentaInterference();
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));

    for (const bool azimuth : {true, false})
    {
        bool fore = false;
        const auto dir = gizmoAxisDir(pos, azimuth, fore);
        if (fore)
            continue;

        const bool active = isDragging && dragConstraint == (azimuth ? DragAxis::Azimuth : DragAxis::Elevation);
        const auto col = active ? accent : green;
        const float th = active ? 2.0f : 1.3f;
        constexpr float in = 12.0f, out = 30.0f;
        const juce::Point<float> perp(-dir.getY(), dir.getX());

        g.setColour(col.withAlpha(active ? 0.95f : 0.40f));
        for (float s : {1.0f, -1.0f})
        {
            const auto a = p + dir * (out * s);
            const auto b = p + dir * (in * s);
            g.drawLine({b, a}, th);
            const auto base = a - dir * (5.0f * s);
            g.drawLine({a, base + perp * 3.0f}, th);
            g.drawLine({a, base - perp * 3.0f}, th);
        }
        const auto lp = p + dir * (out + 9.0f);
        g.setColour(col.withAlpha(active ? 0.9f : 0.5f));
        g.drawText(azimuth ? "AZ" : "EL", (int)lp.x - 9, (int)lp.y - 7, 18, 14, juce::Justification::centred);
    }
}

SpatialHeadView::DragAxis SpatialHeadView::gizmoHitTest(juce::Point<float> cursor) const
{
    if (!isEnabled || targetIsAll() || !canEditCurrentTarget())
        return DragAxis::Free;

    const auto& pos = currentPos();
    float md = 0.0f;
    const auto p = project(sphericalToWorld(pos.az, pos.el, distanceToRadius(pos.dist)), md);
    auto onHandle = [&](bool azimuth) -> bool
    {
        bool fore = false;
        const auto dir = gizmoAxisDir(pos, azimuth, fore);
        if (fore)
            return false;
        const auto rel = cursor - p;
        const float along = rel.getX() * dir.getX() + rel.getY() * dir.getY();
        const float perp = std::abs(rel.getX() * -dir.getY() + rel.getY() * dir.getX());
        return std::abs(along) >= 10.0f && std::abs(along) <= 33.0f && perp <= 9.0f;
    };
    if (onHandle(true))
        return DragAxis::Azimuth;
    if (onHandle(false))
        return DragAxis::Elevation;
    return DragAxis::Free;
}

//==============================================================================
// Layout / timer
//==============================================================================
void SpatialHeadView::resized()
{
    const auto bounds = getLocalBounds().toFloat();
    viewCentre = bounds.getCentre().translated(0.0f, bounds.getHeight() * 0.02f);
    // Fill the viewport: the 1 m sphere nearly
    // fills the short dimension; the 10 m rim (1.22 R) may clip slightly top/
    // bottom, which is fine - its labels sit on the right.
    viewScale = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f / 1.10f;
    updateGLAttachment();
}

void SpatialHeadView::setTimerRate(int hz)
{
    if (hz == timerRateHz && isTimerRunning())
        return;
    timerRateHz = hz;
    startTimerHz(timerRateHz);
}

void SpatialHeadView::timerCallback()
{
    // Repaint only while something visible moves; a still view costs nothing.
    constexpr float kGlowRepaintThreshold = 0.002f;
    bool needsRepaint = false;

    // Audio-reactive glow: follow the echo-branch meter with gentle ballistics
    // (fast attack so it feels responsive, slow release so it doesn't flicker).
    // Perceptual sqrt lift makes quiet passages visible without slamming loud
    // ones. Frame-rate compensated so the feel is constant at 15 or 60 Hz.
    const float rateComp = static_cast<float>(kIdleTimerHz) / static_cast<float>(timerRateHz);
    {
        float target = 0.0f;
        if (audioLevelProvider && isEnabled)
        {
            const float raw = juce::jlimit(0.0f, 1.0f, audioLevelProvider() * 1.25f);
            target = std::sqrt(raw);
        }
        const float previous = audioLevelSmoothed;
        const float coeff = (target > audioLevelSmoothed ? 0.55f : 0.14f) * rateComp;
        audioLevelSmoothed += (target - audioLevelSmoothed) * juce::jlimit(0.0f, 1.0f, coeff);
        if (audioLevelSmoothed < 0.001f)
            audioLevelSmoothed = 0.0f;
        needsRepaint |= std::abs(audioLevelSmoothed - previous) > kGlowRepaintThreshold;
    }

    // Per-tap audio envelope smoothing: drive each marker's glow pulse independently
    // from its tap's audio level (0..2 linear peak-envelope). Frozen during drag/orbit
    // to avoid fighting precision aiming. Separate channel from distance encoding.
    {
        const bool frozen = isDragging || isOrbiting;
        for (size_t tap = 0; tap < tapAudioLevelSmoothed.size(); ++tap)
        {
            const int tapIndex = static_cast<int>(tap);
            float target = 0.0f;
            if (isTapActive(tapIndex) && !frozen && tapAudioLevelProvider && isEnabled)
            {
                const float raw = juce::jlimit(0.0f, 2.0f, tapAudioLevelProvider(tapIndex));
                target = std::sqrt(raw * 0.5f);  // perceptual lift, normalized to 0..1
            }
            auto& smoothed = tapAudioLevelSmoothed[tap];
            const float previous = smoothed;
            const float coeff = (target > smoothed ? 0.55f : 0.14f) * rateComp;
            smoothed += (target - smoothed) * juce::jlimit(0.0f, 1.0f, coeff);
            if (smoothed < 0.001f)
                smoothed = 0.0f;
            needsRepaint |= std::abs(smoothed - previous) > kGlowRepaintThreshold;
        }
    }

    // Orbit inertia: a release throw coasts and decays (~0.9 per 60Hz tick).
    if (!isOrbiting && (std::abs(camYawVelDeg) > 0.02f || std::abs(camPitchVelDeg) > 0.02f))
    {
        camYawDeg = std::fmod(camYawDeg + camYawVelDeg, 360.0f);
        camPitchDeg = juce::jlimit(-67.0f, 98.0f, camPitchDeg + camPitchVelDeg);
        camYawVelDeg *= 0.90f;
        camPitchVelDeg *= 0.90f;
        needsRepaint = true;
        if (std::abs(camYawVelDeg) <= 0.02f && std::abs(camPitchVelDeg) <= 0.02f)
        {
            // Throw finished: drop back to the idle rate.
            camYawVelDeg = camPitchVelDeg = 0.0f;
            setTimerRate(kIdleTimerHz);
        }
    }
    else if (!isOrbiting && timerRateHz != kIdleTimerHz)
    {
        setTimerRate(kIdleTimerHz);  // released without a throw
    }

    // Distance reference shells fade out after a distance edit; one extra
    // frame after they reach zero clears the last of them.
    const bool referencesVisible = distanceReferenceOpacity() > 0.0f;
    needsRepaint |= referencesVisible || distanceReferencesWereVisible;
    distanceReferencesWereVisible = referencesVisible;

    if (needsRepaint)
        repaint();
}

void SpatialHeadView::setDiscoveryCueVisible(bool visible)
{
    if (discoveryCueVisible == visible)
        return;
    discoveryCueVisible = visible;
    repaint();
}

void SpatialHeadView::setRadarEnabled(bool enabled)
{
    if (isEnabled == enabled)
        return;
    isEnabled = enabled;
    if (enabled)
        setTimerRate(kIdleTimerHz);
    else
        stopTimer();
    repaint();
}

//==============================================================================
// Position setters (external - automation/preset sync via SpatialPanel/editor)
//==============================================================================
void SpatialHeadView::setAzimuth(float az)
{
    if (isDragging && !perTapMode)
        return;  // interaction owns the value mid-drag
    const float clamped = juce::jlimit(-180.0f, 180.0f, az);
    if (std::abs(clamped - positions[0].az) < 0.01f)
        return;
    positions[0].az = clamped;
    repaint();
}

void SpatialHeadView::setElevation(float el)
{
    if (isDragging && !perTapMode)
        return;
    const float clamped = juce::jlimit(-90.0f, 90.0f, el);
    if (std::abs(clamped - positions[0].el) < 0.01f)
        return;
    positions[0].el = clamped;
    repaint();
}

void SpatialHeadView::setDistance(float dist)
{
    if (isDragging && !perTapMode)
        return;
    const float clamped = juce::jlimit(kMinDistanceM, kMaxDistanceM, dist);
    if (std::abs(clamped - positions[0].dist) < 0.001f)
        return;
    positions[0].dist = clamped;
    repaint();
}

void SpatialHeadView::setTapPosition(int tapIndex, float azDeg, float elDeg, float distM)
{
    if (tapIndex < 0 || tapIndex > 2)
        return;
    const int slot = tapIndex + 1;
    if (isDragging && perTapMode && (targetIsAll() || slot == primarySlotForTarget()))
        return;  // interaction owns the value mid-drag
    auto& pos = positions[slot];
    const float az = juce::jlimit(-180.0f, 180.0f, azDeg);
    const float el = juce::jlimit(-90.0f, 90.0f, elDeg);
    const float dist = juce::jlimit(kMinDistanceM, kMaxDistanceM, distM);
    if (std::abs(az - pos.az) < 0.01f && std::abs(el - pos.el) < 0.01f && std::abs(dist - pos.dist) < 0.001f)
        return;
    pos = {az, el, dist};
    repaint();
}

void SpatialHeadView::setPerTapMode(bool independentMode)
{
    if (perTapMode == independentMode)
        return;
    perTapMode = independentMode;
    // Clamp the edit target to what the mode can address.
    if (perTapMode && editTarget == Target::Global)
    {
        const int firstActive = DelayHeadActivation::firstActiveIndex(activeTapMask);
        const Target nextTarget = targetForTapIndex(firstActive >= 0 ? firstActive : 0);
        editTarget = nextTarget;
        if (onTargetChanged)
            onTargetChanged(editTarget);
    }
    else if (!perTapMode && editTarget != Target::Global)
        setEditTarget(Target::Global);
    repaint();
}

void SpatialHeadView::setActiveTapMask(DelayHeadActivation::Mask activeHeads)
{
    if (activeTapMask == activeHeads)
        return;

    activeTapMask = activeHeads;

    // A mode change during a direct manipulation must end the gesture on the
    // head that actually owned it; continuing would retarget the same drag to a
    // different head after selection fallback.
    if (perTapMode && isDragging)
    {
        isDragging = false;
        dragTrailCount = 0;
        dragConstraint = DragAxis::Free;
        dragConstrainRequested = false;
        activeSnapLabel.clear();
        endActiveGesture();
    }

    if (hoveredSlot >= 1 && !isTapActive(hoveredSlot - 1))
    {
        hoveredSlot = -1;
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    for (int tap = 0; tap < 3; ++tap)
        if (!isTapActive(tap))
            tapAudioLevelSmoothed[static_cast<std::size_t>(tap)] = 0.0f;

    if (perTapMode && !canEditCurrentTarget())
    {
        const int firstActive = DelayHeadActivation::firstActiveIndex(activeTapMask);
        const Target nextTarget = targetForTapIndex(firstActive >= 0 ? firstActive : 0);
        if (editTarget != nextTarget)
        {
            editTarget = nextTarget;
            if (onTargetChanged)
                onTargetChanged(editTarget);
        }
    }

    repaint();
}

void SpatialHeadView::setEditTarget(Target t)
{
    if (!perTapMode)
        t = Target::Global;
    else if (t == Target::Global)
    {
        const int firstActive = DelayHeadActivation::firstActiveIndex(activeTapMask);
        t = targetForTapIndex(firstActive >= 0 ? firstActive : 0);
    }

    if (perTapMode)
    {
        if (t == Target::All && !DelayHeadActivation::anyActive(activeTapMask))
            return;
        const int tapIndex = tapIndexForTarget(t);
        if (tapIndex >= 0 && !isTapActive(tapIndex))
            return;
    }

    if (editTarget == t)
        return;
    editTarget = t;
    if (onTargetChanged)
        onTargetChanged(editTarget);
    repaint();
}

void SpatialHeadView::setMotionPreviewState(const MotionPreviewState& state)
{
    // The editor pushes the processor's motion preview on its own timer; a
    // frame is drawn only when the preview actually moved (paint cost is
    // measured by SpatialHeadViewPaintBench).
    const auto& old = motionPreviewState;
    const bool changed = state.enabled != old.enabled ||
                         (state.enabled && (state.patternId != old.patternId || state.tapMode != old.tapMode ||
                                            !juce::approximatelyEqual(state.phaseCycles, old.phaseCycles) ||
                                            !juce::approximatelyEqual(state.depth01, old.depth01) ||
                                            !juce::approximatelyEqual(state.currentModAz, old.currentModAz) ||
                                            !juce::approximatelyEqual(state.currentModEl, old.currentModEl) ||
                                            !juce::approximatelyEqual(state.currentModDist, old.currentModDist) ||
                                            !juce::approximatelyEqual(state.tapSpread, old.tapSpread)));
    motionPreviewState = state;
    if (changed && isEnabled)
        repaint();
}

//==============================================================================
// Interaction
//==============================================================================
void SpatialHeadView::beginGesture(GestureMask mask)
{
    if (mask == gestureNone || activeGestureMask != gestureNone)
        return;
    activeGestureMask = mask;
    if (onGestureBegin)
        onGestureBegin(mask);
}

void SpatialHeadView::endActiveGesture()
{
    if (activeGestureMask == gestureNone)
        return;
    if (onGestureEnd)
        onGestureEnd(activeGestureMask);
    activeGestureMask = gestureNone;
}

void SpatialHeadView::notifyTargetEdited(int slot)
{
    if (slot == 0)
    {
        // Global/Linked path: one callback per axis.
        if (onAzimuthChanged)
            onAzimuthChanged(positions[0].az);
        if (onElevationChanged)
            onElevationChanged(positions[0].el);
        if (onDistanceChanged)
            onDistanceChanged(positions[0].dist);
    }
    else if (onTapPositionChanged)
    {
        onTapPositionChanged(slot - 1, positions[slot].az, positions[slot].el, positions[slot].dist);
    }
}

int SpatialHeadView::hitTestMarker(juce::Point<float> screenPos, bool includeInactive) const
{
    float bestDist = kMarkerHitRadiusPx;
    int bestSlot = -1;
    auto testSlot = [&](int slot)
    {
        if (perTapMode && !includeInactive && !isTapActive(slot - 1))
            return;
        float depth = 0.0f;
        const auto p = project(
            sphericalToWorld(positions[slot].az, positions[slot].el, distanceToRadius(positions[slot].dist)), depth);
        const float d = p.getDistanceFrom(screenPos);
        if (d < bestDist)
        {
            bestDist = d;
            bestSlot = slot;
        }
    };
    if (perTapMode)
        for (int slot = 1; slot <= 3; ++slot)
            testSlot(slot);
    else
        testSlot(0);
    return bestSlot;
}

void SpatialHeadView::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus();

    // The corner readout is a click target, never a drag start.
    if (calculatePositionReadoutBounds().contains(event.getPosition()))
    {
        readoutPressed = true;
        return;
    }

    // A gizmo handle grabs the selected marker with that axis forced.
    const auto gizmoAxis = gizmoHitTest(event.getPosition().toFloat());
    const bool gizmoGrab = gizmoAxis != DragAxis::Free;
    const int hit = gizmoGrab ? primarySlotForTarget() : hitTestMarker(event.getPosition().toFloat());
    if (hit < 0)
    {
        // A greyed marker remains visible as stored state, but it is not an
        // empty-space camera target.
        if (perTapMode && hitTestMarker(event.getPosition().toFloat(), true) >= 0)
            return;

        // Empty space -> orbit the camera (marker/gizmo grabs handled above).
        isOrbiting = true;
        lastOrbitPos = event.getPosition();
        camYawVelDeg = camPitchVelDeg = 0.0f;  // cancel any coasting throw
        if (isTimerRunning())
            setTimerRate(kOrbitTimerHz);  // smooth inertia after release
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }

    // Clicking a specific tap marker selects it (even out of ALL mode -
    // an explicit grab is an explicit selection).
    if (perTapMode && !targetIsAll() && hit != primarySlotForTarget())
        setEditTarget(hit == 1 ? Target::H1 : hit == 2 ? Target::H2 : Target::H3);
    else if (perTapMode && targetIsAll())
        ;  // formation drag anchored on whichever marker was grabbed - keep ALL

    GestureMask mask = gestureNone;
    if (!isAzimuthLocked())
        mask |= gestureAzimuth;
    if (!isElevationLocked())
        mask |= gestureElevation;
    if (mask == gestureNone)
        return;

    // Arcball hemisphere is chosen at grab time and held for the whole drag.
    float depth = 0.0f;
    const auto& grabbed = perTapMode && !targetIsAll() ? positions[primarySlotForTarget()]
                          : perTapMode                 ? positions[hit]
                                                       : positions[0];
    project(sphericalToWorld(grabbed.az, grabbed.el, distanceToRadius(grabbed.dist)), depth);
    dragOnNearSide = depth < 0.5f;
    if (perTapMode && targetIsAll())
        allDragAnchorSlot = hit;

    isDragging = true;
    dragTrailCount = 0;
    dragStartPos = event.getPosition();
    dragConstrainRequested = gizmoGrab || event.mods.isShiftDown();
    dragConstraint = gizmoGrab ? gizmoAxis : DragAxis::Free;
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    lastDragPos = event.getPosition();
    beginGesture(mask);
}

void SpatialHeadView::mouseMove(const juce::MouseEvent& event)
{
    const bool overReadout = calculatePositionReadoutBounds().contains(event.getPosition());
    const int hit = overReadout ? -1 : hitTestMarker(event.getPosition().toFloat());
    if (hit != hoveredSlot || overReadout != readoutHovered)
    {
        hoveredSlot = hit;
        readoutHovered = overReadout;
        setMouseCursor(hit >= 0 || overReadout ? juce::MouseCursor::PointingHandCursor
                                               : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void SpatialHeadView::mouseExit(const juce::MouseEvent&)
{
    if (hoveredSlot != -1 || readoutHovered)
    {
        hoveredSlot = -1;
        readoutHovered = false;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void SpatialHeadView::applyAzElDelta(float dAz, float dEl)
{
    auto edit = [&]
    {
        if (!canEditCurrentTarget())
            return;

        auto applyTo = [&](int slot)
        {
            auto& pos = positions[slot];
            if (!isAzimuthLocked() && dAz != 0.0f)
                pos.az = wrapAzimuth(pos.az + dAz);
            if (!isElevationLocked() && dEl != 0.0f)
                pos.el = juce::jlimit(-90.0f, 90.0f, pos.el + dEl);
            notifyTargetEdited(slot);
        };

        if (targetIsAll())
        {
            for (int slot = 1; slot <= 3; ++slot)
                if (isTapActive(slot - 1))
                    applyTo(slot);
        }
        else
            applyTo(primarySlotForTarget());

        repaint();
    };
    if (performPositionBatch)
        performPositionBatch(edit);
    else
        edit();
}

void SpatialHeadView::setCurrentAzEl(float az, float el)
{
    auto edit = [&]
    {
        auto& pos = currentPos();
        const float dAz = wrapAzimuth(az - pos.az);
        const float dEl = el - pos.el;
        applyAzElDelta(isAzimuthLocked() ? 0.0f : dAz, isElevationLocked() ? 0.0f : dEl);
    };
    if (performPositionBatch)
        performPositionBatch(edit);
    else
        edit();
}

void SpatialHeadView::applyDrag(const juce::MouseEvent& event)
{
    // Absolute marker-follow: invert the projection at the grabbed marker's
    // current radius so the glyph tracks the cursor on the sphere surface.
    const int anchorSlot = targetIsAll() ? allDragAnchorSlot : primarySlotForTarget();
    const auto& anchor = positions[juce::jlimit(0, 3, anchorSlot)];

    float az = 0.0f, el = 0.0f;
    screenToSphere(event.getPosition().toFloat(), distanceToRadius(anchor.dist), dragOnNearSide, az, el);

    // Shift-drag: commit to a single axis once the drag direction is clear, then
    // hold the other axis fixed so azimuth and elevation stop moving together.
    if (dragConstrainRequested)
    {
        if (dragConstraint == DragAxis::Free)
        {
            const auto d = event.getPosition() - dragStartPos;
            constexpr int resolveThresholdPx = 4;
            if (juce::jmax(std::abs(d.x), std::abs(d.y)) < resolveThresholdPx)
                return;  // wait for enough movement to disambiguate the axis
            dragConstraint = std::abs(d.x) >= std::abs(d.y) ? DragAxis::Azimuth : DragAxis::Elevation;
        }
        if (dragConstraint == DragAxis::Azimuth)
            el = anchor.el;  // hold elevation
        else if (dragConstraint == DragAxis::Elevation)
            az = anchor.az;  // hold azimuth
    }

    // Magnetic snap to cardinal anchors - free drag only, suspended with Ctrl.
    activeSnapLabel.clear();
    if (dragConstraint == DragAxis::Free && !targetIsAll() && !event.mods.isCtrlDown())
    {
        struct Anchor
        {
            const char* name;
            float az, el;
        };
        static constexpr Anchor anchors[] = {{"FRONT", 0.0f, 0.0f},  {"BEHIND", 180.0f, 0.0f}, {"LEFT", -90.0f, 0.0f},
                                             {"RIGHT", 90.0f, 0.0f}, {"ABOVE", 0.0f, 90.0f},   {"BELOW", 0.0f, -90.0f}};
        const float radius = distanceToRadius(anchor.dist);
        const auto cursor = event.getPosition().toFloat();
        float best = 18.0f;
        int bestIdx = -1;
        for (int i = 0; i < (int)(sizeof(anchors) / sizeof(anchors[0])); ++i)
        {
            float d = 0.0f;
            const auto p = project(sphericalToWorld(anchors[i].az, anchors[i].el, radius), d);
            const float sd = p.getDistanceFrom(cursor);
            if (sd < best)
            {
                best = sd;
                bestIdx = i;
            }
        }
        if (bestIdx >= 0)
        {
            az = anchors[bestIdx].az;
            el = anchors[bestIdx].el;
            activeSnapLabel = anchors[bestIdx].name;
        }
    }

    if (targetIsAll())
    {
        const float dAz = wrapAzimuth(az - anchor.az);
        const float dEl = el - anchor.el;
        applyAzElDelta(isAzimuthLocked() ? 0.0f : dAz, isElevationLocked() ? 0.0f : dEl);
    }
    else
    {
        setCurrentAzEl(az, el);
    }
}

void SpatialHeadView::applyOrbitDelta(float dxPixels, float dyPixels)
{
    constexpr float kYawPerPx = 0.45f;  // degrees of orbit per pixel dragged
    constexpr float kPitchPerPx = 0.40f;
    camYawDeg = std::fmod(camYawDeg + dxPixels * kYawPerPx, 360.0f);
    // Drag up -> tilt toward top-down (raise pitch); clamp so total stays in view.
    camPitchDeg = juce::jlimit(-67.0f, 98.0f, camPitchDeg - dyPixels * kPitchPerPx);
    // Remember the per-frame delta as the inertia velocity for the release throw.
    camYawVelDeg = dxPixels * kYawPerPx;
    camPitchVelDeg = -dyPixels * kPitchPerPx;
    repaint();
}

void SpatialHeadView::resetCamera()
{
    camYawDeg = camPitchDeg = 0.0f;
    camZoom = 1.0f;
    camYawVelDeg = camPitchVelDeg = 0.0f;
    isOrbiting = false;
    repaint();
}

void SpatialHeadView::mouseDrag(const juce::MouseEvent& event)
{
    if (isOrbiting)
    {
        const auto now = event.getPosition();
        applyOrbitDelta((float)(now.x - lastOrbitPos.x), (float)(now.y - lastOrbitPos.y));
        lastOrbitPos = now;
        return;
    }
    if (!isDragging)
        return;
    applyDrag(event);

    // Record the grabbed marker's screen position for the phosphor afterimage.
    const int anchorSlot = targetIsAll() ? allDragAnchorSlot : primarySlotForTarget();
    const auto& pos = positions[juce::jlimit(0, 3, anchorSlot)];
    float depth = 0.0f;
    const auto p = project(sphericalToWorld(pos.az, pos.el, distanceToRadius(pos.dist)), depth);
    if (dragTrailCount == (int)dragTrail.size())
    {
        std::copy(dragTrail.begin() + 1, dragTrail.end(), dragTrail.begin());
        --dragTrailCount;
    }
    dragTrail[(size_t)dragTrailCount++] = p;
}

void SpatialHeadView::mouseUp(const juce::MouseEvent& event)
{
    if (readoutPressed)
    {
        readoutPressed = false;
        if (calculatePositionReadoutBounds().contains(event.getPosition()) && onPositionReadoutClicked)
            onPositionReadoutClicked();
        return;
    }

    if (isOrbiting)
    {
        // Release keeps the last drag velocity as an inertia throw (decays in
        // timerCallback); the view coasts to a gentle stop.
        isOrbiting = false;
        hoveredSlot = hitTestMarker(event.getPosition().toFloat());
        setMouseCursor(hoveredSlot >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        return;
    }

    isDragging = false;
    dragTrailCount = 0;
    dragConstraint = DragAxis::Free;
    dragConstrainRequested = false;
    activeSnapLabel.clear();
    endActiveGesture();
    hoveredSlot = hitTestMarker(event.getPosition().toFloat());
    setMouseCursor(hoveredSlot >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void SpatialHeadView::mouseDoubleClick(const juce::MouseEvent& event)
{
    auto edit = [&]
    {
        // Double-click empty space snaps the orbit camera back to the default view;
        // double-click a marker resets that target's position (front/centre/1 m).
        const auto markerPoint = event.getPosition().toFloat();
        if (perTapMode && hitTestMarker(markerPoint) < 0 && hitTestMarker(markerPoint, true) >= 0)
            return;

        if (hitTestMarker(markerPoint) < 0)
        {
            resetCamera();
            return;
        }

        // Reset all unlocked axes of the current target to front/centre defaults.
        GestureMask changedMask = gestureNone;
        if (!isAzimuthLocked())
            changedMask |= gestureAzimuth;
        if (!isElevationLocked())
            changedMask |= gestureElevation;
        if (!isDistanceLocked())
            changedMask |= gestureDistance;
        if (changedMask == gestureNone)
            return;

        beginGesture(changedMask);
        auto resetSlot = [&](int slot)
        {
            auto& pos = positions[slot];
            if (!isAzimuthLocked())
                pos.az = 0.0f;
            if (!isElevationLocked())
                pos.el = 0.0f;
            if (!isDistanceLocked())
                pos.dist = 1.0f;
            notifyTargetEdited(slot);
        };
        if (targetIsAll())
        {
            for (int slot = 1; slot <= 3; ++slot)
                if (isTapActive(slot - 1))
                    resetSlot(slot);
        }
        else
            resetSlot(primarySlotForTarget());
        endActiveGesture();
        repaint();
    };
    if (performPositionBatch)
        performPositionBatch(edit);
    else
        edit();
}

void SpatialHeadView::applyDistanceDelta(float deltaMeters, bool bracketGesture)
{
    auto edit = [&]
    {
        if (!canEditCurrentTarget() || isDistanceLocked())
            return;

        revealDistanceReferences();

        if (bracketGesture)
            beginGesture(gestureDistance);

        auto applyTo = [&](int slot, float delta)
        {
            auto& pos = positions[slot];
            pos.dist = juce::jlimit(kMinDistanceM, kMaxDistanceM, pos.dist + delta);
            notifyTargetEdited(slot);
        };

        if (targetIsAll())
        {
            // Scale the formation: each tap moves proportionally to its own distance.
            const float factor = deltaMeters / juce::jmax(0.05f, currentPos().dist);
            for (int slot = 1; slot <= 3; ++slot)
                if (isTapActive(slot - 1))
                    applyTo(slot, positions[slot].dist * factor);
        }
        else
        {
            applyTo(primarySlotForTarget(), deltaMeters);
        }

        if (bracketGesture)
            endActiveGesture();
        repaint();
    };
    if (performPositionBatch)
        performPositionBatch(edit);
    else
        edit();
}

void SpatialHeadView::revealDistanceReferences()
{
    distanceReferencesVisibleUntilMs = juce::Time::getMillisecondCounterHiRes() + 900.0;
}

float SpatialHeadView::distanceReferenceOpacity() const
{
    const double remainingMs = distanceReferencesVisibleUntilMs - juce::Time::getMillisecondCounterHiRes();
    if (remainingMs <= 0.0)
        return 0.0f;
    return (float)juce::jlimit(0.0, 1.0, remainingMs / 220.0);
}

void SpatialHeadView::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f)
        return;

    // Cmd/Ctrl + wheel dollies the camera in/out; plain wheel keeps the distance
    // control so the core positioning gesture is unchanged.
    if (event.mods.isCommandDown() || event.mods.isCtrlDown())
    {
        camZoom = juce::jlimit(0.55f, 2.4f, camZoom * (1.0f + 0.18f * wheel.deltaY));
        repaint();
        return;
    }

    if (!canEditCurrentTarget())
        return;

    // Wheel = distance, scaled by the wheel delta (one notch ~= 0.1) and by the
    // current distance so it feels log-like: fine near the head, coarser far away.
    float step = currentPos().dist * 1.2f * -wheel.deltaY;  // scroll up = closer
    if (std::abs(step) < 0.02f)
        step = (step < 0.0f ? -0.02f : 0.02f);
    applyDistanceDelta(step, true);
}

bool SpatialHeadView::keyPressed(const juce::KeyPress& key)
{
    const bool shift = key.getModifiers().isShiftDown();
    const float step = shift ? 0.5f : 2.0f;  // Shift = fine (0.5 deg) on the azimuth arrows

    // Quick target selection: 1/2/3 = taps, G = global/linked, A = ALL.
    if (perTapMode)
    {
        if (key.getTextCharacter() == '1')
        {
            setEditTarget(Target::H1);
            return true;
        }
        if (key.getTextCharacter() == '2')
        {
            setEditTarget(Target::H2);
            return true;
        }
        if (key.getTextCharacter() == '3')
        {
            setEditTarget(Target::H3);
            return true;
        }
        if (key.getTextCharacter() == 'a' || key.getTextCharacter() == 'A')
        {
            setEditTarget(Target::All);
            return true;
        }
    }

    if (!canEditCurrentTarget())
        return false;

    if (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey)
    {
        if (isAzimuthLocked())
            return true;
        const float dir = (key.getKeyCode() == juce::KeyPress::rightKey) ? 1.0f : -1.0f;
        beginGesture(gestureAzimuth);
        applyAzElDelta(dir * step, 0.0f);
        endActiveGesture();
        return true;
    }

    if (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey)
    {
        const float dir = (key.getKeyCode() == juce::KeyPress::upKey) ? 1.0f : -1.0f;
        if (shift)
        {
            applyDistanceDelta(dir * juce::jmax(0.02f, currentPos().dist * 0.1f), true);
            return true;
        }
        if (isElevationLocked())
            return true;
        beginGesture(gestureElevation);
        applyAzElDelta(0.0f, dir * step);
        endActiveGesture();
        return true;
    }

    return false;
}

void SpatialHeadView::focusGained(FocusChangeType)
{
    hasFocus = true;
    repaint();
}
void SpatialHeadView::focusLost(FocusChangeType)
{
    hasFocus = false;
    endActiveGesture();
    repaint();
}

//==============================================================================
// GPU renderer (juce::OpenGLContext) - background 3D; chrome stays juce::Graphics
//==============================================================================
void SpatialHeadView::buildGLLineData()
{
    glLineData.clear();
    auto emit = [this](const std::vector<Polyline>& polys, float bright)
    {
        for (const auto& line : polys)
            for (size_t i = 1; i < line.size(); ++i)
                for (int k : {(int)i - 1, (int)i})
                {
                    const auto& v = line[(size_t)k];
                    glLineData.push_back(v.x);
                    glLineData.push_back(v.y);
                    glLineData.push_back(v.z);
                    glLineData.push_back(bright);
                }
    };
    emit(headLines, 1.0f);
    glLineCount = (int)(glLineData.size() / 4);
}

std::array<float, 16> SpatialHeadView::computeGLMatrix() const
{
    // Column-major 4x4 that reproduces project() exactly for the visible
    // geometry, so the GL background aligns with the juce::Graphics chrome.
    // Derived from project()'s yaw(+y)->pitch(+x)->perspective chain; collapses
    // to the fixed-camera matrix when yaw=0/zoom=1.
    const float cy = std::cos(cameraYawRad()), sy = std::sin(cameraYawRad());
    const float cp = std::cos(cameraPitchRad()), sp = std::sin(cameraPitchRad());
    const float D = kCameraDistance;
    const float S = projScale();
    const float W = juce::jmax(1.0f, (float)getWidth());
    const float H = juce::jmax(1.0f, (float)getHeight());
    const float a = 2.0f * viewCentre.x / W - 1.0f;
    const float b = 1.0f - 2.0f * viewCentre.y / H;
    const float kx = 2.0f * S / W;
    const float ky = 2.0f * S / H;

    std::array<float, 16> m{};  // column-major: m[col*4 + row]
    // clip.x (row 0)
    m[0] = kx * cy - (a / D) * cp * sy;
    m[4] = (a / D) * sp;
    m[8] = kx * sy + (a / D) * cp * cy;
    m[12] = a;
    // clip.y (row 1)
    m[1] = sy * (ky * sp - (b / D) * cp);
    m[5] = ky * cp + (b / D) * sp;
    m[9] = cy * (-ky * sp + (b / D) * cp);
    m[13] = b;
    // clip.w (row 3)
    m[3] = -(cp * sy) / D;
    m[7] = sp / D;
    m[11] = (cp * cy) / D;
    m[15] = 1.0f;
    // clip.z (row 2) stays 0 - depth unused (line list, depth test disabled).
    return m;
}

bool SpatialHeadView::isOverlayForcingSoftwareRendering() const
{
    return (helpOverlay != nullptr && helpOverlay->isVisible()) ||
           (walkthroughOverlay != nullptr && walkthroughOverlay->isVisible()) || aboutDialogOpen || libraryMenuOpen;
}

void SpatialHeadView::updateGLAttachment()
{
    // Detach GL context while any 2D overlay is visible.
    // Overlays must render above the GL background; freezing the head view under
    // overlays is acceptable. When all overlays close, reattach the context.
    // The preset browser is intentionally absent from this list during normal
    // use because its panel stays left of the head. During the tour, the
    // walkthrough itself remains visible and supplies the software fallback.
    const bool anyOverlayVisible = isOverlayForcingSoftwareRendering();

    const bool wantAttached = !glUnavailable && isShowing() && getWidth() > 0 && getHeight() > 0 && !anyOverlayVisible;

    if (wantAttached && !glContext.isAttached())
        glContext.attachTo(*this);
    else if (!wantAttached && glContext.isAttached())
        glContext.detach();
}

void SpatialHeadView::visibilityChanged()
{
    updateGLAttachment();
}
void SpatialHeadView::parentHierarchyChanged()
{
    updateGLAttachment();
}

void SpatialHeadView::newOpenGLContextCreated()
{
    gl = std::make_unique<SpatialHeadGL>();
    if (gl->initialise(glContext, glLineData, glLineCount))
    {
        glActive = true;
    }
    else
    {
        // Context/shader failure -> permanently fall back to the CPU renderer
        // (do the detach on the message thread; SafePointer guards teardown).
        gl.reset();
        juce::Component::SafePointer<SpatialHeadView> safe(this);
        juce::MessageManager::callAsync(
            [safe]
            {
                if (safe != nullptr)
                {
                    safe->glUnavailable = true;
                    safe->updateGLAttachment();
                    safe->repaint();
                }
            });
    }
}

void SpatialHeadView::renderOpenGL()
{
    if (gl == nullptr || !gl->isReady())
        return;
    const float scale = (float)glContext.getRenderingScale();
    const int w = juce::roundToInt((float)getWidth() * scale);
    const int h = juce::roundToInt((float)getHeight() * scale);
    gl->render(glContext, w, h, computeGLMatrix(), isEnabled, audioGlow());
}

void SpatialHeadView::openGLContextClosing()
{
    if (gl != nullptr)
        gl->shutdown();
    gl.reset();
    glActive = false;
}
