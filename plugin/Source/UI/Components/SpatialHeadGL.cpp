// SpatialHeadGL: GPU renderer implementation. All methods run on the juce::OpenGLContext render thread.

#include "SpatialHeadGL.h"

using namespace juce::gl;

namespace
{
constexpr const char* kVersion = "#version 150\n";

const char* kLineVS = R"(
in vec3 position;
in float bright;
uniform mat4 mvp;
uniform vec2 ndcOffset;   // screen-constant thickening offset (NDC units)
out float vBright;
out float vDepth;
void main()
{
    vec4 clip = mvp * vec4(position, 1.0);
    // Offset in NDC scaled by w so the widening is constant in screen pixels
    // after the perspective divide (multi-pass stroke thickening).
    clip.xy += ndcOffset * clip.w;
    gl_Position = clip;
    vBright = bright;
    // clip.w == 1 + z2/D (the view-space depth), so clip.w - 1 is a real depth
    // proxy: ~0 at the head plane, positive behind, negative in front. (The mvp
    // z-row is intentionally 0, so clip.z can't be used here.)
    vDepth = clip.w - 1.0;
}
)";

const char* kLineFS = R"(
in float vBright;
in float vDepth;
out vec4 frag;
uniform vec3 baseColor;
uniform vec3 headColor;
uniform vec3 hazeColor;
uniform float dim;
void main()
{
    // vDepth ~ z2/D in roughly [-0.31, 0.31]; map to 0 (near) .. 1 (far).
    float d = clamp(vDepth * 1.7 + 0.5, 0.0, 1.0);
    // Depth haze: far geometry recedes into a dim phosphor fog (holographic depth).
    vec3 c = mix(baseColor, headColor, vBright);
    c = mix(c, hazeColor, 0.62 * d);
    float fade = 1.0 - 0.55 * d;
    frag = vec4(c * fade * dim, 1.0);
}
)";

const char* kQuadVS = R"(
out vec2 vUv;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kBrightFS = R"(
in vec2 vUv;
out vec4 frag;
uniform sampler2D inputTex;
uniform float threshold;
void main()
{
    vec3 c = texture(inputTex, vUv).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    frag = vec4(l > threshold ? c : vec3(0.0), 1.0);
}
)";

const char* kBlurFS = R"(
in vec2 vUv;
out vec4 frag;
uniform sampler2D inputTex;
uniform vec2 texelSize;
void main()
{
    float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 result = texture(inputTex, vUv).rgb * w[0];
    for (int i = 1; i < 5; ++i)
    {
        result += texture(inputTex, vUv + texelSize * float(i)).rgb * w[i];
        result += texture(inputTex, vUv - texelSize * float(i)).rgb * w[i];
    }
    frag = vec4(result, 1.0);
}
)";

const char* kCompositeFS = R"(
in vec2 vUv;
out vec4 frag;
uniform sampler2D sceneTex;
uniform sampler2D bloomTex;
uniform float bloomStrength;
void main()
{
    vec3 s = texture(sceneTex, vUv).rgb;
    vec3 b = texture(bloomTex, vUv).rgb;
    frag = vec4(s + bloomStrength * b, 1.0);
}
)";

std::unique_ptr<juce::OpenGLShaderProgram> makeProgram(juce::OpenGLContext& ctx, const char* vs, const char* fs,
                                                       const char* name)
{
    auto p = std::make_unique<juce::OpenGLShaderProgram>(ctx);
    if (p->addVertexShader(juce::String(kVersion) + vs) && p->addFragmentShader(juce::String(kVersion) + fs) &&
        p->link())
        return p;
    DBG("SpatialHeadGL: shader '" << name << "' failed: " << p->getLastError());
    juce::ignoreUnused(name);
    return nullptr;
}
}  // namespace

