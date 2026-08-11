/*
    test_tier.cpp — overuje, ze AlterTier.h sedi so strategiou.
    Ziadne JUCE, ziadny framework.

        cd Shared/Tests
        g++ -std=c++17 -Wall -Wextra -I.. test_tier.cpp -o test_tier && ./test_tier
*/

#include "../AlterTier.h"
#include "../AlterSHA256.h"
#include <cstdio>

using namespace alter;

static int failures = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) { std::printf ("  FAIL: %s\n", msg); ++failures; }   \
    } while (0)

static void expectFeature (Tier t, Feature f, bool expected, const char* label)
{
    if (tierHas (t, f) != expected)
    {
        std::printf ("  FAIL: %-10s %-16s ocakavane=%d skutocne=%d\n",
                     tierName (t), label, (int) expected, (int) tierHas (t, f));
        ++failures;
    }
}

int main()
{
    std::printf ("Alter — licencna vrstva\n=======================\n\n[1] Feature matrix\n");

    // --- moduly zdielane vsetkymi planmi ------------------------------------
    for (auto t : { Tier::Demo, Tier::Listener, Tier::Creator, Tier::Pro })
    {
        expectFeature (t, Feature::ModAudioMeter,    true, "AudioMeter");
        expectFeature (t, Feature::ModSpectrum,      true, "Spectrum");
        expectFeature (t, Feature::ModOscilloscope,  true, "Oscilloscope");
        expectFeature (t, Feature::InputSystemAudio, true, "SystemAudio");
    }

    // --- moduly rezervovane pre Listener/Pro -------------------------------
    for (auto f : { Feature::ModSpectrogram, Feature::ModStereoscope, Feature::ModToneAnalyzer })
    {
        expectFeature (Tier::Demo,     f, false, "listener-only");
        expectFeature (Tier::Creator,  f, false, "listener-only");
        expectFeature (Tier::Listener, f, true,  "listener-only");
        expectFeature (Tier::Pro,      f, true,  "listener-only");
    }

    // --- kreativne moduly: Creator/Pro -------------------------------------
    for (auto f : { Feature::ModSynesthesia, Feature::ModChladni, Feature::ModGeometry })
    {
        expectFeature (Tier::Demo,     f, false, "creator-only");
        expectFeature (Tier::Listener, f, false, "creator-only");
        expectFeature (Tier::Creator,  f, true,  "creator-only");
        expectFeature (Tier::Pro,      f, true,  "creator-only");
    }

    // --- Alchemy: EXKLUZIVNE PRE PRO ---------------------------------------
    expectFeature (Tier::Demo,     Feature::ModAlchemy, false, "Alchemy");
    expectFeature (Tier::Listener, Feature::ModAlchemy, false, "Alchemy");
    expectFeature (Tier::Creator,  Feature::ModAlchemy, false, "Alchemy");
    expectFeature (Tier::Pro,      Feature::ModAlchemy, true,  "Alchemy");
    CHECK (std::string (lowestTierWith (Feature::ModAlchemy)) == "Pro",
           "Alchemy upsell ukazuje Pro");

    // --- pro metering depth: Listener/Pro, NIE Creator ---------------------
    expectFeature (Tier::Demo,     Feature::ProMeteringDepth, false, "ProMetering");
    expectFeature (Tier::Creator,  Feature::ProMeteringDepth, false, "ProMetering");
    expectFeature (Tier::Listener, Feature::ProMeteringDepth, true,  "ProMetering");
    expectFeature (Tier::Pro,      Feature::ProMeteringDepth, true,  "ProMetering");

    // --- DAW automatizacia: Creator/Pro ------------------------------------
    expectFeature (Tier::Demo,     Feature::DawAutomation, false, "DawAutomation");
    expectFeature (Tier::Listener, Feature::DawAutomation, false, "DawAutomation");
    expectFeature (Tier::Creator,  Feature::DawAutomation, true,  "DawAutomation");
    expectFeature (Tier::Pro,      Feature::DawAutomation, true,  "DawAutomation");

    // --- funkcie zamknute v Deme -------------------------------------------
    for (auto f : { Feature::SavePreset, Feature::RecordClean, Feature::WindowControls,
                    Feature::Themes, Feature::PerModuleSourcePicker, Feature::FftBinSettings })
    {
        expectFeature (Tier::Demo,     f, false, "demo-locked");
        expectFeature (Tier::Listener, f, true,  "demo-locked");
        expectFeature (Tier::Creator,  f, true,  "demo-locked");
        expectFeature (Tier::Pro,      f, true,  "demo-locked");
    }

    std::printf ("\n[2] Ktory plugin ide ku ktoremu planu\n");
    {
        // Demo      -> ziadny plugin
        // Listener  -> AlterListener
        // Creator   -> AlterCreator
        // Pro       -> oba
        struct Row { Tier t; bool creator; bool listener; const char* label; };
        constexpr Row rows[] =
        {
            { Tier::Demo,     false, false, "Demo      ziadny plugin" },
            { Tier::Listener, false, true,  "Listener  len AlterListener" },
            { Tier::Creator,  true,  false, "Creator   len AlterCreator" },
            { Tier::Pro,      true,  true,  "Pro       oba pluginy" },
        };

        for (const auto& r : rows)
        {
            const bool c = pluginAllowed (r.t, PluginKind::Creator);
            const bool l = pluginAllowed (r.t, PluginKind::Listener);

            if (c != r.creator || l != r.listener)
            {
                std::printf ("  FAIL: %s (creator=%d ocak %d, listener=%d ocak %d)\n",
                             r.label, (int) c, (int) r.creator, (int) l, (int) r.listener);
                ++failures;
            }
            else
            {
                std::printf ("        %s\n", r.label);
            }

            // gate cez UdpReceiver mode musi davat to iste
            CHECK (acceptsInstanceMode (r.t, 1) == r.creator,  "gate mode=1 sedi s pluginAllowed");
            CHECK (acceptsInstanceMode (r.t, 0) == r.listener, "gate mode=0 sedi s pluginAllowed");
        }
    }

    CHECK (pluginKindFromMode (1) == PluginKind::Creator,  "mode 1 -> Creator");
    CHECK (pluginKindFromMode (0) == PluginKind::Listener, "mode 0 -> Listener");
    // neznamy mode z buducej verzie pluginu nesmie odomknut Creator vstup
    CHECK (pluginKindFromMode (7) == PluginKind::Listener, "neznamy mode -> Listener (fail-safe)");
    CHECK (! acceptsInstanceMode (Tier::Creator, 7),       "neznamy mode neprejde cez Creator");

    std::printf ("\n[3] Limity\n");
    CHECK (limitsFor (Tier::Demo).maxModules == 2,      "Demo max 2 moduly");
    CHECK (limitsFor (Tier::Demo).watermarkExport,      "Demo export ma watermark");
    CHECK (limitsFor (Tier::Demo).watermarkPreview,     "Demo preview ma watermark");
    CHECK (limitsFor (Tier::Pro).maxModules == -1,      "Pro neobmedzene");
    CHECK (! limitsFor (Tier::Creator).watermarkExport, "Creator export cisty");
    CHECK (limitsFor (Tier::Listener).maxBlocks == 3,   "3 bloky vsade");

    std::printf ("\n[4] Pro je nadmnozina + ma nieco navyse\n");
    CHECK ((kProMask & kListenerMask) == kListenerMask, "Pro obsahuje vsetko z Listener");
    CHECK ((kProMask & kCreatorMask)  == kCreatorMask,  "Pro obsahuje vsetko z Creator");
    CHECK ((kProMask & kDemoMask)     == kDemoMask,     "Pro obsahuje vsetko z Demo");
    CHECK (kProMask != (kListenerMask | kCreatorMask),
           "Pro ma nieco, co Creator+Listener nemaju (Alchemy)");
    CHECK ((kProOnlyMask & kCreatorMask) == 0,  "kProOnlyMask sa neprekryva s Creator");
    CHECK ((kProOnlyMask & kListenerMask) == 0, "kProOnlyMask sa neprekryva s Listener");

    std::printf ("\n[5] Lemon Squeezy konfiguracia\n");
    {
        // Fail-safe vetvy platia vzdy, aj s nevyplnenym configom.
        CHECK (tierForVariant (0)          == Tier::Demo, "variant 0 -> Demo");
        CHECK (tierForVariant (999999999)  == Tier::Demo, "neznamy variant -> Demo (fail-safe)");

        if (! hasConfiguredVariants())
        {
            std::printf ("        %s\n", "kVariantMap este nie je vyplneny.");
            std::printf ("        %s\n", "Spusti: python ls_check.py activate <KLUC>");
            // Nevyplneny config nie je chyba testu — je to stav pred spustenim.
            // Ale MUSI platit, ze nic neodomkne.
            for (const auto& v : config::kVariantMap)
                CHECK (tierForVariant (v.variantId) == Tier::Demo,
                       "nevyplneny variant nesmie nic odomknut");
        }
        else
        {
            int n = 0;
            for (const auto& v : config::kVariantMap)
            {
                if (v.variantId == 0)
                    continue;

                ++n;
                CHECK (tierForVariant (v.variantId) == (Tier) (int) v.tier,
                       "variant sa mapuje na svoj tier");
                CHECK ((Tier) (int) v.tier != Tier::Demo,
                       "ziadny plateny variant nesmie mapovat na Demo");
            }
            std::printf ("        %d nakonfigurovanych variantov\n", n);
        }

        // store gate
        if (config::kStoreId != 0)
        {
            CHECK (  belongsToUs (config::kStoreId, config::kProductId), "nas obchod prejde");
            CHECK (! belongsToUs (config::kStoreId + 1, config::kProductId),
                   "cudzi obchod NEPREJDE");
            std::printf ("        kStoreId = %lld\n", (long long) config::kStoreId);
        }
        else
        {
            std::printf ("        %s\n",
                "kStoreId = 0  -> kontrola obchodu VYPNUTA (release build sa nezkompiluje)");
        }

        CHECK (config::kCachePepper[0] != '\0',            "pepper nie je prazdny");
        CHECK (std::string (config::kCachePepper).find ("CHANGE_ME") == std::string::npos,
               "pepper uz nie je placeholder");
        CHECK (std::string (config::kCachePepper).size() >= 32, "pepper ma dost entropie");
        CHECK (config::kOfflineGraceDays > config::kRevalidateEveryDays,
               "grace musi byt dlhsia nez interval revalidacie");
    }

    std::printf ("\n[6] Upsell / nazvy\n");
    CHECK (std::string (lowestTierWith (Feature::ModSpectrogram)) == "Listener", "upsell Spectrogram");
    CHECK (std::string (lowestTierWith (Feature::ModChladni))     == "Creator",  "upsell Chladni");
    CHECK (std::string (lowestTierWith (Feature::ModAudioMeter))  == "Demo",     "upsell AudioMeter");
    CHECK (std::string (lowestTierWith (Feature::SavePreset))     == "Listener", "upsell SavePreset");

    for (auto t : { Tier::Demo, Tier::Listener, Tier::Creator, Tier::Pro })
        CHECK (tierFromName (tierName (t)) == t, "tier name roundtrip");

    std::printf ("\n[7] Krypto\n");
    {
        using namespace alter::crypto;
        // NIST FIPS 180-2
        CHECK (sha256Hex ("abc")
                 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "SHA-256(\"abc\")");
        CHECK (sha256Hex ("")
                 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "SHA-256(\"\")");
        // RFC 4231
        CHECK (hmacSha256Hex (std::string (20, '\x0b'), "Hi There")
                 == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
               "HMAC-SHA256 RFC 4231 #1");
        CHECK (hmacSha256Hex ("Jefe", "what do ya want for nothing?")
                 == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
               "HMAC-SHA256 RFC 4231 #2");

        CHECK (  constantTimeEquals ("abc", "abc"), "constantTimeEquals zhoda");
        CHECK (! constantTimeEquals ("abc", "abd"), "constantTimeEquals nezhoda");
        CHECK (! constantTimeEquals ("abc", "ab"),  "constantTimeEquals dlzka");
    }

    std::printf ("\n=======================\n");

    if (failures == 0)
        std::printf ("VSETKO OK\n");
    else
        std::printf ("%d chyb(y)\n", failures);

    return failures == 0 ? 0 : 1;
}
