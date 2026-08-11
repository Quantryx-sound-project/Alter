/*
    AlterLicense.cpp
*/

#include "AlterLicense.h"

namespace alter
{

//==============================================================================
// KeyStatus
//==============================================================================

KeyStatus keyStatusFromString (const juce::String& s) noexcept
{
    if (s.equalsIgnoreCase ("active"))   return KeyStatus::Active;
    if (s.equalsIgnoreCase ("inactive")) return KeyStatus::Inactive;
    if (s.equalsIgnoreCase ("expired"))  return KeyStatus::Expired;
    if (s.equalsIgnoreCase ("disabled")) return KeyStatus::Disabled;
    return KeyStatus::Unknown;
}

const char* keyStatusToString (KeyStatus s) noexcept
{
    switch (s)
    {
        case KeyStatus::Active:   return "active";
        case KeyStatus::Inactive: return "inactive";
        case KeyStatus::Expired:  return "expired";
        case KeyStatus::Disabled: return "disabled";
        case KeyStatus::Unknown:
        default:                  return "unknown";
    }
}

//==============================================================================
// LicenseState
//==============================================================================

int LicenseState::daysSinceValidation() const noexcept
{
    if (lastValidated.toMilliseconds() <= 0)
        return 99999;

    const auto delta = juce::Time::getCurrentTime() - lastValidated;
    return juce::jmax (0, (int) delta.inDays());
}

bool LicenseState::isExpired() const noexcept
{
    if (expiresAt.toMilliseconds() <= 0)
        return false;                       // lifetime

    return juce::Time::getCurrentTime() > expiresAt;
}

juce::var LicenseState::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("tier",            (int) tier);
    o->setProperty ("licenseKey",      licenseKey);
    o->setProperty ("instanceId",      instanceId);
    o->setProperty ("customerEmail",   customerEmail);
    o->setProperty ("productName",     productName);
    o->setProperty ("variantId",       variantId);
    o->setProperty ("variantName",     variantName);
    o->setProperty ("storeId",         storeId);
    o->setProperty ("productId",       productId);
    o->setProperty ("status",          juce::String (keyStatusToString (status)));
    o->setProperty ("lastValidated",   lastValidated.toMilliseconds());
    o->setProperty ("expiresAt",       expiresAt.toMilliseconds());
    o->setProperty ("activationLimit", activationLimit);
    o->setProperty ("activationUsage", activationUsage);
    o->setProperty ("activated",       activated);
    o->setProperty ("machine",         LicenseStore::machineId());
    o->setProperty ("schema",          1);
    return juce::var (o);
}

LicenseState LicenseState::fromVar (const juce::var& v)
{
    LicenseState s;

    if (auto* o = v.getDynamicObject())
    {
        s.tier            = (Tier) (int) o->getProperty ("tier");
        s.licenseKey      = o->getProperty ("licenseKey").toString();
        s.instanceId      = o->getProperty ("instanceId").toString();
        s.customerEmail   = o->getProperty ("customerEmail").toString();
        s.productName     = o->getProperty ("productName").toString();
        s.variantId       = (juce::int64) o->getProperty ("variantId");
        s.variantName     = o->getProperty ("variantName").toString();
        s.storeId         = (juce::int64) o->getProperty ("storeId");
        s.productId       = (juce::int64) o->getProperty ("productId");
        s.status          = keyStatusFromString (o->getProperty ("status").toString());
        s.lastValidated   = juce::Time ((juce::int64) o->getProperty ("lastValidated"));
        s.expiresAt       = juce::Time ((juce::int64) o->getProperty ("expiresAt"));
        s.activationLimit = (int) o->getProperty ("activationLimit");
        s.activationUsage = (int) o->getProperty ("activationUsage");
        s.activated       = (bool) o->getProperty ("activated");
    }

    return s;
}

//==============================================================================
// LicenseStore
//==============================================================================

juce::File LicenseStore::getDataFolder()
{
   #if JUCE_MAC
    // Na macOS vracia userApplicationDataDirectory ~/Library,
    // takze "Application Support" musime pridat sami.
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("Application Support");
   #else
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #endif

    auto folder = base.getChildFile (config::kAppFolderName);
    folder.createDirectory();
    return folder;
}

