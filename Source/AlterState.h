#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <vector>
#include "AlterTheme.h"

// Ultra‑minimal AlterState: holý ValueTree + priame get/set metódy.
// (Žiadne defaulty v CTOR – nastavíš ich v Main.cpp pri štarte.)

class AlterState
{
public:
    AlterState() : state ("alter") {}

    juce::ValueTree&       getTree()       noexcept { return state; }
    const juce::ValueTree& getTree() const noexcept { return state; }

    // --- RMS ---
    bool  rmsOn()        const { return (bool)  state.getProperty ("rmsOn"); }
    void  setRmsOn (bool v)    { state.setProperty ("rmsOn", v, nullptr); }

    float rmsSmooth01()  const { return (float) state.getProperty ("rmsSmooth01"); }
    void  setRmsSmooth01 (float v){ state.setProperty ("rmsSmooth01", juce::jlimit (0.0f, 1.0f, v), nullptr); }

    // --- Spectrum ---
    bool  spectrumOn()   const { return (bool)  state.getProperty ("spectrumOn"); }
    void  setSpectrumOn (bool v){ state.setProperty ("spectrumOn", v, nullptr); }

    bool  specAWeight()  const { return (bool)  state.getProperty ("specAWeight"); }
    void  setSpecAWeight (bool v){ state.setProperty ("specAWeight", v, nullptr); }

    float specSmooth01() const { return (float) state.getProperty ("specSmooth01"); }
    void  setSpecSmooth01 (float v){ state.setProperty ("specSmooth01", juce::jlimit (0.0f, 1.0f, v), nullptr); }

    // scale je fixne "log" – property ponecháme len ak by si chcel do budúcna rozšíriť
    juce::String specScale() const { return state.getProperty ("specScale").toString(); } // očakávame "log"
    void setSpecScale (juce::String s) { state.setProperty ("specScale", s, nullptr); }

    int   specBins()     const { return (int)   state.getProperty ("specBins"); }          // očakávame 1024
    void  setSpecBins (int n)  { state.setProperty ("specBins", n, nullptr); }

    // --- Global FFT resolution (max bins the plugins compute/send) ---
    // 2048 (4096-pt) / 4096 (8192-pt) / 8192 (16384-pt). Default 2048 (current).
    int  maxFftBins()       const { return (int) state.getProperty ("maxFftBins", 2048); }
    void setMaxFftBins (int n)    { state.setProperty ("maxFftBins", n, nullptr); }
    // Parabolic sub-bin interpolation for tone estimation (global toggle).
    bool subBinInterp()     const { return (bool) state.getProperty ("subBinInterp", false); }
    void setSubBinInterp (bool v) { state.setProperty ("subBinInterp", v, nullptr); }
    // Global Constant-Q: when on, Spectrum/Spectrogram use the multi-resolution stream.
    bool globalCqt()        const { return (bool) state.getProperty ("globalCqt", false); }
    void setGlobalCqt (bool v)    { state.setProperty ("globalCqt", v, nullptr); }

    // bins → FFT order for the 'B' control packet (numBins = 2^(order-1)).
    static int fftOrderForBins (int bins)
    {
        bins = juce::jlimit (512, 8192, bins);
        int order = 10;                       // 512 bins
        while ((1 << (order - 1)) < bins && order < 14) ++order;
        return order;                         // 512→10, 1024→11, 2048→12, 4096→13, 8192→14
    }

    // --- Controller okno ---
    bool controllerAlwaysOnTop() const { return (bool) state.getProperty ("controllerAlwaysOnTop"); }
    void setControllerAlwaysOnTop (bool v){ state.setProperty ("controllerAlwaysOnTop", v, nullptr); }

    // controller color (ARGB stored as int)
    juce::Colour controllerColour() const
    {
        auto v = state.getProperty ("controllerColour");
        if (v.isVoid()) return juce::Colours::red;
        return juce::Colour ((uint32_t) (int) v);
    }

    void setControllerColour (juce::Colour c)
    {
        state.setProperty ("controllerColour", (int) c.getARGB(), nullptr);
    }

    // --- Bounds (String serializácia) ---
    void setWindowBounds (const juce::String& key, const juce::Rectangle<int>& r)
    {
        state.setProperty (key, r.toString(), nullptr);
    }

    juce::Rectangle<int> getWindowBounds (const juce::String& key, const juce::Rectangle<int>& fallback) const
    {
        auto s = state.getProperty (key).toString();
        if (s.isEmpty()) return fallback;

        // Parsujeme "x y w h"
        juce::StringArray parts;
        parts.addTokens (s, " ,\t", "");
        parts.removeEmptyStrings();
        if (parts.size() >= 4)
        {
            const int x = parts[0].getIntValue();
            const int y = parts[1].getIntValue();
            const int w = parts[2].getIntValue();
            const int h = parts[3].getIntValue();
            if (w > 0 && h > 0) return { x, y, w, h };
        }
        return fallback;
    }
    
    // =========================================================
       // NEW: Panel layout (Add/Delete, multiple instances)
       // =========================================================

       // názvy node-ov v ValueTree
       static inline const juce::Identifier kPanelsNode { "panels" };
       static inline const juce::Identifier kPanelNode  { "panel"  };

       // properties na paneli
       static inline const juce::Identifier kId      { "id" };
       static inline const juce::Identifier kType    { "type" };     // "rms", "spectrum", neskôr ďalšie
       static inline const juce::Identifier kSmooth  { "smooth01" };
       static inline const juce::Identifier kAWeight { "aWeight" };
       static inline const juce::Identifier kBins    { "bins" };
       static inline const juce::Identifier kRenderPoints { "renderPoints" };
       static inline const juce::Identifier kNeon { "neon" };
       static inline const juce::Identifier kDisplayMode { "displayMode" };
       static inline const juce::Identifier kDisplayPeriods { "displayPeriods" };

       static inline const juce::Identifier kSyncBPM { "syncBPM" };
       static inline const juce::Identifier kZoom { "zoom" };
       static inline const juce::Identifier kRotation { "rotation" };
       static inline const juce::Identifier kSymmetry { "symmetry" };
       static inline const juce::Identifier kSaturation { "saturation" };
       static inline const juce::Identifier kSynBrightness { "synBrightness" }; // synesthesia overall light (0..2, 1 = neutral)
       static inline const juce::Identifier kBloom { "bloom" };
       static inline const juce::Identifier kSpeed { "speed" };       // synesthesia shader evolution speed (0..4, 1=default)
       static inline const juce::Identifier kFragment { "fragment" };     // synesthesia shader variation/morph (0..1) — formerly "Change"
       static inline const juce::Identifier kTransmute { "transmute" }; // synesthesia second (symmetric) morph (0..1)
       static inline const juce::Identifier kMirror { "mirror" };     // synesthesia mirror fold on/off (reflective symmetry)
       static inline const juce::Identifier kClear { "clear" };       // synesthesia 'Clear' cleanup/merge top layer (0..1)
       static inline const juce::Identifier kDenoise { "denoise" };   // synesthesia 'Denoise' fuse dashed secondary curves (0..1)
       static inline const juce::Identifier kCurveSmooth { "curveSmooth" }; // synesthesia curve/ghost motion smoothing (0..1)
       static inline const juce::Identifier kSynTunnel   { "synTunnel" };   // synesthesia symmetric tunnel morph (0..1)
       static inline const juce::Identifier kSynVortex   { "synVortex" };   // synesthesia tunnel swirl/vortex (0..1)
       static inline const juce::Identifier kSynBpmSync  { "synBpmSync" };  // synesthesia BPM mode on/off
       static inline const juce::Identifier kSynBpm      { "synBpm" };      // synesthesia BPM tempo (evolution + light pulses)
       static inline const juce::Identifier kSynBeatDiv  { "synBeatDiv" };  // synesthesia beat-division index for the light pulse
       static inline const juce::Identifier kPeakHold { "peakHold" }; // spectrum peak-hold overlay on/off

       static inline const juce::Identifier kDetached { "detached" };
       static inline const juce::Identifier kWnd      { "wnd" };
       // Whole-row tear-off: every panel in a torn-off block gets kRowDetached=true.
       // kRowWnd stores the shared row-window bounds (duplicated across the row's panels).
       static inline const juce::Identifier kRowDetached { "rowDetached" };
       static inline const juce::Identifier kRowWnd      { "rowWnd" };
       static inline const juce::Identifier kColor    { "color" };
       static inline const juce::Identifier kColor2   { "color2" };   // secondary colour (spectrogram top intensities)
       static inline const juce::Identifier kSelected { "selected" };
       static inline const juce::Identifier kWidthRatio { "widthRatio" };  // Module width proportion (0.0-1.0)
       static inline const juce::Identifier kRotationAngle { "rotationAngle" };  // Module rotation: 0=0°, 1=90°, 2=180°, 3=270°
       static inline const juce::Identifier kMeterMode { "meterMode" };  // meter mode: 0=RMS, 1=True Peak, 2=LUFS, 3=Level history
       static inline const juce::Identifier kColorMode { "colorMode" };  // RMS color mode: 0=Standard, 1=Custom Gradient, 2=Custom Spectrum
       // 'Mirror tone color' — reverses the tone→hue wheel direction. Only meaningful
       // while tone colour is on. Synesthesia / Chladni / Geometry / Spectrum /
       // Spectrogram / Oscilloscope / Stereoscope.
       static inline const juce::Identifier kToneTwist { "toneTwist" };

