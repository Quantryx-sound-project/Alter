/*
    AlterLicense.h
    ------------------------------------------------------------------
    Licencna vrstva pre Alter. POUZIVA JU LEN APLIKACIA.

    AlterCreator ani AlterListener tento subor nepotrebuju a nemaju ho
    zaradeny v .jucer. Pluginy vzdy posielaju vsetko; ci sa data spracuju,
    rozhoduje appka pri prijme (pozri AlterTier.h -> acceptsInstanceMode).

    ZAVISLOSTI: len juce_core + juce_events (oba uz su v Alter.jucer).
    SHA256/HMAC je v AlterSHA256.h — juce_cryptography NIE je potrebne.
*/

#pragma once

#include "AlterTier.h"
#include "AlterSHA256.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>
#include <cstdint>

namespace alter
{

//==============================================================================
// STAV LICENCIE
//------------------------------------------------------------------------------
// Konfiguracia (store ID, varianty, pepper, URL) je v AlterLicenseConfig.h.
//==============================================================================

/** license_key.status z Lemon Squeezy. */
enum class KeyStatus
{
    Unknown = 0,
    Inactive,     // platny, ale bez aktivacii
    Active,
    Expired,      // uplynula platnost alebo skoncilo predplatne
    Disabled      // rucne vypnuty v LS dashboarde
};

KeyStatus keyStatusFromString (const juce::String&) noexcept;
const char* keyStatusToString (KeyStatus) noexcept;

struct LicenseState
{
    Tier          tier            = Tier::Demo;
    juce::String  licenseKey;
    juce::String  instanceId;         // Lemon Squeezy license key instance
    juce::String  customerEmail;
    juce::String  productName;
    juce::String  variantName;
    juce::int64   variantId       = 0;
    juce::int64   storeId         = 0;
    juce::int64   productId       = 0;
    KeyStatus     status          = KeyStatus::Unknown;
    juce::Time    lastValidated;      // posledna USPESNA online validacia
    juce::Time    expiresAt;          // 0 = nikdy (lifetime)
    int           activationLimit = 0;
    int           activationUsage = 0;
    bool          activated       = false;

    bool isDemo() const noexcept  { return tier == Tier::Demo || ! activated; }
    int  daysSinceValidation() const noexcept;
    bool isExpired() const noexcept;

    /** Kolko slotov ostava. -1 ak je limit neobmedzeny/nezname. */
    int slotsLeft() const noexcept
    {
        return activationLimit > 0 ? juce::jmax (0, activationLimit - activationUsage) : -1;
    }

    juce::var toVar() const;
    static LicenseState fromVar (const juce::var&);
};

//==============================================================================
// ULOZISKO
//------------------------------------------------------------------------------
// Windows: %APPDATA%\Alter\license.dat
// macOS:   ~/Library/Application Support/Alter/license.dat
//
// Per-user: ziadny admin nie je potrebny na zapis, takze instalacka nemusi
// riesit prava a app nemusi bezat elevated.
//==============================================================================

class LicenseStore
{
public:
    static juce::File getLicenseFile();
    static juce::File getDataFolder();

    /** Nacita a overi HMAC + machine binding. Pri nezhode vrati Demo. */
    static LicenseState load();

    static bool save (const LicenseState&);
    static void clear();

    /** Stabilny per-machine identifikator. */
    static juce::String machineId();

private:
    static juce::String sign (const juce::String& payload);
};

//==============================================================================
// MANAZER
//==============================================================================

class LicenseManager : private juce::Timer
{
public:
    LicenseManager();
    ~LicenseManager() override;

