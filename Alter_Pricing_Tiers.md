# Alter — rozdelenie na plány, ceny a feature matrix

Rozdelenie vychádza z reálneho kódu aplikácie (`Source/ControllerWindow.cpp`,
`MainComponent.h`, ALT2 protokol) a z tvojich rozhodnutí. Model: **Demo (free)
+ dva horizontálne platené plány (Creator / Listener) + Pro bundle**.

---

## 1. Ceny a pozícia na trhu

| Plán | Cena | Pre koho | Jadro hodnoty |
|---|---|---|---|
| **Demo** | **€0** | Stiahnu si po prihlásení na stránke | Ochutnávka — 2 meracie moduly, system audio |
| **Listener** | **€7,97** | Zvukoví inžinieri, mix/master | Plný metering suite + plugin (VST) vstup |
| **Creator** | **€9,97** | Producenti, VJ, tvorcovia vizuálov | Kreatívne moduly + theme + DAW automatizácia + základné meracie moduly |
| **Pro** | **€14,97** jednorazovo *alebo* **€3,97/mes** | Chcú všetko | Metering + kreatíva + automatizácia + theme + Record |

**Prečo Pro €14,97:** Listener + Creator samostatne = €17,94. Pro za €14,97
je lacnejší ako obe dokopy → pôsobí ako jasná výhoda a stane sa hlavnou
voľbou. Označ ho ako „Best value / Odporúčané".

**Pozícia voči konkurencii:** MiniMeters ~€18 jednorazovo (len metering, žiadna
kreatíva), Magic Music Visuals ~€40+ . Tvoje Pro €14,97 podlieza MiniMeters a
pritom ponúka metering **aj** kreatívu + automatizáciu. Priestor máš — nepredávaj
sa zbytočne lacno.

**Predplatné:** len na Pro. Odporúčam **€3,97/mes** (jednorazovú cenu prekročí
za ~4 mesiace → dlhodobí užívatelia si aj tak kúpia jednorazovo, mesačné slúži
tým čo chcú krátkodobo vyskúšať). €2,97 je mäkšia alternatíva, ale kríži
jednorazovú cenu pomalšie (~5 mes). **Nedávaj mesačné na Creator/Listener** —
podrezalo by to jednorazový predaj.

---

## 2. Feature matrix

Legenda: ✅ plné · ⚪ základné / štandardné (bez pro nástrojov) · ❌ zamknuté
(viditeľné, ale neaktívne)

### Moduly

| Modul | Typ | Demo | Listener | Creator | Pro |
|---|---|:--:|:--:|:--:|:--:|
| Audio Meter | meranie | ⚪ | ✅ plný | ⚪ štandard | ✅ |
| Spectrum | meranie | ⚪ | ✅ | ⚪ štandard | ✅ |
| Oscilloscope | meranie | ⚪ | ✅ | ⚪ štandard | ✅ |
| Spectrogram | meranie | ❌ | ✅ | ❌ | ✅ |
| Stereoscope | meranie | ❌ | ✅ | ❌ | ✅ |
| Tone Analyzer | meranie | ❌ | ✅ | ❌ | ✅ |
| Synesthesia | kreatíva | ❌ | ❌ | ✅ | ✅ |
| Chladni Pattern | kreatíva | ❌ | ❌ | ✅ | ✅ |
| Geometry | kreatíva | ❌ | ❌ | ✅ | ✅ |

Demo + Creator zdieľajú **Audio Meter, Spectrum, Oscilloscope**. **Stereoscope,
Spectrogram, Tone Analyzer** sú rezervované pre Listener/Pro — Stereoscope je
zámerný pro ťahák pre inžinierov (dôvod kúpiť Listener). Pro metering hĺbka
(LUFS, True Peak, Level history, SPAN measurement mode, constant-Q, referenčné
krivky, korelácia) je len v Listener/Pro; v Creator bežia zdieľané moduly v
štandardnom režime.

Demo môže vytvoriť **max 2 moduly** z {Audio Meter, Spectrum, Oscilloscope}.
Ostatné sú v menu **Create** viditeľné, ale zamknuté (tichá persuázia — vidia
Synesthesiu, nemôžu ju spustiť).

### Funkcie a tlačidlá