       /** 'Tone smooth' 0..1 — how fast the colour chases a new note.

           A SEPARATE key from kSmooth on purpose. Synesthesia and Geometry bind
           'Tone smooth' straight to kSmooth because in those modules there is only
           one thing to smooth. The analyser modules already spend kSmooth on their
           own display smoothing (spectrum EMA, scope averaging, spectrogram temporal
           smoothing, stereoscope persistence) — sharing it would tie how fast the
           colour follows the music to how much the picture is averaged, which are
           unrelated choices the user makes for unrelated reasons. */
       static inline const juce::Identifier kToneSmooth { "toneSmooth" };
       static inline const juce::Identifier kCustomColorMode { "customColorMode" };  // RMS color mode (legacy): false=standard, true=custom
       static inline const juce::Identifier kPreferredWidth { "preferredWidth" };    // Fixed pixel width (0 = flexible, proportional)
       static inline const juce::Identifier kSystemGain { "systemGain" };              // System audio capture gain (0.0 = true, up to 10.0)

       // --- Spectrum extended ---
       static inline const juce::Identifier kMeasurementMode { "measurementMode" }; // 0=Visual, 1=Measurement (SPAN-like)
       static inline const juce::Identifier kPsychoCurve     { "psychoCurve" };     // 0=Flat, 1=A-weight, 2=ISO226
       static inline const juce::Identifier kPhon            { "phon" };            // ISO226 phon level (40/60/80)
       static inline const juce::Identifier kSpecReference   { "specReference" };   // 0=Off,1=EDM,2=Bass,3=House,4=HipHop,5=Pop,6=Rock
       static inline const juce::Identifier kSpecStereo      { "specStereo" };      // bool: overlaid L/R spectra

       /** SPECTRUM: mirror the frequency axis — high on the left, low on the
           right. It lives beside Rotation in the editor because it is the other
           half of the same question, which way round is this module; and it is
           the MODULE's property for the same reason rotation is — a Fusion reads
           it, it does not own it. */
       static inline const juce::Identifier kSpecMirror      { "specMirror" };      // bool: flip the Hz axis
       static inline const juce::Identifier kSpecLrColor     { "specLrColor" };     // 0=complementary, 1=analogous R colour

       // --- Spectrogram ---
       static inline const juce::Identifier kSpectroWindow   { "spectroWindow" };   // visible window, seconds 5-120
       static inline const juce::Identifier kSpectroMirror   { "spectroMirror" };   // bool: mirror frequency axis
       static inline const juce::Identifier kSpectroLineFill { "spectroLineFill" }; // 0=thin line .. 1=filled row
       static inline const juce::Identifier kConstantQ       { "constantQ" };        // spectrum/spectrogram: log-band aggregation
       static inline const juce::Identifier kSpectroReassign { "spectroReassign" };  // spectrogram: enhanced-frequency (reassignment) mode

       // --- AudioMeter Trend (waveform over time) ---
       static inline const juce::Identifier kMeasureState { "measureState" }; // 0=idle, 1=measuring
       static inline const juce::Identifier kMeterView    { "meterView" };    // 0=Momentary (bar), 1=Trend (waveform)

       /** AUDIO METER: colour by tone.

           A key of its OWN rather than the shared kColorMode, because on this one
           module kColorMode is already spent: it carries Standard / Custom gradient
           / Custom spectrum, which is not a colour SOURCE but how the loudness zones
           are shaded once a source is chosen. Those two choices are independent —
           tone colour answers "which colour", the shade answers "how do the zones
           differ from it" — so they need two keys. Everywhere else kColorMode is
           free and the shared checkbox writes it directly. */
       static inline const juce::Identifier kMeterToneColor { "meterToneColor" };  // bool: bar/trend colour follows the tone

       /** AUDIO METER: how the loudness zones step away from the tone colour.
           0 = Gradient (same hue, stepped down in brightness), 1 = Complementary
           (the hot zones walk toward the far side of the wheel). The SAME two
           choices kColorMode offers for a picked colour, because it is the same
           question asked of a different base — only read while kMeterToneColor is
           on; in manual colour kColorMode decides. */
       static inline const juce::Identifier kMeterToneShade { "meterToneShade" };

       /** AUDIO METER: draw the peak/max-hold as a SECOND bar beside the smoothed
           one. On by default — a meter that shows only a smoothed level hides the
           transient that actually clipped, and the hold marker line alone is easy
           to lose against the fill. Unticking returns the single-bar meter. */
       static inline const juce::Identifier kMeterTwoBars { "meterTwoBars" };

       /** LEVEL HISTORY / OSCILLOSCOPE: the 0 dBFS boundary, the -6 / -12 / -20 dB
           ruler down the right edge, and a mark over every column that reached full
           scale. On by default; the checkbox restores the bare waveform. Shared by
           both modules — same annotation over the same kind of picture.

           BOTH VIEWS CLAMP WHAT THEY DRAW TO ±1.0, ON PURPOSE. That is what a DAW
           does with a clip overview, and the flat plateau it produces is how an
           engineer reads "this is slammed" at a glance. Letting the trace run past
           the boundary would buy one number — HOW far over — at the cost of shrinking
           every waveform to leave room for the overshoot, and that number belongs to
           True Peak metering (dBTP, inter-sample, on the Audio Meter) rather than to
           a waveform view. So the clamp stays and the over-scale columns are MARKED
           instead: same information, no cost to the picture. */
       static inline const juce::Identifier kClipZone { "clipZone" };

       /** OSCILLOSCOPE: R-channel colour in the overlaid Stereo display —
           0 = complementary, 1 = analogous. Same meaning and same two options as
           Spectrum's kSpecLrColor; a separate key only so the two modules can be
           set differently on the same HUD. */
       static inline const juce::Identifier kOscLrColor { "oscLrColor" };

       // --- Chladni Pattern ---
       static inline const juce::Identifier kChladniM        { "chladniM"    };  // mode index m (1–8)
       static inline const juce::Identifier kChladniN        { "chladniN"    };  // mode index n (1–8)
       static inline const juce::Identifier kChladniAR       { "chladniAR"   };  // aspect ratio 0.25–4.0
       static inline const juce::Identifier kChladniSharp    { "chladniSharp"};  // sand sharpness 0–1
       static inline const juce::Identifier kChladniMaterial { "chladniMat"  };  // 0=Al 1=Steel 2=Glass 3=Acrylic
       static inline const juce::Identifier kChladniReactive { "chladniReactive" }; // audio-reactive m/n (bool)
       static inline const juce::Identifier kChladniParticles{ "chladniParticles" };// sand grain count 1000-15000
       static inline const juce::Identifier kChladniShift    { "chladniShift" };    // reactive mode shift 0-6

       // --- Oscillator long-term view ---
       static inline const juce::Identifier kOscLongTerm { "oscLongTerm" };  // bool
       static inline const juce::Identifier kOscLtWindow { "oscLtWindow" };  // seconds 0.1-30
       static inline const juce::Identifier kOscSymmetry { "oscSymmetry" };  // bool: mirrored |peak| envelope

       // --- Stereoscope ---
       static inline const juce::Identifier kStereoMode { "stereoMode" };  // 0=Particles,1=Goniometer,2=Polar,3=Correlation,4=Correlometer
       static inline const juce::Identifier kStereoParticles { "stereoParticles" }; // goniometer: particle cloud vs line
       static inline const juce::Identifier kStereoDensity   { "stereoDensity" };   // particle count 0..1
       static inline const juce::Identifier kStereoCtrlBins  { "stereoCtrlBins" };  // correlometer: use controller Max-bins (else 512)
       static inline const juce::Identifier kStereoBright    { "stereoBright" };    // gonio/polar trace brightness 0..1 (0.5 = neutral)
       static inline const juce::Identifier kStereoLineW     { "stereoLineW" };     // goniometer beam thickness 0..1 (0.5 = neutral / old 1.6 px)
       static inline const juce::Identifier kStereoPointSize { "stereoPointSize" }; // particle & polar dot size 0..1 (0.5 = neutral / old size)

