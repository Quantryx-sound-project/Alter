/*
    AlterTier.h
    ------------------------------------------------------------------
    Jediny zdroj pravdy pre feature matrix Alter.
    Zdielane medzi Alter (app), AlterCreator (VST3) a AlterListener (VST3).

    Ciste C++17, ziadne JUCE zavislosti -> da sa unit-testovat samostatne:
        g++ -std=c++17 -I../Shared Tests/test_tier.cpp -o test_tier

    Nazvy modulov zodpovedaju realnym suborom v Alter/Source/.
*/

#pragma once

#include "AlterLicenseConfig.h"
#include <cstdint>
#include <string>

namespace alter
{

//==============================================================================
// TIER
//==============================================================================

enum class Tier : int
{
    Demo     = 0,
    Listener = 1,
    Creator  = 2,
    Pro      = 3
};

inline const char* tierName (Tier t) noexcept
{
    switch (t)
    {
        case Tier::Listener: return "Listener";
        case Tier::Creator:  return "Creator";
        case Tier::Pro:      return "Pro";
        case Tier::Demo:
        default:             return "Demo";
    }
}

inline Tier tierFromName (const std::string& s) noexcept
{
    if (s == "Listener") return Tier::Listener;
    if (s == "Creator")  return Tier::Creator;
    if (s == "Pro")      return Tier::Pro;
    return Tier::Demo;
}

//==============================================================================
// MAPOVANIE LEMON SQUEEZY variant_id -> TIER
//------------------------------------------------------------------------------
// Lemon Squeezy vracia v kazdej activate/validate odpovedi meta.variant_id.
// To je JEDINA vec, ktora urcuje tier. Nikdy neparsuj nazvy produktov —
// nazvy menis pri marketingovych zmenach, ID nie.
//
// Samotna tabulka je v AlterLicenseConfig.h — tam sa vyplna vsetko,
// co zavisi od konkretneho Lemon Squeezy obchodu.
//==============================================================================

inline Tier tierForVariant (std::int64_t variantId) noexcept
{
    if (variantId == 0)
        return Tier::Demo;

    for (const auto& v : config::kVariantMap)
        if (v.variantId == variantId && v.variantId != 0)
            return (Tier) (int) v.tier;

    // Neznamy variant = pridal si produkt v LS bez update buildu.
    // Fail-safe smerom NADOL, nikdy nahor.
    return Tier::Demo;
}

/** Je vobec nejaky variant nakonfigurovany? Pouziva sa na diagnosticku
    hlasku "aplikacia nie je nakonfigurovana" namiesto "neplatny kluc". */
inline bool hasConfiguredVariants() noexcept
{
    for (const auto& v : config::kVariantMap)
        if (v.variantId != 0)
            return true;

    return false;
}

/** Kontrola, ze kluc patri TVOJMU obchodu a produktu.
    Bez nej odomkne Alter hocijaky platny kluc z celeho Lemon Squeezy. */
inline bool belongsToUs (std::int64_t storeId, std::int64_t productId) noexcept
{
    if (config::kStoreId != 0)
    {
        const bool storeOk = (storeId == config::kStoreId)
                          || (config::kStoreIdAlt != 0 && storeId == config::kStoreIdAlt);

        if (! storeOk)
            return false;
    }

    if (config::kProductId != 0 && productId != config::kProductId)
        return false;

    return true;
}

//==============================================================================
// FEATURES
//==============================================================================

enum class Feature : int
{
    // --- meracie moduly (Alter/Source/) ---
    ModAudioMeter = 0,    // AudioMeter.h
    ModSpectrum,          // Spectrum.h
    ModOscilloscope,      // Oscilator.h
    ModSpectrogram,       // Spectrogram.h
    ModStereoscope,       // Stereoscope.h
    ModToneAnalyzer,      // ToneAnalyzer.h

    // --- kreativne moduly ---
    ModSynesthesia,       // Synesthesia.h    (k premenovaniu — pozri poznamku nizsie)
    ModChladni,           // ChladniPaterns.h
    ModGeometry,          // GeometryVisual.h
    ModAlchemy,           // AlchemyVisual.h  — LEN PRO (jediny exkluzivny modul)

    // --- funkcie ---
    SavePreset,
    RecordClean,          // Record BEZ watermarku
    WindowControls,       // always-on-top / hold / hide info
    Themes,
    PerModuleSourcePicker,
    FftBinSettings,
    DawAutomation,
    ProMeteringDepth,     // LUFS, True Peak, level history, SPAN, constant-Q, korelacia

    // --- audio vstupy ---
    InputSystemAudio,
    InputCreatorPlugin,
    InputListenerPlugin,

