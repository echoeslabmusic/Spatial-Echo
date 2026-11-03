// SpatialHeadView: Interactive 3D wireframe-head spatial controller (azimuth, elevation, distance, motion preview):
//   OpenGL head background with juce::Graphics chrome, and a juce::Graphics fallback for the whole scene.

#pragma once
#include <JuceHeader.h>
#include "DelayHeadActivation.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>

class SpatialHeadGL;

/**
 * SpatialHeadView - the right-panel hero control: a wireframe head wearing
 * headphones inside an orbit sphere. The glowing source marker is dragged
 * around the head to position the echo in 3D space.
 *
 * Rendering: the head wireframe is a set of unit-space polylines projected
 * through an orbit camera, with depth-faded phosphor-green strokes so far
 * geometry recedes like a CRT trace. When an OpenGL 3.2 context is available
 * (SpatialHeadGL) it draws the head with a bloom pass and paint() overlays the
 * interaction chrome; otherwise, and while a 2D overlay covers the view,
 * paint() draws the whole scene with juce::Graphics. Frames are rendered on
 * demand: the view repaints when its state changes, while the motion preview,
 * audio glow, camera inertia or distance-reference fade is moving.
 *
 * Interaction contract:
 *  - drag           -> azimuth + elevation (relative, arcball-style)
 *  - mouse wheel    -> distance
 *  - arrow keys     -> fine azimuth/elevation; shift+up/down -> distance
 *  - double-click   -> reset (azimuth 0, elevation 0, distance 1 m)
 *  - per-axis locks -> queried via injected lambdas; locked axes ignore input
 *  - gestures       -> onGestureBegin/End bracket every change for host automation
 */