       // --- Tone Analyzer ---
       static inline const juce::Identifier kToneSens { "toneSens" };   // note-detection sensitivity 0..1 (dedicated; not the shared kSmooth)
       static inline const juce::Identifier kToneTuner { "toneTuner" };  // tuner mode: accurate monophonic pitch + needle

       // --- Geometry (generative shapes) ---
       static inline const juce::Identifier kGeoTri        { "geoTri" };        // triangle weight 0..1
       static inline const juce::Identifier kGeoSquare     { "geoSquare" };     // square weight 0..1
       static inline const juce::Identifier kGeoCircle     { "geoCircle" };     // circle weight 0..1
       static inline const juce::Identifier kGeoComplexity { "geoComplexity" }; // layer count 1..48
       static inline const juce::Identifier kGeoRandom     { "geoRandom" };     // birth-angle randomization 0..1
       static inline const juce::Identifier kGeoReact      { "geoReact" };      // audio reactivity 0..1
       static inline const juce::Identifier kGeoDepth      { "geoDepth" };      // spawn distance (perspective depth) 0..1
       static inline const juce::Identifier kGeoTunnel     { "geoTunnel" };     // tunnel: 1 = centred point, 0 = fly through a tube of shapes
       static inline const juce::Identifier kGeoAperture   { "geoAperture" };   // iris: 1 = edges fill centre, 0 = open ring
       static inline const juce::Identifier kGeoGlobalRot  { "geoGlobalRot" };  // whole-module rotation speed 0..1
       static inline const juce::Identifier kGeoBpmSync    { "geoBpmSync" };    // geometry BPM spawn mode on/off
       static inline const juce::Identifier kGeoBeatDiv    { "geoBeatDiv" };    // geometry beat-division index (when shapes spawn)
       static inline const juce::Identifier kBpm           { "bpm" };           // project tempo (host/plugin-driven; default 100), automatable
       static inline const juce::Identifier kSynReact      { "synReact" };      // synesthesia audio reactivity 0..1 (default off)
       static inline const juce::Identifier kAudioInstance { "audioInstance" }; // per-module plugin instance override (0 = auto/global)
       static inline const juce::Identifier kHidden        { "hidden" };        // module hidden from the HUD (still in the controller)

       // --- VST plugin instance selection (global, 0 = auto) ---
       static inline const juce::Identifier kPluginInstance  { "pluginInstance" };

       // --- Global FFT resolution + tone precision ---
       static inline const juce::Identifier kMaxFftBins      { "maxFftBins" };   // 2048/4096/8192
       static inline const juce::Identifier kSubBinInterp    { "subBinInterp" }; // parabolic peak interp
       static inline const juce::Identifier kGlobalCqt       { "globalCqt" };     // global Constant-Q mode

       // --- Global visual theme: 0=Cyber, 1=Dark, 2=Custom, 3=White ---
       static inline const juce::Identifier kTheme           { "theme" };
       static inline const juce::Identifier kThemeColor1     { "themeColor1" };   // custom theme primary (ARGB int)
       static inline const juce::Identifier kThemeColor2     { "themeColor2" };   // custom theme secondary (ARGB int)

       // --- FUSION: a module that fuses OTHER MODULES into one picture ---
       //
       // A layer is not a private copy of a module, it IS one of the modules in
       // this tree: the panel keeps its own node, its own settings and its own
       // render engine, it simply stops being laid out in the HUD and lends its
       // frames to the Fusion instead. That is why there is one property here and
       // not a list — the relation is stored on the LAYER, so removing a panel or
       // a Fusion can never leave a dangling reference behind.
       static constexpr int kMaxFusionLayers = 3;
       static inline const juce::Identifier kFusionHost { "fusionHost" };   // per panel: id of the Fusion using it, 0 = none

       /** Per panel: WHICH slot of that Fusion it occupies, 0..kMaxFusionLayers-1.

           Without this the layer order came out of the panel tree, which is
           CREATION order — so picking module 7 for slot A and module 3 for slot B
           silently put 3 at the bottom of the stack, because 3 was made first. The
           stack is bottom-up and slot A is the base, so that is not a cosmetic
           detail: it decides what is composited onto what. */
       static inline const juce::Identifier kFusionSlot { "fusionSlot" };

       /** PER LAYER. The stack accumulates bottom-up and every layer carries its
           own blend mode, so the properties are indexed rather than global — see
           the model note at the top of FusionShader.h.

           Index 0 is the BASE: it is what everything else is composited onto, so
           its blend mode is never consulted. */
       /** LEGACY MIGRATION. Fusion used to be called Alchemy, and the name reached
           the saved data: panels of type "alchemy", and properties named "alcHost",
           "alcSlot", "alcL0Blend" and so on. Renaming the code alone would have
           left every existing preset loading with its fusion silently gutted — the
           values are all still there, just under keys nobody looks up any more.

           So this rewrites them on the way in. It is the ONLY place the old name
           survives, which is the point: one clearly-labelled door, rather than two
           vocabularies living side by side and waiting to be confused.

           Safe to run on anything: a preset already using the new names has no
           property to rename and is returned untouched. */
       static void migrateLegacyFusionNames (juce::ValueTree tree)
       {
           if (! tree.isValid()) return;

           if (tree.getProperty (kType).toString() == "alchemy")
               tree.setProperty (kType, "fusion", nullptr);

           // Collected first: renaming while iterating the property list would
           // shift the indices out from under the loop.
           struct Renamed { juce::Identifier from, to; juce::var value; };
           std::vector<Renamed> moved;

           for (int i = 0; i < tree.getNumProperties(); ++i)
           {
               const auto name = tree.getPropertyName (i);
               const juce::String s = name.toString();

               if (s.startsWith ("alc"))
                   moved.push_back ({ name,
                                      juce::Identifier ("fusion" + s.substring (3)),
                                      tree.getProperty (name) });
           }

           for (const auto& m : moved)
           {
               tree.setProperty (m.to, m.value, nullptr);
               tree.removeProperty (m.from, nullptr);
           }

           for (int i = 0; i < tree.getNumChildren(); ++i)
               migrateLegacyFusionNames (tree.getChild (i));
       }

       static juce::Identifier fusionLayerProp (const char* name, int index)
       {
           return juce::Identifier (juce::String ("fusionL") + juce::String (index) + name);
       }

       // fusionL{0..2}Blend / Opacity / Scale / OffX / OffY / Amount / Bands / Angle / Edge
       //
       // ROTATION is NOT here. A layer's quarter turn is the module's own
       // kRotationAngle, edited in the module's own editor like every other module
       // property. The fusion reads it and its shader performs the turn — because a
       // layer is never drawn in a rectangle of its own, so the AffineTransform that
       // turns an ordinary panel never reaches it — but it holds no opinion of its
       // own about which way up a module is.
       static inline const char* kFusionLayBlend   = "Blend";    // 0 screen, 1 merge, 2 weave
       static inline const char* kFusionLayOpacity = "Opacity";  // 0..1
       static inline const char* kFusionLayScale   = "Scale";    // 0.1..4
       static inline const char* kFusionLayOffX    = "OffX";     // -1..1, in layer widths
       static inline const char* kFusionLayOffY    = "OffY";
       static inline const char* kFusionLayAmount  = "Amount";   // merge bias / weave share
       static inline const char* kFusionLayBands   = "Bands";    // weave 2..32
       static inline const char* kFusionLayAngle   = "Angle";    // weave, degrees
       static inline const char* kFusionLayEdge    = "Edge";     // weave band hardness 0..1

       /** PER LAYER, the layer's OWN post chain — the same coordinate stages the
           whole stack gets globally, asked once more of this layer alone.

           Two layers can therefore be folded differently and still meet in one
           picture, which is the only reason to keep a stack: a fold every layer
           shares is a fold of the RESULT, and that is what the global stage below
           is for. Before this, the module could only say the second thing. */
       static inline const char* kFusionLayMirror   = "Mirror";    // reflection axes 0..8, 0 = none
       static inline const char* kFusionLayMirrorAng= "MirrorAng"; // degrees, where the first axis lies
       static inline const char* kFusionLaySymmetry = "Symmetry";  // wedges 1..11, 1 = no fold
       static inline const char* kFusionLaySpin     = "Spin";      // degrees
       static inline const char* kFusionLaySpeed    = "Speed";     // drift 0..1, 0 = stands still
       static inline const char* kFusionLayZoom     = "Zoom";      // 0.25..4

       /** PER LAYER, UI only: is this layer's block of settings folded open in the
           controller? Kept in the tree rather than in the controller so it
           survives closing the window and travels with a saved preset — a stack
           of three is unreadable if every slot springs open again on load. */
       static inline const char* kFusionLayOpen     = "Open";      // bool, default true

