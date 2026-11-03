// SpatialHeadGL: GPU renderer for the head-view 3D background - the head wireframe as glowing lines through a
//   bright-pass / blur / composite bloom chain. Owned by SpatialHeadView and driven from its juce::OpenGLContext
//   callbacks (all methods run on the GL thread). The camera MVP + line geometry are supplied by SpatialHeadView so this
//   background aligns pixel-for-pixel with the juce::Graphics interaction chrome (same projection).
#pragma once
#include <JuceHeader.h>
#include <array>
#include <memory>
#include <vector>

class SpatialHeadGL
{
public:
    // vertexData: interleaved [x, y, z, bright] per vertex, GL_LINES (two verts
    // per segment). bright (0..1) selects the backdrop or head line colour so the
    // bright-pass keys the bloom on it. Returns false if any shader fails (the
    // caller then falls back to the CPU renderer).
    bool initialise(juce::OpenGLContext& ctx, const std::vector<float>& vertexData, int vertexCount);
    // audioGlow (0..1) lifts the bloom subtly with the live signal (capped at
    // +20%), on a channel separate from the depth/haze cues.
    void render(juce::OpenGLContext& ctx, int pxWidth, int pxHeight, const std::array<float, 16>& mvpColumnMajor,
                bool enabled, float audioGlow);
    void shutdown();
    bool isReady() const { return ready; }

private:
    void ensureFramebuffers(juce::OpenGLContext& ctx, int w, int h);
    void drawFullscreenQuad();

    std::unique_ptr<juce::OpenGLShaderProgram> lineShader, brightShader, blurShader, compositeShader;
    unsigned int lineVAO = 0, lineVBO = 0, quadVAO = 0;
    int lineVertexCount = 0;

    juce::OpenGLFrameBuffer sceneFBO, brightFBO, blurA, blurB;
    int fboW = 0, fboH = 0;
    bool ready = false;
};