class SpatialHeadView : public juce::Component,
                        public juce::SettableTooltipClient,
                        public juce::OpenGLRenderer,
                        private juce::Timer
{
public:
    struct MotionPreviewState
    {
        bool enabled = false;
        int patternId = 0;         // index into SpatialIDs::motion::kPatternChoices
        float depth01 = 0.5f;      // 0..1 (MotionDepth / 100)
        double phaseCycles = 0.0;  // live phase from processor
        float blockDurationSec = 0.0f;
        float currentModAz = 0.0f;  // DSP's smoothed modulation NOW
        float currentModEl = 0.0f;
        float currentModDist = 0.0f;
        // Per-tap constellation preview (Independent mode trails)
        float tapSpread = 0.0f;  // 0..1 (MotionSpread / 100)
        int tapMode = 0;         // 0=Unison, 1=Constellation, 2=Cascade
    };

    using GestureMask = uint32_t;
    static constexpr GestureMask gestureNone = 0u;
    static constexpr GestureMask gestureAzimuth = 1u << 0;
    static constexpr GestureMask gestureElevation = 1u << 1;
    static constexpr GestureMask gestureDistance = 1u << 2;

    // Edit targets: the global (Linked) source, one of the three taps, or the
    // audible formation (ALL moves active heads together, preserving spacing).
    enum class Target
    {
        Global = 0,
        H1,
        H2,
        H3,
        All
    };

    SpatialHeadView();
    ~SpatialHeadView() override;  // defined in the .cpp (unique_ptr<SpatialHeadGL> needs the full type)

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void focusGained(FocusChangeType cause) override;
    void focusLost(FocusChangeType cause) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    void resized() override;
    void timerCallback() override;

    // Attaches the OpenGL context while the view is showing and no 2D overlay
    // covers it. The juce::Graphics renderer takes over while detached, after
    // a GL context or shader failure, and in headless use (never shown).
    void updateGLAttachment();

    // juce::OpenGLRenderer - all three run on the GL render thread.
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    // Set/get position of the GLOBAL source (degrees / meters, DSP plain values),
    // used by the Linked path.
    void setAzimuth(float azimuthDegrees);
    void setElevation(float elevationDegrees);
    void setDistance(float distMeters);  // 0.1 to 10 m
    void setMotionPreviewState(const MotionPreviewState& state);
    float getAzimuth() const { return positions[0].az; }
    float getElevation() const { return positions[0].el; }
    float getDistance() const { return positions[0].dist; }

    // --- Per-tap constellation (Independent mode) ------------------------
    // Tap positions are pushed in from APVTS; edits flow back via
    // onTapPositionChanged. In per-tap mode the view shows 3 tap markers as
    // luminous emitter nodes (bright core + reticle ring), told apart by an
    // H1/H2/H3 index label; Linked shows one emitter. Selected = magenta accent;
    // heads excluded by Echo Mode stay visible as locked, dim stored positions.
    void setPerTapMode(bool independentMode);
    bool isPerTapMode() const { return perTapMode; }
    void setActiveTapMask(DelayHeadActivation::Mask activeHeads);
    bool isTapActive(int tapIndex) const;
    bool canEditCurrentTarget() const;
    void setTapPosition(int tapIndex, float azDeg, float elDeg, float distM);  // 0..2
    void setEditTarget(Target t);
    Target getEditTarget() const { return editTarget; }
    float getTargetAzimuth() const { return currentPos().az; }
    float getTargetElevation() const { return currentPos().el; }
    float getTargetDistance() const { return currentPos().dist; }
    // Slot addressed by the current target: 0 = global, 1..3 = taps. ALL uses
    // the first active tap as its readout/scale anchor.
    int primarySlotForTarget() const;

    // Fired when a tap position changes through interaction (drag/keys/wheel).
    std::function<void(int tapIndex, float az, float el, float dist)> onTapPositionChanged;
    // Synchronous scope for one full edit, including every active ALL head.
    std::function<void(const std::function<void()>&)> performPositionBatch;
    // Fired when the user click-selects a marker (or the view cycles targets).
    std::function<void(Target)> onTargetChanged;
    // Fired when the corner az/el/dist readout is clicked (opens the
    // exact-entry popover).
    std::function<void()> onPositionReadoutClicked;
    // Bounds of that readout (local coords), for popover anchoring. Computed
    // on demand from live state so it is never stale after a resize.
    juce::Rectangle<int> getPositionReadoutBounds() const { return calculatePositionReadoutBounds(); }

    // Walkthrough-only visual cue. This reveals the same temporary distance
    // rings shown during a wheel gesture and does not edit position state.
    void revealWalkthroughDistanceReferences()
    {
        revealDistanceReferences();
        repaint();
    }

    // Lock state queries (injected by SpatialPanel)
    std::function<bool()> isAzimuthLockedFn;
    std::function<bool()> isDistanceLockedFn;
    std::function<bool()> isElevationLockedFn;

    // Audio-reactive glow source: returns the current echo-branch meter (linear,
    // ~0..1), read on the UI timer. Injected by the editor (the DSP already
    // publishes this meter - read-only, no DSP change). Null -> no reaction.
    std::function<float()> audioLevelProvider;

    // Per-tap audio envelope levels (0..2 peak-envelope linear); read on the UI
    // timer to drive per-marker glow pulses independent of distance. Frozen during
    // drag/orbit. Injected by the editor.
    std::function<float(int tapIndex)> tapAudioLevelProvider;  // tapIndex 0..2 (H1, H2, H3)

    // Whether 3D processing is enabled (drives idle animation + dimming)
    void setRadarEnabled(bool enabled);

    // Overlay references for GL z-order: detach while Help or the walkthrough
    // is visible. The preset browser is deliberately not tracked on its own;
    // normal browser placement never intersects this view, while an active
    // walkthrough already supplies the required software fallback.
    void setOverlayReferences(juce::Component* help, juce::Component* walkthrough = nullptr)
    {
        helpOverlay = help;
        walkthroughOverlay = walkthrough;
    }

    // Set About dialog visibility state (separate modal window not a child component)
    void setAboutDialogOpen(bool isOpen) { aboutDialogOpen = isOpen; }

    // Set spatial preset library popup visibility (a window-parented popup that
    // opens over this view; software-painted popups are invisible above GL)
    void setLibraryMenuOpen(bool isOpen) { libraryMenuOpen = isOpen; }

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    // Test-only: force the smoothed audio-glow level so offscreen captures can
    // render a "loud" frame without a live message loop driving the timer.
    void setAudioGlowForTest(float level01) { audioLevelSmoothed = juce::jlimit(0.0f, 1.0f, level01); }
    // Test-only: drive the orbit camera for offscreen orientation captures
    // (yaw + pitch offset from the default over-the-shoulder view).
    void setCameraForTest(float yawDeg, float pitchOffsetDeg)
    {
        camYawDeg = yawDeg;
        camPitchDeg = pitchOffsetDeg;
    }
    bool isSoftwareFallbackForcedForTest() const { return isOverlayForcingSoftwareRendering(); }
#endif

    // First-load discovery cue for per-tap mode (drawn while in Linked mode
    // until the user first engages PER-TAP; owned by SpatialPanel).
    void setDiscoveryCueVisible(bool visible);

    // Callbacks when position changes via mouse/keyboard
    std::function<void(float azimuth)> onAzimuthChanged;
    std::function<void(float elevation)> onElevationChanged;
    std::function<void(float distance)> onDistanceChanged;

    // Gesture lifecycle for host automation bracketing.
    std::function<void(GestureMask mask)> onGestureBegin;
    std::function<void(GestureMask mask)> onGestureEnd;

private:
    // --- 3D core -----------------------------------------------------------
    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    // World space: +x = head's right, +y = up, +z = front (matches the DSP's
    // BinauralProcessor::toUnitVector convention).
    static Vec3 sphericalToWorld(float azimuthDeg, float elevationDeg, float radius);

    // Camera: behind and above the head ("over the shoulder"), so the head's
    // right is screen-right and headphone left/right match what the user hears.
    juce::Point<float> project(const Vec3& world, float& depthOut) const;

    // Orbit-camera parameters, applied by project(), screenToSphere() and
    // computeGLMatrix() together so the CPU chrome and GL background stay locked.
    float cameraYawRad() const;
    float cameraPitchRad() const;  // kCameraPitchDeg + camPitchDeg, clamped
    float projScale() const;       // viewScale * camZoom
    void resetCamera();
    void applyOrbitDelta(float dxPixels, float dyPixels);

    // Wireframe geometry as unit-space polylines (built once, projected per paint).
    using Polyline = std::vector<Vec3>;
    static std::vector<Polyline> buildHeadWireframe();  // head + ears + nose + headphones

    void drawPolyline(juce::Graphics& g, const Polyline& line, juce::Colour base, float baseAlpha, float thickness,
                      bool endpointDots = false, bool maskBehindHead = false) const;
    float guideVisibilityAt(juce::Point<float> point) const;
    void drawMaskedGuideLine(juce::Graphics& g, juce::Point<float> from, juce::Point<float> to, juce::Colour colour,
                             float alpha, float thickness) const;
    void drawSourceMarker(juce::Graphics& g);
    void drawMotionTrail(juce::Graphics& g);
    void drawDragTrail(juce::Graphics& g);
    void drawCompassLabels(juce::Graphics& g);
    void drawDistanceRings(juce::Graphics& g);
    void drawFacingReadout(juce::Graphics& g);
    void drawPositionReadout(juce::Graphics& g);
    juce::String positionReadoutText() const;
    // Live-state readout rect (empty when disabled) - shared by paint and
    // mouse hit-testing so the two can never disagree.
    juce::Rectangle<int> calculatePositionReadoutBounds() const;

    // Full scene (rings, head, labels, trails, markers). Called twice per
    // paint: once at quarter resolution for the phosphor glow underlay, once
    // crisp on top.
    void renderScene(juce::Graphics& g);
    // Interaction chrome (rings, compass, trails, marker+gizmo, readouts) - always
    // juce::Graphics, composited over the 3D background in either renderer.
    void renderChrome(juce::Graphics& g);
    void renderGlowUnderlay(juce::Graphics& g);

    // GPU renderer plumbing.
    std::array<float, 16> computeGLMatrix() const;  // GL MVP that reproduces project()
    void buildGLLineData();                         // flatten the head polylines for the VBO

    // --- interaction -------------------------------------------------------
    struct SourcePos
    {
        float az = 0.0f, el = 0.0f, dist = 1.0f;
    };

    // Where a slot's marker is drawn: its base position, or the live motion-
    // modulated position while Spatial Motion runs, so the echoes visibly orbit
    // the head. Mirrors the DSP's additive/absolute combine + per-tap phase
    // spread. Hit-testing still uses the base position (pause motion to fine-aim).
    SourcePos motionDisplayPos(int slot) const;

    void beginGesture(GestureMask mask);
    void endActiveGesture();
    void applyDrag(const juce::MouseEvent& event);
    void applyDistanceDelta(float deltaMeters, bool bracketGesture);
    void revealDistanceReferences();
    float distanceReferenceOpacity() const;
    void applyAzElDelta(float dAz, float dEl);
    void setCurrentAzEl(float az, float el);
    bool isAzimuthLocked() const { return isAzimuthLockedFn ? isAzimuthLockedFn() : false; }
    bool isDistanceLocked() const { return isDistanceLockedFn ? isDistanceLockedFn() : false; }
    bool isElevationLocked() const { return isElevationLockedFn ? isElevationLockedFn() : false; }
    void notifyTargetEdited(int slot);

    // Slots: 0 = global, 1..3 = taps. Which slots the current target edits.
    SourcePos& currentPos() { return positions[primarySlotForTarget()]; }
    const SourcePos& currentPos() const { return positions[primarySlotForTarget()]; }
    bool targetIsAll() const { return editTarget == Target::All; }

    // Marker hit-testing + inverse projection (screen -> sphere az/el).
    int hitTestMarker(juce::Point<float> screenPos, bool includeInactive = false) const;
    bool screenToSphere(juce::Point<float> screenPos, float radius, bool nearHemisphere, float& azOut,
                        float& elOut) const;

    // --- state -------------------------------------------------------------
    SourcePos positions[4];  // [0]=global, [1..3]=taps H1..H3
    DelayHeadActivation::Mask activeTapMask{true, true, true};
    bool perTapMode = false;
    Target editTarget = Target::Global;
    bool isDragging = false;
    int hoveredSlot = -1;  // marker under the cursor (affordance glow)
    double distanceReferencesVisibleUntilMs = 0.0;

    // Overlay visibility for GL z-order: detach GL context when any overlay is open
    bool isOverlayForcingSoftwareRendering() const;
    juce::Component* helpOverlay = nullptr;
    juce::Component* walkthroughOverlay = nullptr;
    bool aboutDialogOpen = false;  // About dialog is a separate modal window
    bool libraryMenuOpen = false;  // Spatial preset library popup (window-parented)

    // Axis-constrained drag: hold Shift to commit a drag to a single axis
    // (azimuth OR elevation), resolving the "one drag moves both" ambiguity.
    // Free = unconstrained arcball follow (default; no regression). The axis is
    // auto-picked from the dominant initial drag direction, then held.
    enum class DragAxis
    {
        Free,
        Azimuth,
        Elevation
    };
    DragAxis dragConstraint = DragAxis::Free;
    bool dragConstrainRequested = false;  // Shift was held at grab time
    juce::Point<int> dragStartPos;

    // Magnetic snap: while a free drag lands within ~18 px of a cardinal anchor
    // (Front/Behind/L/R/Above/Below), the marker sticks to it. Ctrl suspends it.
    // Non-empty while snapped (drives the confirm cue in the facing readout).
    juce::String activeSnapLabel;

    // Axis gizmo: AZ/EL handles around the selected marker. Clicking a handle
    // starts a drag constrained to that axis (reuses the drag-constraint path
    // above); magenta while active.
    void drawGizmo(juce::Graphics& g);
    juce::Point<float> gizmoAxisDir(const SourcePos& pos, bool azimuth, bool& foreshortened) const;
    DragAxis gizmoHitTest(juce::Point<float> cursor) const;

    // Corner az/el/dist readout (click target for the exact-entry popover)
    bool readoutHovered = false;
    bool readoutPressed = false;

    // Phosphor glow underlay: scene rendered at quarter res, gaussian-blurred.
    juce::Image glowScene, glowBlur;

    // Manual-drag phosphor trail (screen space, oldest -> newest)
    std::array<juce::Point<float>, 12> dragTrail;
    int dragTrailCount = 0;
    bool dragOnNearSide = true;  // hemisphere chosen at grab time (arcball stability)
    int allDragAnchorSlot = 1;   // marker grabbed for an ALL-mode formation drag
    GestureMask activeGestureMask = gestureNone;
    bool hasFocus = false;
    bool isEnabled = true;
    // True while the GPU renderer owns the 3D background. Written on the GL
    // thread, read by paint() on the message thread.
    std::atomic<bool> glActive{false};
    bool glUnavailable = false;  // set after a GL context or shader failure
    juce::OpenGLContext glContext;
    std::unique_ptr<SpatialHeadGL> gl;  // created on the GL thread
    std::vector<float> glLineData;      // interleaved [x,y,z,bright], built once
    int glLineCount = 0;
    juce::Point<int> lastDragPos;

    // Orbit camera: yaw/pitch offsets from the default over-the-shoulder view
    // (degrees) plus a dolly zoom on the projection scale. Empty-space drag
    // orbits; Cmd/Ctrl+wheel dollies; release throws inertia that decays in
    // timerCallback. camPitchDeg is clamped so the total pitch stays in view.
    float camYawDeg = 0.0f, camPitchDeg = 0.0f, camZoom = 1.0f;
    float camYawVelDeg = 0.0f, camPitchVelDeg = 0.0f;
    bool isOrbiting = false;
    juce::Point<int> lastOrbitPos;

    MotionPreviewState motionPreviewState;
    bool discoveryCueVisible = false;

    // Cached wireframe (unit space) + projection parameters (from resized()).
    std::vector<Polyline> headLines;
    juce::Point<float> viewCentre;
    float viewScale = 100.0f;  // pixels per sphere radius

    // Timer: 15 Hz for glow smoothing, 60 Hz while the orbit camera moves.
    static constexpr int kIdleTimerHz = 15;
    static constexpr int kOrbitTimerHz = 60;
    int timerRateHz = kIdleTimerHz;
    void setTimerRate(int hz);
    bool distanceReferencesWereVisible = false;  // one more frame clears the fade

    // Audio-reactive glow: smoothed echo-branch level (0..1), advanced on the
    // UI timer with fast-attack/slow-release ballistics. Drives the bloom/glow
    // intensity and the selected marker's halo, on a channel SEPARATE from the
    // distance encoding (brightness/size), so signal never fakes distance.
    float audioLevelSmoothed = 0.0f;
    float audioGlow() const { return audioLevelSmoothed; }

    // Per-tap audio envelope smoothing: each tap's (0..2) peak envelope level,
    // smoothed independently. Drives per-marker glow pulses; frozen while dragging
    // or orbiting. Index 0..2 = H1, H2, H3.
    std::array<float, 3> tapAudioLevelSmoothed = {};
    float getTapAudioGlow(int tapIndex) const
    {
        if (tapIndex >= 0 && tapIndex < 3)
            return tapAudioLevelSmoothed[static_cast<size_t>(tapIndex)];
        return 0.0f;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialHeadView)
};