       // ── post chain: coordinate stages applied to the whole stack ──────────
       static inline const juce::Identifier kFusionWarp      { "fusionWarp" };
       static inline const juce::Identifier kFusionWarpAmt   { "fusionWarpAmt" };    // 0..1
       static inline const juce::Identifier kFusionWarpSwirl { "fusionWarpSwirl" };  // 0 = push away, 1 = swirl around

       /** How much of the source's DETAIL the warp is allowed to follow. 0 is the
           raw per-pixel gradient and is exactly what Warp always was; turned up,
           the field is measured wider and its peaks are softened, so it follows
           the shapes rather than the speckle. */
       static inline const juce::Identifier kFusionWarpSmooth { "fusionWarpSmooth" }; // 0..1

       /** DENOISE: a small blur on the warped layer reads. Averages away the
           pixel-to-pixel speckle a strong warp tears in and merges nearby colours
           and curves into simpler shapes. 0 = crisp, as before. */
       static inline const juce::Identifier kFusionWarpDenoise { "fusionWarpDenoise" }; // 0..1
       static inline const juce::Identifier kFusionWarpSrc   { "fusionWarpSrc" };    // which layer drives it, 0..2

       /** Mirror: how many congruent wedges the finished picture is folded into.
           1 = no fold, and that is the default. */
       static inline const juce::Identifier kFusionSymmetry { "fusionSymmetry" };  // 1..11

       /** Reflection axes about the centre, evenly spaced, the first at
           kFusionMirrorAngle. 0 = no fold, 1 = the single reflection this used to
           be, N = an axis every 180/N degrees.

           A COUNT rather than a switch because one axis was only ever the first
           answer: asking for a second one used to mean reaching for Symmetry,
           which folds the radius as well and is therefore a different picture
           rather than a stronger version of the same one.

           MIGRATION: this property used to hold a bool. `false` reads back as 0
           and `true` as 1, which are exactly the two values that meant the same
           thing before, so old presets need no special case. */
       static inline const juce::Identifier kFusionMirror      { "fusionMirror" };        // 0..8
       static inline const juce::Identifier kFusionMirrorAngle { "fusionMirrorAngle" };  // degrees

       static inline const juce::Identifier kFusionSpin  { "fusionSpin" };   // degrees
       static inline const juce::Identifier kFusionZoom  { "fusionZoom" };   // 0.25..4

       /** VORTEX: a turn that grows with the radius — the centre stands still and
           the edge is dragged round, so straight lines become spirals and a
           kaleidoscope stops being a fixed rosette. Bipolar, 0 = off. */
       static inline const juce::Identifier kFusionVortex { "fusionVortex" };  // -1..1
       static inline const juce::Identifier kFusionReact { "fusionReact" };  // audio modulation 0..1

       /** WHICH LAYERS the whole GLOBAL post chain (fold, tunnel, liquid, warp) is
           allowed to touch. One flag per slot A/B/C, default all on — so the global
           chain folds the whole result exactly as before. Unchecking a layer reads
           it straight, untouched by the global stages, which lets a folded stack
           keep one layer standing still through the middle of it. */
       static inline const juce::Identifier kFusionGlobL0 { "fusionGlobL0" };  // bool, default true
       static inline const juce::Identifier kFusionGlobL1 { "fusionGlobL1" };  // bool, default true
       static inline const juce::Identifier kFusionGlobL2 { "fusionGlobL2" };  // bool, default true

       /** LIQUID: a sibling of Warp that melts the whole stack together — a shared
           flowing displacement that carries every targeted layer the same way, so
           their shapes run into each other like one fluid instead of being pushed
           apart. AMOUNT is how far the flow travels, SMOOTH the feature scale
           (fine ripples ↔ broad slow currents). */
       static inline const juce::Identifier kFusionLiquid       { "fusionLiquid" };       // bool, default false
       static inline const juce::Identifier kFusionLiquidAmt    { "fusionLiquidAmt" };    // 0..1
       static inline const juce::Identifier kFusionLiquidSmooth { "fusionLiquidSmooth" }; // 0..1

       /** DENOISE: the same small blur as Warp's, on the layers Liquid melts
           together, so the flow reads as one soft body of colour rather than a noisy
           smear. 0 = crisp, as before. */
       static inline const juce::Identifier kFusionLiquidDenoise { "fusionLiquidDenoise" }; // 0..1

       /** TUNNEL: remap the whole fusion into a radially symmetric tunnel so the
           picture recedes to a vanishing point in the centre — depth, not a flat
           space warp. Just an on/off; the perspective and the inward drift are
           fixed so it always reads as a tunnel. */
       static inline const juce::Identifier kFusionTunnel { "fusionTunnel" };  // bool, default false

       /** LAYER DETAIL: how many pixels each layer is allowed to render, as a rung
           on a ladder rather than a free number.

           A layer is not the picture — it is a texture the fusion then folds,
           scales, warps and composites, and it is sampled bilinearly into the
           fusion's rectangle. Rendering it 1:1 with a fullscreen HUD means the
           expensive modules (Geometry's bloom pyramid, the fractal, the
           spectrogram) each pay full screen resolution SIXTY times a second, on
           top of the fusion's own full-resolution pass — which is why enlarging
           the window is where a fusion started to stutter.

           A cap and not a fraction: below it nothing changes at all, and above it
           the layers simply stop growing. That also means a window dragged past
           the cap stops resizing its layers, so the framebuffer reallocations and
           the static-cache rebuilds that a resize triggers stop with it.

           The 2D modules already do exactly this for themselves (see
           AsyncVisualBase::setRenderPixelBudget, ~1.3 Mpx). This is the same idea
           extended to the three that draw on the GPU, which had no budget at all.

           0 = Low, 1 = Normal, 2 = High, 3 = Native (no cap). */
       static inline const juce::Identifier kFusionDetail { "fusionDetail" };   // 0..3

       /** Pixels a layer may render at each rung. 0 means no cap. */
       static long long fusionLayerPixelBudget (int rung) noexcept
       {
           switch (juce::jlimit (0, 3, rung))
           {
               case 0:  return 640LL * 400LL;      // ~0.26 Mpx
               case 1:  return 1280LL * 800LL;     // ~1.0 Mpx — matches the 2D modules
               case 2:  return 1920LL * 1200LL;    // ~2.3 Mpx
               default: return 0;                  // native, whatever the fusion is
           }
       }

       /** UI only: is the GLOBAL block of settings folded open in the controller?
           Same reasoning as kFusionLayOpen. */
       static inline const juce::Identifier kFusionGlobalOpen { "fusionGlobalOpen" };  // bool, default true

       // --- HUD layers ("blocks"): panels live in horizontal rows 0..2 ---
       static inline const juce::Identifier kLayer     { "layer" };      // per panel: block index
       static inline const juce::Identifier kHudLayers { "hudLayers" };  // global: block count 1-3
       static inline const juce::Identifier kBlockWeights { "blockWeights" }; // global: "w0 w1 w2" height weights

       // --- Stacking INSIDE a block: a block is a row of COLUMNS, and a column
       //     can hold up to kMaxStack modules on top of each other.
       //
       //     Columns are not stored as an id — they are runs in the panel order:
       //     walking a block's panels in tree order, stackRow == 0 opens a NEW
       //     column and stackRow > 0 joins the column opened most recently. That
       //     keeps the grouping stable through every reorder that already exists
       //     (sortPanelsByLayer preserves relative order within a block) without a
       //     second index to keep in sync.
       static constexpr int kMaxStack = 3;                                    // modules above each other per column
       static inline const juce::Identifier kStackRow    { "stackRow" };      // per panel: 0..kMaxStack-1
       static inline const juce::Identifier kStackWeight { "stackWeight" };   // per panel: height share in its column

       // Per-block height weights (default 1,1,1 = equal). Drag the block divider to change.
       std::array<float, 3> blockWeights() const
       {
           std::array<float, 3> w { 1.0f, 1.0f, 1.0f };
           juce::StringArray parts;
           parts.addTokens (state.getProperty (kBlockWeights).toString(), " ,\t", "");
           parts.removeEmptyStrings();
           for (int i = 0; i < 3 && i < parts.size(); ++i)
           {
               const float v = parts[i].getFloatValue();
               if (v > 0.05f) w[(size_t) i] = v;
           }
           return w;
       }

       void setBlockWeights (const std::array<float, 3>& w)
       {
           state.setProperty (kBlockWeights,
                              juce::String (juce::jmax (0.1f, w[0]), 3) + " "
                            + juce::String (juce::jmax (0.1f, w[1]), 3) + " "
                            + juce::String (juce::jmax (0.1f, w[2]), 3),
                              nullptr);
       }