juce::File LicenseStore::getLicenseFile()
{
    return getDataFolder().getChildFile (config::kLicenseFile);
}

juce::String LicenseStore::machineId()
{
    // getUniqueDeviceID() je stabilny naprieč reinstalaciami aplikacii a
    // nemeni sa pri vymene sietovky (na rozdiel od MAC adresy).
    auto raw = juce::SystemStats::getUniqueDeviceID();

    if (raw.isEmpty())
        raw = juce::SystemStats::getComputerName() + juce::SystemStats::getLogonName();

    const std::string in (raw.toRawUTF8(), raw.getNumBytesAsUTF8());
    return juce::String (crypto::sha256Hex (in)).substring (0, 32);
}

juce::String LicenseStore::sign (const juce::String& payload)
{
    const juce::String keyStr = juce::String (config::kCachePepper) + machineId();

    const std::string key (keyStr.toRawUTF8(), keyStr.getNumBytesAsUTF8());
    const std::string msg (payload.toRawUTF8(), payload.getNumBytesAsUTF8());

    return juce::String (crypto::hmacSha256Hex (key, msg));
}

bool LicenseStore::save (const LicenseState& s)
{
    const auto payload = juce::JSON::toString (s.toVar(), true);

    auto* wrapper = new juce::DynamicObject();
    wrapper->setProperty ("d", payload);
    wrapper->setProperty ("s", sign (payload));

    const auto text = juce::Base64::toBase64 (juce::JSON::toString (juce::var (wrapper), true));

    return getLicenseFile().replaceWithText (text);
}

LicenseState LicenseStore::load()
{
    LicenseState demo;   // fail-safe navratova hodnota

    auto file = getLicenseFile();
    if (! file.existsAsFile())
        return demo;

    juce::MemoryOutputStream decoded;
    if (! juce::Base64::convertFromBase64 (decoded, file.loadFileAsString().trim()))
        return demo;

    const auto wrapper = juce::JSON::parse (decoded.toString());
    auto* o = wrapper.getDynamicObject();
    if (o == nullptr)
        return demo;

    const auto payload = o->getProperty ("d").toString();
    const auto sig     = o->getProperty ("s").toString();

    if (sig.isEmpty())
        return demo;

    // HMAC je viazany na machineId -> skopirovany subor na iny stroj neprejde.
    const auto expected = sign (payload);
    const std::string a (sig.toRawUTF8(), sig.getNumBytesAsUTF8());
    const std::string b (expected.toRawUTF8(), expected.getNumBytesAsUTF8());

    if (! crypto::constantTimeEquals (a, b))
        return demo;

    auto s = LicenseState::fromVar (juce::JSON::parse (payload));

    if (! s.activated || s.isExpired())
        return demo;

    if (s.status == KeyStatus::Disabled || s.status == KeyStatus::Expired)
        return demo;

    // Obrana proti podstrceniu cache z ineho LS obchodu a proti stavu ulozenemu
    // starsim buildom, ktory este nemal spravne kVariantMap.
    if (! belongsToUs (s.storeId, s.productId))
        return demo;

    if (tierForVariant (s.variantId) != s.tier)
        return demo;

    // Offline grace: po prekroceni padame na Demo, ale subor NEMAZEME —
    // staci sa pripojit a validateAsync() ho ozivi.
    if (s.daysSinceValidation() > config::kOfflineGraceDays)
    {
        auto degraded = s;
        degraded.tier = Tier::Demo;
        return degraded;
    }

    return s;
}

void LicenseStore::clear()
{
    getLicenseFile().deleteFile();
}

//==============================================================================
// LicenseManager
//==============================================================================

namespace
{
    struct HttpResult
    {
        bool         networkOk = false;
        juce::var    json;
        juce::String rawError;
    };

