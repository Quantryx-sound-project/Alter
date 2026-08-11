/*
  ==============================================================================

    Spectrogram.h
    Scrolling realtime spectrogram — GPU presented.

    - Log-frequency axis 16 Hz .. 20 kHz, colour = energy (theme heat map)
    - History is a DYNAMIC circular buffer sized to exactly match the visible
      window. Enlarging the window grows the buffer (the newly-exposed area
      fills in over time); shrinking it discards the oldest columns.
    - Newest data on the right edge.

    THREADING / WHERE THE WORK HAPPENS
    ----------------------------------
    Three threads, each with one job:

      WORKER (AsyncVisualBase, renderHeadless)
          The analysis, and nothing else. Gap-free hop-driven STFT, optional
          Constant-Q, full time+frequency reassignment, the per-row EMA, the
          row-fill taper. It finishes each column as 2048 bytes of intensity and
          writes them into a plain circular byte buffer (`shadow`).

      GL (AlterGLHost, sgGlPrepare + the shared spectrogram shader)
          Uploads only the columns produced since the last swap — about 2 KB
          each, a few per frame — into the history texture, then draws the
          visible window with a fragment shader at native resolution.

      MESSAGE
          The frequency grid and the labels, painted once and cached. Nothing
          per frame.

    This is the whole point of the design. The previous version rasterised the
    visible image on the worker EVERY frame and handed the finished picture to
    the GL context, which re-uploaded it on EVERY swap — a cost proportional to
    the module's AREA (roughly 0.8 MB/frame in a narrow HUD, 4.8 MB/frame
    fullscreen). The render pixel budget existed purely to bound that, which is
    exactly why short windows looked soft: sharpness and smoothness were being
    paid for out of the same budget. Now they are not related at all — per-frame
    CPU is the analysis alone, independent of how large the module is, and the
    picture is drawn at native resolution with no upscale.

    Consequences worth knowing:
      * scrolling is a fractional texture coordinate, so it is sub-pixel by
        construction rather than by a smoothing trick, and it is advanced once
        per SWAP instead of once per worker frame;
      * the frequency axis is box-filtered in the shader over exactly the rows
        that fall inside one screen pixel, so thin partials stop flickering as
        they cross pixel boundaries;
      * a rotated panel is handled by the shader (see SgShaderState), so the
        GPU picture and the 2D grid overlay always agree.

    ENHANCED FREQUENCY (professional quality) — unchanged:
    - Gap-free hop-driven STFT: every audio sample is analysed exactly once
      (IAudioSource::getMonoStream), ~93 % overlap - no snapshot duplicates,
      no timer jitter.
    - Full TIME + FREQUENCY reassignment (3 FFTs: Hann, dHann/dt, t*Hann):
      each bin's energy is painted at its true instantaneous frequency AND its
      true time-of-occurrence, so tones collapse into razor-thin lines and
      transients into sharp verticals.
    - Adaptive column density: ~1 column per screen pixel at any zoom.
    - 2048 rows for a smooth frequency axis.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <functional>
#include "AudioSourceInterface.h"
#include "AlterTheme.h"
#include "AsyncVisualBase.h"
#include "ConstantQ.h"
#include "SgShader.h"
#include "AlterGLHost.h"
#include "PitchUtils.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <array>
#include <atomic>

class SpectrogramMeter : public AsyncVisualBase,
                         private juce::Timer,
                         public  ISgShaderSource
{
public:
    static constexpr int kMinColRate       = 8;     // coarsest time resolution
    static constexpr int kMaxColRate       = 1200;  // finest time resolution — bounds
                                                    // the analysis cost at 1-2 s windows
                                                    // (reassign mode only; legacy modes
                                                    // are capped in updateDensity)
    static constexpr int kMaxWindowSec     = 120;
    static constexpr int kMaxCols          = 8192;  // hard buffer cap (bounds memory)
    static constexpr int kRows             = 2048;  // vertical (frequency) resolution
    static constexpr int kRaOrder          = 12;    // reassignment FFT: 4096-pt
    static constexpr int kRaSize           = 1 << kRaOrder;
    static constexpr int kPendR            = 6;     // time-reassign reach (± columns)
    static constexpr int kPendLen          = 2 * kPendR + 1;

    explicit SpectrogramMeter (IAudioSource& src)
        // 240 is a CAP, not a target: the real rate is refresh/N (phase-locked
        // to the GL swap), so leaving the cap above every common panel lets a
        // short window run at the full refresh. The frame divisor, driven by
        // windowSec in applyPendingConfig, decides where to give CPU back.
        // NOTE this now paces the ANALYSIS only — the picture is presented by
        // the GL host once per swap regardless.
        : AsyncVisualBase ("AlterSpectrogram", 240), audioSource (src)
    {
        // Transparent: the GL host draws the spectrogram itself into this
        // rectangle and we paint only the grid overlay on top of it.
        setOpaque (false);

        rebuildLut();
        buildRowToFreqTable();
        rowState.assign (kRows, 0.0f);
        colShaped.assign (kRows, 0.0f);
        rowFillTmp.assign (kRows, 0.0f);

        bufferCols = colsForWindow (windowSec);
        shadow.assign ((size_t) bufferCols * (size_t) kRows, 0);

        startTimerHz (30);    // geometry snapshot for the GL host + overlay refresh
        startAsyncRender();   // analysis lives on the worker thread
    }

    // Stop the worker BEFORE our members are destroyed (it writes `shadow`),
    // then leave the host's registry (which blocks until the GL thread has
    // finished any frame that could still be touching us), and only then hand
    // the textures over for deletion.
    ~SpectrogramMeter() override
    {
        stopAsyncRender();
        stopTimer();

        if (currentHost != nullptr)
        {
            currentHost->removeSource (static_cast<ISgShaderSource*> (this));
            currentHost->scheduleTextureDelete (histTex);
            currentHost->scheduleTextureDelete (lutTex);
            currentHost = nullptr;
        }
    }

    // ── Message-thread setters: scalars direct, container work via dirty flags ──

    /** 0..1 temporal smoothing (EMA per frequency row). 0 = raw frames. */
    void setSmoothAmount (float s01) { smoothing = juce::jlimit (0.0f, 0.9f, s01 * 0.9f); }

    /** Visible time window in seconds (1-120). */
    void setTimeWindow (float seconds)
    {
        windowSec = juce::jlimit (1.0f, (float) kMaxWindowSec, seconds);
        layoutDirty.store (true);   // worker resizes the history buffer
    }

    /** Mirror the frequency axis (low frequencies at the top). */
    void setMirrored (bool b)
    {
        if (mirrored == b) return;
        mirrored = b;
        axisDirty.store (true);     // worker rebuilds the row→frequency table
        repaint();                  // grid overlay flips with it
    }

    /** Low-frequency line thickness / row fill: 0 = thin line, 1 = filled row. */
    void setLineFill (float f) { lineFill = juce::jlimit (0.0f, 1.0f, f); }

    /** Constant-Q: aggregate FFT bins per log-frequency row. */
    void setConstantQ (bool b) { constantQ = b; }

    /** Enhanced-frequency mode (full spectral REASSIGNMENT, time + frequency). */
    void setReassign (bool b)
    {
        if (reassign != b)
        {
            reassign = b;
            streamResetPending.store (true);   // worker re-primes the stream
        }
    }

    /** 0 = theme heat map, 1 = custom colour gradient, 2 = colour by tone. */
    void setColourMode (int mode)
    {
        const int m = juce::jlimit (0, 2, mode);
        if (colourMode == m) return;
        colourMode = m;
        // Entering tone mode with a stale hue would slide the whole map across the
        // wheel on the first frame. Snap to what is sounding now.
        if (m == 2) toneHue.reset();
        lutDirty.store (true);
    }

    /** Primary custom colour (mid intensities). */
    void setCustomColour (juce::Colour c)
    {
        customColour = c;
        if (colourMode == 1) lutDirty.store (true);
    }

    /** Secondary custom colour (loudest intensities). */
    void setCustomColour2 (juce::Colour c)
    {
        customColour2 = c;
        if (colourMode == 1) lutDirty.store (true);
    }

    /** 'Mirror tone color' — reverses the tone→hue wheel (see PitchUtils). */
    void setToneTwist (bool b)
    {
        if (toneTwist == b) return;
        toneTwist = b;
        if (colourMode == 2) lutDirty.store (true);
    }

    /** 'Tone smooth' 0..1: HIGH = heavier smoothing (slow colour reaction), LOW =
        fast. Stored RAW — PitchUtils::toneSmoothToRate turns it into a chase rate,
        so the curve is defined once instead of once per module. */
    void setToneSmooth (float s01) { toneSmooth = juce::jlimit (0.0f, 1.0f, s01); }

    /** Info for the host overlay label (drawn rotation-proof by PanelHost). */
    float getTimeWindow() const noexcept { return windowSec; }

    // ── Cursor readout (the host draws it): exact frequency at a vertical pos ──
    juce::String cursorText (juce::Point<float> p) const
    {
        const float w = (float) getWidth(), h = (float) getHeight();
        if (w < 20.0f || h < 20.0f || p.x < 0.0f || p.x > w || p.y < 0.0f || p.y > h) return {};
        const float v = juce::jlimit (0.0f, 1.0f, p.y / h);
        const float t = mirrored ? v : (1.0f - v);                       // 0 = kFreqMin, 1 = kFreqMax
        const double hz = (double) kFreqMin * std::pow ((double) kFreqMax / (double) kFreqMin, (double) t);
        return (hz >= 1000.0) ? juce::String (hz / 1000.0, 2) + " kHz"
                              : juce::String ((int) std::round (hz)) + " Hz";
    }

    // ═══════════════════════ ISgShaderSource (GL THREAD) ═════════════════════

    SgShaderState getSgState() const override
    {
        const juce::ScopedLock sl (stateLock);
        return sgState;
    }

    bool sgGlPrepare (juce::OpenGLContext&, int texUnitHistory, int texUnitLut,
                      float localYPixels, SgFrameData& out) override
    {
        using namespace juce::gl;

        // ── SCROLL GLIDE, advanced once per SWAP ─────────────────────────────
        // Columns arrive from the worker in bursts; the displayed position glides
        // toward the newest one (bounded by a burst of latency). Doing this here
        // rather than on the worker means the motion is resampled at exactly the
        // rate the eye sees, so it is smooth even when the worker runs at a
        // divisor of the refresh.
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        double dt = glLastMs > 0.0 ? (nowMs - glLastMs) * 0.001 : 1.0 / 60.0;
        glLastMs = nowMs;
        dt = juce::jlimit (0.0, 0.25, dt);

        // ── pick up whatever the worker has produced ─────────────────────────
        // TRY-lock, never a blocking one: the GL thread is the presentation
        // thread and must not be stalled behind an FFT burst. A missed frame just
        // reuses the previous numbers, and the glide above keeps the motion going.
        {
            const juce::ScopedTryLock stl (histLock);
            if (stl.isLocked() && bufferCols > 0 && ! shadow.empty())
            {
                // colour map
                if (lutTex == 0)
                {
                    glGenTextures (1, &lutTex);
                    lutTexDirty.store (true);
                }
                if (lutTexDirty.exchange (false))
                {
                    glActiveTexture ((GLenum) (GL_TEXTURE0 + texUnitLut));
                    glBindTexture (GL_TEXTURE_2D, lutTex);
                    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
                    glTexImage2D (GL_TEXTURE_2D, 0, (GLint) GL_RGB8, kLutSize, 1, 0,
                                  GL_RGB, GL_UNSIGNED_BYTE, lutRGB.data());
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }

                // history — kRows WIDE (frequency), bufferCols TALL (time), so one
                // column is a contiguous run and a burst of columns is one upload
                if (histTex == 0)
                    glGenTextures (1, &histTex);

                glActiveTexture ((GLenum) (GL_TEXTURE0 + texUnitHistory));
                glBindTexture (GL_TEXTURE_2D, histTex);
                glPixelStorei (GL_UNPACK_ALIGNMENT, 1);

                if (texCols != bufferCols || texGen != historyGen)
                {
                    glTexImage2D (GL_TEXTURE_2D, 0, (GLint) GL_R8, kRows, bufferCols, 0,
                                  GL_RED, GL_UNSIGNED_BYTE, shadow.data());
                    // NEAREST on purpose: the time axis is a ring buffer, so any
                    // hardware filtering would blend the oldest column into the
                    // newest at the seam. Both axes are filtered explicitly in the
                    // shader instead — sub-pixel in time, box-filtered in frequency.
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    texCols = bufferCols;
                    texGen  = historyGen;
                    glUploadedCols = colsTotal;
                }
                else
                {
                    const int n = (int) juce::jlimit (0.0, (double) juce::jmin (bufferCols, filled),
                                                      colsTotal - glUploadedCols);
                    if (n > 0)
                    {
                        const int start = ((writeCol - n) % bufferCols + bufferCols) % bufferCols;
                        const int seg1  = juce::jmin (n, bufferCols - start);

                        glTexSubImage2D (GL_TEXTURE_2D, 0, 0, start, kRows, seg1,
                                         GL_RED, GL_UNSIGNED_BYTE,
                                         shadow.data() + (size_t) start * (size_t) kRows);
                        if (n > seg1)
                            glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, kRows, n - seg1,
                                             GL_RED, GL_UNSIGNED_BYTE, shadow.data());
                    }
                    glUploadedCols = colsTotal;
                }

                glPixelStorei (GL_UNPACK_ALIGNMENT, 4);   // JUCE's default

                glBufCols   = bufferCols;
                glFilled    = filled;
                glWriteCol  = writeCol;
                glColsTotal = colsTotal;
                glColRate   = columnsPerSec;
                glWindowSec = windowSec;
            }
        }

        // Note filled == 0 is NOT a reason to skip: the shader then paints the
        // theme background across the whole tile, which is what the module has to
        // show before any audio has arrived.
        if (histTex == 0 || lutTex == 0 || glBufCols <= 0)
            return false;

        const double maxLag = juce::jmax (12.0, (double) glColRate * 0.35);
        if (glDispCols <= 0.0 || glColsTotal - glDispCols > maxLag || glColsTotal < glDispCols)
            glDispCols = glColsTotal;                       // (re)sync after a stall
        else
            glDispCols += (glColsTotal - glDispCols) * juce::jlimit (0.05, 1.0, dt * 12.0);

        // The visible span may never eat into the scroll slack, or the left edge
        // starts falling off the end of the history again. In the extreme corner
        // (a very long window at a very high column rate) the buffer hits the
        // texture-size cap and the window shows marginally less time than the
        // label claims — which is invisible, whereas a wandering gap is not.
        const int visCap  = juce::jmax (2, glBufCols - scrollSlackCols (glColRate));
        const int visCols = juce::jlimit (2, visCap,
                                          (int) std::round (glWindowSec * (float) glColRate));

        // How far the displayed position is BEHIND the newest column, in columns.
        const double lag = juce::jmax (0.0, glColsTotal - glDispCols);

        // The head must be expressed relative to writeCol, NOT to colsTotal:
        // resizeBuffer / rescaleToRate relay the ring and reset writeCol while
        // colsTotal keeps counting, so the two are not congruent mod bufCols. The
        // newest column lives at writeCol - 1, hence the -1: at lag 0 the right
        // edge lands exactly on it with an interpolation weight of zero for the
        // (unwritten) column at writeCol.
        double head = std::fmod ((double) glWriteCol - 1.0 - lag, (double) glBufCols);
        if (head < 0.0) head += (double) glBufCols;

        out.headCol = (float) head;
        out.visCols = (float) visCols;
        out.bufCols = (float) glBufCols;
        // Valid history BEHIND the glided head — not behind the newest column, or
        // the left edge would show one burst of garbage while the glide catches up.
        out.filled  = (float) juce::jmax (0.0, (double) glFilled - lag);

        // Vertical box filter: enough taps to cover the rows inside one screen
        // pixel, spread over at least that many texture rows so no row is skipped
        // and no two taps land on the same texel.
        const float lyPx = juce::jmax (1.0f, localYPixels);
        const float taps = juce::jlimit (2.0f, 8.0f, std::round ((float) kRows / lyPx));
        out.yTaps   = taps;
        out.rowSpan = juce::jmax (1.0f / lyPx, taps / (float) kRows);

        glActiveTexture ((GLenum) (GL_TEXTURE0 + texUnitLut));
        glBindTexture (GL_TEXTURE_2D, lutTex);
        glActiveTexture ((GLenum) (GL_TEXTURE0 + texUnitHistory));
        glBindTexture (GL_TEXTURE_2D, histTex);
        return true;
    }

    void sgGlRelease() override
    {
        using namespace juce::gl;
        if (histTex != 0) { glDeleteTextures (1, &histTex); histTex = 0; }
        if (lutTex  != 0) { glDeleteTextures (1, &lutTex);  lutTex  = 0; }
        texCols = -1;
        texGen  = -1;
        lutTexDirty.store (true);
    }

    void deliverSgOffscreenImage (const juce::Image& img) override
    {
        {
            const juce::ScopedLock sl (imgLock);
            offscreenImg = img;
            ++offscreenGen;
        }
        juce::Component::SafePointer<SpectrogramMeter> sp (this);
        juce::MessageManager::callAsync ([sp]() mutable { if (sp != nullptr) sp->repaint(); });
    }

    /** ANY THREAD. Runs `fn` on the last frame the GL host rendered for us, while
        holding the image lock so it cannot be replaced mid-read.

        For Fusion, which uploads a layer's frame into a texture from the GL
        thread. `generation` counts delivered frames, so a caller that caches the
        result gets false when nothing new has arrived. Keep `fn` short. */
    bool readOffscreenFrame (juce::uint32& generation,
                             const std::function<void (const juce::Image&)>& fn)
    {
        const juce::ScopedLock sl (imgLock);

        if (! offscreenImg.isValid() || offscreenGen == generation)
            return false;

        generation = offscreenGen;
        fn (offscreenImg);
        return true;
    }

    /** Render through the FBO even when this window HAS a GL context. Used by
        Fusion, which needs the frame as an image to composite. */
    void setForceOffscreen (bool shouldForce)
    {
        if (forceOffscreen == shouldForce) return;
        forceOffscreen = shouldForce;
        updateHostRegistration();
    }

