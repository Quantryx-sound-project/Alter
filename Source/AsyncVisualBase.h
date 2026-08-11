/*
  ==============================================================================

    AsyncVisualBase.h

    Base class for visual modules whose per-frame drawing is expensive
    (Spectrum, Chladni, Stereoscope...). The heavy rasterisation runs on a
    dedicated WORKER thread into an off-screen image; the message thread only
    blits the finished frame in paint(). This keeps the message thread cheap
    (a memcpy-like image blit) and spreads the real work onto otherwise-idle
    CPU cores, so a tall HUD with many modules no longer serialises into stutter.

    Subclass contract:
      - implement renderImage(g, w, h)  -> runs on the WORKER thread.
          Draw the whole module opaquely into g; the target is w x h pixels.
          Reading from IAudioSource here is safe (those getters lock internally).
          Do NOT touch juce::Component state or the ValueTree from here.
      - call startAsyncRender() once the object is fully constructed.
      - simple scalar parameters written from the message thread (floats, ints,
        bools, enums, Colour) may be read directly by the worker: a torn read is
        at worst one slightly-stale frame. Anything that resizes a container must
        be done on the worker (drive it with an atomic "target" instead).

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cmath>
#include <functional>
#include "AlterTheme.h"
#include "AlterPresentClock.h"

// Build with -DALTER_VISUAL_DEBUG=1 to draw a per-module pacing overlay.
#ifndef ALTER_VISUAL_DEBUG
 #define ALTER_VISUAL_DEBUG 0
#endif

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

class AsyncVisualBase : public ThemedBackground,
                        public juce::Component,
                        private juce::Thread,
                        private juce::AsyncUpdater,
                        private AlterFrameSink
{
public:
    explicit AsyncVisualBase (const juce::String& threadName, int fps = 30)
        : juce::Thread (threadName)
    {
        targetFps.store (juce::jlimit (1, 240, fps));
        setOpaque (true);
    }

    ~AsyncVisualBase() override { stopAsyncRender(); }

protected:
    /** Heavy rendering. Runs on the WORKER thread. Fill the whole w x h area opaquely. */
    virtual void renderImage (juce::Graphics& g, int w, int h) = 0;

    /** Return false when the module's PIXELS are produced on the GPU (a shader in
        AlterGLHost draws straight from its own textures) rather than by
        renderImage. The worker then calls renderHeadless() in place of
        renderImage: it keeps the same present-clock phase lock, the same load
        governor and the same pacing, but allocates no back buffer, performs no
        image swap and posts no repaint — so the module's per-frame cost is its
        analysis alone, independent of how large it is on screen.
        The subclass is then responsible for its own paint() (typically just a 2D
        overlay on a transparent component). */
    virtual bool producesFrameImage() const { return true; }

    /** WORKER THREAD. Called instead of renderImage when producesFrameImage() is
        false. `w` and `h` are the component's current size in pixels, so
        resolution-dependent analysis decisions can still be made. */
    virtual void renderHeadless (int /*w*/, int /*h*/) {}

    /** Lower this worker's priority below the default.

        The default is high, and for the normal modules that is right: their frames
        are short and the raised priority is what keeps Windows from parking them
        on efficiency cores after ~30 s of inactivity.

        A module whose frame is LONG is a different case. A long frame at high
        priority does not merely render slowly, it holds a core against the message
        thread, and then the whole app stutters — not just that module. Such a
        module should say so, and take the throttling risk instead of taking the
        app down with it. Call before startAsyncRender(). */
    void setWorkerBelowNormalPriority (bool b) noexcept { modestWorker = b; }

    /** Start the worker. Call after the subclass is fully constructed. */
    void startAsyncRender()
    {
        // PHASE LOCK to the GL swap (AlterPresentClock): the worker is released
        // once per presented frame, so its rate is the refresh divided by an
        // integer and its phase is the display's.
        AlterPresentClock::get().addSink (this);

        if (! isThreadRunning())
            startThread (modestWorker ? juce::Thread::Priority::low
                                      : juce::Thread::Priority::high);
            // High priority: after ~30 s without user input Windows starts parking
            // the process onto slow/efficiency scheduling. The process-wide EcoQoS
            // opt-out in Main.cpp covers most of it; the raised priority keeps the
            // render workers off the throttled path on hybrid (P/E-core) CPUs too,
            // so the visuals never degrade while the app is left running idle.
    }

    /** Stop the worker. Called automatically from the destructor. */
    void stopAsyncRender()
    {
        // removeSink blocks until any in-flight tick() has finished.
        AlterPresentClock::get().removeSink (this);
        signalThreadShouldExit();
        wake.signal();
        stopThread (2000);
        cancelPendingUpdate();   // after the worker is guaranteed stopped (no more posts)
    }

    void setTargetFps (int fps) noexcept { targetFps.store (juce::jlimit (1, 240, fps)); }

    /** Never present more often than every Nth refresh. 1 = every refresh. */
    void setMinFrameDivisor (int n) noexcept { minFrameDivisor.store (juce::jlimit (1, 4, n)); }

    /** Time base for animation: the timestamp of the swap this frame is for. */
    double getPresentTimeSec() const noexcept
    {
        if (presentClockLive())
        {
            const double t = nextPresentSec.load();
            if (t > 0.0) return t;
        }
        return juce::Time::getMillisecondCounterHiRes() * 0.001;
    }

    /** Measured swap period in ms (0 when the presentation clock is not live). */
    double getRefreshPeriodMs() const noexcept
    {
        return AlterPresentClock::get().isLive() ? AlterPresentClock::get().getPeriodMs() : 0.0;
    }

   #if ALTER_VISUAL_DEBUG
    double statPaintFpsForDebug() const noexcept { return statPaintFps.load(); }

    void drawPacingStats (juce::Graphics& g, int w, int h,
                          const juce::StringArray& extraLines = {}) const
    {
        juce::StringArray lines;
        lines.add ("rnd " + juce::String (statRenderMs.load(), 1) + "ms"
                 + "  fps " + juce::String (statRenderFps.load(), 1)
                 + "  div " + juce::String (frameDivisor.load())
                 + (presentClockLive() ? "  swap " + juce::String (AlterPresentClock::get().getPeriodMs(), 2) + "ms"
                                       : juce::String ("  swap OFF")));
        lines.add ("paint " + juce::String (statPaintFps.load(), 1) + " fps"
                 + "  jitter " + juce::String (statPaintJitterMs.load(), 2) + "ms"
                 + "  worst " + juce::String (statPaintWorstMs.load(), 1) + "ms");
        lines.addArray (extraLines);

        const int lh = 13, pad = 5;
        auto box = juce::Rectangle<int> (pad, pad, juce::jmin (w - pad * 2, 430),
                                         lh * lines.size() + pad * 2);
        g.setColour (juce::Colours::black.withAlpha (0.78f));
        g.fillRect (box);
        g.setColour (juce::Colours::lime);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                  11.0f, juce::Font::plain)));
        for (int i = 0; i < lines.size(); ++i)
            g.drawText (lines[i], box.getX() + pad, box.getY() + pad + i * lh,
                        box.getWidth() - pad * 2, lh, juce::Justification::centredLeft, false);
    }
   #endif

    /** Cap the worker's render resolution to this many pixels (w*h). When the
        component is larger, the frame is rasterised at this budget (aspect
        preserved) and upscaled by the GPU compositor in paint(), so the CPU cost
        stays bounded no matter how large the module is dragged / fullscreened.
        Pass 0 to disable capping (always render 1:1 at the native size). */
    void setRenderPixelBudget (long long px) noexcept { renderPixelBudget.store (px); }

    /** Upscale the capped frame with nearest-neighbour (crisp, hard pixels) instead
        of smoothing. Use for pixel/particle visuals that should stay sharp when the
        module is enlarged to fullscreen. */
    void setCrispUpscale (bool b) noexcept { crispUpscale = b; }

    /** MESSAGE THREAD. Blit the last frame the worker finished.

        This is what paint() does, exposed for the subclasses that override paint()
        because their pixels normally come from the GL host — a module that can fall
        BACK to renderImage (see producesFrameImage) needs to present those frames
        itself, and cannot reach the base class's paint() through an override. */
    void paintLastRenderedFrame (juce::Graphics& g) { blitFrame (g); }