bool SpatialHeadGL::initialise(juce::OpenGLContext& ctx, const std::vector<float>& vertexData, int vertexCount)
{
    // Without a modern context (no GPU driver, virtual machines, remote
    // sessions) these entry points are null and calling one crashes the host.
    // Report failure so the view falls back to the CPU renderer.
    const bool hasModernGL = ctx.areShadersAvailable() && glGenVertexArrays != nullptr &&
                             glBindVertexArray != nullptr && glDeleteVertexArrays != nullptr &&
                             glGenBuffers != nullptr && glBindBuffer != nullptr && glBufferData != nullptr &&
                             glDeleteBuffers != nullptr && glBindFramebuffer != nullptr && glActiveTexture != nullptr &&
                             glGetAttribLocation != nullptr && glVertexAttribPointer != nullptr &&
                             glEnableVertexAttribArray != nullptr;
    if (!hasModernGL)
        return false;

    lineVertexCount = vertexCount;

    glGenVertexArrays(1, &lineVAO);
    glBindVertexArray(lineVAO);
    glGenBuffers(1, &lineVBO);
    glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertexData.size() * sizeof(float)), vertexData.data(), GL_STATIC_DRAW);

    lineShader = makeProgram(ctx, kLineVS, kLineFS, "line");
    brightShader = makeProgram(ctx, kQuadVS, kBrightFS, "bright");
    blurShader = makeProgram(ctx, kQuadVS, kBlurFS, "blur");
    compositeShader = makeProgram(ctx, kQuadVS, kCompositeFS, "composite");

    if (lineShader != nullptr)
    {
        const GLint posLoc = glGetAttribLocation(lineShader->getProgramID(), "position");
        const GLint brightLoc = glGetAttribLocation(lineShader->getProgramID(), "bright");
        const GLsizei stride = 4 * (GLsizei)sizeof(float);
        if (posLoc >= 0)
        {
            glVertexAttribPointer((GLuint)posLoc, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
            glEnableVertexAttribArray((GLuint)posLoc);
        }
        if (brightLoc >= 0)
        {
            glVertexAttribPointer((GLuint)brightLoc, 1, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray((GLuint)brightLoc);
        }
    }
    glBindVertexArray(0);
    glGenVertexArrays(1, &quadVAO);

    ready = lineShader && brightShader && blurShader && compositeShader;
    return ready;
}

void SpatialHeadGL::ensureFramebuffers(juce::OpenGLContext& ctx, int w, int h)
{
    if (w == fboW && h == fboH && sceneFBO.isValid())
        return;
    fboW = w;
    fboH = h;
    const int lw = juce::jmax(1, w / 2);
    const int lh = juce::jmax(1, h / 2);
    sceneFBO.release();
    brightFBO.release();
    blurA.release();
    blurB.release();
    sceneFBO.initialise(ctx, w, h);
    brightFBO.initialise(ctx, lw, lh);
    blurA.initialise(ctx, lw, lh);
    blurB.initialise(ctx, lw, lh);
}

void SpatialHeadGL::drawFullscreenQuad()
{
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void SpatialHeadGL::render(juce::OpenGLContext& ctx, int pxWidth, int pxHeight, const std::array<float, 16>& mvp,
                           bool enabled, float audioGlow)
{
    if (!ready)
        return;

    const int w = juce::jmax(1, pxWidth);
    const int h = juce::jmax(1, pxHeight);
    ensureFramebuffers(ctx, w, h);

    const GLuint defaultFBO = juce::OpenGLFrameBuffer::getCurrentFrameBufferTarget();
    const int lw = juce::jmax(1, w / 2);
    const int lh = juce::jmax(1, h / 2);
    const float dim = enabled ? 1.0f : 0.35f;

    glDisable(GL_DEPTH_TEST);

    // --- scene pass -> sceneFBO (multi-pass additive stroke thickening) ---
    sceneFBO.makeCurrentRenderingTarget();
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);  // accumulate the offset copies
    lineShader->use();
    lineShader->setUniformMat4("mvp", mvp.data(), 1, GL_FALSE);
    lineShader->setUniform("baseColor", 0.0f, 0.48f, 0.23f);   // lines tagged bright = 0 (backdrop tone)
    lineShader->setUniform("headColor", 0.22f, 0.66f, 0.40f);  // head (bright = 1): kept below white
    lineShader->setUniform("hazeColor", 0.0f, 0.14f, 0.10f);
    glBindVertexArray(lineVAO);

    // A small rosette of screen-space offsets turns 1px GL lines into thick,
    // bright emissive strokes with soft edges - the fuel a lush phosphor bloom
    // needs (glLineWidth>1 is unreliable in GL core on macOS). Offsets in NDC;
    // weights fall off so the stroke core saturates and the edges feather.
    const float ox = 2.0f / (float)w;  // 1 px in NDC-x
    const float oy = 2.0f / (float)h;  // 1 px in NDC-y
    const float rr = 1.3f;             // stroke half-width in px
    struct Tap
    {
        float x, y, wgt;
    };
    const Tap taps[] = {
        {0.0f, 0.0f, 0.62f},
        {rr, 0.0f, 0.30f},
        {-rr, 0.0f, 0.30f},
        {0.0f, rr, 0.30f},
        {0.0f, -rr, 0.30f},
        {rr * 0.7f, rr * 0.7f, 0.20f},
        {-rr * 0.7f, rr * 0.7f, 0.20f},
        {rr * 0.7f, -rr * 0.7f, 0.20f},
        {-rr * 0.7f, -rr * 0.7f, 0.20f},
    };
    for (const auto& t : taps)
    {
        lineShader->setUniform("ndcOffset", t.x * ox, t.y * oy);
        lineShader->setUniform("dim", dim * t.wgt);
        glDrawArrays(GL_LINES, 0, lineVertexCount);
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);

    // --- bright pass -> brightFBO ---
    brightFBO.makeCurrentRenderingTarget();
    glViewport(0, 0, lw, lh);
    brightShader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneFBO.getTextureID());
    brightShader->setUniform("inputTex", (GLint)0);
    brightShader->setUniform("threshold", 0.14f);  // higher -> bloom keys on the brightest strokes only
    drawFullscreenQuad();

    // --- separable blur ping-pong (4 iterations for a wider phosphor halo) ---
    juce::OpenGLFrameBuffer* src = &brightFBO;
    for (int i = 0; i < 4; ++i)
    {
        blurA.makeCurrentRenderingTarget();
        glViewport(0, 0, lw, lh);
        blurShader->use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src->getTextureID());
        blurShader->setUniform("inputTex", (GLint)0);
        blurShader->setUniform("texelSize", 1.7f / (float)lw, 0.0f);  // wider halo
        drawFullscreenQuad();

        blurB.makeCurrentRenderingTarget();
        glViewport(0, 0, lw, lh);
        blurShader->use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, blurA.getTextureID());
        blurShader->setUniform("inputTex", (GLint)0);
        blurShader->setUniform("texelSize", 0.0f, 1.7f / (float)lh);
        drawFullscreenQuad();
        src = &blurB;
    }

    // --- composite scene + bloom -> default framebuffer ---
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFBO);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    compositeShader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneFBO.getTextureID());
    compositeShader->setUniform("sceneTex", (GLint)0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, blurB.getTextureID());
    compositeShader->setUniform("bloomTex", (GLint)1);
    // Audio-reactive lift: bloom breathes with the signal, capped at +20% so it
    // stays a subtle glow (never a strobe). Distance/haze cues are untouched.
    const float glow = juce::jlimit(0.0f, 1.0f, audioGlow);
    compositeShader->setUniform("bloomStrength", 2.2f * (1.0f + 0.20f * glow));
    drawFullscreenQuad();
}

void SpatialHeadGL::shutdown()
{
    if (lineVBO != 0)
        glDeleteBuffers(1, &lineVBO);
    if (lineVAO != 0)
        glDeleteVertexArrays(1, &lineVAO);
    if (quadVAO != 0)
        glDeleteVertexArrays(1, &quadVAO);
    lineVBO = lineVAO = quadVAO = 0;
    sceneFBO.release();
    brightFBO.release();
    blurA.release();
    blurB.release();
    fboW = fboH = 0;
    lineShader.reset();
    brightShader.reset();
    blurShader.reset();
    compositeShader.reset();
    ready = false;
}