       // Remove a SPECIFIC HUD block (by index): its panels merge into the block
       // above (or block 0), higher blocks shift down, and the height weights
       // close the gap — so "delete block 2 of 3" behaves as expected.
       void removeBlock (int idx)
       {
           const int n = hudLayers();
           if (n <= 1 || idx < 0 || idx >= n) return;

           auto panels = getPanelsRoot();
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               const int l = (int) p.getProperty (kLayer, 0);
               if      (l == idx) p.setProperty (kLayer, juce::jmax (0, idx - 1), nullptr);
               else if (l >  idx) p.setProperty (kLayer, l - 1, nullptr);
           }

           auto w = blockWeights();
           for (int i = idx; i < 2; ++i) w[(size_t) i] = w[(size_t) (i + 1)];
           w[2] = 1.0f;
           setBlockWeights (w);

           state.setProperty (kHudLayers, n - 1, nullptr);
           sortPanelsByLayer();
       }

       // ── whole-block visibility ──────────────────────────────────────────────
       // There is no separate "block hidden" flag: a block IS its modules, so we
       // hide the block by hiding every module it holds. The HUD layout already
       // drops rows that end up with nothing visible in them and hands the space
       // to the remaining blocks, so this collapses the row for free — and each
       // module keeps its own kHidden state in presets exactly as before.
       int  blockModuleCount (int idx) const
       {
           auto panels = getPanelsRoot();
           if (! panels.isValid()) return 0;
           int n = 0;
           for (int i = 0; i < panels.getNumChildren(); ++i)
               if ((int) panels.getChild (i).getProperty (kLayer, 0) == idx)
                   ++n;
           return n;
       }

       /** True when the block holds at least one module and every one of them is
           hidden. An EMPTY block reports false: there is nothing to reveal, so
           offering "Show block" for it would be a dead menu entry. */
       bool blockIsHidden (int idx) const
       {
           auto panels = getPanelsRoot();
           if (! panels.isValid()) return false;

           bool any = false;
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               if ((int) p.getProperty (kLayer, 0) != idx) continue;
               any = true;
               if (! (bool) p.getProperty (kHidden, false)) return false;
           }
           return any;
       }

       void setBlockHidden (int idx, bool shouldBeHidden)
       {
           auto panels = getPanelsRoot();
           if (! panels.isValid()) return;

           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               if ((int) p.getProperty (kLayer, 0) != idx) continue;
               p.setProperty (kHidden, shouldBeHidden, nullptr);
           }
       }

       // Keep the panels physically GROUPED by block (stable: relative order inside a
       // block is preserved). The controller list shows one "bubble" per block, so
       // all block-0 modules must sit together, then block 1, then block 2.
       void sortPanelsByLayer()
       {
           auto panels = getPanelsRoot();
           const int n = panels.getNumChildren();
           int target = 0;
           for (int layer = 0; layer <= 2; ++layer)
               for (int i = target; i < n; ++i)
                   if ((int) panels.getChild (i).getProperty (kLayer, 0) == layer)
                   {
                       if (i != target)
                           panels.moveChild (i, target, nullptr);
                       ++target;
                   }

           normaliseStacks();
       }

       /** Repair the column runs after panels have been moved, removed or pushed
           into another block.

           A run is only well-formed if it starts with stackRow == 0 and grows by
           one, never past kMaxStack. Anything else (a stacked panel whose column
           head moved away, a column that swallowed a fourth module when a block
           was deleted) is rewritten into the nearest valid arrangement, which at
           worst means the module becomes its own column again. */
       void normaliseStacks()
       {
           auto panels = getPanelsRoot();
           bool first = true;
           int  prevLayer = 0;
           int  rowInColumn = 0;      // how many modules the open column already holds

           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               const int layer = (int) p.getProperty (kLayer, 0);
               const int want  = (int) p.getProperty (kStackRow, 0);

               // A new block always starts a fresh column.
               const bool startsColumn = first
                                      || (layer != prevLayer)
                                      || want <= 0
                                      || rowInColumn >= kMaxStack;
               first = false;

               const int row = startsColumn ? 0 : rowInColumn;
               if (row != want)
                   p.setProperty (kStackRow, row, nullptr);

               if (((float) p.getProperty (kStackWeight, 0.0f)) <= 0.0f)
                   p.setProperty (kStackWeight, 1.0f, nullptr);

               rowInColumn = row + 1;
               prevLayer   = layer;
           }
       }

       int  hudLayers() const     { return juce::jlimit (1, 3, (int) state.getProperty (kHudLayers, 1)); }
       void setHudLayers (int n)
       {
           n = juce::jlimit (1, 3, n);
           state.setProperty (kHudLayers, n, nullptr);

           // panels in removed blocks fall into the last remaining one
           auto panels = getPanelsRoot();
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               if ((int) p.getProperty (kLayer, 0) > n - 1)
                   p.setProperty (kLayer, n - 1, nullptr);
           }
           sortPanelsByLayer();
       }
    

       // vráti (a vytvorí ak neexistuje) "panels" node
       juce::ValueTree getPanelsRoot()
       {
           auto panels = state.getChildWithName (kPanelsNode);
           if (! panels.isValid())
           {
               panels = juce::ValueTree (kPanelsNode);
               state.addChild (panels, -1, nullptr);
           }
           return panels;
       }

       juce::ValueTree getPanelsRoot() const
       {
           auto panels = state.getChildWithName (kPanelsNode);
           return panels;
       }

       // Vyrob nové unikátne ID (1,2,3,...)
       int nextPanelId()
       {
           int maxId = 0;
           auto panels = getPanelsRoot();
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               maxId = juce::jmax (maxId, (int) p.getProperty (kId));
           }
           return maxId + 1;
       }

       // Pridaj nový panel určitého typu ("rms" alebo "spectrum")
       // Vráti ID nového panelu (aby si ho vedel hneď označiť v UI)
       int addPanel (const juce::String& type)
       {
           auto panels = getPanelsRoot();

           juce::ValueTree p (kPanelNode);
           p.setProperty (kId, nextPanelId(), nullptr);
           p.setProperty (kType, type, nullptr);
           
        // default panel colour: cycle through the ALTER theme accents
        p.setProperty (kColor,
                       (int) AlterTheme::accentForIndex ((int) p.getProperty (kId)).getARGB(),
                       nullptr);
           // detatchovanie okien z HUD
           p.setProperty (kDetached, false, nullptr);
           p.setProperty (kWnd, juce::Rectangle<int>(200,200,600,180).toString(), nullptr);

           // Default width ratio: equal distribution (will be normalized by layout)
           // Start with 1.0 - layout will normalize based on number of modules
           p.setProperty (kWidthRatio, 1.0f, nullptr);

           // Default rotation: 0 (no rotation)
           p.setProperty (kRotationAngle, 0, nullptr);

           // Default HUD block: the first one, as its own column (not stacked)
           p.setProperty (kLayer, 0, nullptr);
           p.setProperty (kStackRow,    0,     nullptr);
           p.setProperty (kStackWeight, 1.0f,  nullptr);

           

           // defaulty podľa typu
            if (type == "rms" || type == "audiometer")
            {
                p.setProperty (kSmooth, 0.5f, nullptr);
                p.setProperty (kRotationAngle, 0, nullptr);       // Default: 0° (no rotation)
                // A level meter is read as a COLUMN: the bar runs bottom-to-top, so
                // the module wants to be clearly taller than it is wide. 96 px is the
                // narrowest width that still fits the readout strip ("-23.4 dBTP")
                // and the dB scale without truncating them — thin, but not a sliver.
                //
                // This is the width the meter STARTS at and keeps until the user says
                // otherwise — it is a default, not a lock. The layout does not grow it
                // on its own (that is the whole point: add a spectrum next to a meter
                // and the spectrum takes everything except this), but dragging the
                // module's right edge writes a new number here and it can be taken all
                // the way to the width of the block. See ResizeDivider::mouseDrag.
                p.setProperty (kPreferredWidth, 96, nullptr);     // starting width in pixels
                p.setProperty (kWidthRatio, 0.15f, nullptr);      // Fallback ratio if preferred not used
                p.setProperty (kMeasureState, 0, nullptr);        // Trend: idle by default
                p.setProperty (kMeterView,    0, nullptr);        // 0=Momentary, 1=Trend

                // Colour by tone is OFF here for the same reason it is off on the
                // Spectrum: a meter is read for its NUMBER, and a bar that changes
                // colour with the material makes the loudness zones harder to place
                // at a glance. The user opts in; the shade then decides how the
                // zones separate from the note.
                p.setProperty (kMeterToneColor, false, nullptr);
                p.setProperty (kMeterToneShade, 0,     nullptr);  // 0=Gradient, 1=Complementary, 2=Analogous
                p.setProperty (kToneTwist,      false, nullptr);
                p.setProperty (kToneSmooth,     0.5f,  nullptr);

                // Two bars ON by default: the smoothed bar says where the level IS,
                // the hold bar says where it just WAS, and on a meter the second
                // question is the one that catches the peak that clipped.
                p.setProperty (kMeterTwoBars,   true,  nullptr);
                p.setProperty (kClipZone,       true,  nullptr);  // level-history 0 dBFS boundary
            }
           else if (type == "spectrum")
           {
               p.setProperty (kSmooth, 0.5f, nullptr);
               p.setProperty (kAWeight, false, nullptr);
               p.setProperty (kBins, 8192, nullptr);   // use the full resolution the source provides
               p.setProperty (kRenderPoints, 1024, nullptr);
               p.setProperty (kMeasurementMode, 0, nullptr);   // 0=Visual, 1=Measurement
               p.setProperty (kPsychoCurve, 0, nullptr);        // 0=Flat, 1=A-weight, 2=ISO226
               p.setProperty (kPhon, 60, nullptr);              // ISO226 phon level
               p.setProperty (kSpecReference, 0, nullptr);      // reference curve off
               p.setProperty (kSpecStereo, false, nullptr);     // mono spectrum
               p.setProperty (kSpecMirror, false, nullptr);     // low frequencies on the left
               p.setProperty (kSpecLrColor, 0, nullptr);        // R = complementary colour
               // Colour by tone is OFF by default here, unlike Chladni/Geometry/
               // Synesthesia. Those are generative visuals whose whole point is the
               // note; an analyser is a measuring instrument first, and a curve that
               // changes colour as the material changes is harder to read against the
               // grid. The user opts in.
               p.setProperty (kColorMode,   0,     nullptr);    // 0=manual colour, 1=by tone
               p.setProperty (kToneTwist,   false, nullptr);    // shared wheel direction
               p.setProperty (kToneSmooth,  0.5f, nullptr);    // hue chase rate
           }
           else if (type == "oscillator" || type == "oscilator")
           {
               p.setProperty (kSmooth, 0.5f, nullptr);
               p.setProperty (kNeon, false, nullptr);
               p.setProperty (kDisplayMode, 0, nullptr);     // 0=Mono, 1=Stereo, 2=Mirror
               p.setProperty (kZoom, 0.420f, nullptr);        // 0.0=85ms .. 1.0=0.1ms, default ~5ms
               p.setProperty (kOscLongTerm, false, nullptr);  // short-term by default
               p.setProperty (kOscLtWindow, 10.0f, nullptr);  // 10 s window
               p.setProperty (kOscSymmetry, false, nullptr);  // true waveform shape
               p.setProperty (kOscLrColor,  0,     nullptr);  // R = complementary colour (matches Spectrum)
               p.setProperty (kClipZone,    true,  nullptr);  // 0 dBFS boundary in the Long Waveform view
               p.setProperty (kColorMode,   0,     nullptr);  // 0=manual colour, 1=by tone
               p.setProperty (kToneTwist,   false, nullptr);
               p.setProperty (kToneSmooth,  0.5f, nullptr);
           }
           else if (type == "chladni")
           {
               p.setProperty (kChladniM,        2,     nullptr);  // m=2,n=3 → classic first pattern
               p.setProperty (kChladniN,        3,     nullptr);
               p.setProperty (kChladniAR,       1.0f,  nullptr);  // square plate
               p.setProperty (kChladniSharp,    0.5f,  nullptr);
               p.setProperty (kChladniMaterial, 1,     nullptr);  // Steel (default plate)
               p.setProperty (kChladniReactive, true,  nullptr);  // audio-reactive m/n
               p.setProperty (kChladniParticles, 5000, nullptr);  // sand grain count
               p.setProperty (kChladniShift,    0,     nullptr);  // reactive mode shift
               p.setProperty (kColorMode,       1,     nullptr);  // 1=tone-dependent colour ON at start (matches Synesthesia/Geometry)
               p.setProperty (kToneTwist,       false, nullptr);  // shared wheel direction
               // 0.374 is not arbitrary: toneSmoothToRate maps it to a lerp rate of
               // 0.15, the constant Chladni's hue chase was hardcoded to before it
               // became adjustable. Existing presets carry no value, fall back to
               // this, and look exactly as they always have.
               p.setProperty (kToneSmooth,      0.374f, nullptr);
               p.setProperty (kPreferredWidth,  200,   nullptr);  // squarish default
               p.setProperty (kWidthRatio,      0.25f, nullptr);
           }
           else if (type == "toneanalyzer")
           {
               p.setProperty (kToneSens,       0.5f,  nullptr);   // detection sensitivity (dedicated key)
               p.setProperty (kToneTuner,      false, nullptr);   // tuner mode off by default
               // Off by default like the other analysers: the bars and the readout
               // are structure, and structure that recolours itself is harder to
               // read than structure that stays put. Opt in for the HUD look.
               p.setProperty (kColorMode,      0,     nullptr);   // 0=manual accent, 1=by tone
               p.setProperty (kToneTwist,      false, nullptr);
               p.setProperty (kToneSmooth,     0.5f,  nullptr);
               p.setProperty (kWidthRatio,     0.3f,  nullptr);
           }
           else if (type == "spectrogram")
           {
               p.setProperty (kSmooth,         0.35f, nullptr);   // temporal smoothing
               p.setProperty (kSpectroWindow,  30.0f, nullptr);   // 30 s visible window
               p.setProperty (kSpectroMirror,  false, nullptr);   // normal frequency axis
               p.setProperty (kSpectroLineFill, 0.0f, nullptr);   // thin line (default)
               p.setProperty (kSpectroReassign, false, nullptr);  // enhanced-frequency mode off by default
               p.setProperty (kColorMode,      0,     nullptr);   // 0=theme heat map, 1=custom gradient, 2=by tone
               p.setProperty (kToneTwist,      false, nullptr);
               p.setProperty (kToneSmooth,     0.5f, nullptr);
               p.setProperty (kWidthRatio,     0.45f, nullptr);
           }
           else if (type == "stereoscope")
           {
               p.setProperty (kStereoMode,      0,     nullptr);  // 0=Particles
               p.setProperty (kSmooth,          0.5f,  nullptr);  // display smoothing / persistence
               p.setProperty (kStereoParticles, false, nullptr);  // goniometer: line by default
               p.setProperty (kStereoDensity,   0.5f,  nullptr);  // particle count
               p.setProperty (kStereoCtrlBins,  false, nullptr);  // correlometer: 512 bins by default
               p.setProperty (kStereoBright,    0.5f,  nullptr);  // gonio/polar brightness (neutral)
               // Both thickness controls default to 0.5, which their leg-maps
               // define as EXACTLY the sizes that used to be hard-coded — so a
               // stereoscope that has never been touched looks unchanged.
               p.setProperty (kStereoLineW,     0.5f,  nullptr);  // goniometer beam width (neutral)
               p.setProperty (kStereoPointSize, 0.5f,  nullptr);  // particle / polar dot size (neutral)
               p.setProperty (kColorMode,       0,     nullptr);  // 0=manual colour, 1=by tone
               p.setProperty (kToneTwist,       false, nullptr);
               p.setProperty (kToneSmooth,      0.5f, nullptr);
               p.setProperty (kWidthRatio,      0.3f,  nullptr);
           }
           else if (type == "geometry")
           {
               p.setProperty (kSmooth,        0.15f, nullptr);
               p.setProperty (kZoom,          1.0f,  nullptr);
               p.setProperty (kRotation,      0.0f,  nullptr);
               p.setProperty (kSymmetry,      1,     nullptr);
               p.setProperty (kSaturation,    1.0f,  nullptr);
               p.setProperty (kBloom,         0.2f,  nullptr);
               p.setProperty (kSpeed,         1.0f,  nullptr);
               p.setProperty (kGeoTri,        0.5f,  nullptr);
               p.setProperty (kGeoSquare,     0.5f,  nullptr);
               p.setProperty (kGeoCircle,     0.5f,  nullptr);
               p.setProperty (kGeoComplexity, 12,    nullptr);
               p.setProperty (kGeoRandom,     1.0f,  nullptr);  // birth-angle randomization
               p.setProperty (kGeoReact,      0.0f,  nullptr);  // audio reactivity (default off)
               p.setProperty (kGeoDepth,      0.7f,  nullptr);  // spawn distance (higher = deeper/smaller spawn)
               p.setProperty (kGeoTunnel,     0.0f,  nullptr);  // 0 = spread around centre (normal look) by default
               p.setProperty (kGeoAperture,   0.8f,  nullptr);  // iris mostly closed (centre filled) by default
               p.setProperty (kGeoGlobalRot,  0.0f,  nullptr);  // whole-module rotation off by default
               p.setProperty (kSymmetry,      6,     nullptr);  // 6 symmetric points around the centre (nice default)
               p.setProperty (kGeoBpmSync,    false, nullptr);  // FREE speed mode by default
               p.setProperty (kGeoBeatDiv,    5,     nullptr);  // 1/4 note (one beat)
               p.setProperty (kBpm,           100.0f, nullptr); // default tempo until the plugin sends the host's
               p.setProperty (kColorMode,     1,     nullptr);  // 1=tone-dependent colour ON at start
               p.setProperty (kToneTwist,     false, nullptr);  // shared wheel direction
               // base colour: keep the cycled default colour set above (kColor)
           }
           else if (type == "fusion")
           {
               // Starts EMPTY and INERT: the layers are existing modules the user
               // picks, and every effect is off. A module that folds, spins and
               // reacts the instant it is created is not showing off, it is making
               // noise the user then has to undo.
               for (int i = 0; i < kMaxFusionLayers; ++i)
               {
                   // Screen is the honest default: the layers simply stack and
                   // nothing is claimed about how they meet until the user says so.
                   p.setProperty (fusionLayerProp (kFusionLayBlend,   i), 0,     nullptr);
                   p.setProperty (fusionLayerProp (kFusionLayOpacity, i), 1.0f,  nullptr);
                   p.setProperty (fusionLayerProp (kFusionLayScale,   i), 1.0f,  nullptr);
                   p.setProperty (fusionLayerProp (kFusionLayOffX,    i), 0.0f,  nullptr);
                   p.setProperty (fusionLayerProp (kFusionLayOffY,    i), 0.0f,  nullptr);
                   p.setProperty (fusionLayerProp (kFusionLayAmount,  i), 0.5f,  nullptr);
                   p.setProperty (fusionLayerProp (kFusionLayBands,   i), 8,     nullptr);
                   p.setProperty (fusionLayerProp (kFusionLayAngle,   i), 0.0f,  nullptr);
                   p.setProperty (fusionLayerProp (kFusionLayEdge,    i), 0.6f,  nullptr);

                   // The layer's own post chain, all of it inert.
                   p.setProperty (fusionLayerProp (kFusionLayMirror,    i), 0,    nullptr);
                   p.setProperty (fusionLayerProp (kFusionLayMirrorAng, i), 0.0f, nullptr);
                   p.setProperty (fusionLayerProp (kFusionLaySymmetry,  i), 1,    nullptr);
                   p.setProperty (fusionLayerProp (kFusionLaySpin,      i), 0.0f, nullptr);
                   p.setProperty (fusionLayerProp (kFusionLaySpeed,     i), 0.0f, nullptr);
                   p.setProperty (fusionLayerProp (kFusionLayZoom,      i), 1.0f, nullptr);

                   // Open to begin with: a new fusion has nothing in it, so there
                   // is nothing to hide from and everything to explain.
                   p.setProperty (fusionLayerProp (kFusionLayOpen,      i), true, nullptr);
               }

               p.setProperty (kFusionWarp,      false, nullptr);
               p.setProperty (kFusionWarpAmt,   0.5f,  nullptr);
               p.setProperty (kFusionWarpSwirl, 0.6f,  nullptr);
               p.setProperty (kFusionWarpSmooth,0.0f,  nullptr);   // raw field, as it always was
               p.setProperty (kFusionWarpDenoise,0.0f, nullptr);   // crisp, as it always was
               p.setProperty (kFusionWarpSrc,   0,     nullptr);

               p.setProperty (kFusionSymmetry,   1,     nullptr);   // 1 = no fold
               p.setProperty (kFusionMirror,     0,     nullptr);   // 0 = no reflection axes
               p.setProperty (kFusionMirrorAngle,0.0f,  nullptr);
               p.setProperty (kFusionGlobalOpen, true,  nullptr);
               p.setProperty (kFusionSpin,       0.0f,  nullptr);
               p.setProperty (kFusionZoom,       1.0f,  nullptr);
               p.setProperty (kFusionVortex,     0.0f,  nullptr);   // no swirl
               p.setProperty (kFusionReact,      0.0f,  nullptr);   // audio does nothing until asked

               // Global chain targets ALL layers by default → folds the whole result,
               // exactly as it did before this existed.
               p.setProperty (kFusionGlobL0, true, nullptr);
               p.setProperty (kFusionGlobL1, true, nullptr);
               p.setProperty (kFusionGlobL2, true, nullptr);

               p.setProperty (kFusionLiquid,       false, nullptr);  // inert until asked
               p.setProperty (kFusionLiquidAmt,    0.5f,  nullptr);
               p.setProperty (kFusionLiquidSmooth, 0.5f,  nullptr);
               p.setProperty (kFusionLiquidDenoise,0.0f,  nullptr);  // crisp, as it always was

               p.setProperty (kFusionTunnel,       false, nullptr);  // flat until asked
               p.setProperty (kFusionDetail,     1,     nullptr);   // ~1 Mpx per layer
               p.setProperty (kSpeed,         0.0f,  nullptr);   // and nothing turns on its own
               p.setProperty (kWidthRatio,    0.4f,  nullptr);   // wants room; it is a picture, not a readout
           }
           else
           {
               // neznámy typ: aspoň smooth
               p.setProperty (kSmooth, 0.5f, nullptr);
           }

           panels.addChild (p, -1, nullptr);
           return (int) p.getProperty (kId);
           
       }

       // Nájde panel podľa ID (vráti invalid ValueTree ak nenájde)
       juce::ValueTree getPanelById (int id)
       {
           auto panels = getPanelsRoot();
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               if ((int) p.getProperty (kId) == id)
                   return p;
           }
           return {};
       }

       // Duplicate a panel: deep-copy all its settings, assign a new unique id, and
       // insert it directly after the source so the copy appears next to the original
       // in the HUD and just below it in the controller list. Returns the new id (-1 on fail).
       int duplicatePanel (int sourceId)
       {
           auto panels = getPanelsRoot();
           int srcIndex = -1;
           for (int i = 0; i < panels.getNumChildren(); ++i)
               if ((int) panels.getChild (i).getProperty (kId) == sourceId) { srcIndex = i; break; }
           if (srcIndex < 0) return -1;

           juce::ValueTree copy = panels.getChild (srcIndex).createCopy();   // all settings
           const int newId = nextPanelId();
           copy.setProperty (kId, newId, nullptr);
           copy.setProperty (kDetached, false, nullptr);   // sit in the HUD next to the source
           copy.setProperty (kHidden,   false, nullptr);
           copy.removeProperty (kSelected, nullptr);

           panels.addChild (copy, srcIndex + 1, nullptr);
           return newId;
       }

       // ── Fusion layer relation ───────────────────────────────────────────
       // The panels a Fusion currently uses as layers, in tree order (which is
       // the order they are composited in). Never longer than kMaxFusionLayers.
       /** The Fusion's layers IN SLOT ORDER — the order the user put them in, not
           the order the modules happened to be created in. Index 0 is slot A, the
           base of the stack. */
       std::vector<int> fusionLayerIds (int fusionId)
       {
           std::vector<int> out;
           if (fusionId <= 0) return out;

           std::array<int, kMaxFusionLayers> bySlot;
           bySlot.fill (0);

           auto panels = getPanelsRoot();
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               if ((int) p.getProperty (kFusionHost, 0) != fusionId)
                   continue;

               const int slot = juce::jlimit (0, kMaxFusionLayers - 1,
                                              (int) p.getProperty (kFusionSlot, 0));
               // First writer wins, so a state file that somehow has two modules
               // claiming one slot degrades to "one of them" rather than to chaos.
               if (bySlot[(size_t) slot] == 0)
                   bySlot[(size_t) slot] = (int) p.getProperty (kId, -1);
           }

           // Compacted: an empty slot A must not push everything else down a place,
           // because the shader only ever knows about consecutive texture units.
           for (int id : bySlot)
               if (id > 0)
                   out.push_back (id);

           return out;
       }

       /** The module in ONE slot, or 0. The pickers and the per-slot settings need
           this rather than the compacted list: an empty slot A must not make slot
           B's module answer to slot A's controls. */
       int fusionLayerIdInSlot (int fusionId, int slot)
       {
           if (fusionId <= 0 || slot < 0 || slot >= kMaxFusionLayers) return 0;

           auto panels = getPanelsRoot();
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               if ((int) p.getProperty (kFusionHost, 0) == fusionId
                   && juce::jlimit (0, kMaxFusionLayers - 1, (int) p.getProperty (kFusionSlot, 0)) == slot)
                   return (int) p.getProperty (kId, -1);
           }
           return 0;
       }

       /** Which slot of `fusionId` is free, or -1 when it is full. */
       int firstFreeFusionSlot (int fusionId)
       {
           bool taken[kMaxFusionLayers] = { false, false, false };

           auto panels = getPanelsRoot();
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               if ((int) p.getProperty (kFusionHost, 0) == fusionId)
                   taken[juce::jlimit (0, kMaxFusionLayers - 1,
                                       (int) p.getProperty (kFusionSlot, 0))] = true;
           }

           for (int i = 0; i < kMaxFusionLayers; ++i)
               if (! taken[i])
                   return i;

           return -1;
       }

       /** Hands every layer of this Fusion back to the HUD. */
       void releaseFusionLayers (int fusionId)
       {
           auto panels = getPanelsRoot();
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               if ((int) p.getProperty (kFusionHost, 0) == fusionId)
                   p.setProperty (kFusionHost, 0, nullptr);
           }
       }

       /** Makes `panelId` a layer of `fusionId` (0 = release it back to the HUD).

           Refuses the two cases that would break the model: a Fusion cannot be
           its own layer, and it cannot take more than kMaxFusionLayers. Returns false
           when the request was refused, so the caller can leave the UI unchanged
           instead of showing a layer that was never taken. */
       /** `slot` is which layer of the Fusion this module becomes; -1 takes the
           first free one. The slot is what the stack order follows. */
       bool setFusionLayer (int panelId, int fusionId, int slot = -1)
       {
           auto p = getPanelById (panelId);
           if (! p.isValid()) return false;

           if (fusionId <= 0)
           {
               p.setProperty (kFusionHost, 0, nullptr);
               return true;
           }

           if (panelId == fusionId) return false;
           if (p.getProperty (kType).toString() == "fusion") return false;   // no nesting

           const auto current = fusionLayerIds (fusionId);
           const bool alreadyMine = std::find (current.begin(), current.end(), panelId) != current.end();

           if ((int) current.size() >= kMaxFusionLayers && ! alreadyMine)
               return false;

           if (slot < 0)
               slot = firstFreeFusionSlot (fusionId);
           if (slot < 0)
               return false;

           p.setProperty (kFusionSlot, juce::jlimit (0, kMaxFusionLayers - 1, slot), nullptr);

           // A layer lives inside the HUD's component tree, so it cannot also be
           // floating in a window of its own — the two would fight over who its
           // parent is. Coming back from a window is the natural reading of
           // "put this module into that Fusion", so pull it back first.
           p.setProperty (kDetached,    false, nullptr);
           p.setProperty (kRowDetached, false, nullptr);

           p.setProperty (kFusionHost, fusionId, nullptr);
           return true;
       }

       // Vymaže panel podľa ID (true ak zmazal)
       bool removePanelById (int id)
       {
           auto panels = getPanelsRoot();
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               if ((int) p.getProperty (kId) == id)
               {
                   // Deleting a Fusion must not take its layers with it — they are
                   // ordinary modules that were only lent to it.
                   if (p.getProperty (kType).toString() == "fusion")
                       releaseFusionLayers (id);

                   panels.removeChild (i, nullptr);
                   return true;
               }
           }
           return false;
       }

    // ════════════════════════════════════════════════════════════════════════
    //  Undo / Redo (Ctrl+Z / Ctrl+Shift+Z)
    //  Snapshot history of the whole state tree. Any change marks the state
    //  dirty; after 400 ms of quiet the previous snapshot becomes ONE undo step
    //  (a slider drag coalesces into a single step). undo()/redo() restore
    //  snapshots via a GRANULAR merge — per-property sets and per-child
    //  add/remove/move matched by panel kId — so every existing ValueTree
    //  listener reacts exactly as it would to a normal user edit, and bindings
    //  to panel sub-trees stay valid (children keep their identity).
    // ════════════════════════════════════════════════════════════════════════
    class UndoHistory : private juce::ValueTree::Listener,
                        private juce::Timer
    {
    public:
        explicit UndoHistory (juce::ValueTree& s) : state (s)
        {
            committed = state.createCopy();
            state.addListener (this);
            startTimer (150);
        }

        ~UndoHistory() override { state.removeListener (this); }

        bool canUndo() const noexcept { return dirty || ! undoStack.empty(); }
        bool canRedo() const noexcept { return ! redoStack.empty(); }

        bool undo()
        {
            if (dirty) commit();
            if (undoStack.empty()) return false;
            redoStack.push_back (state.createCopy());
            const auto target = undoStack.back();
            undoStack.pop_back();
            applySnapshot (target);
            return true;
        }

        bool redo()
        {
            if (dirty) commit();
            if (redoStack.empty()) return false;
            undoStack.push_back (state.createCopy());
            const auto target = redoStack.back();
            redoStack.pop_back();
            applySnapshot (target);
            return true;
        }

    private:
        void applySnapshot (const juce::ValueTree& target)
        {
            restoring = true;
            mergeInto (state, target);
            restoring = false;
            committed = state.createCopy();
            dirty = false;
        }

        void markDirty()
        {
            if (restoring) return;
            dirty = true;
            lastChangeMs = juce::Time::getMillisecondCounter();
        }

        void commit()
        {
            undoStack.push_back (committed);
            if ((int) undoStack.size() > 64)
                undoStack.erase (undoStack.begin());
            redoStack.clear();
            committed = state.createCopy();
            dirty = false;
        }

        void timerCallback() override
        {
            if (dirty && juce::Time::getMillisecondCounter() - lastChangeMs > 400)
                commit();
        }

        // Make dst equal src through ORDINARY property/child operations.
        static void mergeInto (juce::ValueTree dst, const juce::ValueTree& src)
        {
            for (int i = dst.getNumProperties(); --i >= 0;)
            {
                const auto name = dst.getPropertyName (i);
                if (! src.hasProperty (name))
                    dst.removeProperty (name, nullptr);
            }
            for (int i = 0; i < src.getNumProperties(); ++i)
            {
                const auto name = src.getPropertyName (i);
                if (dst.getProperty (name) != src.getProperty (name))
                    dst.setProperty (name, src.getProperty (name), nullptr);
            }

            auto matches = [] (const juce::ValueTree& a, const juce::ValueTree& b)
            {
                if (a.getType() != b.getType()) return false;
                if (a.hasProperty (AlterState::kId) || b.hasProperty (AlterState::kId))
                    return a.getProperty (AlterState::kId) == b.getProperty (AlterState::kId);
                return true;
            };

            // drop children that no longer exist in the snapshot
            for (int i = dst.getNumChildren(); --i >= 0;)
            {
                bool found = false;
                for (int j = 0; j < src.getNumChildren() && ! found; ++j)
                    found = matches (dst.getChild (i), src.getChild (j));
                if (! found)
                    dst.removeChild (i, nullptr);
            }

            // add / reorder / recurse (positions < j are already final)
            for (int j = 0; j < src.getNumChildren(); ++j)
            {
                const auto sc = src.getChild (j);
                int di = -1;
                for (int i = j; i < dst.getNumChildren(); ++i)
                    if (matches (dst.getChild (i), sc)) { di = i; break; }

                if (di < 0)
                    dst.addChild (sc.createCopy(), j, nullptr);
                else
                {
                    if (di != j) dst.moveChild (di, j, nullptr);
                    mergeInto (dst.getChild (j), sc);
                }
            }
        }

        void valueTreePropertyChanged   (juce::ValueTree&, const juce::Identifier&) override { markDirty(); }
        void valueTreeChildAdded        (juce::ValueTree&, juce::ValueTree&) override        { markDirty(); }
        void valueTreeChildRemoved      (juce::ValueTree&, juce::ValueTree&, int) override   { markDirty(); }
        void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override                { markDirty(); }
        void valueTreeRedirected        (juce::ValueTree&) override    // tree replaced → reset
        {
            undoStack.clear();
            redoStack.clear();
            committed = state.createCopy();
            dirty = false;
        }

        juce::ValueTree& state;
        juce::ValueTree committed;                        // state at the last commit
        std::vector<juce::ValueTree> undoStack, redoStack;
        bool dirty { false }, restoring { false };
        juce::uint32 lastChangeMs { 0 };
    };

    /** Fired after undo()/redo() rewrote the state — UI panels that cache widget
        values (the Controller's right editor) re-sync themselves from it. */
    std::function<void()> onHistoryRestored;

    bool undo() { const bool ok = undoHistory.undo(); if (ok && onHistoryRestored) onHistoryRestored(); return ok; }
    bool redo() { const bool ok = undoHistory.redo(); if (ok && onHistoryRestored) onHistoryRestored(); return ok; }

    /** Global Ctrl+Z / Ctrl+Shift+Z (+ Ctrl+Y) handler — call from any window's
        keyPressed. Returns true if the key was consumed. */
    bool handleUndoRedoKey (const juce::KeyPress& k)
    {
        if (! k.getModifiers().isCommandDown())
            return false;
        if (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z')
            return k.getModifiers().isShiftDown() ? redo() : undo();
        if (k.getKeyCode() == 'Y' || k.getKeyCode() == 'y')
            return redo();
        return false;
    }

private:
    juce::ValueTree state;
    UndoHistory undoHistory { state };   // must sit AFTER `state` (init order)
};