    /** Preco aktivacia/validacia zlyhala. UI podla toho vie ponuknut
        spravnu akciu (kupit / deaktivovat / skusit neskor / napisat support). */
    enum class Failure
    {
        None = 0,
        EmptyKey,
        Network,            // offline, timeout, DNS — skus neskor
        InvalidKey,         // LS nepozna tento kluc
        ActivationLimit,    // vycerpane sloty -> ponukni deaktivaciu
        Expired,            // uplynula platnost / skoncilo predplatne
        Disabled,           // rucne vypnuty v LS dashboarde
        WrongStore,         // platny kluc, ale z INEHO Lemon Squeezy obchodu
        UnknownVariant,     // nas obchod, ale variant nie je v kVariantMap
        NotConfigured,      // kVariantMap/kStoreId nie su vyplnene
        EmailMismatch
    };

    struct Result
    {
        bool         ok = false;
        Failure      failure = Failure::None;
        juce::String error;          // hotovy text pre pouzivatela
        LicenseState state;

        /** Pri UnknownVariant: ID, ktore treba doplnit do kVariantMap. */
        juce::int64  unknownVariantId = 0;

        /** Da sa to vyriesit zopakovanim? (siet) */
        bool isTransient() const noexcept { return failure == Failure::Network; }

        /** Ma UI ukazat tlacidlo "Deaktivovat ine zariadenie"? */
        bool suggestsDeactivate() const noexcept { return failure == Failure::ActivationLimit; }

        /** Ma UI ukazat tlacidlo "Kupit / obnovit"? */
        bool suggestsPurchase() const noexcept
        {
            return failure == Failure::Expired || failure == Failure::Disabled
                || failure == Failure::WrongStore;
        }
    };

    using Callback = std::function<void (Result)>;

    /** Aktualny stav — vzdy dostupny okamzite, nikdy neblokuje. */
    const LicenseState& state() const noexcept  { return current; }
    Tier tier() const noexcept                  { return effectiveTier; }

    bool   has (Feature f) const noexcept       { return tierHas (effectiveTier, f); }
    Limits limits() const noexcept              { return limitsFor (effectiveTier); }

    /** GATE NA PLUGIN INSTANCIE.
        Vola sa v UdpReceiver::effectiveInstance() a pri 'C' control paketoch.
        Pluginy posielaju vzdy vsetko — toto je jedine miesto, kde sa
        rozhoduje, ci sa to spracuje.

        @param instanceMode  UdpReceiver::InstanceRecord::mode (1=Creator, 0=Listener)

        Realtime-safe: cita jeden atomik, ziadna alokacia, ziadny lock. */
    bool acceptsInstance (int instanceMode) const noexcept
    {
        return acceptsInstanceMode ((Tier) gateTier.load (std::memory_order_relaxed),
                                    instanceMode);
    }

    /** Aktivacia kluca. Bezi na pozadi, callback na message threade. */
    void activateAsync (const juce::String& licenseKey, Callback);

    /** Revalidacia. Vola sa automaticky, ale da sa vynutit tlacidlom. */
    void validateAsync (Callback = {});

    /** Uvolni slot v Lemon Squeezy (pri prechode na iny pocitac). */
    void deactivateAsync (Callback);

    /** Otvori LS obchod. Bez argumentu hlavnu stranku. */
    static void openStore (const juce::String& checkoutSlug = {});

    /** Otvori priamy checkout pre konkretny plan (slugy su v configu). */
    static void openCheckoutFor (Tier t);

    /** Otvori LS customer portal (sprava predplatneho, faktury). */
    static void openCustomerPortal();

    /** Je licencna vrstva vobec nakonfigurovana? Ak nie, UI ma povedat
        "nie je nakonfigurovane" a nie "neplatny kluc". */
    static bool isConfigured() noexcept;

    /** Zavola sa po kazdej zmene tieru — sem zaves refresh UI. */
    std::function<void()> onStateChanged;

private:
    void timerCallback() override;
    void applyState (const LicenseState&);
    void recomputeEffectiveTier();

    LicenseState current;
    Tier         effectiveTier = Tier::Demo;

    // Kopia effectiveTier pre acceptsInstance(): cita sa aj z audio a socket
    // threadu, takze musi byt atomicka.
    std::atomic<int> gateTier { (int) Tier::Demo };

    struct Task;
    juce::CriticalSection lock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicenseManager)
};

} // namespace alter