| Funkcia | Demo | Listener | Creator | Pro |
|---|:--:|:--:|:--:|:--:|
| Create (pridať modul) | ⚪ max 2 | ✅ | ✅ | ✅ |
| Destroy (zmazať modul) | ✅ | ✅ | ✅ | ✅ |
| Explore (info okná) | ✅ | ✅ | ✅ | ✅ |
| Save preset | ❌ | ✅ | ✅ | ✅ |
| Load preset | ❌ | ✅ | ✅ | ✅ |
| Record (video HUD) | ❌ | ❌ | ❌ | ✅ |
| Always on top | ❌ | ✅ | ✅ | ✅ |
| Hold | ❌ | ✅ | ✅ | ✅ |
| Hide info | ❌ | ✅ | ✅ | ✅ |
| Theme | ❌ | ❌ | ✅ | ✅ |
| Výber / prehadzovanie modulov (reorder) | ❌ | ✅ | ✅ | ✅ |
| Počet blokov (radov) | 1 | 3 | 3 | 3 |
| Audio Input: System Audio | ✅ | ✅ | ✅ | ✅ |
| Audio Input: Plugin / UDP inštancie | ❌ | ✅ | ✅ | ✅ |
| Per-module Source picker | ❌ | ✅ | ✅ | ✅ |
| DAW automatizácia (Ableton, Control-mode) | ❌ | ❌ | ✅ | ✅ |
| Počet modulov spolu | max 2 | neobmedzene | neobmedzene | neobmedzene |

> **Automatizácia** je viazaná na kreatívne moduly (Synesthesia, Chladni,
> Geometry), ktoré sú len v Creator a Pro — takže automatizáciu automaticky
> dostáva len Creator a Pro.
>
> **Record** je exkluzívny pre Pro — ťahák na najvyšší plán.

---

## 3. Rozpis po plánoch

### Demo — €0

**Odomknuté:** System Audio capture · Create (max 2 moduly z Audio Meter /
Spectrum / Oscilloscope) · Destroy · Explore · Quit · Close.

**Zamknuté (viditeľné, neaktívne):** Save, Load, Record, Always on top, Hold,
Hide info, Theme, výber Audio Inputu, per-module Source, **výber/prehadzovanie
modulov** · plugin (VST) vstup · kreatívne moduly (Synesthesia, Chladni,
Geometry) · Spectrogram, Stereoscope, Tone Analyzer · viac ako 1 blok · viac ako
2 moduly.

**Persuázia (bez otravovania):**
- Cez pridané moduly ide **priehľadné logo značky Alter** s jemným textom
  „Unlock full version" / „Buy full license" — nízka opacita, nezakrýva
  vizualizáciu, funguje ako trvalý brand watermark aj ako klikateľný CTA.
- Keď skúsi zamknutú akciu (Save, prehadzovanie modulov, plugin vstup…), appka
  jemne signalizuje „nemáš na to oprávnenie" + odkáže na upgrade — žiadny
  blokujúci pop-up, len vizuálny náznak (napr. zámok + krátky štítok).
- Zamknuté moduly nechaj viditeľné v Create menu → vidieť Synesthesiu ≠ mať ju.

### Listener — €7,97

**Odomknuté:** celý metering suite naplno — Audio Meter (RMS / True Peak / LUFS /
Level history, Momentary / Trend), Spectrum (SPAN, constant-Q, referenčné krivky,
peak-hold, stereo overlay), Spectrogram, **Stereoscope (korelácia)**, Oscilloscope
(short/long term, symmetry), Tone Analyzer · plugin (VST) / UDP vstup +
per-module Source (výber FLOW inštancií v DAW) · Save/Load · Explore · Destroy ·
výber/prehadzovanie modulov · Always on top · Hold · Hide info · 3 bloky ·
neobmedzene modulov.

**Zamknuté:** kreatívne moduly (Synesthesia, Chladni, Geometry) · Theme · DAW
automatizácia · Record.

### Creator — €9,97

**Odomknuté:** kreatívne moduly Synesthesia, Chladni, Geometry (plné, so
všetkými parametrami + BPM sync) · Audio Meter, Spectrum, Oscilloscope
(štandardný režim — aby videl zvuk) · Theme · DAW automatizácia kreatívnych
modulov · plugin (VST) / UDP vstup + per-module Source · Save/Load · Explore ·
Destroy · výber/prehadzovanie modulov · Always on top · Hold · Hide info · 3
bloky · neobmedzene modulov.