    HttpResult postForm (const juce::String& endpoint, const juce::StringPairArray& fields)
    {
        HttpResult r;

        juce::String body;
        for (int i = 0; i < fields.size(); ++i)
        {
            if (body.isNotEmpty())
                body << "&";

            body << fields.getAllKeys()[i]
                 << "="
                 << juce::URL::addEscapeChars (fields.getAllValues()[i], true);
        }

        juce::URL url (juce::String (config::kApiBase) + "/" + endpoint);
        url = url.withPOSTData (body);

        int statusCode = 0;
        juce::StringPairArray responseHeaders;

        // POZNAMKA: niektore verzie JUCE pridavaju Content-Type k POST datam samy.
        // Ak dostanes HTTP 400 s prazdnym telom, odstran riadok s Content-Type —
        // pravdepodobne sa hlavicka duplikuje.
        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                          .withExtraHeaders ("Accept: application/json\r\n"
                                             "Content-Type: application/x-www-form-urlencoded\r\n")
                          .withConnectionTimeoutMs (config::kHttpTimeoutMs)
                          .withStatusCode (&statusCode)
                          .withResponseHeaders (&responseHeaders)
                          .withNumRedirectsToFollow (3);

        std::unique_ptr<juce::InputStream> stream (url.createInputStream (options));

        if (stream == nullptr)
        {
            r.rawError = "Nepodarilo sa pripojit na server.";
            return r;
        }

        const auto text = stream->readEntireStreamAsString();
        r.networkOk = true;
        r.json      = juce::JSON::parse (text);

        // Lemon Squeezy vracia 400/404 s JSON telom pri neplatnom kluci —
        // to je legitimna odpoved, nie sietova chyba.
        if (! r.json.isObject())
        {
            r.networkOk = false;
            r.rawError  = "Neocakavana odpoved servera (HTTP " + juce::String (statusCode) + ").";
        }

        return r;
    }

    /** Naplni LicenseState z odpovede Lemon Squeezy. */
    void fillFromResponse (LicenseState& s, const juce::var& json)
    {
        auto* root = json.getDynamicObject();
        if (root == nullptr)
            return;

        // Drzime var v pomenovanej premennej — getDynamicObject() na docasnom
        // objekte by vratil ukazovatel na potencialne zruseny objekt.
        const juce::var lkVar   = root->getProperty ("license_key");
        const juce::var instVar = root->getProperty ("instance");
        const juce::var metaVar = root->getProperty ("meta");

        if (auto* lk = lkVar.getDynamicObject())
        {
            s.licenseKey      = lk->getProperty ("key").toString();
            s.activationLimit = (int) lk->getProperty ("activation_limit");
            s.activationUsage = (int) lk->getProperty ("activation_usage");
            s.status          = keyStatusFromString (lk->getProperty ("status").toString());

            const auto exp = lk->getProperty ("expires_at").toString();
            s.expiresAt = (exp.isEmpty() || exp == "null")
                            ? juce::Time (0)
                            : juce::Time::fromISO8601 (exp);
        }

        if (auto* inst = instVar.getDynamicObject())
            s.instanceId = inst->getProperty ("id").toString();

        if (auto* meta = metaVar.getDynamicObject())
        {
            s.storeId       = (juce::int64) meta->getProperty ("store_id");
            s.productId     = (juce::int64) meta->getProperty ("product_id");
            s.variantId     = (juce::int64) meta->getProperty ("variant_id");
            s.productName   = meta->getProperty ("product_name").toString();
            s.variantName   = meta->getProperty ("variant_name").toString();
            s.customerEmail = meta->getProperty ("customer_email").toString();
        }

        // TIER SA URCUJE VYLUCNE Z variant_id.
        s.tier = tierForVariant (s.variantId);
    }

