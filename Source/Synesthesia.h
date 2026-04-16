/*
  ==============================================================================

    VisualSynesthesia.h
    REAL GPU SHADER - Fragment shader running on GPU!

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "AudioSourceInterface.h"
#include <cmath>

#if defined(JUCE_MAC)
#define JUCE_CHECK_OPENGL_ERROR_MAC(label) \
    { GLenum err = juce::gl::glGetError(); \
      if (err != juce::gl::GL_NO_ERROR) DBG("OpenGL error " + juce::String(err) + " at " label); }
#else
#define JUCE_CHECK_OPENGL_ERROR_MAC(label)
#endif

class VisualSynesthesia : public juce::Component,
                          private juce::Timer,
                          private juce::OpenGLRenderer
{
public:
    explicit VisualSynesthesia (IAudioSource& receiver) : audioSource (receiver)
    {
        DBG("Synesthesia: constructor called");
        // Set OpenGL context to Core Profile (required for VAO on macOS)
        openGLContext.setOpenGLVersionRequired(juce::OpenGLContext::OpenGLVersion::openGL3_2);
        openGLContext.setRenderer (this);
        openGLContext.attachTo (*this);
        openGLContext.setContinuousRepainting (true);
        startTimerHz (30);
    }

    ~VisualSynesthesia() override
    {
        DBG("Synesthesia: destructor called");
        stopTimer();
        shutdownOpenGL();
    }

    void shutdownOpenGL()
    {
        DBG("Synesthesia: shutdownOpenGL called");
        openGLContext.detach();
    }

    void setSmoothAmount (float smooth01)
    {
        smoothAmount = juce::jlimit (0.01f, 0.99f, smooth01);
    }

    void setZoom (float z) { zoom = juce::jlimit (0.5f, 2.0f, z); }
    void setRotation (float r) { rotation = r; }
    void setSymmetry (int s) { symmetry = juce::jlimit (1, 8, s); }
    void setSaturation (float s) { saturation = juce::jlimit (0.0f, 2.0f, s); }
    void setBloom (float b) { bloom = juce::jlimit (0.0f, 1.0f, b); }
    void setSyncToBPM (bool sync) { syncToBPM = sync; }

private:
    void timerCallback() override
    {
        updateAudioData();

        if (smoothRMS > 0.02f)
        {
            float speed = 1.0f;
            if (syncToBPM)
            {
                float bpm = audioSource.getBPM();
                speed = (bpm / 120.0f);
            }
            animationTime += (1.0 / 30.0) * speed;
        }
    }

    void updateAudioData()
    {
        currentPitch = detectPitch();
        currentOctave = detectOctave();
        currentRMS = getRMSLevel();

        smoothPitch = smoothPitch + (currentPitch - smoothPitch) * smoothAmount;
        smoothOctave = smoothOctave + (currentOctave - smoothOctave) * (smoothAmount * 0.5f);
        smoothRMS = smoothRMS + (currentRMS - smoothRMS) * smoothAmount;
    }

    float detectPitch()
    {
        std::vector<float> spectrum;
        const int bins = audioSource.getLastFft (spectrum);
        if (bins == 0) return smoothPitch;

        int peakBin = 0;
        float peakMag = 0.0f;
        const int startBin = 3, endBin = 85;

        for (int i = startBin; i < endBin && i < bins; ++i)
            if (spectrum[i] > peakMag) { peakMag = spectrum[i]; peakBin = i; }

        if (peakMag < 0.01f) return smoothPitch;

        const float freq = peakBin * (48000.0f / 2048.0f);
        const float midiNote = 69.0f + 12.0f * std::log2 (freq / 440.0f);
        return static_cast<float> (static_cast<int> (std::round (midiNote)) % 12);
    }

    float detectOctave()
    {
        std::vector<float> spectrum;
        const int bins = audioSource.getLastFft (spectrum);
        if (bins == 0) return smoothOctave;

        int peakBin = 0;
        float peakMag = 0.0f;
        const int startBin = 1, endBin = 170;

        for (int i = startBin; i < endBin && i < bins; ++i)
            if (spectrum[i] > peakMag) { peakMag = spectrum[i]; peakBin = i; }

        if (peakMag < 0.01f) return smoothOctave;

        const float freq = peakBin * (48000.0f / 2048.0f);
        const float octave = std::log2 (freq / 16.35f);
        return juce::jlimit (0.0f, 7.0f, octave);
    }

    float getRMSLevel()
    {
        float raw = audioSource.getLastRms();
        if (raw < 0.001f) return 0.0f;  // noise gate ~-60 dBFS

        // Convert to dB and map: -36 dBFS -> 0.0, 0 dBFS -> 1.0
        // At -18 dBFS the value is 0.5 (starts fading noticeably)
        float dB = 20.0f * std::log10 (raw);
        return juce::jlimit (0.0f, 1.0f, (dB + 36.0f) / 36.0f);
    }

    // ===== OPENGL RENDERER =====

    void newOpenGLContextCreated() override
    {
        DBG("Synesthesia: newOpenGLContextCreated called");
        createShaders();
        static const float quadVerts[] = { -1.0f,-1.0f,  1.0f,-1.0f,  -1.0f,1.0f,  1.0f,1.0f };
        juce::gl::glGenVertexArrays (1, &quadVAO);
        JUCE_CHECK_OPENGL_ERROR_MAC("glGenVertexArrays");
        juce::gl::glGenBuffers (1, &quadVBO);
        JUCE_CHECK_OPENGL_ERROR_MAC("glGenBuffers");
        juce::gl::glBindVertexArray (quadVAO);
        JUCE_CHECK_OPENGL_ERROR_MAC("glBindVertexArray");
        juce::gl::glBindBuffer (juce::gl::GL_ARRAY_BUFFER, quadVBO);
        JUCE_CHECK_OPENGL_ERROR_MAC("glBindBuffer");
        juce::gl::glBufferData (juce::gl::GL_ARRAY_BUFFER, sizeof (quadVerts), quadVerts, juce::gl::GL_STATIC_DRAW);
        JUCE_CHECK_OPENGL_ERROR_MAC("glBufferData");
        juce::gl::glVertexAttribPointer (0, 2, juce::gl::GL_FLOAT, juce::gl::GL_FALSE, 0, nullptr);
        JUCE_CHECK_OPENGL_ERROR_MAC("glVertexAttribPointer");
        juce::gl::glEnableVertexAttribArray (0);
        JUCE_CHECK_OPENGL_ERROR_MAC("glEnableVertexAttribArray");
        juce::gl::glBindVertexArray (0);
        JUCE_CHECK_OPENGL_ERROR_MAC("glBindVertexArray(0)");
        // Explicit check for VAO/VBO creation
        if (quadVAO == 0 || quadVBO == 0)
            DBG("Synesthesia: VAO/VBO creation failed! quadVAO=" + juce::String(quadVAO) + ", quadVBO=" + juce::String(quadVBO));
    }

    void paint (juce::Graphics& g) override
    {
        DBG("Synesthesia: paint called, OpenGL attached = " + juce::String(openGLContext.isAttached() ? "true" : "false"));
        if (!openGLContext.isAttached())
        {
            g.setColour(juce::Colours::red);
            g.fillRect(getLocalBounds());
            g.setColour(juce::Colours::white);
            g.setFont(20.0f);
            g.drawText("OpenGL not rendering!", getLocalBounds(), juce::Justification::centred, true);
        }
        else if (quadVAO == 0)
        {
            g.setColour(juce::Colours::orange);
            g.fillRect(getLocalBounds());
            g.setColour(juce::Colours::black);
            g.setFont(20.0f);
            g.drawText("OpenGL VAO creation failed!", getLocalBounds(), juce::Justification::centred, true);
        }
        g.setColour(juce::Colours::darkgrey);
        g.drawRect(getLocalBounds(), 1);
    }

    void renderOpenGL() override
    {
        DBG("Synesthesia: renderOpenGL called");
#if JUCE_MAC
        if (!juce::OpenGLHelpers::isContextActive() || !shaderProgram)
        {
            DBG("Synesthesia: OpenGL context not active or shader missing, skipping frame");
            return;
        }
#endif
        const auto desktopScale = (float) openGLContext.getRenderingScale();
        juce::gl::glViewport (0, 0, juce::roundToInt (desktopScale * getWidth()), juce::roundToInt (desktopScale * getHeight()));
        JUCE_CHECK_OPENGL_ERROR_MAC("glViewport");
        juce::OpenGLHelpers::clear (juce::Colours::black);
        JUCE_CHECK_OPENGL_ERROR_MAC("OpenGLHelpers::clear");
        DBG("Synesthesia: smoothRMS = " + juce::String(smoothRMS));

        // Skip shader rendering entirely when audio is silent (saves GPU on Mac & Windows)
        if (smoothRMS < 0.001f)
            return;

        if (!shaderProgram)
        {
            DBG("Synesthesia: shaderProgram missing, skipping draw");
            return;
        }
        shaderProgram->use();
        JUCE_CHECK_OPENGL_ERROR_MAC("shaderProgram->use");
        DBG("Synesthesia: shaderProgram valid, VAO = " + juce::String(quadVAO) + ", VBO = " + juce::String(quadVBO));
        // Set uniforms using Uniform objects
        if (uniformResolution != nullptr) {
            uniformResolution->set ((float) (getWidth() * desktopScale), (float) (getHeight() * desktopScale));
            JUCE_CHECK_OPENGL_ERROR_MAC("uniformResolution->set");
        }
        if (uniformTime != nullptr) {
            uniformTime->set ((float) animationTime * 0.4f);
            JUCE_CHECK_OPENGL_ERROR_MAC("uniformTime->set");
        }
        if (uniformPitch != nullptr) {
            uniformPitch->set (smoothPitch);
            JUCE_CHECK_OPENGL_ERROR_MAC("uniformPitch->set");
        }
        if (uniformOctave != nullptr) {
            uniformOctave->set (smoothOctave);
            JUCE_CHECK_OPENGL_ERROR_MAC("uniformOctave->set");
        }
        if (uniformRMS != nullptr) {
            uniformRMS->set (smoothRMS);
            JUCE_CHECK_OPENGL_ERROR_MAC("uniformRMS->set");
        }
        if (uniformZoom != nullptr) {
            uniformZoom->set (zoom);
            JUCE_CHECK_OPENGL_ERROR_MAC("uniformZoom->set");
        }
        if (uniformRotation != nullptr) {
            uniformRotation->set (rotation);
            JUCE_CHECK_OPENGL_ERROR_MAC("uniformRotation->set");
        }
        if (uniformSymmetry != nullptr) {
            uniformSymmetry->set ((float) symmetry);
            JUCE_CHECK_OPENGL_ERROR_MAC("uniformSymmetry->set");
        }
        if (uniformSaturation != nullptr) {
            uniformSaturation->set (saturation);
            JUCE_CHECK_OPENGL_ERROR_MAC("uniformSaturation->set");
        }
        if (uniformBloom != nullptr) {
            uniformBloom->set (bloom);
            JUCE_CHECK_OPENGL_ERROR_MAC("uniformBloom->set");
        }
        juce::gl::glBindVertexArray (quadVAO);
        JUCE_CHECK_OPENGL_ERROR_MAC("glBindVertexArray");
        juce::gl::glDrawArrays (juce::gl::GL_TRIANGLE_STRIP, 0, 4);
        JUCE_CHECK_OPENGL_ERROR_MAC("glDrawArrays");
        juce::gl::glBindVertexArray (0);
        JUCE_CHECK_OPENGL_ERROR_MAC("glBindVertexArray(0)");
        DBG("Synesthesia: draw call completed");
    }

    void openGLContextClosing() override
    {
        DBG("Synesthesia: openGLContextClosing called");
        if (quadVBO != 0) { juce::gl::glDeleteBuffers (1, &quadVBO); quadVBO = 0; }
        if (quadVAO != 0) { juce::gl::glDeleteVertexArrays (1, &quadVAO); quadVAO = 0; }

        uniformResolution.reset();
        uniformTime.reset();
        uniformPitch.reset();
        uniformOctave.reset();
        uniformRMS.reset();
        uniformZoom.reset();
        uniformRotation.reset();
        uniformSymmetry.reset();
        uniformSaturation.reset();
        uniformBloom.reset();
        shaderProgram.reset();
    }

    void createShaders()
    {
        // ✅ SIMPLE VERTEX SHADER
        const char* vertexShader =
            "attribute vec2 position;\n"
            "void main()\n"
            "{\n"
            "    gl_Position = vec4(position, 0.0, 1.0);\n"
            "}\n";

        // ✅ SIMPLE BUT WORKING FRAGMENT SHADER
        const char* fragmentShader =
            "uniform vec2 iResolution;\n"
            "uniform float iTime;\n"
            "uniform float iPitch;\n"
            "uniform float iOctave;\n"
            "uniform float iRMS;\n"
            "uniform float iZoom;\n"
            "uniform float iRotation;\n"
            "uniform float iSymmetry;\n"
            "uniform float iSaturation;\n"
            "uniform float iBloom;\n"
            "\n"
            "void main()\n"
            "{\n"
            "    vec2 uv = (gl_FragCoord.xy * 2.0 - iResolution.xy) / iResolution.xy;\n"
            "    float angle = iRotation * 3.14159 / 180.0;\n"
            "    float s = sin(angle);\n"
            "    float c = cos(angle);\n"
            "    uv = vec2(uv.x * c - uv.y * s, uv.x * s + uv.y * c);\n"
            "    uv /= iZoom;\n"
            "    if (iSymmetry > 1.0)\n"
            "    {\n"
            "        float a = atan(uv.y, uv.x);\n"
            "        float r = length(uv);\n"
            "        float seg = 6.28318 / iSymmetry;\n"
            "        a = mod(a, seg) - seg * 0.5;\n"
            "        uv = vec2(cos(a), sin(a)) * r;\n"
            "    }\n"
            "    vec2 uv0 = uv;\n"
            "    vec3 finalColor = vec3(0.0);\n"
            "    float baseHue = iPitch / 12.0;\n"
            "    int maxIter = int(clamp(ceil(iOctave), 1.0, 7.0));\n"
            "    for (int i = 0; i < 7; i++)\n"
            "    {\n"
            "        if (i >= maxIter) break;\n"
            "        uv = fract(uv * 1.2) - 0.5;\n"
            "        float d = length(uv) * exp(-length(uv0));\n"
            "        float hue = fract(baseHue + float(i) * 0.05 + length(uv0) * 0.05);\n"
            "        float r = 0.5 + 0.5 * cos(6.28318 * (hue + 0.0));\n"
            "        float g = 0.5 + 0.5 * cos(6.28318 * (hue + 0.333));\n"
            "        float b = 0.5 + 0.5 * cos(6.28318 * (hue + 0.667));\n"
            "        vec3 col = vec3(r, g, b);\n"
            "        float gray = (col.r + col.g + col.b) / 3.0;\n"
            "        col = mix(vec3(gray), col, iSaturation);\n"
            "        d = sin(d * 8.0 + iTime) / 8.0;\n"
            "        d = abs(d);\n"
            "        d = pow(0.01 / max(d, 0.001), 1.2) / float(i + 1);\n"
            "        finalColor += col * d;\n"
            "    }\n"
            "    float intensity = iRMS * iRMS * 2.0;\n"
            "    finalColor *= intensity;\n"
            "    finalColor += finalColor * iBloom * 0.5;\n"
            "    gl_FragColor = vec4(finalColor, 1.0);\n"
            "}\n";
        shaderProgram.reset (new juce::OpenGLShaderProgram (openGLContext));
        bool shadersOk = shaderProgram->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (vertexShader))
                      && shaderProgram->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fragmentShader));
        DBG("Synesthesia: createShaders vertex/fragment status = " + juce::String(shadersOk ? "true" : "false"));
        if (shadersOk)
            juce::gl::glBindAttribLocation (shaderProgram->getProgramID(), 0, "position");
        if (shadersOk && shaderProgram->link())
        {
            DBG("Synesthesia: shader linked successfully");
            uniformResolution.reset (new juce::OpenGLShaderProgram::Uniform (*shaderProgram, "iResolution"));
            uniformTime.reset (new juce::OpenGLShaderProgram::Uniform (*shaderProgram, "iTime"));
            uniformPitch.reset (new juce::OpenGLShaderProgram::Uniform (*shaderProgram, "iPitch"));
            uniformOctave.reset (new juce::OpenGLShaderProgram::Uniform (*shaderProgram, "iOctave"));
            uniformRMS.reset (new juce::OpenGLShaderProgram::Uniform (*shaderProgram, "iRMS"));
            uniformZoom.reset (new juce::OpenGLShaderProgram::Uniform (*shaderProgram, "iZoom"));
            uniformRotation.reset (new juce::OpenGLShaderProgram::Uniform (*shaderProgram, "iRotation"));
            uniformSymmetry.reset (new juce::OpenGLShaderProgram::Uniform (*shaderProgram, "iSymmetry"));
            uniformSaturation.reset (new juce::OpenGLShaderProgram::Uniform (*shaderProgram, "iSaturation"));
            uniformBloom.reset (new juce::OpenGLShaderProgram::Uniform (*shaderProgram, "iBloom"));
            DBG ("GPU Shader compiled successfully!");
        }
        else
        {
#if JUCE_MAC
            DBG ("Shader error: " + shaderProgram->getLastError());
            DBG ("Shader source (fragment):\n" + juce::String(fragmentShader));
#endif
            shaderProgram.reset();
        }
    }

    IAudioSource& audioSource;
    juce::OpenGLContext openGLContext;
    std::unique_ptr<juce::OpenGLShaderProgram> shaderProgram;
    unsigned int quadVAO = 0, quadVBO = 0;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> uniformResolution, uniformTime, uniformPitch, uniformOctave, uniformRMS;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> uniformZoom, uniformRotation, uniformSymmetry, uniformSaturation, uniformBloom;

    float currentPitch = 0.0f, currentOctave = 2.0f, currentRMS = 0.0f;
    float smoothPitch = 0.0f, smoothOctave = 2.0f, smoothRMS = 0.0f;
    float smoothAmount = 0.15f;
    double animationTime = 0.0;

    float zoom = 1.0f;
    float rotation = 0.0f;
    int symmetry = 1;
    float saturation = 1.0f;
    float bloom = 0.0f;
    bool syncToBPM = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualSynesthesia)
};