**Zamknuté:** pro metering hĺbka — LUFS, True Peak, Level history, SPAN
measurement mode, constant-Q, referenčné krivky · Stereoscope · Spectrogram ·
Tone Analyzer · Record.

### Pro — €14,97 (alebo €3,97/mes)

**Odomknuté: všetko** — všetkých 9 modulov naplno, celý metering suite + celá
kreatíva, Theme, DAW automatizácia, plugin vstup, Record, všetky tlačidlá, 3
bloky, neobmedzene modulov.

---

## 4. Technická realizácia (odporúčanie)

Nerozdeľuj appku fyzicky na 4 buildy. Jeden build + **licenčný kľúč**, ktorý
odomyká feature flagy:

```
tier = demo | listener | creator | pro
flags:
  audio.plugin_input      = tier != demo
  modules.creative        = tier in {creator, pro}     // synesthesia, chladni, geometry
  modules.metering_full   = tier in {listener, pro}    // + spectrogram, stereoscope, tone, pro modes
  modules.metering_std    = tier in {creator}          // audio meter, spectrum, oscilloscope (standard)
  automation.daw          = tier in {creator, pro}
  theme                   = tier in {creator, pro}
  presets.save_load       = tier != demo
  record                  = tier == pro
  modules.reorder         = tier != demo
  buttons.utility         = tier != demo               // on-top, hold, hide info
  buttons.destroy_explore = true                        // vždy, aj v deme
  limits.max_modules      = (tier == demo) ? 2 : unlimited
  limits.max_blocks       = (tier == demo) ? 1 : 3
```

Pluginy (FLOW listener/creator) pridávaš k plánu samostatne — Alter ich len
prijíma cez ALT2 (UDP), takže stačí povoliť plugin vstup podľa tieru.

---

## 5. Poznámky

- **Listener (€7,97) je zámerne lacný** — necháme tak, časom uvidíme podľa
  dopytu. Medzera Creator↔Pro je €5 (robí z Pro no-brainer), Listener↔Pro €7.
- **Stereoscope** je kľúčový rezervovaný pro ťahák pre Listener/Pro — vizuálne
  atraktívny Oscilloscope zostáva v demo+creator ako hook na pozornosť.

---

## 6. Platby a licencie

**Zvolený setup:** predaj + predplatné + kľúče cez **Lemon Squeezy** ·
app postavená na **free JUCE Starter** tiere.

### 6.1 Prečo Lemon Squeezy

- Je **Merchant of Record** → **DPH vybaví celé za teba** (registrácia,
  sadzby, priznania). Nemusíš riešiť OSS ani kvôli tomu zakladať živnosť.
- Postavený na predaj softvéru — má vstavané **subscriptions** aj
  **generovanie a validáciu license kľúčov** (v cene, bez príplatku).
- Beží na Stripe rails (dnes ho vlastní Stripe), takže spoľahlivosť/bezpečnosť
  platieb je na úrovni Stripe.

### 6.2 Poplatky — koľko, z čoho, kvôli čomu

Lemon Squeezy si berie **5 % + $0,50 za transakciu** (5 % = platforma +
spracovanie karty + DPH servis; $0,50 = fixný poplatok za transakciu). Kurz
~1,087 USD/EUR → $0,50 ≈ €0,46.

**Kľúčové — DPH závisí od cenového režimu v LS:**

- **Režim A „vrátane dane" (tax-inclusive):** zákazník vidí a zaplatí presne
  sticker cenu; **DPH sa vytiahne z tvojej sumy**. Toto je pri EÚ spotrebiteľoch
  bežne nutné (EÚ právo chce finálnu cenu vrátane DPH).
- **Režim B „bez dane" (tax-exclusive):** DPH sa pripočíta zákazníkovi navrch;
  ty si necháš celý základ. Bežné pri SaaS/B2B.

**DPH sadzba závisí od krajiny kupujúceho**, nie od tvojej (DE 19 %, FR 20 %,
SK 23 %, HU 27 %). V režime A ti preto net kolíše podľa krajiny zákazníka.

**Režim A (tax-inclusive) — príklad SK 23 %:**

