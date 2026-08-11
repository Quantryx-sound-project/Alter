/*
  ==============================================================================

    MacSystemAudioCapture.h
    System audio capture for macOS using ScreenCaptureKit (macOS 13+).

    Plain C++ interface (no Objective-C) so it can be included from normal .cpp
    translation units. The implementation lives in MacSystemAudioCapture.mm.

    Requirements:
      - macOS 13.0+
      - ScreenCaptureKit + CoreMedia frameworks linked (added in the .jucer)
      - "Screen Recording" permission (the first capture triggers the prompt)

  ==============================================================================
*/

#pragma once
#include <vector>
#include <memory>

class MacSystemAudioCapture
{
public:
    MacSystemAudioCapture();
    ~MacSystemAudioCapture();

    bool isActive() const;
    /** Actual capture rate: the Core Audio tap follows the output device
        (44.1/48/96 kHz...); the ScreenCaptureKit fallback is fixed at 48 kHz. */
    int  getSampleRate() const;

    /** Moves all newly-captured mono samples into `mono`; returns the count. */
    int getAudioData (std::vector<float>& mono);

    /** Moves all newly-captured INTERLEAVED stereo (L,R) samples; returns frame count. */
    int getStereoData (std::vector<float>& interleaved);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