    /** Spolocne kontroly pre activate aj validate.
        Vracia Failure::None, ak je vsetko v poriadku. */
    LicenseManager::Result checkResponse (const LicenseState& s)
    {
        using F = LicenseManager::Failure;
        LicenseManager::Result r;
        r.state = s;

        // --- 1. nie sme vobec nakonfigurovani? ---
        if (! hasConfiguredVariants())
        {
            r.failure = F::NotConfigured;
            r.error   = "Alter nie je nakonfigurovany pre licencie.\n"
                        "Chyba kVariantMap v AlterLicenseConfig.h.";
            return r;
        }

        // --- 2. patri kluc NAM? ---
        // Bez tejto kontroly odomkne Alter hocijaky platny kluc z celeho
        // Lemon Squeezy, aj z uplne ineho predajcu.
        if (! belongsToUs (s.storeId, s.productId))
        {
            r.failure = F::WrongStore;
            r.error   = "Tento licencny kluc nepatri k produktu Alter.";
            return r;
        }

        // --- 3. stav kluca ---
        if (s.status == KeyStatus::Disabled)
        {
            r.failure = F::Disabled;
            r.error   = juce::String ("Tento licencny kluc bol deaktivovany.\nNapis na ")
                          + config::kSupportEmail + ", ak si myslis, ze je to omyl.";
            return r;
        }

        if (s.status == KeyStatus::Expired || s.isExpired())
        {
            r.failure = F::Expired;
            r.error   = "Platnost licencie uplynula.\n"
                        "Ak mas predplatne, obnov ho v zakaznickom portali.";
            return r;
        }

        // --- 4. poznam tento variant? ---
        if (s.tier == Tier::Demo)
        {
            r.failure          = F::UnknownVariant;
            r.unknownVariantId = s.variantId;
            r.error = "Kluc je platny, ale tento produkt nepoznam.\n"
                      "Variant ID: " + juce::String (s.variantId)
                        + (s.variantName.isNotEmpty() ? " (" + s.variantName + ")" : "")
                        + "\nAktualizuj Alter na najnovsiu verziu.";

            // Do logu ide rovno riadok, ktory staci skopirovat do configu.
            DBG ("ALTER LICENCIA: neznamy variant. Doplnte do AlterLicenseConfig.h:");
            DBG ("    { " << s.variantId << ", TierId::???, \"" << s.variantName << "\" },");
            return r;
        }

        // --- 5. e-mail (volitelne) ---
        if (config::kRequireEmailMatch && s.customerEmail.isEmpty())
        {
            r.failure = F::EmailMismatch;
            r.error   = "Nepodarilo sa overit e-mail zakaznika.";
            return r;
        }

        r.ok = true;
        return r;
    }
}

//------------------------------------------------------------------------------

struct LicenseManager::Task
{
    static void run (std::function<Result()> work, Callback cb)
    {
        juce::Thread::launch ([work = std::move (work), cb = std::move (cb)]
        {
            auto result = work();

            if (cb)
                juce::MessageManager::callAsync ([cb, result] { cb (result); });
        });
    }
};

//------------------------------------------------------------------------------

LicenseManager::LicenseManager()
{
    current = LicenseStore::load();
    recomputeEffectiveTier();

    // Ticha revalidacia na pozadi po starte, ak je cas.
    if (current.activated && current.daysSinceValidation() >= config::kRevalidateEveryDays)
        validateAsync ({});

    startTimer (6 * 60 * 60 * 1000);   // kontrola kazdych 6 hodin
}

LicenseManager::~LicenseManager()
{
    stopTimer();
}

void LicenseManager::timerCallback()
{
    if (current.activated && current.daysSinceValidation() >= config::kRevalidateEveryDays)
        validateAsync ({});
}

void LicenseManager::recomputeEffectiveTier()
{
    if (! current.activated || current.isExpired())
        effectiveTier = Tier::Demo;
    else if (current.daysSinceValidation() > config::kOfflineGraceDays)
        effectiveTier = Tier::Demo;
    else
        effectiveTier = current.tier;

    // Gate cita socket aj audio thread -> publikuj cez atomik.
    gateTier.store ((int) effectiveTier, std::memory_order_relaxed);
}

void LicenseManager::applyState (const LicenseState& s)
{
    {
        const juce::ScopedLock sl (lock);
        current = s;
    }

    LicenseStore::save (s);
    recomputeEffectiveTier();

    if (onStateChanged)
        onStateChanged();
}

//------------------------------------------------------------------------------

