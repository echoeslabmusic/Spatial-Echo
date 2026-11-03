// PhosphorScope: Real-time oscilloscope with phosphor-persistence decay, min/max peak tracking, and beam glow effect.

#pragma once

#include <JuceHeader.h>
#include "CRTLookAndFeel.h"
#include "ScopeDataFifo.h"
#include <cmath>

//==============================================================================
// PhosphorScope
//
// Live-beam CRT oscilloscope with phosphor persistence.
// Combines:
//   - Min/max peak tracking (true envelope at any zoom)
//   - Real-time beam with glow (bright white core, green bloom)
//   - Per-frame ARGB decay for natural phosphor trail
//   - Free-running continuous sweep (always alive, never waiting)
//   - 1.2 s window refreshed at 60 Hz
//==============================================================================
class PhosphorScope : public juce::Component, private juce::Timer
{
public:
    explicit PhosphorScope(ScopeDataFifo& fifo)
        : scopeFifo(fifo)
    {
    }
    ~PhosphorScope() override { stopTimer(); }

    // Sweep timing follows the processor's rate; the editor calls this as the
    // rate changes. Non-positive rates (not yet prepared) are ignored.
    void setSampleRate(double sr)
    {
        if (sr <= 0.0 || juce::approximatelyEqual(sr, sampleRate))
            return;
        sampleRate = sr;
        recalcTiming();
    }

    void visibilityChanged() override
    {
        if (isVisible())
            startTimerHz(kRefreshHz);
        else
            stopTimer();
    }

    void resized() override
    {
        W = juce::jmax(1, getWidth());
        H = juce::jmax(1, getHeight());
        halfH = H / 2;
        scale = static_cast<float>(halfH) * 0.88f;

        phosphorBuf = juce::Image(juce::Image::ARGB, W, H, true);
        {
            juce::Graphics gc(phosphorBuf);
            gc.fillAll(juce::Colours::transparentBlack);
        }
        xCursor = 0;
        recalcTiming();

        if (isVisible() && !isTimerRunning())
            startTimerHz(kRefreshHz);
    }

    void paint(juce::Graphics& g) override
    {
        // Deep green-black background
        g.fillAll(juce::Colour(0xFF010A02));

        // Phosphor image (the accumulated glow)
        if (phosphorBuf.isValid())
            g.drawImageAt(phosphorBuf, 0, 0, false);

        const float fw = static_cast<float>(W);
        const float fh = static_cast<float>(H);

        // Graticule
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.05f));
        for (int i = 1; i < 8; ++i)
            g.drawVerticalLine(W * i / 8, 0.0f, fh);
        for (int i = 1; i < 4; ++i)
            g.drawHorizontalLine(H * i / 4, 0.0f, fw);

        // Centre zero-line
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.12f));
        g.drawHorizontalLine(halfH, 0.0f, fw);

        // Sweep cursor glow
        if (xCursor > 0 && xCursor < W)
        {
            g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.06f));
            g.drawVerticalLine(xCursor, 0.0f, fh);
        }

        // Border
        CRTLookAndFeel::drawTerminalBorder(g, getLocalBounds().reduced(1).toFloat(), 8.0f);
    }