| Plán | Zákazník platí | − DPH 23 % | − LS poplatok | **Tvoj net** |
|---|---|---|---|---|
| Listener | €7,97 | €1,49 | €0,86 | **€5,62** |
| Creator | €9,97 | €1,86 | €0,96 | **€7,15** |
| Pro (jednorazovo) | €14,97 | €2,80 | €1,21 | **€10,96** |
| Pro (mesačne) | €3,97 | €0,74 | €0,66 | **€2,57** |

**Režim B (tax-exclusive) — DPH navrch, základ ti ostáva:**

| Plán | Základ | Zákazník platí (SK 23 %) | − LS poplatok | **Tvoj net** |
|---|---|---|---|---|
| Listener | €7,97 | €9,80 | €0,86 | **€7,11** |
| Creator | €9,97 | €12,26 | €0,96 | **€9,01** |
| Pro (jednorazovo) | €14,97 | €18,41 | €1,21 | **€13,76** |
| Pro (mesačne) | €3,97 | €4,88 | €0,66 | **€3,31** |

> **Odporúčanie:** rozhodni sa, či sticker cena je to, čo zákazník platí
> (režim A → DPH ide z tvojho, daj vyššie sticker ceny nech ti ostane dosť),
> alebo základ + DPH navrch (režim B → vyššia finálna cena pre zákazníka).
> Presné odvody vidíš v LS dashboarde. Pri kartách mimo EÚ +1,5 %, pri menovej
> konverzii ďalší malý poplatok.

### 6.3 Príklad mesiaca (ilustračné objemy)

| Položka | Ks | Gross | Net |
|---|---|---|---|
| Listener €7,97 | 30 | €239,10 | €213,30 |
| Creator €9,97 | 20 | €199,40 | €180,20 |
| Pro €14,97 | 15 | €224,55 | €206,40 |
| Pro €3,97/mes | 40 | €158,80 | €132,40 |
| **Spolu** | | **€821,85** | **≈ €732,30** |

Pri tomto mixe si LS vezme **~€89,55 (~10,9 %)** a DPH je mimo (rieši ho LS).
Objemy sú len príklad — nahraď vlastným odhadom, štruktúra výpočtu ostáva.

> Pozn.: tento príklad vychádza z **režimu B** (DPH navrch, základ ti ostáva).
> Ak zvolíš **režim A** (tax-inclusive), použi nižšie nety z tabuľky vyššie
> (napr. Pro €10,96 namiesto €13,76).

### 6.4 Licenčné kľúče

- **Jednorazové (Listener / Creator / Pro one-time):** dve možnosti —
  (a) nechať kľúče generovať a overovať priamo **LS** (jednoduchšie, overenie
  online cez ich API), alebo (b) **DIY offline** cez `juce::RSAKey` spustené
  z LS webhooku (funguje bez internetu po aktivácii). Pre štart stačí (a).
- **Kľúč nesie tier** → app podľa neho nastaví feature flagy z časti 4.

### 6.5 Mesačné Pro (predplatné)

- Samotné opakované strhávanie z karty, zlyhané platby, rušenie a obnovy
  **musí riešiť LS** — nedá sa to spraviť v JUCE.
- App si **len periodicky overí cez LS API**, či predplatné beží; ak nie,
  zamkne Pro funkcie. Nechaj **grace period** (pár dní offline), nech to
  nespadne v štúdiu bez netu.

### 6.6 JUCE licencia (dôležité — oddelené od platieb)

- Appka beží na **free JUCE Starter** tiere → **za JUCE neplatíš** (do
  $20 000 ročného príjmu, čo na štarte určite spĺňaš).
- **JUCE splash logo NESÚVISÍ** s tým, kde generuješ kľúče ani s LS. Ovplyvňuje
  ho len JUCE licencia. Odstránenie = platený **JUCE Indie ($50/mes alebo
  $1 000 jednorazovo)** — na štarte sa **neoplatí**. Podľa aktuálnych info navyše
  JUCE 8 Starter možno splash ani nevyžaduje → over na juce.com/get-juce.

### 6.7 Zhrnutie toku

```
Web (Lemon Squeezy checkout) → platba → LS vygeneruje kľúč + odvedie DPH
   → user vloží kľúč do Alter → app overí + nastaví tier flagy
   → Pro monthly: app periodicky re-check cez LS API
   → updaty appky nezávisle (WinSparkle/Sparkle), kľúče platia ďalej
```
