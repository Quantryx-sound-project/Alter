# Alter — 22 reelsov · Day 12 → Day 33 (7. – 28. august)

**Formát:** 9:16 natívne · jeden take · tvoj hlas po anglicky · HUD v každom videu
**Cieľ fázy 1 (23.8.):** 5 producentov = full verzia zadarmo za feedback
**Cieľ fázy 2 (28.8.):** 10 producentov = full licencia zadarmo za promo videá
**Kľúčové:** ideš na dovolenku → natoč všetko pred odchodom, naplánuj cez Meta Business Suite

> Day 11 („river" reel) išiel 6.8. Plán beží od **Day 12 (7.8.)** po **Day 33 (28.8.)**.

## Dramaturgia

| Deň | Dátum | Téma | Typ |
|---|---|---|---|
| 12 | pi 7.8. | zostava od nuly | úvod |
| 13 | so 8.8. | zvuk ako svetlo (odpoveď na komentár) | téza |
| **14** | **ne 9.8.** | **RMS / crest** | **metering 1/4 · level** |
| **15** | **po 10.8.** | **LUFS** | **metering 2/4 · level** |
| **16** | **ut 11.8.** | **True Peak** | **metering 3/4 · level** |
| **17** | **st 12.8.** | **Level History** | **metering 4/4 · level** |
| 18 | št 13.8. | Oscilloscope — 30 s okno | time |
| 19 | pi 14.8. | Spectrum — Fourier + constant-Q | pitch |
| 20 | so 15.8. | Spectrogram — reasignácia | pitch + time |
| **21** | **ne 16.8.** | **Stereoscope — particles · goniometer · polar** | **space 1/2** |
| **22** | **po 17.8.** | **Stereoscope — correlation · correlometer** | **space 2/2** |
| 23 | ut 18.8. | Tone Analyzer — MIDI + tuner | key |
| **24** | **st 19.8.** | **Chladni — fyzika** | **kreatíva 1/3** |
| **25** | **št 20.8.** | **Synesthesia — mud** | **kreatíva 2/3** |
| **26** | **pi 21.8.** | **Geometry — uhádni žáner** | **kreatíva 3/3** |
| 27 | so 22.8. | plugin hoarder | humor · rozohriatie |
| **28** | **ne 23.8.** | **⭐ BETA CALL** | **CTA** |
| 29 | po 24.8. | BETA pripomienka | CTA |
| 30 | ut 25.8. | Record | funkcia |
| 31 | st 26.8. | Ableton automatizácia | flex |
| 32 | št 27.8. | funguje na čokoľvek | zábava · rozbeh |
| **33** | **pi 28.8.** | **⭐ PROMO SQUAD** | **CTA** |

**Logika oblúka:** Day 12 sľúbi „more dimensions", Day 13 to dokáže číslom — zvuk a svetlo sú tá istá vec o 40 oktáv vedľa. Tým dostane celá séria tézu. Potom ide **celá vzdelávacia chrbtica súvisle**, rozmer po rozmere: *level* (metering 14–17), *time* (Oscilloscope 18), *pitch* (Spectrum 19), *pitch + time* (Spectrogram 20), *space* (Stereoscope 21–22), *key* (Tone Analyzer 23), a kreatívny blok (24–26) ako odmena, kde sa tie dáta prekreslia do svetla. **Až potom** príde launch chvost — plugin hoarder, beta call, pripomienka, funkcie a finálne SQUAD.

> **Beta / launch chvost (27–33) je zatiaľ len hrubo poskladaný — doladíme ho neskôr.** Pozor na jednu vec: beta call je teraz až **28 (23.8.)**, dosť neskoro pred dovolenkou. Ak chceš testerov skôr, buď ho vytiahni dopredu, alebo ich rovno oslov v DM (30 producentov v následovníkoch = 5 testerov za večer).

**Téza a jej vrstvy.** Parametrické moduly robia prevod zvuk→svetlo **presne** (číslo, krivka, bar), kreatívne ho robia **doslova** (tón sa stane farbou). Do captionov to môžeš pripomínať jednou vetou, ktorý rozmer to je.

Mini série — do captionu daj číslovanie:
- **metering** 14–17 → `1/4` až `4/4`
- **stereo** 21–22 → `1/2`, `2/2`
- **kreatíva** 24–26 → `1/3` až `3/3`

---

# ČASŤ 1 — Systém nahrávania

## Zlaté pravidlo: komentár prvý

1. **Nahraj VO** (všetkých 22 naraz, oddelené 3 s tichom)
2. **Rozsekaj na 22 klipov**, každý do vlastného Ableton setu vedľa beatu
3. **Markery** na miesta, kde má prísť akcia — vidíš ich prichádzať, netrafíš naslepo
4. **Jeden priechod nanečisto**, druhý ostrý
5. Ak nestíhaš klikať → **škrtni slovo z VO**, nikdy nespomaľuj klikanie

VO ide z Abletonu cez reproduktory, takže **Alter reaguje aj na tvoj hlas aj na hudbu naraz, v reálnom čase**. To je celý efekt. Preto sa nesmie nahrávať naopak.

## Čím nahrávať

| Situácia | Nástroj |
|---|---|
| V zábere je **len HUD** | **vstavaný Record** — najčistejší obraz, audio digitálne |
| V zábere je aj **Controller** | **OBS**, dve okná pod sebou |
| V zábere je aj **Ableton** | **OBS**, dve okná pod sebou |

OBS: plátno **1080 × 1920**, desktop audio **48 kHz**, nahrávaj do MKV a remuxni na MP4.

Tri scény: `alter only` · `alter + controller` (HUD 62 % / Controller 38 %) · `alter + ableton`.

## Kedy je Controller v zábere

**Áno** — keď staviaš modul od nuly alebo **prepínaš mód**.
**Nie** — keď je zostava pripravená a ty len komentuješ.

> Polovica dní stojí na prepnutí módu a ten klik musí byť vidieť, inak divák nepochopí, že sa niečo zmenilo.

## Pravidlo layoutu

**Široké od prírody** — Spectrum, Spectrogram, Oscilloscope, Stereoscope → **sám vo svojom riadku**, cez celú šírku.

**Bary a čísla** — Audio Meter (RMS/TP/LUFS/Level History), Tone Analyzer → **tri vedľa seba v jednom riadku**.

**Kreatívne** — Synesthesia, Chladni, Geometry, Fusion → **celý rám sám** alebo dva riadky z troch.

```
riadok 1  [ Spectrum — cez celú šírku        ]
riadok 2  [ LUFS ] [ True Peak ] [ RMS       ]
riadok 3  [ Oscilloscope — cez celú šírku    ]
```

## Časovanie na takty

Pri **180 BPM** je takt 1,333 s, 20 s = presne 15 taktov. Cue každé 3 takty = presne 4 s:
**bar 4 → 4,00 s · bar 7 → 8,00 s · bar 10 → 12,00 s · bar 13 → 16,00 s**

Guide track: `Day12_cue_guide_180bpm.wav` — dvojité pípnutie = štart, jednoduché = cue.

---

# ČASŤ 2 — Obsahové pravidlo

Sám si to zmeral: **kreatívny modul na celú obrazovku + hudba + popisok = 200–500 videní.**

> **Kreatívny modul nikdy nie je obsah videa. Je to odmena za hook.**

Každý deň musí mať **spor** („všetci ti hovoria X, a je to blbosť"), **odhalenie** („toto sa deje v tvojom tracku a ty to nevidíš") alebo **otázku**.

**Pri módoch platí navyše:** prepnutie módu samo o sebe nie je obsah. Divák musí vidieť **rozdiel a dôvod** — preto má každý mód-deň dva cue body: raz zlý stav, raz dobrý.

---

## Tón hlasu — tvoja formula

Päť krokov v tomto poradí:

1. **Upokoj alebo prevráť** — „your mix doesn't sound bad". Divák čakal kritiku, dostane opak.
2. **Filozofické pozorovanie** — „it's the only art you can't see". Jedna veta, žiadne rozvíjanie.
3. **Vtip na vlastný účet** — „unless you're high or something". Zhodí to pátos skôr, než začne byť trápny.
4. **Úprimné „but I believe"** — presvedčenie povedané rovno, aj keď to znie naivne. Tá naivita je tam to najlepšie.
5. **Produkt ako obyčajný fakt** — „it's called Alter and it's launching soon". Žiadny tlak.

- **„hahaha" nechaj v texte** a naozaj sa zasmej. Max v polovici videí, inak je to tik.
- **Vety spájaj cez „so", „and", „but"** namiesto bodiek.
- **Priznaj, že si to nevedel** — silnejšie než akákoľvek funkcia.
- **Nevysvetľuj produkt, opíš problém.** Appka príde na rad až v poslednej vete.
- **Nekonči CTA tam, kde nič nepýtaš.** Nechaj to visieť.

---

# ČASŤ 3 — Čo appka vie a kde to je pokryté

Vytiahnuté priamo zo zdrojákov.

| Modul | Módy a varianty | Kde |
|---|---|---|
| **Audio Meter** | RMS · True Peak · LUFS · Level History · zobrazenie Mono/Stereo/Mirror/MirrorStereo · farby Standard/Gradient/Spectrum | D14–D17 · farebné módy zatiaľ nepoužité |
| **Spectrum** | Fourier rozklad · constant-Q · measurement (SPAN) · A-weight / phon · stereo L+R | D19 · zvyšok zatiaľ nepoužitý |
| **Oscilloscope** | Mono/Stereo/Mirror/MirrorStereo · okno až 30 s | D18 |
| **Spectrogram** | spektrálna reasignácia · farby theme/custom/by tone | D20 |
| **Stereoscope** | `0` Particles · `1` Goniometer · `2` Polar · `3` Correlation · `4` Correlometer | D21 (0,1,2) · D22 (3,4) |
| **Tone Analyzer** | tuner (MPM) · MIDI priamo z DAW stopy · detekcia akordov | D23 |
| **Synesthesia** | farba z tónu vs manuálna · BPM sync | D25 |
| **Chladni** | audio-reactive vs manuálne (m,n) · mode shift · tvar platne | D24 |
| **Geometry** | FREE ring vs BPM spawn · farba z tónu | D26 |
| **Fusion** | vrstvenie modulov cez seba | zatiaľ nepoužité |
| Systém | System Audio vs VST vstup · Instance · Theme · Always on top · Hold | D12, D30, D32 |

---

# ČASŤ 4 — Dni

## Day 12 — piatok 7.8. · zostava od nuly

**VO (53 slov · ~20 s):**

> „Your mix doesn't sound bad. You just need to see it from another perspective. It's the only art you can't see, unless you're high or something. But I believe music has more dimensions, so I built a tool that lets you see it without drugs, hahaha. It's called Alter and it's launching soon."

**Hook:** „Your mix doesn't sound bad."

**Nahrávanie:** `alter + controller` · OBS
**Layout:** začínaš s prázdnym HUD, staviaš pred očami
**Marker cues:** bar 4 → Create Spectrum (riadok 1) · bar 7 → Create LUFS, True Peak, RMS (riadok 2) · bar 10 → Create Oscilloscope (riadok 3) · bar 13 → Always on top

**Text na obrazovke:** `you're mixing blind`
**Caption:** `Day 12. Spent years making a thing I could never actually look at. Fixed that. Follow, it drops soon.`
**Hashtagy:** `#musicproducer #musictech #mixingandmastering #ableton #buildinpublic`

---

## Day 13 — sobota 8.8. · zvuk ako svetlo (odpoveď na komentár)

**VO (55 slov · ~21 s):**

> „Someone asked what I mean by more dimensions, so here's the real answer. The visible spectrum is one octave wide. Take an A, raise it forty octaves, and it comes out orange. Sound and light are both waves, they're just forty octaves apart. That's not a metaphor, that's what the whole app is built on."

**Hook:** „The visible spectrum is one octave wide."

**Overené čísla — môžeš ich pokojne povedať nahlas:**
- Viditeľné svetlo: 750 nm = **400 THz**, 380 nm = **789 THz**. Pomer **1,97×**. Hudobná oktáva = **2,00×**. Farba od červenej po fialovú je to isté zdvojnásobenie ako od C po C.
- **A4 = 440 Hz, +40 oktáv = 4,84 × 10¹⁴ Hz = 620 nm = oranžová.**
- Presnosť: nie každá nota sadne do viditeľného pri rovnakom posune — pri +40 je C4 ešte v infračervenom a B4 už zelené. Preto hovor „raise it forty octaves", nie „every note maps exactly". Takto ťa nikto nechytí.

**Nahrávanie:** `alter + controller` · OBS
**Layout:** Tone Analyzer hore, Synesthesia cez zvyšné dva riadky
**Marker cues:**

| Marker | Čas | Čo sa deje |
|---|---|---|
| bar 1 | 0,00 s | Tone Analyzer ukazuje jednu notu |
| bar 4 | 4,00 s | Synesthesia — tá istá nota už ako farba |
| bar 7 | 8,00 s | zahraj inú notu, farba sa posunie |
| bar 10 | 12,00 s | zahraj akord, farby sa premiešajú |
| bar 13 | 16,00 s | Synesthesia cez celý rám |

> Tento sled odpovedá na obe jeho otázky naraz. Prvá polovica hovorí **čo tie dimenzie sú**, druhá **ako to čítať** — farba *je* tá nota, nie ilustrácia k nej.

**Text na obrazovke:** `A = 620 nm = orange`
**Caption:** `Day 13. Replying to the comment. The visible spectrum is one octave wide — 400 to 790 THz, almost exactly a doubling. Raise a note 40 octaves and it becomes a colour. That's why the visuals are mapped the way they are.`
**Hashtagy:** `#musicproducer #synesthesia #physics #audioreactive #musictech`

> **Použi Instagram „reply to comment"** — komentár sa zobrazí ako nálepka vo videu. Natívny formát, lepší dosah, a vyzerá to, že reaguješ na ľudí, nie že odjazďuješ plán.
>
> **Toto je najsilnejší hook v celom pláne** a zároveň tvoja téza. Nie je to marketingová veta, je to vec, ktorú si overil a ktorá je pravdivá. Preto funguje.

---

## Day 14 — nedeľa 9.8. · RMS / crest — metering 1/4 (viral hook)

**VO (54 slov · ~21 s):**

> „Silence is the loudest thing in your track. Your ear only reacts to change, so a drop never hits because it's loud, it hits because of the quiet you put in front of it. Flatten that gap and the punch is just gone. It was never in the drop, it was in the silence."

**Hook:** „Silence is the loudest thing in your track."

**Čo to je a načo:**
- **RMS** = priemerná energia, ako hlasno to pôsobí dlhodobo. **Peak** = okamžitá špička. Rozdiel medzi nimi je **crest factor** — miera dynamiky.
- **Prečo je RMS prvé:** je to surová energia, holý fakt o signáli. Ostatné merania sú len rôzne pohľady na to isté. Preto sa začína tu.
- **Ako to využiť:** sleduj crest pri limitovaní. Keď medzera medzi RMS a peakom mizne, práve zabíjaš punch. Pre EDM drop chceš, aby peak výrazne trčal nad RMS — to je ten úder.

**Nahrávanie:** `alter + controller` · OBS
**Layout:** Audio Meter v **RMS** móde + druhý v **True Peak** vedľa seba, Oscilloscope dole
**Marker cues:** bar 4 → dynamická pasáž, veľký rozdiel RMS vs peak · bar 10 → prižeň limiter, medzera sa zmenší a oscilloscope sa zaplní

> Toto je viralový kus série. „Silence is the loudest thing in your track" je tvrdenie, ktoré chce každý overiť — a ty ho hneď dokážeš na obrazovke.

**Text na obrazovke:** `the punch is in the silence`
**Caption:** `Day 14. Metering 1 of 4. Your drop doesn't hit because it's loud. It hits because of the quiet before it.`
**Hashtagy:** `#mixingandmastering #musicproducer #edmproducer #musictech #mastering`

---

## Day 15 — pondelok 10.8. · LUFS — metering 2/4 (vnímanie)

**VO (86 slov · ~33 s):**

> „Loudness isn't only in the sound, it's in you. Your ear cares way more about mids than bass, so two sounds with the exact same energy can feel completely different. In Alter, the LUFS meter follows your ear instead of the waveform. Streaming platforms turn your whole track down by one fixed amount, set by its loudest part. So the perception of loudness balance across the track is on you. That's why you'll want to look at the loudness trend curve to see that balance across time."

**Hook:** „Loudness isn't only in the sound, it's in you."

**Čo to je a načo:**
- **LUFS** = ako hlasno to počuje človek, nie mašina. Kým RMS je čistá energia, LUFS ju **preváži podľa ucha** — stredy počítajú viac než basy. Preto je LUFS hneď po RMS: je to jeho vnímaná verzia.
- **Integrated číslo** za celý track je pri tvorbe skoro zbytočné, dôležité je až pri odovzdaní. **Trend / short-term je to zaujímavé** — ukáže hlasitosť v čase, takže vidíš, či druhý drop je slabší, či breakdown nespadol do diery.
- **Kľúčové, čo z toho plynie:** streaming aplikuje **jeden pevný posun na celý track**, nie kompresiu. Keď máš integrated −8 a cieľ je −14, stiahne sa všetko o −6 dB naraz — tiché aj hlasné rovnako. Pomer medzi nimi ostane. Preto platforma nevyrovnaný track **neopraví**, len ho celý posunie — tvoja vnútorná rovnováha je jediné, čo reálne počúvajú.
- **Pol ticho, pol hlasno?** Integrated má gating (ticho pod prahom sa neráta), takže číslo určuje hlavne hlasná polovica. Track sa stiahne podľa nej a tichá polovica ide dole s ňou o to isté — ostane taká tichá, akú si ju spravil.
- **Ako to využiť:** nechaj Trend bežať cez celý track a hľadaj nerovnosti, ktoré fader nezobrazí. To je kontrola vyváženia, ktorú ti normalizácia nikdy neurobí za teba.

**Overené čísla:**
- Spotify, YouTube, Tidal, Amazon normalizujú na **−14 LUFS**; Apple Music **−16 LUFS**
- EDM a klubové mastery reálne sedia na **−10 až −7 LUFS** (klub až −8 až −6)
- Hlasnejší master ti na streame **nedá výhodu**, platforma to stiahne

**Nahrávanie:** `alter + controller` · OBS
**Layout:** Audio Meter veľký v riadku 1, Spectrum v riadku 2
**Marker cues:** bar 4 → prepni meter mode na **LUFS** · bar 10 → prepni na **Momentary / Trend**, nechaj krivku prejsť cez drop

**Text na obrazovke:** `you're mixing perception`
**Caption:** `Day 15. Metering 2 of 4. LUFS doesn't measure your track. It measures a model of your ear.`
**Hashtagy:** `#mixingandmastering #musicproducer #edmproducer #musictech #mastering`

---

## Day 16 — utorok 11.8. · True Peak — metering 3/4 (fyzikálny paradox)

**VO (70 slov · ~27 s):**

> „The loudest moment in your track was never recorded. Between every two samples the wave gets rebuilt, and it can rise higher than any point you can see. That's true peak, a spike that only exists once the signal turns back into sound. You clip on something that isn't even in the file. That's why it matters to catch it before it happens, and Alter can show it to you."

**Hook:** „The loudest moment in your track was never recorded."

**Čo to je a načo:**
- **True Peak** meria skutočný vrchol **medzi vzorkami** (inter-sample), nie len najvyššiu vzorku. Bežný peak meter tie špičky nevidí, lebo v súbore neexistujú — vznikajú až pri rekonštrukcii vlny.
- **Načo to potrebuješ:** pri prevode na MP3/AAC sa tie skryté špičky prejavia a skreslia zvuk. V DAW čisto, na Spotify praská. True Peak je jediný meter, čo ťa varuje vopred.
- **Ako to využiť:** drž True Peak pod **−1 dBTP** na master bus. Keď ide do červena, stiahni limiter alebo master gain.

**Nahrávanie:** `alter + controller` · OBS
**Layout:** Audio Meter veľký, Oscilloscope pod ním
**Marker cues:** bar 4 → prepni na **True Peak** · bar 7 → hlasná pasáž, TP nad −1, červená · bar 13 → stiahni o 2 dB, červená zmizne

> Dôkaz „červená → nie červená" je to, čo ľudia pošlú ďalej. Bez neho je to prednáška.

**Text na obrazovke:** `see that bit over the line? you can't hear it yet`
**Caption:** `Day 16. The clip you can't see. It's between the samples, invisible until it becomes sound — then your hats start crackling. Streaming might turn it down. A DJ deck won't. Keep true peak under -1.`
**Hashtagy:** `#mastering #mixingandmastering #musicproducer #audioengineering #musictech`

---

## Day 17 — streda 12.8. · Level History — metering 4/4 (pamäť)

**VO (53 slov · ~20 s):**

> „Hearing is memory of the universe. You never really hear a moment, you hear what it just was. So instead of a meter that forgets, I drew it into a window you can size yourself, holding the RMS and true peak of the last few seconds. So you can see if your drop is actually bigger than your intro, or if it just feels that way."

**Hook:** „Hearing is memory of the universe."

**Čo to je a načo:**
- **Level History** nie je nový údaj — je to **RMS a peak nakreslené v čase**, posledných ~30 sekúnd naraz. Bar ti povie stav *teraz*, toto ti povie *priebeh*.
- **Čo ti hovorí:** vidíš tvar hlasitosti celej pasáže ako siluetu. Kde bola špička, kde diera, či sa dynamika drží alebo sa postupne stláča. Je to RMS, crest a Trend v jednom obraze.
- **Ako to využiť:** nemusíš civieť na bar — špička ostane nakreslená. Skvelé na porovnanie dvoch dropov alebo odhalenie miesta, kde limiter pracuje tvrdšie.
- **Nezameň s Oscilloscope (Day 18):** Level History drží **čísla metra** (RMS + true peak) v čase — teda *ako hlasno* to bolo. Oscilloscope 30 s ukazuje **tvar samotnej vlny** — teda *ako to vyzeralo*. Jeden je meranie, druhý je signál. Preto sú to dve rôzne videá, hoci obe pokrývajú ~30 s.
- **Prečo je štvrtý:** dáva zmysel až keď divák pozná RMS, LUFS aj peak. Uzatvára sériu — je to ich spoločná pamäť.

**Nahrávanie:** `alter + controller` · OBS
**Layout:** Audio Meter v **Level History** móde cez dva riadky (je široký), Spectrum dole
**Marker cues:** bar 4 → prepni meter mode na **Level History** · bar 10 → prepni display mode na **Stereo**, potom **Mirror**

> Ukáž, ako sa envelope skroluje a ako je vidieť peak spred desiatich sekúnd. To je celý argument.

**Text na obrazovke:** `hearing is memory`
**Caption:** `Day 17. Metering 4 of 4. You never hear a moment, only how it compares to the last one. So I drew the last 30 seconds.`
**Hashtagy:** `#mixingandmastering #musicproducer #audioengineering #musictech #mastering`

---

## Day 18 — štvrtok 13.8. · Oscilloscope (téza: time · krása session)

**VO (50 slov · ~19 s):**

> „Sound is the architecture you can't touch. But you can see it. The waveform is the visible, flowing representation of moving air, the thing we hear and call sound. I could pretend I built this to analyse my mix, but honestly I just wanted my session to look fucking cool."

**Hook:** „Sound is the architecture you can't touch. But you can see it."

> **Stojí samostatne** — mnohí ho uvidia ako prvé video, takže nič nepredpokladá. Uhol je **krása, nie analýza**: oscilloscope je najkrajšia vec, akú si počas produkcie necháš na obrazovke, a fyzika to len podopiera.

**Fyzika za tým (do komentov):** vlna, ktorú vidíš, je výchylka — doslova poloha vzduchu / pohyb membrány reproduktora v čase. Nie je to znázornenie zvuku, je to jeho presný tvar tesne pred tým, než sa ti dostane do ucha. A keď okno roztiahneš na 30 s, z časovej osi sa stane priestor — celý track ako jeden tvar, ten „rozmer navyše" zo Day 13.

**Nahrávanie:** `alter + controller` · OBS
**Layout:** Oscilloscope sám cez dva riadky, na celý rám nech to dýcha
**Marker cues:** bar 4 → tesné okno, jedna vlna · bar 10 → **roztiahni na 30 s**, celý track ako tvar · bar 13 → display mode **Mirror** (symetria, čisto pre krásu)

**Text na obrazovke:** `architecture you can't touch, but you can see`
**Caption:** `Day 18. Sound is architecture you can't touch — but you can see it. The waveform is moving air made visible. I keep it on screen because a session should look as good as it sounds.`
**Hashtagy:** `#musicproducer #ableton #mixingandmastering #musictech #bedroomproducer`

---

## Day 19 — piatok 14.8. · Spectrum — Fourier + constant-Q (téza: pitch)

**VO (64 slov · ~25 s):**

> „Every sound you've ever heard is secretly just sine waves stacked on top of each other. A kick, a hi-hat, your own voice, all pure tones added together, you just can't pull them apart by ear. That's what a spectrum does. And a normal one crushes the low end, so I let every octave take the same space. Now you can see the sub."

**Hook:** „Every sound you've ever heard is secretly just sine waves."

**Fyzika za tým (do komentov):** Fourierova veta — akýkoľvek zvuk, hocijako zložitý, sa dá presne rozložiť na súčet čistých sínusoviek. Spektrum robí presne to rozloženie v reálnom čase. Constant-Q potom dáva každej oktáve rovnaký priestor, lebo naše vnímanie výšky je logaritmické (oktáva = zdvojnásobenie), takže lineárny FFT skresľuje presne to, čo počuješ ako hudbu.

**Nahrávanie:** `alter + controller` · OBS
**Layout:** Spectrum sám cez dva riadky, Audio Meter dole
**Marker cues:** bar 4 → lineárne spektrum, sub nečitateľný · bar 10 → **zapni constant-Q**, spodok sa roztiahne

> Prvá polovica je téza (zvuk = sínusovky), druhá je úžitok (vidíš sub). Rozdiel constant-Q je okamžitý a netreba ho vysvetľovať.

**Text na obrazovke:** `it's all just sine waves`
**Caption:** `Day 19. Every sound is just pure tones added together — that's Fourier. Alter pulls them apart, and constant-Q gives every octave the same room so the sub finally shows up.`
**Hashtagy:** `#mixingandmastering #musicproducer #musictech #audioengineering #mastering`

---

## Day 20 — sobota 15.8. · Spectrogram — reasignácia (téza: pitch + time)

**VO (61 slov · ~23 s):**

> „Sound has a shape you've never seen, because it needs two directions at once, time going one way, pitch the other. That's a spectrogram, your whole track photographed from above. Most of them smear every note into a cloud. This one puts each grain of energy back exactly where it came from, so a hi-hat looks like a hi-hat, not fog."

**Hook:** „Sound has a shape you've never seen."

**Fyzika za tým (do komentov):** spektrogram je jediný pohľad, kde vidíš **frekvenciu aj čas naraz** — čas na jednej osi, výška na druhej, jas = energia. To spája pitch (Spectrum) a time (Oscilloscope) do jedného obrazu. **Spektrálna reasignácia** je nerd časť: bežný spektrogram rozmaže každý tón do rozmazanej škvrny, reasignácia vráti každé zrnko energie presne tam, odkiaľ prišlo — preto hi-hat vyzerá ako ostrá čiara, nie ako oblak.

**Nahrávanie:** `alter + controller` · OBS
**Layout:** Spectrogram sám cez celý rám (je široký aj vysoký zároveň — jediný modul, čo znesie plnú plochu)
**Marker cues:** bar 4 → bežný spektrogram, rozmazané · bar 10 → **zapni reasignáciu**, obraz sa zaostrí · bar 13 → prepni farby (theme / by tone)

> Most medzi meraním a kreatívou — je to posledný „technický" deň pred kreatívnym blokom a zároveň najkrajší z nich. Reasignácia je predtým/potom, ktoré nikto nečaká.

**Text na obrazovke:** `your track from above`
**Caption:** `Day 20. A spectrogram is your whole track seen from above — time one way, pitch the other. Most smear every note into a cloud. Alter puts the energy back exactly where it came from.`
**Hashtagy:** `#musicproducer #musictech #audioengineering #mixingandmastering #spectrogram`

---

## Day 21 — nedeľa 16.8. · Stereoscope 1/2 — particles · goniometer · polar (otázka)

**VO (51 slov · ~20 s):**

> „Stereo isn't real. There's no actual space between your speakers, your brain builds the whole thing out of tiny differences between left and right. So I drew that difference three different ways. Same signal, three scopes, and I genuinely couldn't pick which one I liked, so you get all of them."

**Hook:** „Stereo isn't real."

**Fyzika za tým (do komentov):** máš dva reproduktory a medzi nimi žiadny priestor — stereo obraz je ilúzia, ktorú mozog dopočíta z drobných rozdielov v hlasitosti a čase medzi ľavým a pravým kanálom. Tri scopy sú tri spôsoby, ako ten rozdiel nakresliť: **Particles** ho rozloží po frekvencii, **Goniometer** je klasický Lissajous kruh, **Polar** vystreľuje vzorky zdola nahor.

**Nahrávanie:** `alter + controller` · OBS
**Layout:** Stereoscope sám cez dva riadky, Spectrum dole
**Marker cues:** bar 4 → **Particles** · bar 7 → **Goniometer** · bar 10 → **Polar** · bar 13 nechaj Polar dobehnúť

> Otázka na konci captionu zbiera komenty. Toto je „space" rozmer zo Day 13.

**Text na obrazovke:** `stereo is an illusion`
**Caption:** `Day 21. Stereo 1 of 2. There's no space between your speakers — your brain invents it. Three scopes that draw the illusion: particles, goniometer, polar. Which would you actually use? 👇`
**Hashtagy:** `#mixingandmastering #musicproducer #audioengineering #musictech #studiolife`

---

## Day 22 — pondelok 17.8. · Stereoscope 2/2 — correlation · correlometer (odhalenie)

**VO (59 slov · ~23 s):**

> „Two identical waves, one flipped upside down, add up to nothing. Silence, made entirely out of sound. That's phase, and it's why half your mix can vanish the second it folds to mono. Knowing your track is out of phase isn't useful though, knowing which frequency is. That's the correlometer, and I don't know why nobody else does it."

**Hook:** „Two identical waves can add up to nothing."

**Fyzika za tým (do komentov):** deštruktívna interferencia — keď sa rovnaká vlna stretne so svojou prevrátenou kópiou, presne sa vyrušia a ostane ticho. To sa deje pri mono fold-downe: čo je v protifáze medzi L a R, zmizne. **Correlation** ukáže, či je track vôbec v protifáze, **Correlometer** ukáže **ktoré frekvenčné pásmo** — takže vidíš, či je to sub a nie reverb.

**Nahrávanie:** `alter + controller` · OBS
**Layout:** Stereoscope sám cez dva riadky, Audio Meter dole
**Marker cues:** bar 4 → **Correlation**, scrolling história · bar 10 → **Correlometer**, per-band korelácia · bar 13 → ukáž pásmo, ktoré je v mínuse

> Najsilnejšie tvrdenie o odlišnosti v celom pláne. Per-band korelácia je vec, ktorú bežné metre nemajú.

**Text na obrazovke:** `silence made of sound`
**Caption:** `Day 22. Stereo 2 of 2. Two waves can cancel into pure silence — that's phase, and it's what eats your mix in mono. The correlometer shows which frequency is doing it, not just that something is.`
**Hashtagy:** `#mixingandmastering #mastering #audioengineering #musicproducer #musictech`

---

## Day 23 — utorok 18.8. · Tone Analyzer — MIDI + tuner (odhalenie)

**VO (49 slov · ~19 s):**

> „Your bassline isn't in key. Mine wasn't either for about three years, I'd just vibe it and hope nobody noticed. This one reads the MIDI straight from the track so it isn't guessing the note, it knows it. And on audio it uses pitch detection and still gets it."

**Hook:** „Your bassline isn't in key."

**Nahrávanie:** `alter + ableton` · OBS
**Layout:** Tone Analyzer veľký hore, dole jedna MIDI stopa z Abletonu
**Marker cues:** bar 4 → MIDI mód, noty sedia s clipom · bar 10 → prepni na audio zdroj, pitch detection · bar 13 → zahraj akord, detekcia akordu

**Text na obrazovke:** `it reads the MIDI`
**Caption:** `Day 23. It pulls the notes straight off your MIDI track. Chords too. Been producing years and this still catches me out.`
**Hashtagy:** `#musicproducer #musictheory #ableton #musictech #bedroomproducer`

---

## Day 24 — streda 19.8. · Chladni — kreatíva 1/3 (fyzika)

**VO (51 slov · ~20 s):**

> „You put sand on a metal plate, you play a tone, and the sand runs away from wherever the plate is moving. It just draws by itself. Physics did that, not a designer. I rebuilt it so my own track draws it, and I've been watching this for way too long."

**Hook:** „You put sand on a metal plate and play a tone."

**Nahrávanie:** `alter only` · vstavaný Record
**Layout:** Chladni cez celý rám
**Marker cues:** bar 7 → tichá pasáž, vzor stabilný · bar 10 → sub bass drop, platňa snapne na iný eigenmode

**Text na obrazovke:** `real physics, real audio`
**Caption:** `Day 24. Creative 1 of 3. Chladni figures, but driven by your track instead of a sine generator. Sound has a shape.`
**Hashtagy:** `#chladni #physics #audioreactive #musicproducer #generativeart`

---

## Day 25 — štvrtok 20.8. · Synesthesia — kreatíva 2/3 (odhalenie)

**VO (46 slov · ~18 s):**

> „You can literally see the second your mix goes muddy. Everything piles into the low mids and turns into soup. I spent like three years guessing where the problem was, and it turns out I just couldn't see it. Kind of annoyed about those years, honestly."

**Hook:** „You can literally see the second your mix goes muddy."

**Nahrávanie:** `alter only` · vstavaný Record
**Layout:** Synesthesia cez 2 riadky, Spectrum v treťom (dôkaz k tvrdeniu)
**Marker cues:** bar 4 → preplnená low-mid pasáž, smear · bar 10 → čistá pasáž, rozdiel

> Bez porovnania je to len pekný vizuál. **Kontrast je celý obsah videa.**

**Text na obrazovke:** `watch the low mids`
**Caption:** `Day 25. Creative 2 of 3. Turns out mud has a look. Wish someone had shown me this in 2021.`
**Hashtagy:** `#mixingandmastering #musicproducer #audioreactive #musictech #bedroomproducer`

---

## Day 26 — piatok 21.8. · Geometry — kreatíva 3/3 (otázka)

**VO (50 slov · ~19 s):**

> „Guess the genre from the shape. I'm serious. The kick sets the size and the highs set the detail, so drum and bass and lofi look nothing alike. Every genre has a shape and none of us knew. I made no music today, hahaha. Tell me what this one is."

**Hook:** „Guess the genre from the shape. I'm serious."

**Nahrávanie:** `alter only` · vstavaný Record
**Layout:** Geometry cez celý rám, **BPM spawn mode zapnutý**
**Marker cues:** bar 4 žáner A · bar 7 žáner B · bar 10 žáner C — každý rovnako dlho

**Text na obrazovke:** `guess it 👇`
**Caption:** `Day 26. Creative 3 of 3. Every genre has a shape. What do you reckon this one is?`
**Hashtagy:** `#musicproducer #audioreactive #generativeart #musictech #producercommunity`

---

## Day 27 — sobota 22.8. · plugin hoarder (rozohriatie pred CTA)

**VO (43 slov · ~17 s):**

> „You have four hundred plugins and you use six. I'm not judging, my folder is a graveyard too. Half of them are just bars that move and we all paid real money for that. So I made another one, hahaha. Mine's prettier though."

**Hook:** „You have four hundred plugins and you use six."

**Nahrávanie:** `alter only` · vstavaný Record
**Layout:** plná zostava, nič sa nemení
**Marker cues:** žiadne — jediný pohyb je hudba. Sústreď sa na prednes.

> Zámerne deň pred beta callom. Ľahký, zdieľateľný, priláka ľudí — a zajtra od nich niečo pýtaš.

**Text na obrazovke:** `be so fr rn`
**Caption:** `Day 27. Plugin folder is 90% shame. What's your most useless purchase?`
**Hashtagy:** `#musicproducer #producerhumor #bedroomproducer #studiolife #musictech`

---

## Day 28 — nedeľa 23.8. · ⭐ BETA CALL

**VO (45 slov · ~17 s):**

> „Okay, real thing. I'm giving five producers the full version, free, forever. I don't want money, I want somebody to tell me it's broken. I've been staring at this thing so long I honestly can't see it anymore, hahaha. Comment BETA and I'll message you."

**Hook:** „I'm giving five producers the full version, free, forever."

**Nahrávanie:** `alter only` · vstavaný Record
**Layout:** meniaca sa zostava
**Marker cues:** bar 1 Synesthesia · bar 4 Stereoscope + metre · bar 7 Chladni · bar 10 Geometry · bar 13 plná zostava

**Text na obrazovke:** `5 producers. free forever. comment BETA`
**Caption:** `Day 28. I need 5 people to break this before I launch it. Full version, free, yours to keep. Just be brutally honest. Comment BETA 👇`
**Hashtagy:** `#musicproducer #betatesting #musictech #buildinpublic #producercommunity`

> Najdôležitejší post fázy 1. Odpovedaj na komenty do pár hodín, inak dosah padne.

---

## Day 29 — pondelok 24.8. · BETA pripomienka

**VO (42 slov · ~16 s):**

> „Quick one, some of the free spots are gone. If you actually make music and you'd use this, comment BETA, I'm reading all of them. I'd rather give it to one person who tells me the truth than to nobody at all."

**Hook:** „Some of the free spots are gone."

**Nahrávanie:** `alter only` · vstavaný Record
**Layout:** plná zostava, pokojná

> **Natoč dve verzie:** „some spots are gone" a „still open". Z dovolenky nebudeš vedieť prerobiť video podľa toho, koľko ľudí sa reálne ozve.

**Text na obrazovke:** `spots going. comment BETA`
**Caption:** `Day 29. Still a couple of free full versions left for people who'll actually break it. Comment BETA 👇`
**Hashtagy:** `#musicproducer #betatesting #producercommunity #musictech #buildinpublic`

---

## Day 30 — utorok 25.8. · Record

**VO (39 slov · ~15 s):**

> „It records itself. Not your cursor and your messy desktop, the actual visual, clean, straight into a file. So you make the track, you get the clip, you post it. That was the whole point from the start, honestly."

**Hook:** „It records itself."

**Nahrávanie:** `alter + controller` · OBS
**Layout:** HUD hore, Controller dole (Record tlačidlo musí byť vidieť)
**Marker cues:** bar 4 → klik Record · bar 10 → stop, otvor priečinok, prehraj výsledok

> Meta záber: nahrávaš appku, ktorá nahráva samu seba. To je zaujímavejšie než samotná funkcia.

**Text na obrazovke:** `your beat, but postable`
**Caption:** `Day 30. Make the track, get the clip, post it. No screen recorder, no cursor, no mess.`
**Hashtagy:** `#musicproducer #contentcreator #audioreactive #musictech #buildinpublic`

---

## Day 31 — streda 26.8. · Ableton automatizácia (flex)

**VO (46 slov · ~18 s):**

> „The visuals are just an automation lane. You draw it once and the thing plays itself forever. Build comes, it opens up. Beat hits, it snaps. I'm not touching anything right now, the session is doing it alone, which is slightly unsettling but I love it."

**Hook:** „The visuals are just an automation lane."

**Nahrávanie:** `alter + ableton` · OBS
**Layout:** HUD hore 62 % (Geometry), dole 38 % **len jedna automation lane** zoomnutá na maximum
**Marker cues:** bar 4 → playhead vojde do stúpajúcej krivky, Geometry sa otvára · bar 10 → krivka spadne, Geometry sa stiahne

> Jeden z dvoch dní, kde má byť Ableton v zábere (druhý je D26). Inde nie — inak si ľudia pomýlia tvoj produkt s DAW.

**Text na obrazovke:** `automated. not touched.`
**Caption:** `Day 31. Visuals as an automation lane. Draw it once, it plays itself every time.`
**Hashtagy:** `#ableton #abletonlive #musicproducer #vjing #audioreactive`

---

## Day 32 — štvrtok 27.8. · funguje na čokoľvek (rozbeh na SQUAD)

**VO (43 slov · ~17 s):**

> „It doesn't care what's playing. It's not a plugin, it just reads whatever your computer is doing. So obviously I fed it my favourite track, then a YouTube video, then an ad, and the ad went stupidly hard. Everything is a visualizer now."

**Hook:** „It doesn't care what's playing."

**Nahrávanie:** `alter + controller` · OBS
**Layout:** HUD hore, Controller dole (prepnutie Audio Input musí byť vidieť)
**Marker cues:** bar 4 → Audio Input → **System Audio**, pusti cudziu skladbu · bar 10 → prepni zdroj, HUD beží ďalej

**Text na obrazovke:** `system audio. anything goes.`
**Caption:** `Day 32. Not a plugin — it reads your whole system. Put anything through it. Tomorrow I'm giving some licences away 👀`
**Hashtagy:** `#audioreactive #musictech #musicproducer #visualart #buildinpublic`

> Posledná veta captionu je zámerná — vytvára očakávanie na finále.

---

## Day 33 — piatok 28.8. · ⭐ PROMO SQUAD (finále)

**VO (39 slov · ~15 s):**

> „Okay, better offer. Ten producers get a full licence, free, permanently. What I want back is a couple of videos, on your page, in your voice, no script, no brand nonsense. Just show it doing something real. Comment SQUAD."

**Hook:** „Ten producers get a full licence, free, permanently."

**Nahrávanie:** `alter only` · vstavaný Record
**Layout:** najlepšia zostava akú máš, plus prepnutie Theme okolo bar 10

**Text na obrazovke:** `10 licences. comment SQUAD`
**Caption:** `Day 33. 10 free full licences for 10 producers who'll actually make something with it. Comment SQUAD 👇`
**Hashtagy:** `#musicproducer #contentcreator #musictech #buildinpublic #producercommunity`

---

# ČASŤ 5 — Poradie nahrávania

Nahrávaj **po scénach**, nie po dňoch.

**Blok 1 — `alter only`, vstavaný Record (7 dní, 8 videí):**
Dni 24, 25, 26, 27, 28, 29 (×2 verzie), 33

**Blok 2 — `alter + controller`, OBS (13 videí):**
Dni 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 30, 32

**Blok 3 — `alter + ableton`, OBS (2 videá):**
Dni 23, 31

---

# ČASŤ 6 — Ešte nepoužité (na po 28.8.)

Nie zásobník na odloženie — reálne zvyšné veci, ktoré appka vie a ešte si ich neukázal:

- **Fusion — vrstvenie modulov cez seba.** Vizuálne najneobvyklejšia vec v appke a konkurencia to nemá. Toto by malo ísť ako prvé po finále.
- **Spectrum — measurement (SPAN) mód, A-weighting, stereo L+R prekryté**
- **Audio Meter — farebné módy** (Standard / Gradient / Spectrum)
- **Synesthesia — BPM sync**, **Chladni — manuálne (m,n) a mode shift**
- **Theme prepínanie**, **Instance / VST vstup**
- **build in public** a **cena vs konkurencia** (až keď máš cenu uzavretú)

---

# ČASŤ 7 — Čo sledovať, keď sa vrátiš

1. **Koľko ľudí napísalo BETA a SQUAD** — jediná metrika, ktorá teraz rozhoduje
2. **Watch time pod 50 %** = zlyhal hook, nie obsah
3. **Porovnaj metering blok (14–17) s kreatívnym (24–26) a stereo (21–22)** — uvidíš, či tvoje publikum chce učiť sa alebo pozerať

Ak z 22 videí vyjdú dve dobre, je to normálne. Cieľ nie je 22 hitov, cieľ je 15 producentov v DM.
