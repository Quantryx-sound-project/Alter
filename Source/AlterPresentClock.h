/*
  ==============================================================================

    AlterPresentClock.h

    THE presentation clock — the one the eye actually sees.

    AlterGLHost attaches an OpenGLContext with setContinuousRepainting(true), so
    the GL thread already runs one frame per buffer swap, vsync-paced by the
    driver. That swap is the only clock in the app that corresponds to a real
    photon leaving the panel. Everything that animates must be phase-locked to
    it, or it will beat against it.

    That beat is subtle and worth spelling out, because it survives every fix
    applied upstream of it. Suppose a worker renders a perfectly even 60 fps and
    the display swaps a perfectly even 60 Hz, but the two are not phase-locked.
    Their phases drift; whenever they cross, one swap shows the previous frame
    again and the next skips one. On a slow visual that is invisible. On a
    spectrogram at WINDOW 1 s — where the image travels a full module width per
    second, ~24 px per frame — it is a 24 px jerk, several times a second. No
    amount of smoothing inside the module can remove it: the module's own frames
    were fine, the sampling of them was not.

    So: the GL thread ticks this clock once per swap, and every render worker
    takes its release signal AND its time base from here. Producer and consumer
    then share one phase by construction.

    (A VBlankAttachment is NOT equivalent. It fires on the display's vblank,
    which is a different event from the GL context's swap completing, and adding
    it alongside continuous repainting just creates a second, competing pacer.)

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <vector>
#include <algorithm>

/** Implemented by anything that wants to be driven by the presentation clock. */
struct AlterFrameSink
{
    virtual ~AlterFrameSink() = default;

    /** Called on the GL THREAD once per swap. Must be cheap and non-blocking —
        signal an event and return; never render here. `tSec` is the timestamp of
        the swap that just began, i.e. the correct time base for the frame the
        sink is about to produce. */
    virtual void onPresentTick (double tSec) = 0;
};

class AlterPresentClock
{
public:
    static AlterPresentClock& get()
    {
        static AlterPresentClock instance;
        return instance;
    }

    void addSink (AlterFrameSink* s)
    {
        if (s == nullptr) return;
        const juce::ScopedLock sl (lock);
        if (std::find (sinks.begin(), sinks.end(), s) == sinks.end())
            sinks.push_back (s);
    }

    /** Blocks until any in-flight tick() finishes, so a sink can never be
        destroyed while the GL thread is calling into it. */
    void removeSink (AlterFrameSink* s)
    {
        const juce::ScopedLock sl (lock);
        sinks.erase (std::remove (sinks.begin(), sinks.end(), s), sinks.end());
    }

    /** GL thread, once per swap. Call at the TOP of renderOpenGL(): the workers
        then have a whole frame period to prepare the next image while this frame
        is still being composited. */
    void tick()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();

        if (lastMs > 0.0)
        {
            const double d = now - lastMs;
            // Only accept intervals that could plausibly be a DISPLAY refresh.
            //
            // The upper bound ignores hitches and mode changes, as before. The lower
            // bound is the important one: below about 1.5 ms (660 Hz) the swaps are
            // not being paced by anything — vsync forced off in the driver, or the
            // brief free-run while a window and its context are torn down and
            // rebuilt. Feeding those into the average produces a "refresh rate" of
            // several hundred Hz, and everything downstream that divides by it then
            // computes nonsense. Holding the last good estimate instead is right:
            // either vsync comes back, or no sample is ever accepted, periodMs stays
            // zero, isLive() reports false and every worker falls back to its own
            // timer — which is exactly the correct behaviour when there is no
            // presentation clock to lock to.
            if (d > 1.5 && d < 200.0)
            {
                const double cur = periodMs.load();
                periodMs.store (cur <= 0.0 ? d : cur + (d - cur) * 0.10);
            }
        }
        lastMs = now;
        lastTickTick.store (juce::Time::getMillisecondCounter());

        const juce::ScopedLock sl (lock);
        for (auto* s : sinks)
            s->onPresentTick (now * 0.001);
    }

    /** Measured swap period in ms (0 until two swaps have happened). */
    double getPeriodMs() const noexcept { return periodMs.load(); }

    /** True while swaps are actually happening (no GL context / hidden window /
        detached secondary window all make this false, and callers fall back to
        their own timer). */
    bool isLive() const noexcept
    {
        return periodMs.load() > 0.0
            && juce::Time::getMillisecondCounter() - lastTickTick.load() < 250;
    }

private:
    AlterPresentClock() = default;

    juce::CriticalSection        lock;
    std::vector<AlterFrameSink*> sinks;
    std::atomic<double>          periodMs     { 0.0 };
    std::atomic<juce::uint32>    lastTickTick { 0 };
    double                       lastMs       { 0.0 };   // GL thread only

    JUCE_DECLARE_NON_COPYABLE (AlterPresentClock)
};
