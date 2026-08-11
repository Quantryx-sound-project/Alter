/*
    AlterLicenseConfig.h
    ==================================================================
    JEDINY SUBOR, KTORY MUSIS VYPLNIT PRE SPOJENIE S LEMON SQUEEZY.

    Vsetko ostatne v Shared/ je logika a nemusis sa jej dotknut.
    Vsetky miesta na vyplnenie su oznacene  >>> VYPLN <<<

    AKO ZISTIT HODNOTY
    ------------------
    Najrychlejsie: spusti overovaci skript s ktorymkolvek svojim licencnym
    klucom (staci Python, netreba nic buildovat):

        python Shared/Tests/ls_check.py activate <TVOJ-KLUC>

    Vypise store_id, product_id, variant_id a rovno vygeneruje riadok
    do kVariantMap nizsie. Sprav to pre kazdy variant, ktory predavas.

    Rucne: Lemon Squeezy dashboard -> Products -> variant ma ID v URL
    (/variants/<ID>). Store ID je v Settings -> Stores.

    Ciste C++17, ziadne JUCE — pouziva to aj AlterTier.h aj testy.
*/

#pragma once

#include <cstdint>

namespace alter::config
{

//==============================================================================
// 1. IDENTITA OBCHODU  — BEZPECNOSTNE KRITICKE
//------------------------------------------------------------------------------
// Lemon Squeezy License API overi KAZDY platny kluc z CELEJ platformy, nielen
// z tvojho obchodu. Bez tejto kontroly by ktokolvek odomkol Alter na Pro
// klucom kupenym za 5 dolarov od uplne ineho predajcu.
//
// Lemon Squeezy to vyslovene odporuca:
// "We recommend hard-coding the store_id, product_id and/or variant_id into
//  your client and using them to validate that the key belongs to your product."
// https://docs.lemonsqueezy.com/guides/tutorials/license-keys
//==============================================================================

// >>> VYPLN <<<  Settings -> Stores, alebo vystup ls_check.py
inline constexpr std::int64_t kStoreId = 0;      // 0 = kontrola vypnuta (LEN pre vyvoj!)

// Druhy povoleny obchod. Vyplnas LEN ak ti test mode vrati iny store_id
// nez live mode (over cez ls_check.py v oboch rezimoch). 0 = nepouzity.
inline constexpr std::int64_t kStoreIdAlt = 0;

// >>> VYPLN <<<  ak mas jeden produkt "Alter" so styrmi variantmi (odporucane)
// Nechaj 0, ak chces, aby jeden build fungoval s test aj live produktmi —
// test a live maju RÔZNE product_id. Kontrolu obchodu (kStoreId) mas aj tak.
inline constexpr std::int64_t kProductId = 0;    // 0 = kontrola vypnuta

//==============================================================================
// 2. MAPOVANIE VARIANTOV NA PLANY
//------------------------------------------------------------------------------
// variant_id z odpovede Lemon Squeezy je jediny zdroj pravdy o tiere.
// Nikdy neparsuj nazvy produktov — nazvy menis pri marketingovych zmenach,
// ID nie.
//
// ODPORUCANA STRUKTURA V LS: jeden produkt "Alter", styri varianty.
// Upgrade Creator -> Pro je potom nativna zmena variantu v subscription,
// kluc zostava rovnaky a tier sa prepne sam pri revalidacii.
//
// Neznamy variant NIKDY neodomkne nic — appka spadne na Demo a povie ti,
// ake ID prislo, aby si ho sem doplnil.
//==============================================================================

enum class TierId : int { Demo = 0, Listener = 1, Creator = 2, Pro = 3 };

struct VariantMapping
{
    std::int64_t variantId;
    TierId       tier;
    const char*  label;        // len pre logy a chybove hlasky
};

// >>> VYPLN <<<  ls_check.py ti vypise hotove riadky
//
// DOLEZITE — TEST MODE vs LIVE MODE:
// Lemon Squeezy ma dva uplne oddelene svety. Ked v LS klikbes "Copy to Live
// Mode", produkty dostanu NOVE ID. Keby si tu mal len testovacie ID, po
// prechode na ostru prevadzku by kazdy skutocny zakaznik skoncil ako Demo.
//
// RIESENIE: nechaj tu OBE sady sucasne. Je to obycajna vyhladavacia tabulka,
// takze jeden build funguje v teste aj naostro a prechod je naozaj len
// prepnutie v LS dashboarde — ziadny rebuild.
inline constexpr VariantMapping kVariantMap[] =
{
    // ---- TEST MODE ----
    { 0, TierId::Listener, "TEST Alter Listener — monthly"  },
    { 0, TierId::Listener, "TEST Alter Listener — yearly"   },
    { 0, TierId::Creator,  "TEST Alter Creator — monthly"   },
    { 0, TierId::Creator,  "TEST Alter Creator — yearly"    },
    { 0, TierId::Pro,      "TEST Alter Pro — monthly"       },
    { 0, TierId::Pro,      "TEST Alter Pro — yearly"        },
    { 0, TierId::Pro,      "TEST Alter Pro — lifetime"      },

    // ---- LIVE MODE ----
    { 0, TierId::Listener, "Alter Listener — monthly"  },
    { 0, TierId::Listener, "Alter Listener — yearly"   },
    { 0, TierId::Creator,  "Alter Creator — monthly"   },
    { 0, TierId::Creator,  "Alter Creator — yearly"    },
    { 0, TierId::Pro,      "Alter Pro — monthly"       },
    { 0, TierId::Pro,      "Alter Pro — yearly"        },
    { 0, TierId::Pro,      "Alter Pro — lifetime"      },
};

//==============================================================================
// 3. OBCHOD A CHECKOUT
//==============================================================================

// >>> VYPLN <<<  tvoja LS subdomena
inline constexpr const char* kStoreUrl = "https://alter.lemonsqueezy.com";

// Priame checkout linky pre tlacidla "Upgradovat na ...".
// V LS: Products -> variant -> Share -> skopiruj cast za /buy/
// >>> VYPLN <<<
inline constexpr const char* kCheckoutListener = "";   // napr. "a1b2c3d4-...."
inline constexpr const char* kCheckoutCreator  = "";
inline constexpr const char* kCheckoutPro      = "";

// Stranka, kde si zakaznik spravuje predplatne (LS Customer Portal).
inline constexpr const char* kCustomerPortalUrl = "https://app.lemonsqueezy.com/my-orders";

inline constexpr const char* kSupportEmail = "support@alter.app";

//==============================================================================
// 4. API
//==============================================================================

inline constexpr const char* kApiBase = "https://api.lemonsqueezy.com/v1/licenses";

// Test mode v Lemon Squeezy pouziva ROVNAKY endpoint — kluce z test modu
// funguju rovnako. Netreba nic prepinat, len pouzi test-mode kluc.

inline constexpr int kHttpTimeoutMs = 8000;

//==============================================================================
// 5. SPRAVANIE
//==============================================================================

inline constexpr const char* kAppFolderName = "Alter";
inline constexpr const char* kLicenseFile   = "license.dat";

// Ako casto sa online revaliduje (dni).
inline constexpr int kRevalidateEveryDays = 7;

// Kolko dni offline tolerujeme od poslednej uspesnej validacie, nez spadneme
// na Demo. Audio softver musi fungovat v studiu bez netu.
//
// 30 dni je vedomy kompromis. Kratsia doba tresta legitimnych pouzivatelov
// (offline studio, turne, zla wifi) tvrdsie nez piratov, ktorych aj tak
// neudrzis.
inline constexpr int kOfflineGraceDays = 30;

// Overit, ci e-mail zadany pouzivatelom sedi s meta.customer_email?
// LS to odporuca ako ochranu pred zdielanim klucov. Cena: jedno pole navyse
// v aktivacnom okne a support tickety od ludi, ktori kupili cez iny e-mail.
// Odporucanie: nechaj vypnute, activation_limit robi rovnaku pracu bez trenia.
inline constexpr bool kRequireEmailMatch = false;

//==============================================================================
// 6. PEPPER PRE LOKALNY CACHE
//------------------------------------------------------------------------------
// HMAC klucom (pepper + machineId) je podpisany license.dat.
//
// NIE JE to bezpecnostna hranica — kto reverzuje binarku, pepper ziska.
// Ucel je konkretny a splnitelny:
//   - zabranit, aby niekto otvoril subor, prepisal tier a nasdielal navod
//   - machine binding znemoznuje skopirovat license.dat na iny pocitac
//
// Tato hodnota je vygenerovana nahodne (24 bajtov entropie). Ak davas repo
// na verejny GitHub, presun tento riadok do necommitovaneho suboru.
//==============================================================================

inline constexpr const char* kCachePepper =
    "alter_aa633598d6380908ab3a22363882ee9d3196fbd5401cc07d";

//==============================================================================
// KONTROLA, ZE SI NIC NEZABUDOL
//------------------------------------------------------------------------------
// Release build s nevyplnenou konfiguraciou sa NEZKOMPILUJE.
// Pocas vyvoja to nevadi — kontrola bezi len ked je NDEBUG.
//==============================================================================

#if defined (NDEBUG) && ! defined (ALTER_ALLOW_UNCONFIGURED_LICENSE)

 static_assert (kStoreId != 0,
     "AlterLicenseConfig.h: kStoreId nie je vyplnene. Bez neho odomkne Alter "
     "hocijaky licencny kluc z celeho Lemon Squeezy. Spusti: "
     "python Shared/Tests/ls_check.py activate <TVOJ-KLUC>");

 static_assert (kVariantMap[0].variantId != 0,
     "AlterLicenseConfig.h: kVariantMap nie je vyplneny — kazdy kluc by skoncil "
     "ako Demo. Spusti: python Shared/Tests/ls_check.py activate <TVOJ-KLUC>");

#endif

} // namespace alter::config