void LicenseManager::activateAsync (const juce::String& licenseKey, Callback cb)
{
    const auto key = licenseKey.trim();

    if (key.isEmpty())
    {
        Result r;
        r.failure = Failure::EmptyKey;
        r.error   = "Zadaj licencny kluc.";
        r.state   = current;
        if (cb) cb (r);
        return;
    }

    if (! isConfigured())
    {
        Result r;
        r.failure = Failure::NotConfigured;
        r.error   = "Alter nie je nakonfigurovany pre licencie.\n"
                    "Chyba kStoreId / kVariantMap v AlterLicenseConfig.h.";
        r.state   = current;
        if (cb) cb (r);
        return;
    }

    // instance_name = ludsky citatelny nazov stroja + hash.
    // V LS dashboarde tak vidis, ktore zariadenia zabrali sloty, a vies
    // zakaznikovi poradit, ktore deaktivovat.
    const auto instanceName = juce::SystemStats::getComputerName()
                                + " (" + juce::SystemStats::getOperatingSystemName() + ") "
                                + LicenseStore::machineId().substring (0, 8);

    Task::run ([key, instanceName]() -> Result
    {
        juce::StringPairArray f;
        f.set ("license_key",   key);
        f.set ("instance_name", instanceName);

        auto http = postForm ("activate", f);

        if (! http.networkOk)
        {
            Result r;
            r.failure = Failure::Network;
            r.error   = http.rawError;
            return r;
        }

        auto* root = http.json.getDynamicObject();
        const bool activated = root != nullptr && (bool) root->getProperty ("activated");

        if (! activated)
        {
            const auto raw = root != nullptr ? root->getProperty ("error").toString()
                                             : juce::String();

            Result r;

            // Aj pri neuspechu vracia LS license_key + meta -> vieme povedat preco.
            LicenseState partial;
            fillFromResponse (partial, http.json);
            r.state = partial;

            if (raw.containsIgnoreCase ("activation limit"))
            {
                r.failure = Failure::ActivationLimit;
                r.error   = "Tento kluc uz vycerpal pocet aktivacii ("
                              + juce::String (partial.activationUsage) + "/"
                              + juce::String (partial.activationLimit) + ").\n\n"
                              "Deaktivuj Alter na inom pocitaci, alebo napis na "
                              + config::kSupportEmail + ".";
            }
            else if (raw.containsIgnoreCase ("disabled"))
            {
                r.failure = Failure::Disabled;
                r.error   = "Tento licencny kluc bol deaktivovany.";
            }
            else if (raw.containsIgnoreCase ("expired"))
            {
                r.failure = Failure::Expired;
                r.error   = "Platnost licencie uplynula.";
            }
            else
            {
                r.failure = Failure::InvalidKey;
                r.error   = raw.isEmpty()
                              ? "Tento licencny kluc neexistuje. Skontroluj, ci si ho "
                                "skopiroval cely."
                              : raw;
            }

            return r;
        }

        LicenseState s;
        fillFromResponse (s, http.json);
        s.activated     = true;
        s.lastValidated = juce::Time::getCurrentTime();

        // store_id / product_id / status / variant — vsetky kontroly naraz
        auto checked = checkResponse (s);

        // Kluc bol prave aktivovany a zaberá slot. Ak ho odmietame (cudzi obchod,
        // neznamy variant), slot musime vratit — inak zakaznikovi tichnu sloty.
        if (! checked.ok && s.instanceId.isNotEmpty())
        {
            juce::StringPairArray d;
            d.set ("license_key", s.licenseKey);
            d.set ("instance_id", s.instanceId);
            postForm ("deactivate", d);
        }

        return checked;
    },
    [this, cb] (Result r)
    {
        if (r.ok)
            applyState (r.state);

        if (cb)
            cb (r);
    });
}

//------------------------------------------------------------------------------