    NumFeatures
};

// POZNAMKA K NAZVU "Synesthesia":
// synesthesia.live je etablovany VJ softver v presne tvojom segmente.
// Vygooglenie "alter synesthesia" posle zakaznika ku konkurencii.
// Premenovanie je refaktor cez Synesthesia.h, InfoWindowSynesthesia.h,
// SynShader.h a zaznamy v Alter.jucer — urob ho, kym nemas pouzivatelov
// s ulozenymi presetmi. Tu staci premenovat jeden enum.

//==============================================================================
// FEATURE MATRIX
//------------------------------------------------------------------------------
// Bitmaska na tier. Jedna tabulka — ziadne if-y roztrusene po MainComponent.cpp.
//==============================================================================

inline constexpr std::uint64_t bit (Feature f) noexcept
{
    return std::uint64_t (1) << static_cast<int> (f);
}

static_assert (static_cast<int> (Feature::NumFeatures) <= 64,
               "Feature sa uz nezmesti do 64-bitovej masky — zmen na std::bitset");

inline constexpr std::uint64_t kDemoMask =
      bit (Feature::ModAudioMeter)
    | bit (Feature::ModSpectrum)
    | bit (Feature::ModOscilloscope)
    | bit (Feature::InputSystemAudio);
    // Record je v deme povoleny, ale RecordClean NIE -> watermark.
    // Destroy a Explore su vzdy dostupne, nemaju feature bit.

inline constexpr std::uint64_t kListenerMask =
      bit (Feature::ModAudioMeter)
    | bit (Feature::ModSpectrum)
    | bit (Feature::ModOscilloscope)
    | bit (Feature::ModSpectrogram)
    | bit (Feature::ModStereoscope)
    | bit (Feature::ModToneAnalyzer)
    | bit (Feature::SavePreset)
    | bit (Feature::RecordClean)
    | bit (Feature::WindowControls)
    | bit (Feature::Themes)
    | bit (Feature::PerModuleSourcePicker)
    | bit (Feature::FftBinSettings)
    | bit (Feature::ProMeteringDepth)
    | bit (Feature::InputSystemAudio)
    | bit (Feature::InputListenerPlugin);

inline constexpr std::uint64_t kCreatorMask =
      bit (Feature::ModAudioMeter)
    | bit (Feature::ModSpectrum)
    | bit (Feature::ModOscilloscope)
    | bit (Feature::ModSynesthesia)
    | bit (Feature::ModChladni)
    | bit (Feature::ModGeometry)
    | bit (Feature::SavePreset)
    | bit (Feature::RecordClean)
    | bit (Feature::WindowControls)
    | bit (Feature::Themes)
    | bit (Feature::PerModuleSourcePicker)
    | bit (Feature::FftBinSettings)
    | bit (Feature::DawAutomation)
    | bit (Feature::InputSystemAudio)
    | bit (Feature::InputCreatorPlugin);
    // POZOR: Creator NEMA ProMeteringDepth — zdielane moduly bezia v standardnom rezime.

// Exkluzivne pre Pro: nie je to ani v Listener, ani v Creator.
// Jediny dovod, preco si niekto kupi Pro namiesto Creator+Listener zvlast.
inline constexpr std::uint64_t kProOnlyMask =
      bit (Feature::ModAlchemy);

inline constexpr std::uint64_t kProMask = kListenerMask | kCreatorMask | kProOnlyMask;

inline constexpr std::uint64_t tierMask (Tier t) noexcept
{
    switch (t)
    {
        case Tier::Listener: return kListenerMask;
        case Tier::Creator:  return kCreatorMask;
        case Tier::Pro:      return kProMask;
        case Tier::Demo:
        default:             return kDemoMask;
    }
}

inline constexpr bool tierHas (Tier t, Feature f) noexcept
{
    return (tierMask (t) & bit (f)) != 0;
}

//==============================================================================
// KVANTITATIVNE LIMITY
//==============================================================================

struct Limits
{
    int  maxModules;          // -1 = neobmedzene
    int  maxBlocks;
    int  maxModulesPerBlock;
    bool watermarkPreview;
    bool watermarkExport;
};

inline constexpr Limits limitsFor (Tier t) noexcept
{
    if (t == Tier::Demo)
        return Limits { 2, 3, 3, true, true };

    return Limits { -1, 3, 3, false, false };
}

//==============================================================================
// GATE NA PLUGIN INSTANCIE
//------------------------------------------------------------------------------
// DOLEZITE: pluginy same NIE SU viazane na licenciu. AlterCreator aj
// AlterListener vzdy posielaju vsetko, bez ohladu na to, co si zakaznik kupil.
// Nemaju v sebe ziadny licencny kod a nepotrebuju vediet o Lemon Squeezy.
//
// Filtrovanie robi VYLUCNE aplikacia, ked data prijima. Vyhody:
//   - jedno miesto, kde sa rozhoduje  ->  jedno miesto, kde sa da pomylit
//   - pluginy netreba pri zmene planov vobec prebuildovat
//   - ziadny licencny stav v DAW procese, ziadna siet v audio plugine
//   - po aktivacii licencie netreba restartovat DAW: appka jednoducho
//     zacne prijate data prepustat dalej
//
// Rozlisenie instancii: UdpReceiver::InstanceRecord::mode
//   mode == 1  ->  AlterCreator  (Control: PluginProcessor.cpp riadok ~321)
//   mode == 0  ->  AlterListener (ciste streamovanie audia)
//==============================================================================

enum class PluginKind : std::uint8_t { Creator = 0, Listener = 1 };

inline bool pluginAllowed (Tier t, PluginKind k) noexcept
{
    return k == PluginKind::Creator
             ? tierHas (t, Feature::InputCreatorPlugin)
             : tierHas (t, Feature::InputListenerPlugin);
}

/** Preklad UdpReceiver mode -> druh pluginu. */
inline PluginKind pluginKindFromMode (int instanceMode) noexcept
{
    return instanceMode == 1 ? PluginKind::Creator : PluginKind::Listener;
}

/** Smie appka konzumovat data z tejto instancie?
    Vola sa v UdpReceiver::effectiveInstance() a pri 'C' control paketoch. */
inline bool acceptsInstanceMode (Tier t, int instanceMode) noexcept
{
    return pluginAllowed (t, pluginKindFromMode (instanceMode));
}

//==============================================================================
// UPSELL — co ukazat pri zamknutej feature
//==============================================================================

inline const char* lowestTierWith (Feature f) noexcept
{
    if (tierHas (Tier::Demo,     f)) return "Demo";
    if (tierHas (Tier::Listener, f)) return "Listener";
    if (tierHas (Tier::Creator,  f)) return "Creator";
    if (tierHas (Tier::Pro,      f)) return "Pro";
    return "Pro";
}

} // namespace alter
