// SpatialHeadViewPaintBenchTest: Offscreen paint-time benchmark for SpatialHeadView (can it paint at 60 fps), plus
//   optional PNG captures of the view.

#include "SystemTests.h"

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "../../UI/Components/SpatialHeadView.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

namespace SpatialEchoTests::System
{

namespace
{
struct BenchStats
{
    double avgMs = 0.0, p50Ms = 0.0, p95Ms = 0.0, maxMs = 0.0;
};

BenchStats summarise(std::vector<double>& frameMs)
{
    std::sort(frameMs.begin(), frameMs.end());
    BenchStats s;
    for (double v : frameMs)
        s.avgMs += v;
    s.avgMs /= (double)frameMs.size();
    s.p50Ms = frameMs[frameMs.size() / 2];
    s.p95Ms = frameMs[(size_t)((double)frameMs.size() * 0.95)];
    s.maxMs = frameMs.back();
    return s;
}

int countBrightRunsInColumn(const juce::Image& image, int x, int yStart, int yEnd)
{
    int runs = 0;
    bool inRun = false;
    for (int y = yStart; y < yEnd; ++y)
    {
        const auto pixel = image.getPixelAt(x, y);
        const bool bright = pixel.getGreen() >= 8 && pixel.getGreen() > pixel.getRed();
        if (bright && !inRun)
            ++runs;
        inRun = bright;
    }
    return runs;
}

int maxPeripheralGuideRuns(const juce::Image& image)
{
    int maxRuns = 0;
    for (const int x : {90, 120, 150})
        maxRuns = juce::jmax(maxRuns, countBrightRunsInColumn(image, x, 130, 410));
    return maxRuns;
}

juce::Image renderGuideScene(SpatialHeadView& view)
{
    juce::Image image(juce::Image::ARGB, 560, 520, true);
    juce::Graphics g(image);
    view.paintEntireComponent(g, false);
    return image;
}

juce::String checkGuideVisibilityPolicy(bool& passed)
{
    SpatialHeadView view;
    view.setBounds(0, 0, 560, 520);
    view.setRadarEnabled(true);
    view.setAzimuth(0.0f);
    view.setElevation(0.0f);
    view.setDistance(1.0f);

    const int idleRuns = maxPeripheralGuideRuns(renderGuideScene(view));

    view.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0));
    const int adjustingRuns = maxPeripheralGuideRuns(renderGuideScene(view));

    constexpr int maxAllowedRuns = 4;
    constexpr int minAdjustmentLift = 3;
    passed = idleRuns <= maxAllowedRuns && adjustingRuns >= idleRuns + minAdjustmentLift;

    juce::String result;
    result << "  contextual guide policy: idle " << idleRuns << " runs (allowed " << maxAllowedRuns << "), adjusting "
           << adjustingRuns << " runs (minimum lift " << minAdjustmentLift << ") "
           << (passed ? "[PASS]\n" : "[FAIL]\n");
    return result;
}

// Paints one configuration for a fixed number of simulated 60 fps frames
// into an offscreen image, advancing the motion-preview phase per frame so
// the trail/now-dot path is exercised exactly as a live animation would.
juce::String benchConfig(const juce::String& label, int width, int height, bool perTap, bool enabled)
{
    SpatialHeadView view;
    view.setBounds(0, 0, width, height);
    view.setRadarEnabled(enabled);

    if (perTap)
    {
        view.setPerTapMode(true);
        view.setTapPosition(0, -40.0f, 10.0f, 0.8f);
        view.setTapPosition(1, 25.0f, 0.0f, 1.6f);
        view.setTapPosition(2, 135.0f, -15.0f, 3.2f);
        view.setEditTarget(SpatialHeadView::Target::H1);
    }

    SpatialHeadView::MotionPreviewState motion;
    motion.enabled = enabled;
    motion.patternId = 1;     // Orbit pattern
    motion.depth01 = 0.5f;    // 50% depth
    motion.tapMode = 2;       // Cascade: per-tap trails (worst case)
    motion.tapSpread = 0.5f;  // 50% spread
    motion.blockDurationSec = 1.0f / 60.0f;

    juce::Image target(juce::Image::ARGB, width, height, true);

    constexpr int warmupFrames = 24;
    constexpr int measuredFrames = 240;
    constexpr float rateHz = 1.4f;
    std::vector<double> frameMs;
    frameMs.reserve(measuredFrames);

    for (int i = 0; i < warmupFrames + measuredFrames; ++i)
    {
        motion.phaseCycles = (double)i * (double)rateHz / 60.0;
        view.setMotionPreviewState(motion);

        juce::Graphics g(target);
        const auto t0 = std::chrono::steady_clock::now();
        view.paintEntireComponent(g, false);
        const auto t1 = std::chrono::steady_clock::now();

        if (i >= warmupFrames)
            frameMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    const auto s = summarise(frameMs);
    const auto verdict = s.p95Ms <= 15.0 ? "60fps OK" : s.p95Ms <= 31.0 ? "30fps only" : "below 30fps";

    juce::String line;
    line << "  " << label << " (" << width << "x" << height << "): "
         << "avg " << juce::String(s.avgMs, 2) << "ms, "
         << "p50 " << juce::String(s.p50Ms, 2) << "ms, "
         << "p95 " << juce::String(s.p95Ms, 2) << "ms, "
         << "max " << juce::String(s.maxMs, 2) << "ms -> " << verdict << "\n";
    return line;
}
}  // namespace

juce::String runSpatialHeadViewPaintBenchTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);

    juce::String out;
    out << "SpatialHeadViewPaintBench on " << juce::SystemStats::getCpuModel() << " ("
        << juce::SystemStats::getNumCpus() << " cores, " << juce::SystemStats::getMemorySizeInMegabytes() << " MB), "
        << juce::SystemStats::getOperatingSystemName() << "\n";
    out << "  note: offscreen JUCE software renderer; onscreen macOS paints via "
           "CoreGraphics, so figures are indicative, not exact.\n";

    bool guideVisibilityPolicyPassed = false;
    out << checkGuideVisibilityPolicy(guideVisibilityPolicyPassed);

    // Sizes bracket the head view at the editor floor (1200x558), default
    // (1400x650), and maximum (1800x837). Glow underlay + motion trails on.
    out << benchConfig("linked, motion+glow", 560, 430, false, true);
    out << benchConfig("per-tap cascade, motion+glow", 560, 430, true, true);
    out << benchConfig("per-tap cascade, floor", 430, 330, true, true);
    out << benchConfig("per-tap cascade, max", 700, 520, true, true);
    out << benchConfig("disabled view (no glow), reference", 560, 430, true, false);

    // Optional capture (SPATIAL_ECHO_HEADVIEW_PNG=/path.png): one
    // clean linked frame so the markers + AZ/EL gizmo can be eyeballed offscreen
    // (no live GUI). Not part of the benchmark; skipped unless the env var is set.
    if (const char* pngPath = std::getenv("SPATIAL_ECHO_HEADVIEW_PNG"))
    {
        SpatialHeadView view;
        view.setBounds(0, 0, 560, 520);
        view.setRadarEnabled(true);
        view.setAzimuth(-35.0f);
        view.setElevation(22.0f);
        view.setDistance(1.4f);
        juce::Image img(juce::Image::ARGB, 560, 520, true);
        {
            juce::Graphics g(img);
            view.paintEntireComponent(g, false);
        }
        juce::File f(juce::String::fromUTF8(pngPath));
        f.deleteFile();
        if (auto os = std::unique_ptr<juce::FileOutputStream>(f.createOutputStream()))
        {
            juce::PNGImageFormat png;
            png.writeImageToStream(img, *os);
        }
        out << "  wrote head-view PNG: " << pngPath << "\n";
    }

    // Orbit capture (SPATIAL_ECHO_HEADVIEW_PNG_ORBIT=/path.png): the same
    // scene after a real empty-space orbit drag, driven through the public mouse
    // API. Proves the camera rotates AND that project()/chrome stay aligned; the
    // GL background follows via the pixel-locked MVP (see computeGLMatrix).
    if (const char* orbitPath = std::getenv("SPATIAL_ECHO_HEADVIEW_PNG_ORBIT"))
    {
        SpatialHeadView view;
        view.setBounds(0, 0, 560, 520);
        view.setRadarEnabled(true);
        view.setAzimuth(-35.0f);
        view.setElevation(22.0f);
        view.setDistance(1.4f);

        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto now = juce::Time::getCurrentTime();
        auto ev = [&](juce::Point<float> p, juce::Point<float> downPos, int clicks, bool dragged)
        {
            return juce::MouseEvent(src, p, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &view, &view, now,
                                    downPos, now, clicks, dragged);
        };
        const juce::Point<float> p0(60.0f, 320.0f);   // empty space (left, mid)
        const juce::Point<float> p1(210.0f, 250.0f);  // orbit: +yaw, tilt up
        view.mouseDown(ev(p0, p0, 1, false));
        view.mouseDrag(ev(p1, p0, 1, true));
        view.mouseUp(ev(p1, p0, 1, true));

        juce::Image oimg(juce::Image::ARGB, 560, 520, true);
        {
            juce::Graphics g(oimg);
            view.paintEntireComponent(g, false);
        }
        juce::File of(juce::String::fromUTF8(orbitPath));
        of.deleteFile();
        if (auto os = std::unique_ptr<juce::FileOutputStream>(of.createOutputStream()))
        {
            juce::PNGImageFormat png;
            png.writeImageToStream(oimg, *os);
        }
        out << "  wrote orbited head-view PNG: " << orbitPath << "\n";
    }

    // Audio-reactive capture (SPATIAL_ECHO_HEADVIEW_PNG_LOUD=/path.png): the
    // selected marker + phosphor glow at a forced "loud" level, to eyeball the
    // reactive halo/underlay lift vs the silent front frame. Test-only seam.
    if (const char* loudPath = std::getenv("SPATIAL_ECHO_HEADVIEW_PNG_LOUD"))
    {
        SpatialHeadView view;
        view.setBounds(0, 0, 560, 520);
        view.setRadarEnabled(true);
        view.setAzimuth(-35.0f);
        view.setElevation(22.0f);
        view.setDistance(1.4f);
        view.setAudioGlowForTest(0.85f);
        juce::Image img(juce::Image::ARGB, 560, 520, true);
        {
            juce::Graphics g(img);
            view.paintEntireComponent(g, false);
        }
        juce::File f(juce::String::fromUTF8(loudPath));
        f.deleteFile();
        if (auto os = std::unique_ptr<juce::FileOutputStream>(f.createOutputStream()))
        {
            juce::PNGImageFormat png;
            png.writeImageToStream(img, *os);
        }
        out << "  wrote loud head-view PNG: " << loudPath << "\n";
    }

    // Per-tap emitter nodes (SPATIAL_ECHO_HEADVIEW_PNG_TAPS=/path.png): the
    // three echo markers in per-tap mode (bright core + reticle + H1/H2/H3
    // labels).
    if (const char* tapsPath = std::getenv("SPATIAL_ECHO_HEADVIEW_PNG_TAPS"))
    {
        SpatialHeadView view;
        view.setBounds(0, 0, 560, 520);
        view.setRadarEnabled(true);
        view.setPerTapMode(true);
        view.setTapPosition(0, -55.0f, 12.0f, 0.7f);
        view.setTapPosition(1, 20.0f, -8.0f, 1.6f);
        view.setTapPosition(2, 130.0f, 28.0f, 3.4f);
        view.setEditTarget(SpatialHeadView::Target::H2);  // H2 selected (magenta)
        view.setAudioGlowForTest(0.6f);
        // Motion ON at a mid-phase: each echo should sit displaced along its
        // orbit trail (not at its base position).
        SpatialHeadView::MotionPreviewState m;
        m.enabled = true;
        m.patternId = 1;
        m.depth01 = 0.44f;
        m.tapMode = 1;
        m.tapSpread = 0.5f;  // Constellation: taps spread in phase
        m.phaseCycles = 0.18;
        m.blockDurationSec = 1.0f / 60.0f;
        view.setMotionPreviewState(m);
        juce::Image img(juce::Image::ARGB, 560, 520, true);
        {
            juce::Graphics g(img);
            view.paintEntireComponent(g, false);
        }
        juce::File f(juce::String::fromUTF8(tapsPath));
        f.deleteFile();
        if (auto os = std::unique_ptr<juce::FileOutputStream>(f.createOutputStream()))
        {
            juce::PNGImageFormat png;
            png.writeImageToStream(img, *os);
        }
        out << "  wrote per-tap head-view PNG: " << tapsPath << "\n";
    }

    // Orientation captures (SPATIAL_ECHO_HEADVIEW_PNG_ORIENT=/prefix): writes
    // <prefix>-shoulder/-face/-topdown/-side.png with the same static front
    // source (az 0) in each, so "F" sits by the nose and the camera is the only
    // variable.
    if (const char* orientPrefix = std::getenv("SPATIAL_ECHO_HEADVIEW_PNG_ORIENT"))
    {
        struct View
        {
            const char* suffix;
            float yaw, pitchOffset;
        };
        for (const auto& v : {View{"shoulder", 0.0f, 0.0f}, View{"face", 180.0f, 0.0f}, View{"topdown", 0.0f, -42.0f},
                              View{"side", 90.0f, 18.0f}})  // level profile: nose must point at F (right)
        {
            SpatialHeadView view;
            view.setBounds(0, 0, 620, 480);
            view.setRadarEnabled(true);
            view.setAzimuth(0.0f);
            view.setElevation(12.0f);
            view.setDistance(1.3f);
            view.setCameraForTest(v.yaw, v.pitchOffset);
            juce::Image img(juce::Image::ARGB, 620, 480, true);
            {
                juce::Graphics g(img);
                view.paintEntireComponent(g, false);
            }
            juce::File f(juce::String::fromUTF8(orientPrefix) + "-" + v.suffix + ".png");
            f.deleteFile();
            if (auto os = std::unique_ptr<juce::FileOutputStream>(f.createOutputStream()))
            {
                juce::PNGImageFormat png;
                png.writeImageToStream(img, *os);
            }
            out << "  wrote orientation PNG: " << f.getFullPathName() << "\n";
        }
    }

    out << "SpatialHeadViewPaintBench complete " << (guideVisibilityPolicyPassed ? "[PASS]" : "[FAIL]");
    return out;
}

}  // namespace SpatialEchoTests::System

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