public:
    /** ANY THREAD. Runs `fn` on the last finished frame while holding the frame
        lock, so the worker cannot swap the buffer away mid-read.

        This exists for Fusion, which uploads a layer's frame into a GPU texture
        from the GL thread. Handing out the juce::Image instead would be a race:
        it is reference counted, so the caller would keep the pixels alive, but the
        worker would go on rendering INTO them and the upload would tear.

        `generation` counts finished frames. A caller that caches the result — an
        upload is exactly that — passes back what it saw last time and gets false
        when nothing new has been produced, which is what stops a module running at
        30 fps from being uploaded 60 times a second.

        Keep `fn` short: the worker's next swap waits on it. */
    bool readFrontFrame (juce::uint32& generation,
                         const std::function<void (const juce::Image&)>& fn)
    {
        const juce::ScopedLock sl (imgLock);

        if (! haveFrame || ! frontImg.isValid() || frameGeneration == generation)
            return false;

        generation = frameGeneration;
        fn (frontImg);
        return true;
    }

protected:

private:
    bool presentClockLive() const noexcept { return AlterPresentClock::get().isLive(); }

    /** GL thread, once per swap. Releases one worker frame every Nth swap. */
    void onPresentTick (double tSec) override
    {
        const int div = juce::jmax (1, frameDivisor.load());
        if (++presentTickCount >= div)
        {
            presentTickCount = 0;
            nextPresentSec.store (tSec);
            wake.signal();
        }
    }

    void run() override
    {
       #if JUCE_WINDOWS && defined (THREAD_POWER_THROTTLING_EXECUTION_SPEED)
        // Per-THREAD EcoQoS opt-out (belt-and-braces on top of the process-wide
        // opt-out in Main.cpp): without it Windows may still demote individual
        // background threads to efficiency mode after ~30 s of user inactivity,
        // which inflates render times → the load governor drops the fps → the
        // scrolling visuals (spectrogram!) turn visibly choppy until the mouse
        // moves again.
        THREAD_POWER_THROTTLING_STATE ts {};
        ts.Version     = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        ts.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        ts.StateMask   = 0;   // 0 = never throttle this thread
        SetThreadInformation (GetCurrentThread(), ThreadPowerThrottling, &ts, sizeof (ts));
       #endif

       #if JUCE_WINDOWS
        // HIGH-RESOLUTION frame pacing: after ~30 s without user input Windows
        // stops honouring fine timer resolution for the process, and a plain
        // event wait(16 ms) silently stretches to ~30+ ms — the worker then runs
        // at half its fps and the module visibly stutters until the mouse moves.
        // A high-resolution waitable timer (Win10 1803+) fires with ~0.5 ms
        // precision REGARDLESS of that throttling (the same technique browsers
        // and game engines use). Falls back to the plain wait when unavailable.
       #ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
       #endif
        HANDLE hrTimer = CreateWaitableTimerExW (nullptr, nullptr,
                                                 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                 TIMER_ALL_ACCESS);
       #endif

        double adaptFps    = (double) juce::jmax (1, targetFps.load());   // load-governed local rate
        double renderMsEma = 0.0;                                 // smoothed per-frame cost

        while (! threadShouldExit())
        {
            const double tStart = juce::Time::getMillisecondCounterHiRes();

            // HUD "Hold": stop producing frames; the last rendered image stays on
            // screen (paint() keeps blitting frontImg) until Hold is released.
            if (AlterTheme::hudFrozen.load())
            {
                wake.wait (juce::jlimit (16, 100, 1000 / juce::jmax (1, targetFps.load())));
                continue;
            }

            // Read the component size directly. These are plain int reads; a torn
            // read during a resize at worst yields one wrong-sized frame (paint()
            // then stretches the last good frame), which self-corrects next tick.
            const int w = getWidth();
            const int h = getHeight();

            if (w >= 2 && h >= 2 && ! producesFrameImage())
            {
                // GPU-presented module: run the analysis only. No back buffer, no
                // image swap, no repaint post — the GL host draws the pixels.
                try                { renderHeadless (w, h); }
                catch (...)        { jassertfalse; }
            }
            else if (w >= 2 && h >= 2)
            {
                // Bound the render resolution: large modules render at the capped
                // resolution and are upscaled on present, keeping per-frame CPU work
                // (background fills, particle/persistence buffers, path strokes)
                // constant so the visualisation stays smooth when enlarged.
                int rw = w, rh = h;
                const long long budget = renderPixelBudget.load();
                const long long area   = (long long) w * (long long) h;
                if (budget > 0 && area > budget)
                {
                    const double s = std::sqrt ((double) budget / (double) area);
                    rw = juce::jmax (2, (int) std::lround ((double) w * s));
                    rh = juce::jmax (2, (int) std::lround ((double) h * s));
                }

                if (! backImg.isValid() || backImg.getWidth() != rw || backImg.getHeight() != rh)
                    backImg = juce::Image (juce::Image::ARGB, rw, rh, false, juce::SoftwareImageType());

                // The back buffer is REUSED — it is swapped with the front one, never
                // reallocated — so it still holds the frame from two presents ago.
                // That is invisible while the module paints its own opaque
                // background over the whole rectangle, and it is exactly what would
                // show through once it stops. Clear it first when the module is
                // drawing on transparency for a Fusion.
                if (isTransparentBackground())
                    backImg.clear (backImg.getBounds(), juce::Colours::transparentBlack);

                // Defensive frame guard: one bad frame in one module must never take
                // the whole app down — drop the frame, keep the last good one, go on.
                bool frameOk = true;
                try
                {
                    juce::Graphics g (backImg);
                    renderImage (g, rw, rh);
                }
                catch (...)
                {
                    frameOk = false;
                    jassertfalse;   // debug: investigate; release: self-heal silently
                }

                if (threadShouldExit())
                    break;

                if (frameOk)
                {
                    {
                        const juce::ScopedLock sl (imgLock);
                        std::swap (frontImg, backImg);
                        haveFrame = true;
                        ++frameGeneration;
                    }
                    triggerAsyncUpdate();   // -> repaint() on the message thread
                }
            }

            // ── ADAPTIVE LOAD GOVERNOR (overload protection) ──────────────────
            // Track the real per-frame cost and CONVERGE on the rate whose frame
            // period the render fills to ~55 %. The old stepwise controller
            // (drop ×0.8 above 65 %, creep +2 below 30 %) had no stable point
            // under sustained load — it sawtoothed between rates, which read as
            // periodic stutter (worst on the Spectrum at high bin counts). The
            // proportional glide settles at one steady rate instead.
            const double renderMs = juce::Time::getMillisecondCounterHiRes() - tStart;
            renderMsEma += (renderMs - renderMsEma) * 0.2;

            if (lastRenderStartMs > 0.0)
            {
                const double d = tStart - lastRenderStartMs;
                if (d > 0.05 && d < 1000.0)
                {
                    renderIntervalEma += (d - renderIntervalEma) * 0.1;
                    statRenderFps.store (1000.0 / juce::jmax (0.001, renderIntervalEma));
                }
            }
            lastRenderStartMs = tStart;
            statRenderMs.store (renderMsEma);

            // ── VBLANK-LOCKED PACING (preferred) ──────────────────────────────
            // Rate is refresh/N with N an INTEGER, so the on-screen step is even.
            if (presentClockLive())
            {
                const double refreshMs = juce::jmax (1.0, AlterPresentClock::get().getPeriodMs());
                const int    div       = juce::jmax (1, frameDivisor.load());

                // CLAMPED to the divisor's own legal range, and that clamp is load
                // bearing. The ceil() term is "how many swaps per frame do I need
                // to not exceed targetFps", which is only sane while the swap rate
                // really is the display's. If vsync is not being honoured — forced
                // off in the driver, or a moment of free-running while a window and
                // its context are being recreated — the measured period collapses
                // to the 1 ms floor and the term explodes: at targetFps 60 it asks
                // for ceil(1000/60) = 17.
                //
                // Unclamped that produced BOTH of the symptoms it caused. In a debug
                // build jlimit(17, 4, d) trips jassert(lowerLimit <= upperLimit) and
                // breaks into the debugger. In a release build jlimit with inverted
                // bounds returns the lower one, so frameDivisor became 17 and every
                // module was released once per 17 swaps — about 3.5 fps, which reads
                // as the whole HUD freezing.
                const int minDiv = juce::jlimit (1, 4,
                                     juce::jmax (minFrameDivisor.load(),
                                                 (int) std::ceil ((1000.0 / refreshMs)
                                                                  / (double) juce::jmax (1, targetFps.load())
                                                                  - 1.0e-3)));

                // BOTH TESTS ASK ABOUT THE SAME QUANTITY: does one frame's work
                // fit in the slot a given divisor gives it? Step up when it
                // overran the slot it has; step down only when it would fit the
                // SMALLER slot with 15 % to spare.
                //
                // Asking the down-question about d - 1 rather than about d is the
                // whole correction, and without it the controller could not settle
                // on any module whose frame costs the same whatever rate it runs
                // at — which is most of them, and the Spectrum above all.
                //
                // The old rule compared duty at the CURRENT divisor against 0.85.
                // Duty is renderMs / (refresh * d), so the moment the divisor went
                // up the duty HALVED — from just over 1.0 to just over 0.5 — and
                // 0.5 is below 0.85, so the next decision stepped straight back
                // down, where the cost overran again. 60, 30, 60, 30, every 500 ms,
                // for as long as the load sat near the boundary. That is why it
                // took a while to appear: the render-time average has to converge
                // near a slot edge first, and then it never stops.
                //
                // The comment justifying it claimed duty was divisor-invariant
                // "because halving the rate doubles the columns analysed per
                // frame". True of the Spectrogram, which does more work per frame
                // when it runs less often. Not true of anything that simply draws
                // the picture it has — a Spectrum renders one spectrum whether it
                // is asked 60 times a second or 30.
                //
                // Written this way the two thresholds bracket one number, so the
                // measured cost has to genuinely move by 15 % to reverse a
                // decision. No load that is merely steady can make it oscillate.
                const auto nowTick = juce::Time::getMillisecondCounter();
                if (nowTick - lastDivChangeMs >= 500)
                {
                    const double cost = renderMsEma;
                    int d = div;

                    if      (cost > refreshMs * (double) d && d < 4)
                        ++d;
                    else if (d > minDiv && cost < refreshMs * (double) (d - 1) * 0.85)
                        --d;

                    d = juce::jlimit (minDiv, 4, d);
                    if (d != div) { frameDivisor.store (d); lastDivChangeMs = nowTick; }
                }
                else if (div < minDiv)
                {
                    frameDivisor.store (minDiv);
                }

                wake.wait (250);        // released by the swap; timeout is a safety net
                continue;
            }

            // ── FALLBACK: self-paced timer (no GL swap clock available) ───────
            const double tgt     = juce::jmin (60.0, (double) juce::jmax (1, targetFps.load()));
            // Floor 24 (12 read as visible stutter) and a ~70 % duty target: the
            // governor only backs off under sustained REAL overload. Asymmetric
            // glide — recover fast, drop slowly — so a transient spike (or a brief
            // OS power dip while the machine idles) can no longer drag the rate
            // down; the old symmetric 55 % controller amplified idle throttling
            // into a low-fps lock-in until the next user input.
            const double desired = juce::jlimit (24.0, tgt, 700.0 / juce::jmax (0.25, renderMsEma));
            adaptFps += (desired - adaptFps) * (desired < adaptFps ? 0.06 : 0.35);
            adaptFps  = juce::jmin (adaptFps, tgt);

            // Stable pacing: subtract the render time so the frame interval stays
            // even (a long render no longer adds on top of a full wait → no jitter).
            const double period  = 1000.0 / juce::jmax (1.0, adaptFps);
            const double elapsed = juce::Time::getMillisecondCounterHiRes() - tStart;
            const double waitMs  = juce::jmax (1.0, period - elapsed);

           #if JUCE_WINDOWS
            if (hrTimer != nullptr)
            {
                // precise pacing immune to idle timer-resolution throttling.
                // (Exit responsiveness: bounded by one frame period ≤ ~42 ms —
                // well within stopAsyncRender()'s 2 s stopThread window.)
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG) (waitMs * 10000.0);   // relative, 100 ns units
                if (SetWaitableTimer (hrTimer, &due, 0, nullptr, nullptr, FALSE))
                {
                    WaitForSingleObject (hrTimer, (DWORD) (waitMs + 100.0));
                    continue;
                }
            }
           #endif
            wake.wait ((int) waitMs);
        }

       #if JUCE_WINDOWS
        if (hrTimer != nullptr) CloseHandle (hrTimer);
       #endif
    }

    void handleAsyncUpdate() override { repaint(); }

    void paint (juce::Graphics& g) override
    {
        // PRESENTATION clock — the one the eye sees.
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (lastPaintMs > 0.0)
            {
                const double d = now - lastPaintMs;
                if (d > 0.05 && d < 1000.0)
                {
                    paintIntervalEma += (d - paintIntervalEma) * 0.1;
                    paintJitterEma   += (std::abs (d - paintIntervalEma) - paintJitterEma) * 0.1;
                    statPaintFps.store (1000.0 / juce::jmax (0.001, paintIntervalEma));
                    statPaintJitterMs.store (paintJitterEma);

                    const auto tick = juce::Time::getMillisecondCounter();
                    if (d > worstPaintMs) worstPaintMs = d;
                    if (tick - worstResetMs > 2000) { statPaintWorstMs.store (worstPaintMs);
                                                      worstPaintMs = 0.0; worstResetMs = tick; }
                }
            }
            lastPaintMs = now;
        }

        blitFrame (g);
    }

    void blitFrame (juce::Graphics& g)
    {
        const juce::ScopedLock sl (imgLock);

        if (haveFrame && frontImg.isValid())
        {
            if (frontImg.getWidth() == getWidth() && frontImg.getHeight() == getHeight())
                g.drawImageAt (frontImg, 0, 0);                       // exact: fast 1:1 blit
            else
            {
                // capped or mid-resize: upscale the finished frame (GPU-composited,
                // so this is cheap). Crisp modules use nearest-neighbour so pixels /
                // particles stay sharp; everything else uses smoothing.
                g.setImageResamplingQuality (crispUpscale ? juce::Graphics::lowResamplingQuality
                                                          : juce::Graphics::mediumResamplingQuality);
                g.drawImage (frontImg, getLocalBounds().toFloat());
            }
        }
        else
        {
            g.fillAll (juce::Colours::black);                         // very first frame only
        }
    }

    std::atomic<int>       targetFps          { 30 };
    std::atomic<long long> renderPixelBudget  { 1280LL * 1024LL };   // ~1.3 Mpx cap
    bool                   crispUpscale       { false };             // nearest-neighbour upscale
    bool                   modestWorker       { false };             // see setWorkerBelowNormalPriority

    // phase lock (see AlterPresentClock / onPresentTick)
    std::atomic<double>  nextPresentSec  { 0.0 };
    std::atomic<int>     frameDivisor    { 1 };
    std::atomic<int>     minFrameDivisor { 1 };
    int          presentTickCount { 0 };            // GL thread only
    juce::uint32 lastDivChangeMs  { 0 };            // worker thread only

    // pacing diagnostics
    std::atomic<double> statRenderMs      { 0.0 };
    std::atomic<double> statRenderFps     { 0.0 };
    std::atomic<double> statPaintFps      { 0.0 };
    std::atomic<double> statPaintJitterMs { 0.0 };
    std::atomic<double> statPaintWorstMs  { 0.0 };
    double       lastRenderStartMs  { 0.0 }, renderIntervalEma { 16.7 };   // worker only
    double       lastPaintMs        { 0.0 }, paintIntervalEma  { 16.7 };   // message only
    double       paintJitterEma     { 0.0 }, worstPaintMs      { 0.0 };
    juce::uint32 worstResetMs       { 0 };

    juce::CriticalSection imgLock;
    juce::Image frontImg, backImg;
    bool haveFrame = false;            // written/read only under imgLock
    juce::uint32 frameGeneration = 0;  // ditto; see readFrontFrame

    juce::WaitableEvent wake { false }; // auto-reset

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AsyncVisualBase)
};