private:
    ScopeDataFifo& scopeFifo;

    //-- Tuning
    static constexpr int kRefreshHz = 60;
    static constexpr double kWindowMs = 1200.0;      // Time window shown (ms)
    static constexpr float kDecay = 0.95f;           // Per-frame RGB decay
    static constexpr float kSilenceThresh = 0.003f;  // Below this = silence (beam stops)
    static constexpr int kReadBufSize = 8192;

    double sampleRate = 44100.0;
    double samplesPerCol = 6.615;

    int W = 1, H = 1, halfH = 0;
    float scale = 1.0f;
    int xCursor = 0;

    //-- Min/max peak tracking state
    double sampleAccum = 0.0;
    float colMin = 1.0f;
    float colMax = -1.0f;
    float prevMidY = 0.0f;  // Previous column midpoint for line interpolation
    bool hasPrev = false;   // Whether we have a valid previous point

    //-- Render
    juce::Image phosphorBuf;
    float readBuf[kReadBufSize]{};

    //==========================================================================
    void recalcTiming()
    {
        if (W <= 0)
            return;
        samplesPerCol = juce::jmax(0.5, (sampleRate * kWindowMs / 1000.0) / static_cast<double>(W));
    }

    //==========================================================================
    // Decay all pixels: multiply RGB by kDecay, keep alpha at 255
    void applyDecay()
    {
        juce::Image::BitmapData bm(phosphorBuf, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < H; ++y)
        {
            uint8_t* row = bm.getLinePointer(y);
            for (int x = 0; x < W * 4; x += 4)
            {
                row[x + 0] = static_cast<uint8_t>(row[x + 0] * kDecay);  // B
                row[x + 1] = static_cast<uint8_t>(row[x + 1] * kDecay);  // G
                row[x + 2] = static_cast<uint8_t>(row[x + 2] * kDecay);  // R
                // row[x + 3] = alpha, leave as-is
            }
        }
    }

    //==========================================================================
    // Gaussian-profile glow at a single point (Richard Andersson / m1el technique)
    // centerY is float for sub-pixel positioning; intensity controls brightness;
    // barHalf extends the "core" region above/below center (min/max range)
    void drawGlowPoint(juce::Image::BitmapData& bm, int x, float centerY, float barHalf, float intensity)
    {
        if (x < 0 || x >= W)
            return;

        static constexpr float kSigma = 2.2f;  // Gaussian spread
        static constexpr int kRadius = 5;      // Glow extent in pixels
        static constexpr float kSigmaSq2 = 2.0f * kSigma * kSigma;

        for (int dy = -kRadius; dy <= kRadius; ++dy)
        {
            const int py = static_cast<int>(centerY) + dy;
            if (py < 0 || py >= H)
                continue;

            // Distance from the beam core (inside barHalf range = 0 distance)
            float distFromCore = std::abs(static_cast<float>(dy));
            if (distFromCore < barHalf)
                distFromCore = 0.0f;
            else
                distFromCore -= barHalf;

            // Exponential radial falloff (gaussian beam profile)
            const float glow = std::exp(-(distFromCore * distFromCore) / kSigmaSq2) * intensity;

            if (glow < 0.004f)
                continue;

            // Core→edge colour shift: white-green core fades to pure green
            const float coreBlend = (distFromCore < 1.0f) ? 1.0f : std::exp(-distFromCore * 0.7f);

            // Core: warm white-green (R180 G255 B120)
            // Edge: pure green       (R0   G180 B0  )
            const uint8_t r = static_cast<uint8_t>(glow * (180.0f * coreBlend));
            const uint8_t g = static_cast<uint8_t>(glow * (180.0f + 75.0f * coreBlend));
            const uint8_t b = static_cast<uint8_t>(glow * (120.0f * coreBlend));

            addPixelF(bm, x, py, r, g, b);
        }

        // Horizontal glow spread to adjacent columns (softer)
        for (int dx = 1; dx <= 2; ++dx)
        {
            const float hGlow = intensity * ((dx == 1) ? 0.35f : 0.10f);
            for (int dy = -kRadius + 1; dy < kRadius; ++dy)
            {
                const int py = static_cast<int>(centerY) + dy;
                if (py < 0 || py >= H)
                    continue;

                float distFromCore = std::abs(static_cast<float>(dy));
                if (distFromCore < barHalf)
                    distFromCore = 0.0f;
                else
                    distFromCore -= barHalf;

                const float g2 = std::exp(-(distFromCore * distFromCore) / kSigmaSq2) * hGlow;
                if (g2 < 0.004f)
                    continue;

                const uint8_t gr = static_cast<uint8_t>(g2 * 200.0f);
                const uint8_t rr = static_cast<uint8_t>(g2 * 30.0f);
                const uint8_t br = static_cast<uint8_t>(g2 * 20.0f);

                if (x - dx >= 0)
                    addPixelF(bm, x - dx, py, rr, gr, br);
                if (x + dx < W)
                    addPixelF(bm, x + dx, py, rr, gr, br);
            }
        }
    }

    //==========================================================================
    // Draw beam segment: the min/max bar at column x, plus interpolated
    // connecting line from previous column's midpoint (analog continuity)
    void drawBeamSegment(juce::Image::BitmapData& bm, int x, float yTop, float yBot, float prevY)
    {
        const float curMidY = (yTop + yBot) * 0.5f;
        const float barHalf = std::abs(yBot - yTop) * 0.5f;

        // Beam speed intensity: slower beam = brighter (key analog characteristic)
        // (Richard Andersson: "intensity proportional to time spent at pixel")
        const float dy = curMidY - prevY;
        const float speed = std::sqrt(1.0f + dy * dy);
        const float intensity = juce::jlimit(0.35f, 1.0f, 2.5f / speed);

        // Draw connecting line pixels if beam moved more than 1 pixel vertically
        if (std::abs(dy) > 1.5f)
        {
            const int steps = static_cast<int>(std::abs(dy));
            for (int s = 1; s < steps; ++s)
            {
                const float t = static_cast<float>(s) / static_cast<float>(steps);
                const float interpY = prevY + dy * t;
                // Interpolated points: half-intensity, no bar extension
                drawGlowPoint(bm, x, interpY, 0.0f, intensity * 0.5f);
            }
        }

        // Draw the main glow at this column
        drawGlowPoint(bm, x, curMidY, barHalf, intensity);
    }

    //==========================================================================
    // Additive pixel blend (BGRA) - float-safe version
    static void addPixelF(juce::Image::BitmapData& bm, int x, int y, uint8_t addR, uint8_t addG, uint8_t addB)
    {
        uint8_t* px = bm.getLinePointer(y) + x * 4;
        px[0] = static_cast<uint8_t>(juce::jmin(255, px[0] + addB));
        px[1] = static_cast<uint8_t>(juce::jmin(255, px[1] + addG));
        px[2] = static_cast<uint8_t>(juce::jmin(255, px[2] + addR));
        px[3] = 255;
    }

    //==========================================================================
    void timerCallback() override
    {
        if (!phosphorBuf.isValid() || W <= 0 || H <= 0)
            return;

        // Decay previous frame
        applyDecay();

        // Pull and process new samples
        const int n = scopeFifo.pull(readBuf, kReadBufSize);
        if (n > 0)
        {
            juce::Image::BitmapData bm(phosphorBuf, juce::Image::BitmapData::readWrite);

            for (int i = 0; i < n; ++i)
            {
                const float sample = juce::jlimit(-1.0f, 1.0f, readBuf[i]);

                // Track min/max for current column
                if (sample < colMin)
                    colMin = sample;
                if (sample > colMax)
                    colMax = sample;

                sampleAccum += 1.0;

                if (sampleAccum >= samplesPerCol)
                {
                    sampleAccum -= samplesPerCol;

                    // Only draw if there is actual signal energy
                    const float peakAbs = juce::jmax(std::abs(colMin), std::abs(colMax));
                    if (peakAbs > kSilenceThresh)
                    {
                        // Convert min/max to float pixel Y
                        const float yTopF =
                            juce::jlimit(0.0f, static_cast<float>(H - 1), static_cast<float>(halfH) - colMax * scale);
                        const float yBotF =
                            juce::jlimit(0.0f, static_cast<float>(H - 1), static_cast<float>(halfH) - colMin * scale);

                        // Use previous midpoint for line interpolation
                        const float prevY = hasPrev ? prevMidY : (yTopF + yBotF) * 0.5f;

                        // Draw analog-style beam segment with glow
                        drawBeamSegment(bm, xCursor, yTopF, yBotF, prevY);

                        prevMidY = (yTopF + yBotF) * 0.5f;
                        hasPrev = true;

                        // Advance cursor
                        xCursor = (xCursor + 1) % W;
                    }

                    // Reset min/max for next column
                    colMin = 1.0f;
                    colMax = -1.0f;
                }
            }
        }

        repaint();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhosphorScope)
};
