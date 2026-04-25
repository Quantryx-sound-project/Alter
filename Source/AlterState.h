#pragma once
#include <JuceHeader.h>

// Add MIDI identifier to AlterState
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
       // Panel layout (Add/Delete, multiple instances)
       // =========================================================

       // názvy node-ov v ValueTree
       static inline const juce::Identifier kPanelsNode { "panels" };
       static inline const juce::Identifier kPanelNode  { "panel"  };

       // properties na paneli
       static inline const juce::Identifier kId      { "id" };
       static inline const juce::Identifier kType    { "type" };     // "rms", "spectrum", "midi", neskôr ďalšie
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
       static inline const juce::Identifier kBloom { "bloom" };

       static inline const juce::Identifier kDetached { "detached" };
       static inline const juce::Identifier kWnd      { "wnd" };
       static inline const juce::Identifier kColor    { "color" };
       static inline const juce::Identifier kSelected { "selected" };
       static inline const juce::Identifier kWidthRatio { "widthRatio" };  // Module width proportion (0.0-1.0)
       static inline const juce::Identifier kRotationAngle { "rotationAngle" };  // Module rotation: 0=0°, 1=90°, 2=180°, 3=270°
       static inline const juce::Identifier kMeterMode { "meterMode" };  // RMS meter mode: 0=RMS, 1=True Peak, 2=LUFS
       static inline const juce::Identifier kColorMode { "colorMode" };  // RMS color mode: 0=Standard, 1=Custom Gradient, 2=Custom Spectrum
       static inline const juce::Identifier kCustomColorMode { "customColorMode" };  // RMS color mode (legacy): false=standard, true=custom
       static inline const juce::Identifier kPreferredWidth { "preferredWidth" };    // Fixed pixel width (0 = flexible, proportional)
       static inline const juce::Identifier kSystemGain { "systemGain" };              // System audio capture gain (0.0 = true, up to 10.0)

       // --- Spectrum extended ---
       static inline const juce::Identifier kMeasurementMode { "measurementMode" }; // 0=Visual, 1=Measurement (SPAN-like)
       static inline const juce::Identifier kPeakHold        { "peakHold" };        // bool
       static inline const juce::Identifier kPsychoCurve     { "psychoCurve" };     // 0=Flat, 1=A-weight, 2=ISO226
       static inline const juce::Identifier kPhon            { "phon" };            // ISO226 phon level (40/60/80)

       // --- AudioMeter Trend (waveform over time) ---
       static inline const juce::Identifier kMeasureState { "measureState" }; // 0=idle, 1=measuring
       static inline const juce::Identifier kMeterView    { "meterView" };    // 0=Momentary (bar), 1=Trend (waveform)

       // --- Chladni Pattern ---
       static inline const juce::Identifier kChladniM        { "chladniM"    };  // mode index m (1–8)
       static inline const juce::Identifier kChladniN        { "chladniN"    };  // mode index n (1–8)
       static inline const juce::Identifier kChladniAR       { "chladniAR"   };  // aspect ratio 0.25–4.0
       static inline const juce::Identifier kChladniSharp    { "chladniSharp"};  // sand sharpness 0–1
       static inline const juce::Identifier kChladniMaterial { "chladniMat"  };  // 0=Al 1=Steel 2=Glass 3=Acrylic
    

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
           
        // default panel colour: pick from a small palette based on id
        {
            const juce::Colour palette[] = { juce::Colours::deepskyblue, juce::Colours::limegreen, juce::Colours::orange, juce::Colours::violet, juce::Colours::yellow };
            const int id = (int) p.getProperty (kId);
            const juce::Colour c = palette[id % (sizeof(palette)/sizeof(palette[0]))];
            p.setProperty (kColor, (int) c.getARGB(), nullptr);
        }
           // detatchovanie okien z HUD
           p.setProperty (kDetached, false, nullptr);
           p.setProperty (kWnd, juce::Rectangle<int>(200,200,600,180).toString(), nullptr);

           // Default width ratio: equal distribution (will be normalized by layout)
           // Start with 1.0 - layout will normalize based on number of modules
           p.setProperty (kWidthRatio, 1.0f, nullptr);

           // Default rotation: 0 (no rotation)
           p.setProperty (kRotationAngle, 0, nullptr);

           

           // defaulty podľa typu
            if (type == "rms" || type == "audiometer")
            {
                p.setProperty (kSmooth, 0.5f, nullptr);
                p.setProperty (kRotationAngle, 0, nullptr);       // Default: 0° (no rotation)
                p.setProperty (kPreferredWidth, 80, nullptr);     // Fixed thin width in pixels
                p.setProperty (kWidthRatio, 0.15f, nullptr);      // Fallback ratio if preferred not used
                p.setProperty (kMeasureState, 0, nullptr);        // Trend: idle by default
                p.setProperty (kMeterView,    0, nullptr);        // 0=Momentary, 1=Trend
            }
           else if (type == "spectrum")
           {
               p.setProperty (kSmooth, 0.5f, nullptr);
               p.setProperty (kAWeight, false, nullptr);
               p.setProperty (kBins, 2048, nullptr);
               p.setProperty (kRenderPoints, 1024, nullptr);
               p.setProperty (kMeasurementMode, 0, nullptr);   // 0=Visual, 1=Measurement
               p.setProperty (kPeakHold, false, nullptr);       // Peak hold off by default
               p.setProperty (kPsychoCurve, 0, nullptr);        // 0=Flat, 1=A-weight, 2=ISO226
               p.setProperty (kPhon, 60, nullptr);              // ISO226 phon level
           }
           else if (type == "oscillator" || type == "oscilator")
           {
               p.setProperty (kSmooth, 0.5f, nullptr);
               p.setProperty (kNeon, false, nullptr);
               p.setProperty (kDisplayMode, 0, nullptr);     // 0=Mono, 1=Stereo, 2=Mirror
               p.setProperty (kZoom, 0.420f, nullptr);        // 0.0=85ms .. 1.0=0.1ms, default ~5ms
           }
           else if (type == "chladni")
           {
               p.setProperty (kChladniM,        2,     nullptr);  // m=2,n=3 → classic first pattern
               p.setProperty (kChladniN,        3,     nullptr);
               p.setProperty (kChladniAR,       1.0f,  nullptr);  // square plate
               p.setProperty (kChladniSharp,    0.5f,  nullptr);
               p.setProperty (kChladniMaterial, 0,     nullptr);  // Aluminium
               p.setProperty (kPreferredWidth,  200,   nullptr);  // squarish default
               p.setProperty (kWidthRatio,      0.25f, nullptr);
           }
           else if (type == "midi")
           {
               p.setProperty (kPreferredWidth,  300,   nullptr);  // Default width
               p.setProperty (kWidthRatio,      0.25f, nullptr);  // Proportional fallback
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

       // Vymaže panel podľa ID (true ak zmazal)
       bool removePanelById (int id)
       {
           auto panels = getPanelsRoot();
           for (int i = 0; i < panels.getNumChildren(); ++i)
           {
               auto p = panels.getChild (i);
               if ((int) p.getProperty (kId) == id)
               {
                   panels.removeChild (i, nullptr);
                   return true;
               }
           }
           return false;
       }

private:
    juce::ValueTree state;
};