void LicenseManager::validateAsync (Callback cb)
{
    LicenseState snapshot;
    {
        const juce::ScopedLock sl (lock);
        snapshot = current;
    }

    if (snapshot.licenseKey.isEmpty())
    {
        Result r;
        r.failure = Failure::EmptyKey;
        r.error   = "Ziadna licencia na overenie.";
        r.state   = snapshot;
        if (cb) cb (r);
        return;
    }

    Task::run ([snapshot]() -> Result
    {
        juce::StringPairArray f;
        f.set ("license_key", snapshot.licenseKey);

        if (snapshot.instanceId.isNotEmpty())
            f.set ("instance_id", snapshot.instanceId);

        auto http = postForm ("validate", f);

        // OFFLINE: nic nemenime, vraciame povodny stav. Grace period sa postara.
        if (! http.networkOk)
        {
            Result r;
            r.failure = Failure::Network;
            r.error   = http.rawError;
            r.state   = snapshot;
            return r;
        }

        auto* root = http.json.getDynamicObject();
        const bool valid = root != nullptr && (bool) root->getProperty ("valid");

        if (! valid)
        {
            // Server jednoznacne povedal NIE -> okamzity pad na Demo.
            // Refund, chargeback, zrusene predplatne, zmazana instancia,
            // rucne vypnuty kluc v dashboarde.
            LicenseState revoked;
            revoked.activated     = false;
            revoked.tier          = Tier::Demo;
            revoked.licenseKey    = snapshot.licenseKey;
            revoked.lastValidated = juce::Time::getCurrentTime();

            const auto raw = root != nullptr ? root->getProperty ("error").toString()
                                             : juce::String();

            Result r;
            r.state = revoked;

            if (raw.containsIgnoreCase ("expired"))
            {
                r.failure = Failure::Expired;
                r.error   = "Platnost licencie uplynula. Ak mas predplatne, obnov ho.";
            }
            else if (raw.containsIgnoreCase ("disabled"))
            {
                r.failure = Failure::Disabled;
                r.error   = "Tento licencny kluc bol deaktivovany.";
            }
            else
            {
                r.failure = Failure::InvalidKey;
                r.error   = raw.isEmpty() ? "Licencia uz nie je platna." : raw;
            }

            return r;
        }

        LicenseState s = snapshot;
        fillFromResponse (s, http.json);
        s.activated     = true;
        s.lastValidated = juce::Time::getCurrentTime();

        // Zachyti aj upgrade planu: variant_id sa zmenil -> novy tier.
        return checkResponse (s);
    },
    [this, cb] (Result r)
    {
        // Zapisujeme aj pri neuspechu, ak server odpovedal (revoked stav).
        // Pri sietovej chybe NIE — inak by offline pouzivatel prisiel o licenciu.
        if (r.ok || (! r.isTransient() && ! r.state.activated))
            applyState (r.state);

        if (cb)
            cb (r);
    });
}

//------------------------------------------------------------------------------

void LicenseManager::deactivateAsync (Callback cb)
{
    LicenseState snapshot;
    {
        const juce::ScopedLock sl (lock);
        snapshot = current;
    }

    if (snapshot.licenseKey.isEmpty() || snapshot.instanceId.isEmpty())
    {
        LicenseStore::clear();
        applyState (LicenseState {});

        Result r;
        r.ok = true;
        if (cb) cb (r);
        return;
    }

    Task::run ([snapshot]() -> Result
    {
        juce::StringPairArray f;
        f.set ("license_key", snapshot.licenseKey);
        f.set ("instance_id", snapshot.instanceId);

        auto http = postForm ("deactivate", f);

        Result r;

        if (! http.networkOk)
        {
            r.failure = Failure::Network;
            r.error   = http.rawError;
            r.state   = snapshot;
            return r;
        }

        auto* root = http.json.getDynamicObject();
        r.ok = root != nullptr && (bool) root->getProperty ("deactivated");

        if (! r.ok)
        {
            r.failure = Failure::InvalidKey;
            r.error   = root != nullptr && root->getProperty ("error").toString().isNotEmpty()
                          ? root->getProperty ("error").toString()
                          : "Deaktivacia zlyhala.";
            r.state = snapshot;
        }

        return r;
    },
    [this, cb] (Result r)
    {
        if (r.ok)
        {
            LicenseStore::clear();
            applyState (LicenseState {});
        }

        if (cb)
            cb (r);
    });
}

//------------------------------------------------------------------------------

bool LicenseManager::isConfigured() noexcept
{
    return hasConfiguredVariants() && config::kStoreId != 0;
}

void LicenseManager::openStore (const juce::String& checkoutSlug)
{
    juce::String url (config::kStoreUrl);

    if (checkoutSlug.isNotEmpty())
        url << "/buy/" << checkoutSlug;

    juce::URL (url).launchInDefaultBrowser();
}

void LicenseManager::openCheckoutFor (Tier t)
{
    switch (t)
    {
        case Tier::Listener: openStore (config::kCheckoutListener); break;
        case Tier::Creator:  openStore (config::kCheckoutCreator);  break;
        case Tier::Pro:      openStore (config::kCheckoutPro);      break;
        case Tier::Demo:
        default:             openStore();                           break;
    }
}

void LicenseManager::openCustomerPortal()
{
    juce::URL (juce::String (config::kCustomerPortalUrl)).launchInDefaultBrowser();
}

} // namespace alter