private:
    // ═════════════════════ MESSAGE THREAD (overlay + geometry) ═══════════════

    /** The GL host owns our pixels, so renderImage is never called: the worker
        runs renderHeadless() instead and does analysis only. */
    bool producesFrameImage() const override { return false; }

    /** Our pixels come from a shader inside AlterGLHost, and that shader now
        honours the flag: with uNoBg set, coverage comes from the ENERGY — the one
        thing a spectrogram actually knows — instead of the palette being painted
        over the background. */
    bool transparencyReachesPixels() const override { return true; }

    void renderImage (juce::Graphics&, int, int) override {}

    void paint (juce::Graphics& g) override
    {
        auto plot = getLocalBounds();

        if (isOffscreen)
        {
            // Detached window: no context of its own, so the primary host renders
            // us into an FBO and we blit the result.
            const juce::ScopedLock sl (imgLock);
            if (offscreenImg.isValid())
                g.drawImage (offscreenImg, plot.toFloat());
            else if (! isTransparentBackground())
            // On transparency this fallback would stamp an opaque gradient into a
            // fusion or an alpha export — the very thing being exported AROUND.
                AlterTheme::paintBackground (g, plot.toFloat());
        }
        else if (currentHost == nullptr)
        {
            // No GL host at all (context creation failed). Degrade to an empty
            // themed panel rather than to whatever was behind us — unless we are
            // meant to be transparent, in which case empty means empty.
            if (! isTransparentBackground())
                AlterTheme::paintBackground (g, plot.toFloat());
        }
        // Inline with a host: nothing — the shader has already drawn the picture
        // straight into this rectangle, and we only overlay the grid below.

        // Readable detail, not chrome — kept even in a transparent export. "Hide
        // info" is what removes it, there and on screen alike.
        if (! AlterTheme::hudInfoHidden.load())
            drawFrequencyGrid (g, plot);

        if (! hasData.load())
        {
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.setColour (AlterTheme::textDim);
            g.drawText ("waiting for audio...", plot, juce::Justification::centred, false);
        }
    }

    void parentHierarchyChanged() override { updateHostRegistration(); updateSgState(); }
    void resized() override                { updateSgState(); }
    void moved() override                  { updateSgState(); }
    void visibilityChanged() override      { updateSgState(); }

    void updateHostRegistration()
    {
        // Host of THIS window: found for the HUD, null for a detached window.
        auto* localHost = AlterGLHost::forComponent (this);
        // Detached -> render offscreen via the primary (HUD) host instead.
        AlterGLHost* target = (localHost != nullptr) ? localHost : AlterGLHost::getPrimary();

        // forceOffscreen: a Fusion layer needs PIXELS it can composite, not a
        // spectrogram painted straight into the window behind everything. It uses
        // the host it already has, just through the FBO path.
        isOffscreen = forceOffscreen ? (target != nullptr)
                                     : ((localHost == nullptr) && (target != nullptr));

        if (target != currentHost)
        {
            if (currentHost != nullptr)
            {
                currentHost->removeSource (static_cast<ISgShaderSource*> (this));
                // The textures belong to the OLD context; hand them back to it.
                currentHost->scheduleTextureDelete (histTex);
                currentHost->scheduleTextureDelete (lutTex);
                histTex = 0; lutTex = 0; texCols = -1; texGen = -1;
                lutTexDirty.store (true);
            }
            currentHost = target;
            if (currentHost != nullptr)
                currentHost->addSource (static_cast<ISgShaderSource*> (this));
        }
    }

    /** Snapshot of where we are, in the host component's coordinates, plus the
        affine basis that maps that rectangle back into our own local space (see
        SgShaderState) so a rotated panel stays aligned with the grid overlay. */
    void updateSgState()
    {
        SgShaderState st;
        st.offscreen = isOffscreen;
        st.noBackground = isTransparentBackground();

        const float lw = juce::jmax (1.0f, (float) getWidth());
        const float lh = juce::jmax (1.0f, (float) getHeight());

        if (isOffscreen)
        {
            st.x = 0; st.y = 0;
            st.w = getWidth(); st.h = getHeight();
            st.active = isShowing() && st.w > 1 && st.h > 1;
        }
        else if (currentHost != nullptr)
        {
            if (auto* hc = currentHost->getAttachedComponent())
            {
                const auto r = hc->getLocalArea (this, getLocalBounds());
                st.x = r.getX();     st.y = r.getY();
                st.w = r.getWidth(); st.h = r.getHeight();

                auto toLocalN = [this, hc, lw, lh] (juce::Point<int> p)
                {
                    const auto q = getLocalPoint (hc, p.toFloat());
                    return juce::Point<float> (q.x / lw, q.y / lh);
                };
                const auto p00 = toLocalN (r.getTopLeft());
                st.local0  = p00;
                st.localDU = toLocalN (r.getTopRight())   - p00;
                st.localDV = toLocalN (r.getBottomLeft()) - p00;

                st.active = isShowing() && st.w > 1 && st.h > 1;
            }
        }

        const juce::ScopedLock sl (stateLock);
        sgState = st;
    }

    void timerCallback() override
    {
        // Self-heal: the panel may well have been built before the window's GL
        // context existed, in which case parentHierarchyChanged found no host.
        if (currentHost == nullptr)
            updateHostRegistration();

        updateSgState();

        // The overlay is static, so it is painted once and left in JUCE's cache —
        // that is the other half of the per-frame cost this module used to pay.
        // Repaint only when something it actually shows has changed.
        const juce::int64 sig = (juce::int64) (hasData.load() ? 1 : 0)
                              | ((juce::int64) (mirrored ? 1 : 0)         << 1)
                              | ((juce::int64) AlterTheme::themeGeneration << 2)
                              | ((juce::int64) juce::jmax (0, getWidth()) << 8)
                              | ((juce::int64) juce::jmax (0, getHeight())<< 32);
        if (sig != overlaySig)
        {
            overlaySig = sig;
            repaint();
        }
    }

    // ═════════════════════ WORKER THREAD (renderHeadless) ════════════════════
    void renderHeadless (int w, int /*h*/) override
    {
        applyPendingConfig (w);

        // TIME BASE = the timestamp of the swap this frame will be presented in,
        // not "now", so the on-screen step is even even if the worker's own
        // cadence wobbles.
        const double nowMs = getPresentTimeSec() * 1000.0;
        double dt = lastTickMs > 0.0 ? (nowMs - lastTickMs) * 0.001 : 1.0 / 60.0;
        lastTickMs = nowMs;
        dt = juce::jlimit (0.0, 0.25, dt);   // also absorbs a time-base switch

        // ── produce new columns ──
        int wrote = 0;
        if (reassign)
        {
            wrote = pumpReassign (dt);          // sample-driven (gap-free stream)
        }
        else
        {
            // legacy snapshot modes: wall-clock column accounting (exact time axis)
            colAcc += dt * (double) columnsPerSec;
            colAcc = juce::jmin (colAcc, 256.0);   // bound the debt after a stall
            // The accumulator is only DEBITED once the columns have actually been
            // written. Debiting it up front (as this did) silently dropped every
            // column whose analysis frame was not ready yet — the history then
            // fell permanently behind the window it is supposed to cover, and the
            // scroll advanced in uneven jerks instead of evenly.
            const int n = juce::jmin ((int) colAcc, 64);   // must cover a frame's worth at
                                                           // kMaxColRate even when the load
                                                           // governor lowers the fps
            if (n > 0)
            {
                if (constantQ && audioSource.getLastCqt (cqtBuf) >= 16)
                {
                    colAcc -= (double) n;
                    // per-column recompute: the CQT frame-glide blend advances with
                    // EVERY column, so bursts never repeat identical columns (the
                    // horizontal blocks that used to look pixelated).
                    for (int i = 0; i < n; ++i)
                    {
                        writeColumnCqt();
                        publishColumn();
                    }
                    wrote = n;
                }
                else
                {
                    const int bins = audioSource.getLastFft (fft);
                    if (bins >= 64)
                    {
                        colAcc -= (double) n;
                        // ANTI-BLOCK GLIDE (the linear counterpart of the CQT
                        // frame-glide above). The FFT feed refreshes at 60 Hz
                        // while this branch emits up to 240 columns/s, so
                        // committing the same column n times painted a hard block
                        // n·pxPerCol wide. Ramping from the previous committed
                        // column to the new one makes every column distinct.
                        writeColumn (bins);

                        if (colPrev.size() != colShaped.size())
                            colPrev = colShaped;      // first column: no ramp out of silence
                        colTarget = colShaped;

                        for (int i = 0; i < n; ++i)
                        {
                            const float t = (float) (i + 1) / (float) n;
                            for (size_t r = 0; r < colShaped.size(); ++r)
                                colShaped[r] = colPrev[r] + t * (colTarget[r] - colPrev[r]);

                            publishColumn();
                        }
                        colPrev = colTarget;
                        wrote = n;
                    }
                }
            }
        }
        if (wrote > 0) hasData.store (true);
    }

    // Apply message-thread requests (theme/LUT, axis, buffer size, density).
    void applyPendingConfig (int w)
    {
        // NO RENDER PIXEL BUDGET any more. It existed only to bound the per-frame
        // rasterisation + texture upload, both of which are gone: the shader draws
        // at native resolution for free. This is what removes the old trade-off
        // where a short window had to be rendered small (and upscaled soft) to
        // stay smooth.

        // WINDOW-ADAPTIVE WORKER RATE. This no longer throttles the PICTURE — the
        // GL host presents every swap regardless — it decides how often the worker
        // wakes to pump the analysis. Column production is sample-driven, so
        // waking less often simply batches more columns per wake; a long window
        // scrolls slowly enough that a third of the refresh costs it nothing.
        setMinFrameDivisor (windowSec <= 4.0f  ? 1
                          : windowSec <= 20.0f ? 2
                          :                      3);

        // ── COLOUR BY TONE: the map itself follows the note ───────────────────
        //
        // The picture is a 256-entry LUT uploaded as a texture — the shader does the
        // lookup, so recolouring the whole spectrogram costs one LUT rebuild, not a
        // re-render of the history. But a rebuild + texture upload EVERY frame, for
        // a hue that moves by a thousandth of the wheel, is pure waste: the deadband
        // below means a held note rebuilds nothing at all and a note change costs a
        // short burst of rebuilds while the hue slides. 1/512 of the wheel is well
        // under one 8-bit step in any channel, so nothing visible is skipped.
        if (colourMode == 2)
        {
            toneHue.update (audioSource, toneTwist, toneSmooth);
            if (std::abs (toneHue.hue() - lutToneHue) > (1.0f / 512.0f))
            {
                lutToneHue = toneHue.hue();
                lutDirty.store (true);
            }
        }

        if (lutTheme != (int) AlterTheme::themeGeneration || lutDirty.exchange (false))
        {
            lutTheme = (int) AlterTheme::themeGeneration;
            rebuildLut();
        }

        if (axisDirty.exchange (false))
            buildRowToFreqTable();

        if (streamResetPending.exchange (false))
        {
            streamPos = 0;
            if (! pend.empty()) std::fill (pend.begin(), pend.end(), 0.0f);
        }

        // Buffer resize is THROTTLED: during a continuous WINDOW drag setTimeWindow
        // fires every tick, and a per-frame realloc+copy of the (large) history
        // buffer is exactly the stutter the zoom used to have. The shader uses the
        // draw-side visCols meanwhile, so a short delay here is invisible.
        if (layoutDirty.exchange (false))
        {
            const auto now = juce::Time::getMillisecondCounter();
            if (filled == 0 || now - lastResizeMs >= 250)
            {
                resizeBuffer();
                lastResizeMs = now;
            }
            else
                layoutDirty.store (true);   // re-checked next frame (throttled)
        }

        updateDensity (w);
    }

    // ── Colour LUT: theme heat map or custom-colour gradient ─────────────────
    void rebuildLut()
    {
        // VIBRANCE boost for the custom gradient: the user picks the HUE, the map
        // guarantees punch. Straight RGB interpolation from the dark background
        // desaturated the midtones (any picked pair looked washed-out next to the
        // theme heat map); boosting saturation/brightness of the endpoints and
        // easing the dark→colour ramp (reaches full colour sooner) keeps every
        // combination as vivid as the preset.
        auto vivid = [] (juce::Colour c)
        {
            return juce::Colour::fromHSV (c.getHue(),
                                          juce::jmin (1.0f, c.getSaturation() * 1.35f + 0.05f),
                                          juce::jmin (1.0f, c.getBrightness() * 1.15f + 0.05f),
                                          1.0f);
        };
        // TONE MODE reuses the custom-gradient SHAPE (dark → colour → hot) rather
        // than flat-tinting everything, because the shape is what carries loudness.
        // A single hue applied at constant saturation would make a quiet 200 Hz
        // rumble and a peaking 2 kHz transient the same colour, and the spectrogram
        // would stop being readable as an intensity plot the moment tone colour was
        // switched on. The note supplies the HUE; the endpoints supply the ramp:
        // the note at full strength, then washing toward white at the loudest end,
        // exactly as the preset heat map does.
        // Falls back to the picked colour until a note has actually been detected:
        // an untracked hue is 0, i.e. pure red, and a heat map that silently turns
        // red is a lie about the audio rather than a missing feature.
        const juce::Colour toneC = toneHue.hasTone() ? toneHue.colour() : vivid (customColour);
        const juce::Colour c1 = (colourMode == 2) ? toneC             : vivid (customColour);
        const juce::Colour c2 = (colourMode == 2) ? toneC.brighter (0.85f)
                                                  : vivid (customColour2);

        // Locked because the GL thread uploads these bytes straight into the
        // colour-map texture.
        const juce::ScopedLock sl (histLock);

        for (int i = 0; i < kLutSize; ++i)
        {
            const float v = (float) i / (float) (kLutSize - 1);
            juce::Colour c;
            if (colourMode == 0)
                c = AlterTheme::heatColour (v);
            else   // 1 = custom gradient, 2 = tone colour (same ramp, note-derived ends)
            {
                // dark → primary colour → secondary colour (loudest)
                if (v < 0.5f)
                    c = AlterTheme::bgVoid.interpolatedWith (c1, std::pow (v / 0.5f, 0.70f));
                else
                    c = c1.interpolatedWith (c2, (v - 0.5f) / 0.5f);
            }

            lutRGB[(size_t) (i * 3 + 0)] = c.getRed();
            lutRGB[(size_t) (i * 3 + 1)] = c.getGreen();
            lutRGB[(size_t) (i * 3 + 2)] = c.getBlue();
        }

        lutTexDirty.store (true);
    }

    // ── Row → frequency band table (log mapping; row 0 = top) ────────────────
    void buildRowToFreqTable()
    {
        rowFreqLo.resize (kRows);
        rowFreqHi.resize (kRows);

        const float logMin = std::log10 (kFreqMin), logMax = std::log10 (kFreqMax);
        for (int row = 0; row < kRows; ++row)
        {
            // normal: top = highest f;  mirrored: top = lowest f
            float t1 = 1.0f - (float)  row      / (float) kRows;
            float t0 = 1.0f - (float) (row + 1) / (float) kRows;
            if (mirrored) { t0 = 1.0f - t0; t1 = 1.0f - t1; std::swap (t0, t1); }

            rowFreqLo[(size_t) row] = std::pow (10.0f, logMin + t0 * (logMax - logMin));
            rowFreqHi[(size_t) row] = std::pow (10.0f, logMin + t1 * (logMax - logMin));
        }
    }

    // ── Adaptive density: ~1 column per screen pixel at the current zoom ──────
    void updateDensity (int w)
    {
        // Legacy (snapshot / CQT) columns recompute ALL kRows per column, so their
        // per-second cost scales with the rate — cap them at 240 col/s. Only the
        // reassignment path (cheap per column: splat + flush) runs up to the full
        // kMaxColRate for per-pixel time resolution at short windows.
        const int maxRate = reassign ? kMaxColRate : 240;
        // Reassign mode targets EXACTLY 1 column per screen pixel: the shader
        // blits columns 1:1 with no horizontal resampling at all → crisp
        // scrolling. Legacy modes keep a little oversampling.
        const float density = reassign ? 1.0f : 1.4f;
        const int target = juce::jlimit (kMinColRate, maxRate,
                                         (int) std::ceil (density * (float) juce::jmax (100, w) / windowSec));

        // DEBOUNCED: while the user drags the WINDOW the target rate changes every
        // frame, and rescaling the whole history per frame stuttered badly. The
        // rate is only committed once the target has been STABLE for 250 ms — one
        // rescale shortly after the drag stops (the column rate is a pure
        // resolution knob: the time axis stays correct at any rate meanwhile).
        if (target != pendingRate)
        {
            pendingRate   = target;
            pendingRateMs = juce::Time::getMillisecondCounter();
        }
        else if (target != columnsPerSec
                 && (filled == 0
                     || juce::Time::getMillisecondCounter() - pendingRateMs >= 250))
        {
            rescaleToRate (target);
        }

        const double sr = juce::jmax (8000.0, audioSource.getSampleRate());
        hopSamples = juce::jmax (32, (int) std::round (sr / (double) columnsPerSec));

        // keep the EMA's TIME constant independent of the column rate
        effSmooth = smoothing <= 0.0f ? 0.0f
                                      : std::pow (smoothing, 60.0f / (float) columnsPerSec);
    }

    /** Hard ceiling on the history length: our own memory cap, further limited by
        what the driver will accept as a texture height. */
    static int maxCols() noexcept
    {
        return juce::jlimit (256, kMaxCols, AlterGLHost::maxHistoryColumns().load());
    }

    /** SCROLL SLACK — history kept BEYOND the visible window, in columns.
        The displayed position glides BEHIND the newest column by up to maxLag
        columns (see sgGlPrepare), which is what makes the scroll smooth when the
        worker delivers columns in bursts. Sliding the head back by L columns
        means the LEFT edge of the window now needs the column L further back
        than the oldest one the window nominally covers — and if the buffer holds
        exactly one window, that column does not exist. The result is a strip of
        background on the left whose width is the current lag, so it wanders and
        jumps every time the glide resynchronises.
        Keeping this much extra history makes the required column always present.
        (The CPU path had the identical problem and solved it the identical way,
        under the name cacheSlack.) maxLag is 0.35 s of columns, so 0.75 s is
        comfortable headroom at any rate. */
    static int scrollSlackCols (int rate) noexcept
    {
        return juce::jmax (96, (int) std::ceil (0.75 * (double) juce::jmax (1, rate)));
    }

    // ── Buffer capacity for a given window length: the window itself plus the
    //    scroll slack, clamped to the hard cap. ──
    int colsForWindow (float sec) const
    {
        return juce::jlimit (2, maxCols(),
                             (int) (sec * (float) columnsPerSec) + scrollSlackCols (columnsPerSec));
    }

    // ── Resize the dynamic buffer to match the current window, preserving the
    //    most recent columns (same column rate). ──
    void resizeBuffer()
    {
        const int newCols = colsForWindow (windowSec);
        if (newCols == bufferCols && ! shadow.empty()) return;

        const int keep = juce::jmin (filled, newCols);

        // Built OUTSIDE the lock; only the swap is locked, so the GL thread is
        // never held up by the allocation or the copy.
        std::vector<juce::uint8> ns ((size_t) newCols * (size_t) kRows, 0);

        if (keep > 0 && ! shadow.empty())
        {
            // copy the `keep` newest columns into dest cols [0, keep)
            const int startCol = ((writeCol - keep) % bufferCols + bufferCols) % bufferCols;
            const int seg1     = juce::jmin (keep, bufferCols - startCol);
            const int seg2     = keep - seg1;

            std::memcpy (ns.data(),
                         shadow.data() + (size_t) startCol * (size_t) kRows,
                         (size_t) seg1 * (size_t) kRows);
            if (seg2 > 0)
                std::memcpy (ns.data() + (size_t) seg1 * (size_t) kRows,
                             shadow.data(), (size_t) seg2 * (size_t) kRows);
        }

        const juce::ScopedLock sl (histLock);
        shadow.swap (ns);
        bufferCols = newCols;
        filled     = keep;
        writeCol   = keep % newCols;
        ++historyGen;      // the GL thread reallocates and refills its texture
    }

    // ── Change the column rate (zoom/resize): horizontally rescale the existing
    //    history so time alignment is preserved while the new data fills in. ──
    void rescaleToRate (int newRate)
    {
        const int    newCols   = juce::jlimit (2, maxCols(),
                                               (int) (windowSec * (float) newRate)
                                               + scrollSlackCols (newRate));
        const double ratio     = (double) newRate / (double) juce::jmax (1, columnsPerSec);
        const int    newFilled = juce::jmin (newCols, (int) std::round ((double) filled * ratio));

        std::vector<juce::uint8> ns ((size_t) newCols * (size_t) kRows, 0);

        if (filled > 0 && newFilled > 0 && ! shadow.empty())
        {
            const int startCol = ((writeCol - filled) % bufferCols + bufferCols) % bufferCols;
            for (int d = 0; d < newFilled; ++d)
            {
                const int sIdx = juce::jlimit (0, filled - 1, (int) ((double) d / ratio));
                const int sCol = (startCol + sIdx) % bufferCols;
                std::memcpy (ns.data() + (size_t) d * (size_t) kRows,
                             shadow.data() + (size_t) sCol * (size_t) kRows,
                             (size_t) kRows);
            }
        }

        const juce::ScopedLock sl (histLock);
        shadow.swap (ns);
        bufferCols    = newCols;
        filled        = newFilled;
        writeCol      = newFilled % newCols;
        columnsPerSec = newRate;
        ++historyGen;
    }

    /** Finish the current column and make it visible to the GL thread. Everything
        the GL thread reads about the history moves under this one lock, so it can
        never observe a half-written column or a stale writeCol. */
    void publishColumn()
    {
        const juce::ScopedLock sl (histLock);
        commitColumn();
        writeCol   = (writeCol + 1) % bufferCols;
        filled     = juce::jmin (filled + 1, bufferCols);
        colsTotal += 1.0;
    }

    // ── Legacy linear-FFT column (fills colShaped; caller commits) ───────────
    void writeColumn (int bins)
    {
        const float nyq   = (float) (juce::jmax (8000.0, audioSource.getSampleRate()) * 0.5);
        const float binHz = nyq / (float) bins;

        for (int row = 0; row < kRows; ++row)
        {
            const float fLo = rowFreqLo[(size_t) row];
            const float fHi = rowFreqHi[(size_t) row];
            const float bandBins = (fHi - fLo) / binHz;   // how many bins this row covers

            float v;
            if (bandBins >= 1.0f || constantQ)
            {
                // wide band (or Constant-Q): MAX over the bins in this log band →
                // tones never get lost and the row shows true per-band energy.
                const int iLo = juce::jlimit (1, bins - 1, (int) (fLo / binHz));
                const int iHi = juce::jlimit (iLo, bins - 1, (int) std::ceil (fHi / binHz));
                v = 0.0f;
                for (int i = iLo; i <= iHi; ++i)
                    v = juce::jmax (v, fft[(size_t) i]);
            }
            else
            {
                // narrow band (lows): one bin covers many pixel rows.
                // Interpolate between bins and draw a THIN line centred on the
                // actual bin frequency instead of a fat pixelated block.
                const float pos  = juce::jlimit (1.0f, (float) bins - 1.001f,
                                                 0.5f * (fLo + fHi) / binHz);
                const int   i0   = (int) pos;
                const float frac = pos - (float) i0;
                v = fft[(size_t) i0] * (1.0f - frac) + fft[(size_t) (i0 + 1)] * frac;

                const float d     = std::abs (pos - std::round (pos));      // 0..0.5 bins
                const float sigma = juce::jmax (bandBins * 0.9f, 0.10f);    // narrower → thinner
                const float gauss = std::exp (-(d * d) / (2.0f * sigma * sigma));
                v *= gauss;   // thin line — thickness is applied in commitColumn()
            }

            // temporal smoothing per row + mild gamma lift for quiet content
            auto& st = rowState[(size_t) row];
            st = effSmooth * st + (1.0f - effSmooth) * v;
            colShaped[(size_t) row] = std::pow (juce::jlimit (0.0f, 1.0f, st), 0.75f);
        }
    }

    // Constant-Q column: each log row samples the (already log-spaced) CQT bins.
    // ANTI-PIXELATION: the CQT stream refreshes only ~30×/s while columns are
    // written much faster, so a raw copy repeats the same snapshot 4-8 columns
    // = horizontal blocks. The display buffer GLIDES between analysis frames
    // instead (per-column blend sized to the column rate), and the vertical
    // sampling uses Catmull-Rom between the coarse log bins, so neither axis
    // shows stepping.
    void writeColumnCqt()
    {
        const int n = (int) cqtBuf.size();

        if ((int) cqtDispSg.size() != n)
            cqtDispSg = cqtBuf;
        else
        {
            const float a = juce::jlimit (0.15f, 1.0f,
                                          34.0f / (float) juce::jmax (1, columnsPerSec));
            for (int i = 0; i < n; ++i)
                cqtDispSg[(size_t) i] += a * (cqtBuf[(size_t) i] - cqtDispSg[(size_t) i]);
        }

        auto sampleCq = [this, n] (int idx) -> float
        {
            return cqtDispSg[(size_t) juce::jlimit (0, n - 1, idx)];
        };

        for (int row = 0; row < kRows; ++row)
        {
            const float f = 0.5f * (rowFreqLo[(size_t) row] + rowFreqHi[(size_t) row]);
            const double k = (double) ConstantQAnalyzer::kBinsPerOctave
                           * std::log2 ((double) f / ConstantQAnalyzer::kFMin);
            float v = 0.0f;
            if (k >= 0.0 && k < (double) n)
            {
                const int   k0 = (int) k;
                const float t  = (float) (k - (double) k0);
                const float p0 = sampleCq (k0 - 1), p1 = sampleCq (k0);
                const float p2 = sampleCq (k0 + 1), p3 = sampleCq (k0 + 2);
                const float t2 = t * t, t3 = t2 * t;
                const float m1 = 0.5f * (p2 - p0);
                const float m2 = 0.5f * (p3 - p1);
                v = (2*t3 - 3*t2 + 1)*p1 + (t3 - 2*t2 + t)*m1
                  + (-2*t3 + 3*t2)*p2 + (t3 - t2)*m2;
            }

            auto& st = rowState[(size_t) row];
            st = effSmooth * st + (1.0f - effSmooth) * juce::jlimit (0.0f, 1.0f, v);
            colShaped[(size_t) row] = std::pow (juce::jlimit (0.0f, 1.0f, st), 0.75f);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    //  ENHANCED FREQUENCY — gap-free, hop-driven TIME+FREQUENCY reassignment
    // ═══════════════════════════════════════════════════════════════════════════

    // Fast LUT versions of the hot-path transcendentals: at high column rates the
    // splat/flush loops call them millions of times per second — LUTs keep the
    // per-frame cost low so short windows stay smooth (visually identical).
    static float expNegLut (float x) noexcept   // ≈ exp(-x), x >= 0
    {
        static const auto lut = []
        {
            std::array<float, 257> t{};
            for (int i = 0; i <= 256; ++i)
                t[(size_t) i] = std::exp (-13.0f * (float) i / 256.0f);
            return t;
        }();
        const float f = x * (256.0f / 13.0f);
        if (f >= 256.0f) return 0.0f;
        const int i = f < 0.0f ? 0 : (int) f;
        return lut[(size_t) i] + (f - (float) i) * (lut[(size_t) i + 1] - lut[(size_t) i]);
    }

    static float fastLog10 (float x) noexcept    // ≈ log10(x); |err| < 0.0014 log2 (≈0.008 dB)
    {
        int e;
        const float m  = std::frexp (x, &e);      // x = m·2^e, m in [0.5, 1)
        const float l2m = ((1.2313083f * m - 4.1179710f) * m + 6.0214606f) * m - 3.1338165f;
        return 0.30103f * ((float) e + l2m);
    }

    static float gamma06Lut (float v) noexcept   // ≈ pow(v, 0.6), v in 0..1
    {
        static const auto lut = []
        {
            std::array<float, 513> t{};
            for (int i = 0; i <= 512; ++i)
                t[(size_t) i] = std::pow ((float) i / 512.0f, 0.6f);
            return t;
        }();
        const float f = (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) * 512.0f;
        const int i = (int) f;
        if (i >= 512) return 1.0f;
        return lut[(size_t) i] + (f - (float) i) * (lut[(size_t) i + 1] - lut[(size_t) i]);
    }

    void ensureReassignSetup()
    {
        // ADAPTIVE analysis window: at high column rates (short windows, wide
        // module) a 2048-pt frame halves the FFT + splat cost per column, so the
        // zoomed view runs at maximum time resolution WITHOUT stuttering.
        // Reassignment keeps tones razor-thin either way — f̂ is exact for stable
        // partials regardless of the raw bin width.
        const int wantOrder = (columnsPerSec > 360) ? (kRaOrder - 1) : kRaOrder;

        if (raOrder == wantOrder && (int) raWin.size() == raSize && ! pend.empty())
            return;

        raOrder = wantOrder;
        raSize  = 1 << raOrder;
        raFft   = juce::dsp::FFT (raOrder);

        raWin .resize ((size_t) raSize);
        raDWin.resize ((size_t) raSize);
        raTWin.resize ((size_t) raSize);
        const float twoPiN  = juce::MathConstants<float>::twoPi / (float) (raSize - 1);
        const float centre  = 0.5f * (float) (raSize - 1);
        for (int i = 0; i < raSize; ++i)
        {
            raWin [(size_t) i] = 0.5f * (1.0f - std::cos (twoPiN * (float) i));   // Hann
            raDWin[(size_t) i] = 0.5f * twoPiN * std::sin (twoPiN * (float) i);   // dHann/dn
            raTWin[(size_t) i] = ((float) i - centre) * raWin[(size_t) i];        // t·Hann (samples, centred)
        }

        anaRing.assign ((size_t) raSize, 0.0f);
        anaWrite = 0; anaTotal = 0; hopFill = 0;
        pend.assign ((size_t) kPendLen * (size_t) kRows, 0.0f);
        frameIdx = 0;
    }

    float* pendCol (std::int64_t c)
    {
        const auto slot = (size_t) (((c % kPendLen) + kPendLen) % kPendLen);
        return pend.data() + slot * (size_t) kRows;
    }

    // Pull ALL new samples since the last frame and run an STFT frame per hop.
    int pumpReassign (double dt)
    {
        ensureReassignSetup();

        if (! audioSource.getMonoStream (streamPos, streamBuf))
            return pumpReassignSnapshot (dt);   // source can't stream → legacy fallback

        int wrote = 0, frameBudget = 160;       // CPU guard when catching up a stall
        for (float s : streamBuf)
        {
            anaRing[(size_t) anaWrite] = s;
            anaWrite = (anaWrite + 1) % raSize;
            ++anaTotal;

            if (++hopFill >= hopSamples)
            {
                hopFill -= hopSamples;
                if (anaTotal >= (std::uint64_t) raSize && frameBudget-- > 0)
                    wrote += processReassignFrame();
            }
        }
        return wrote;
    }

    // One analysis frame: 3 FFTs → reassigned energy splatted into the pending
    // column accumulators → flush the column that can no longer change.
    int processReassignFrame()
    {
        const double sr = juce::jmax (8000.0, audioSource.getSampleRate());
        const int    nb = raSize / 2;

        // TIME reassignment only pays off when columns are COARSE (it moves energy
        // between columns). Above ~600 col/s a column spans < 2 ms — the correction
        // is sub-visible, but its third FFT is a third of the analysis budget, so
        // skip it there (this is what keeps a 1 s window smooth at full width).
        const bool useTime = columnsPerSec <= 600;

        raWorkA.assign ((size_t) (2 * raSize), 0.0f);   // Hann-windowed
        raWorkB.assign ((size_t) (2 * raSize), 0.0f);   // derivative-windowed  → frequency correction
        if (useTime)
            raWorkT.assign ((size_t) (2 * raSize), 0.0f);   // time-weighted → time correction
        for (int i = 0; i < raSize; ++i)
        {
            const float s = anaRing[(size_t) ((anaWrite + i) % raSize)];   // oldest → newest
            raWorkA[(size_t) i] = s * raWin [(size_t) i];
            raWorkB[(size_t) i] = s * raDWin[(size_t) i];
            if (useTime)
                raWorkT[(size_t) i] = s * raTWin[(size_t) i];
        }
        raFft.performRealOnlyForwardTransform (raWorkA.data());
        raFft.performRealOnlyForwardTransform (raWorkB.data());
        if (useTime)
            raFft.performRealOnlyForwardTransform (raWorkT.data());

        const float norm    = 4.0f / (float) raSize;
        const float fScale  = (float) (sr / (double) juce::MathConstants<float>::twoPi);
        const float logLo   = std::log (kFreqMin), logHi = std::log (kFreqMax);
        const float binHz   = (float) (sr / (double) raSize);
        const float colSpan = (float) (sr / (double) columnsPerSec);   // samples per column

        // row splat: small Gaussian → smooth sub-pixel lines; column splat: linear
        // (sigma scales with kRows so the line keeps the same on-screen thickness)
        const float sigma = 0.7f * (float) kRows / 1024.0f, invS2 = 1.0f / (2.0f * sigma * sigma);
        const int   splatR = juce::jmax (2, (int) std::ceil (2.5f * sigma));
        const float maxFDev = 3.5f * binHz;              // incoherent (noise) bins scatter → reject
        const float maxTDev = 0.5f * (float) raSize;     // beyond the window half → junk

        for (int k = 1; k < nb; ++k)
        {
            const float hr = raWorkA[(size_t) (2 * k)], hi = raWorkA[(size_t) (2 * k + 1)];
            const float mag2 = hr * hr + hi * hi;
            if (mag2 < 1.0e-11f) continue;               // low threshold → faint structure shows

            // f̂ = f_k − (fs/2π)·Im(Xdh·conj(Xh))/|Xh|²   (exact for a pure tone)
            const float dr   = raWorkB[(size_t) (2 * k)], di = raWorkB[(size_t) (2 * k + 1)];
            const float imv  = (di * hr - dr * hi) / mag2;
            const float fDev = -fScale * imv;
            if (std::abs (fDev) > maxFDev) continue;     // no stable frequency locus
            const float fhat = (float) k * binHz + fDev;
            if (fhat < kFreqMin || fhat > kFreqMax) continue;

            // t̂ − t_centre = Re(Xth·conj(Xh))/|Xh|²  (samples; exact for an impulse)
            float tDev = 0.0f;
            if (useTime)
            {
                const float tr = raWorkT[(size_t) (2 * k)], ti = raWorkT[(size_t) (2 * k + 1)];
                tDev = (tr * hr + ti * hi) / mag2;
                if (std::abs (tDev) > maxTDev) continue;
            }

            const float mag = std::sqrt (mag2) * norm;

            // target: row (log-frequency) × column (reassigned time)
            const float t  = (std::log (fhat) - logLo) / (logHi - logLo);
            const float rf = (mirrored ? t : (1.0f - t)) * (float) (kRows - 1);
            const float cf = juce::jlimit (-(float) kPendR, (float) kPendR - 1.001f,
                                           tDev / colSpan);

            const int   c0  = (int) std::floor (cf);
            const float cw1 = cf - (float) c0;           // weight of column c0+1
            const float cw0 = 1.0f - cw1;
            const int   r0  = (int) std::floor (rf);

            for (int dc = 0; dc <= 1; ++dc)
            {
                const float cw = (dc == 0 ? cw0 : cw1);
                if (cw < 0.001f) continue;
                float* col = pendCol (frameIdx + (std::int64_t) (c0 + dc));
                for (int r = r0 - splatR; r <= r0 + splatR + 1; ++r)
                {
                    if (r < 0 || r >= kRows) continue;
                    const float dz = (float) r - rf;
                    col[r] += mag * cw * expNegLut ((dz * dz) * invS2);
                }
            }
        }

        // column (frameIdx − kPendR) can no longer receive energy → display it
        int flushed = 0;
        const std::int64_t done = frameIdx - (std::int64_t) kPendR;
        if (done >= 0) { flushPendingColumn (done); flushed = 1; }
        ++frameIdx;
        return flushed;
    }

    // linear energy → dB → temporal smoothing → gamma; then commit to the history.
    void flushPendingColumn (std::int64_t c)
    {
        float* col = pendCol (c);
        for (int row = 0; row < kRows; ++row)
        {
            float dB = 20.0f * fastLog10 (col[row] + 1.0e-9f);
            dB = juce::jlimit (-100.0f, 0.0f, dB);
            auto& st = rowState[(size_t) row];
            st = effSmooth * st + (1.0f - effSmooth) * ((dB + 100.0f) / 100.0f);
            colShaped[(size_t) row] = gamma06Lut (st);
            col[row] = 0.0f;   // clear the slot for reuse
        }
        publishColumn();
    }

    // Fallback when the source has no sample stream: snapshot reassignment
    // (frequency only), wall-clock column accounting — the old behaviour.
    int pumpReassignSnapshot (double dt)
    {
        colAcc += dt * (double) columnsPerSec;
        colAcc = juce::jmin (colAcc, 256.0);
        const int n = juce::jmin ((int) colAcc, 32);
        if (n <= 0) return 0;

        // Debit only on success — see the note in renderHeadless.
        if (! computeSnapshotReassignColumn()) return 0;
        colAcc -= (double) n;
        for (int i = 0; i < n; ++i) publishColumn();
        return n;
    }

    bool computeSnapshotReassignColumn()
    {
        ensureReassignSetup();

        std::vector<float> wL, wR;
        const int n = audioSource.getLastWaveform (wL, wR);
        if (n < raSize) return false;

        const double sr  = juce::jmax (8000.0, audioSource.getSampleRate());
        const int    off = n - raSize;                     // most-recent raSize samples
        raWorkA.assign ((size_t) (2 * raSize), 0.0f);
        raWorkB.assign ((size_t) (2 * raSize), 0.0f);
        for (int i = 0; i < raSize; ++i)
        {
            const float s = 0.5f * (wL[(size_t) (off + i)] + wR[(size_t) (off + i)]);
            raWorkA[(size_t) i] = s * raWin [(size_t) i];
            raWorkB[(size_t) i] = s * raDWin[(size_t) i];
        }
        raFft.performRealOnlyForwardTransform (raWorkA.data());
        raFft.performRealOnlyForwardTransform (raWorkB.data());

        std::vector<float> acc ((size_t) kRows, 0.0f);
        const int   nb     = raSize / 2;
        const float norm   = 4.0f / (float) raSize;
        const float fScale = (float) (sr / (double) juce::MathConstants<float>::twoPi);
        const float logLo  = std::log (kFreqMin), logHi = std::log (kFreqMax);
        const float sigma  = 0.7f * (float) kRows / 1024.0f, invS2 = 1.0f / (2.0f * sigma * sigma);
        const int   splatR = juce::jmax (2, (int) std::ceil (2.5f * sigma));

        for (int k = 1; k < nb; ++k)
        {
            const float hr = raWorkA[(size_t) (2 * k)], hi = raWorkA[(size_t) (2 * k + 1)];
            const float dr = raWorkB[(size_t) (2 * k)], di = raWorkB[(size_t) (2 * k + 1)];
            const float mag2 = hr * hr + hi * hi;
            if (mag2 < 1.0e-11f) continue;
            const float mag = std::sqrt (mag2) * norm;

            const float imv  = (di * hr - dr * hi) / mag2;
            const float fk   = (float) ((double) k * sr / (double) raSize);
            const float fhat = fk - fScale * imv;
            if (fhat < kFreqMin || fhat > kFreqMax) continue;

            const float t  = (std::log (fhat) - logLo) / (logHi - logLo);
            const float rf = (mirrored ? t : (1.0f - t)) * (float) (kRows - 1);
            const int   r0 = (int) std::floor (rf);
            for (int r = r0 - splatR; r <= r0 + splatR + 1; ++r)
            {
                if (r < 0 || r >= kRows) continue;
                const float dz = (float) r - rf;
                acc[(size_t) r] += mag * expNegLut ((dz * dz) * invS2);
            }
        }

        for (int row = 0; row < kRows; ++row)
        {
            float dB = 20.0f * std::log10 (acc[(size_t) row] + 1.0e-9f);
            dB = juce::jlimit (-100.0f, 0.0f, dB);
            auto& st = rowState[(size_t) row];
            st = effSmooth * st + (1.0f - effSmooth) * ((dB + 100.0f) / 100.0f);
            colShaped[(size_t) row] = gamma06Lut (st);
        }
        return true;
    }

    // Apply the row-fill (vertical line thickness) and write the finished column
    // into the history at writeCol. Caller holds histLock (publishColumn).
    void commitColumn()
    {
        const float* src = colShaped.data();
        if (lineFill > 0.001f)
        {
            std::copy (colShaped.begin(), colShaped.end(), rowFillTmp.begin());
            const int R = 1 + (int) std::round (lineFill * (float) kRows * 0.05f);
            // stride the taper for large R: visually identical (the weights change
            // slowly) but keeps the cost bounded at high row counts × column rates
            const int step = 1 + R / 24;
            for (int r = 0; r < kRows; ++r)
            {
                float m = rowFillTmp[(size_t) r];
                for (int j = 1; j <= R; j += step)
                {
                    const float w = lineFill * (1.0f - (float) j / (float) (R + 1));
                    if (r - j >= 0)    m = juce::jmax (m, rowFillTmp[(size_t) (r - j)] * w);
                    if (r + j < kRows) m = juce::jmax (m, rowFillTmp[(size_t) (r + j)] * w);
                }
                colCommit[(size_t) r] = m;
            }
            src = colCommit.data();
        }

        // ONE BYTE PER ROW. The colour lookup now happens in the shader, so this
        // is a straight intensity write — a third of the memory traffic the old
        // RGB blit needed, with no per-column BitmapData object in the way.
        juce::uint8* p = shadow.data() + (size_t) writeCol * (size_t) kRows;
        for (int row = 0; row < kRows; ++row)
            p[row] = (juce::uint8) juce::jlimit (0, kLutSize - 1,
                                                 (int) (src[row] * (float) (kLutSize - 1)));
    }

    // ── Overlay grid ─────────────────────────────────────────────────────────
    void drawFrequencyGrid (juce::Graphics& g, juce::Rectangle<int> area)
    {
        // same marks as Spectrum so both modules line up next to each other
        static const float marks[] = { 50.0f, 100.0f, 200.0f, 500.0f,
                                       1000.0f, 2000.0f, 5000.0f, 10000.0f };

        const float logMin = std::log10 (kFreqMin), logMax = std::log10 (kFreqMax);
        g.setFont (juce::Font (juce::FontOptions (9.0f)));

        for (float f : marks)
        {
            float t = (std::log10 (f) - logMin) / (logMax - logMin);
            if (mirrored) t = 1.0f - t;
            const int y = area.getY() + (int) ((1.0f - t) * (float) area.getHeight());

            g.setColour (AlterTheme::iceBlue.withAlpha (0.10f));
            g.drawHorizontalLine (y, (float) area.getX(), (float) area.getRight());

            g.setColour (AlterTheme::iceBlue.withAlpha (0.45f));
            g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k"
                                     : juce::String ((int) f),
                        area.getX() + 3, y - 11, 34, 10,
                        juce::Justification::centredLeft, false);
        }
    }

    // ── Members ──────────────────────────────────────────────────────────────
    static constexpr float kFreqMin = 16.0f;     // matches Spectrum exactly (down to sub-bass)
    static constexpr float kFreqMax = 20000.0f;
    static constexpr int   kLutSize = 256;

    IAudioSource& audioSource;

    // ── History: a circular byte buffer, one intensity per row, columns laid out
    //    contiguously (shadow[col * kRows + row]). This is BOTH the source the GL
    //    thread uploads from AND the copy that survives a context loss or a
    //    resize, which is why it is kept on the CPU side at all.
    juce::CriticalSection    histLock;
    std::vector<juce::uint8> shadow;
    int  bufferCols  { 0 };         // current capacity = window length in columns
    int  filled      { 0 };         // valid columns written so far (≤ bufferCols)
    int  writeCol    { 0 };         // next column to write (circular within bufferCols)
    double colsTotal { 0.0 };       // columns ever written (scroll position source)
    int  historyGen  { 0 };         // bumped on any relayout → GL rebuilds its texture
    std::atomic<bool> hasData { false };

    std::vector<float> fft;
    std::vector<float> rowFreqLo, rowFreqHi;
    std::vector<float> rowState;
    std::vector<float> colShaped, rowFillTmp;
    std::vector<float> colPrev, colTarget;   // linear-path anti-block glide endpoints
    std::vector<float> colCommit = std::vector<float> ((size_t) kRows, 0.0f);   // row-fill scratch

    std::array<juce::uint8, kLutSize * 3> lutRGB {};   // colour map, uploaded as a 256x1 texture
    std::atomic<bool> lutTexDirty { true };

    // message-thread → worker requests
    std::atomic<bool> layoutDirty        { false };
    std::atomic<bool> axisDirty          { false };
    // TRUE at construction: lutRGB starts as all zeros, i.e. a completely black
    // colour map, so the LUT must be built before the first frame is presented.
    // This used to happen by accident — setColourMode() unconditionally dirtied the
    // LUT and the settings apply calls it on every panel refresh — but it now
    // early-returns when the mode has not changed, and lutTheme's initial value can
    // legitimately match the current theme generation, so nothing else would have
    // triggered the first build. Priming it here makes the invariant explicit
    // instead of depending on an unrelated setter being called.
    std::atomic<bool> lutDirty           { true };
    std::atomic<bool> streamResetPending { false };

    float smoothing { 0.3f };
    float effSmooth { 0.3f };   // rate-compensated EMA coefficient (same time constant at any col rate)
    float lineFill  { 0.0f };   // low-band line thickness: 0 = thin line, 1 = filled row
    bool  constantQ { false };  // true multi-resolution CQT stream (vs linear FFT)
    bool  reassign  { false };  // enhanced-frequency (spectral reassignment) mode
    int   columnsPerSec { 60 }; // adaptive: ≈1 column per screen pixel (updateDensity)
    int   hopSamples    { 800 };
    double colAcc     { 0.0 };  // fractional column accumulator (legacy modes)
    double lastTickMs { 0.0 };
    juce::uint32 lastResizeMs  { 0 };   // buffer-resize throttle (window drag)
    int          pendingRate   { 0 };   // debounced column-rate target
    juce::uint32 pendingRateMs { 0 };

    std::vector<float> cqtDispSg;   // CQT frame-glide buffer (anti-block smoothing)

    // gap-free stream consumption + hop-driven analysis window
    std::uint64_t      streamPos { 0 };
    std::vector<float> streamBuf;
    std::vector<float> anaRing;
    int                anaWrite { 0 };
    std::uint64_t      anaTotal { 0 };
    int                hopFill  { 0 };

    // time-reassignment pending columns (linear-energy accumulators)
    std::vector<float> pend;                 // kPendLen × kRows
    std::int64_t       frameIdx { 0 };

    juce::dsp::FFT     raFft { kRaOrder };   // reassignment FFT (rebuilt on raOrder change)
    int                raOrder { kRaOrder }; // ADAPTIVE: 2048-pt above 360 col/s (see
    int                raSize  { kRaSize };  // ensureReassignSetup), 4096-pt otherwise
    std::vector<float> raWin, raDWin, raTWin;            // Hann, dHann/dt, t·Hann
    std::vector<float> raWorkA, raWorkB, raWorkT;        // FFT work buffers
    std::vector<float> cqtBuf;  // latest CQT magnitudes (log-spaced)
    float windowSec { 30.0f };
    bool  mirrored  { false };
    int   lutTheme  { 0 };
    int   colourMode { 0 };                              // 0=theme, 1=custom, 2=by tone
    juce::Colour customColour  { AlterTheme::pictonBlue };
    juce::Colour customColour2 { 0xFFF4FFFB };           // loudest intensities

    // Colour by tone. The tracker is only ever touched from applyPendingConfig
    // (the worker); lutToneHue is the hue the current LUT was baked at, so a held
    // note rebuilds nothing.
    bool  toneTwist  { false };
    float toneSmooth { 0.5f };    // the SLIDER value; the rate curve lives in PitchUtils
    float lutToneHue { -1.0f };
    PitchUtils::ToneHueTracker toneHue;

    // ── GPU presentation ─────────────────────────────────────────────────────
    AlterGLHost* currentHost = nullptr;      // message thread
    bool         isOffscreen = false;        // detached window → rendered via FBO image
    bool         forceOffscreen = false;     // hosted inside Fusion → always via FBO image
    mutable juce::CriticalSection stateLock;
    SgShaderState sgState;

    mutable juce::CriticalSection imgLock;
    juce::Image  offscreenImg;               // last frame rendered for a detached window
    juce::uint32 offscreenGen = 0;   // ditto; see readOffscreenFrame
    juce::int64  overlaySig = -1;            // change detector for the static overlay

    // GL-thread-only state
    unsigned int histTex { 0 }, lutTex { 0 };
    int          texCols { -1 }, texGen { -1 };
    double       glUploadedCols { 0.0 };     // colsTotal already in the texture
    double       glDispCols     { 0.0 };     // glided scroll position
    double       glColsTotal    { 0.0 };
    double       glLastMs       { 0.0 };
    int          glBufCols { 0 }, glFilled { 0 }, glWriteCol { 0 }, glColRate { 60 };
    float        glWindowSec { 30.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramMeter)
};
