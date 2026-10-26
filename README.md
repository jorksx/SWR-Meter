# ESP32 HF SWR & Power Meter (1.8 – 50 MHz)

Reaaliaikainen, langaton ja erittäin tarkka HF-alueen (160m – 6m) SWR- ja tehomittari radioamatöörikäyttöön. 

Mittaus perustuu **Stockton / Tandem-Match** -suuntakytkimeen ja **ESP32**-mikrokontrolleriin. Laitteessa on sisäänrakennettu nopea Web-käyttöliittymä (portti 2222) sekä kustomoitu, kehyksetön työpöytäsovellus Windowsille viidellä eri teemalla (ml. aito mekaanisesti vaimennettu Cross-Needle -ristiviisarimittari).

---

## ⚠️ ERITTÄIN TÄRKEÄÄ: 10 kΩ PURKUVASTUKSET (HUOMIOI KIINAN KITIT!)

> [!CAUTION]
> ### 🔴 KIINAN TANDEM MATCH / STOCKTON -KITEISSÄ ON KRIITTINEN PUUTE!
> Kiinasta (AliExpress, eBay, Banggood) tilattavissa edullisissa Stockton/Tandem Match DIY-kytkinpaketeissa **EI TULE MUKANA** ilmaisindiodeiden kuorma-/purkuvastuksia (bleeder / pulldown resistors), tai ne puuttuvat piirilevyltä kokonaan!
>
> **Miksi tämä on kriittistä?**
> Suuntakytkimen ilmaisindiodit tasasuuntaavat RF-jännitteen suodatinkonkkien yli tasajännitteeksi (DC). ESP32:n analogiatulojen (GPIO 34 & 35) tuloimpedanssi on erittäin suuri (>10 MΩ). **Ilman kuormavastuksia kondensaattorit eivät pääse koskaan purkautumaan maihin!**
>
> **Mitä tapahtuu, jos unohdat 10 kΩ vastukset:**
> 1. Kondensaattorit latautuvat ensimmäisellä lähetteellä huippujännitteeseen ja **jäävät jumiin** – mittari näyttää jatkuvaa tehoa ja SWR-lukemaa vielä lähetyksen päätyttyäkin!
> 2. Tulot alkavat kellua ja poimivat 50 Hz verkkohurinaa ja staattista häiriötä, jolloin RX-tilassa näkyy "haamutehoja".
> 3. SWR-laskenta antaa täysin vääriä lukemia.
>
> ### ✅ RATKAISU:
> Kytke kaksi tavallista **10 kΩ vastusta** (1/4 W tai SMD):
> - **1 kpl 10 kΩ vastus FWD-lähdön ja MAAN (GND) väliin**
> - **1 kpl 10 kΩ vastus REV-lähdön ja MAAN (GND) väliin**
>
> *(Voit juottaa vastukset suoraan kytkinpiirilevyn FWD/REV-lähtöjen ja maapinnan väliin tai ESP32:n GPIO 34/35 -pinnien ja GND:n väliin).*

---

## 🔌 Kytkentäkaavio (Wiring)

```
 [ Radio / TX ] ---> [ Stockton / Tandem Coupler ] ---> [ Antenni / Keinokuorma ]
                               |           |
                              FWD         REV
                               |           |
                               +--[10kΩ]   +--[10kΩ]   (PURKUVASTUKSET MAIHIN!)
                               |    |      |    |
                              ( )  GND    ( )  GND
                               |           |
                               v           v
                          ESP32 GPIO 34  ESP32 GPIO 35
                          (PIN_FWD)      (PIN_REV)

 Yhteinen maa: Kytkimen GND  ===========>  ESP32 GND
 Virransyöttö: ESP32 Micro-USB (5V) tai VIN-pinni
```

### Pinnijärjestys (ESP32):
| ESP32 Pinni | Signaali | Huomiot |
|---|---|---|
| **GPIO 34** | `FWD` (Etenevä jännite) | + 10 kΩ vastus maihin (GND) |
| **GPIO 35** | `REV` (Heijastunut jännite) | + 10 kΩ vastus maihin (GND) |
| **GND** | Maa | Kytketään suuntakytkimen maahan |
| **5V / USB** | Käyttöjännite | 5V virransyöttö |

---

## 🎯 Kalibrointiohje (Vaihe vaiheelta)

Kalibrointi tehdään kahdessa helpossa vaiheessa. Tarvitset **radion**, **50 Ω keinokuorman (Dummy Load)** ja luotettavan tehomittarin (tai radion oman tehonäytön).

### Vaihe 1: Etenevän tehon kalibrointi (`PWR_CAL_FACTOR`)
1. Kytke: **Radio ➔ Suuntakytkimen TX-liitin ➔ Suuntakytkimen ANT-liitin ➔ 50 Ω Keinokuorma**.
2. Lähetä jatkuvaa kantoaaltoa (CW, FM tai RTTY) tunnetulla teholla, esim. **100 W** (tai 50 W).
3. Katso työpöytäohjelman alareunasta tai Web UI:sta havaittu jännite **`fwd_v`** (esim. `2.68 V`).
4. Laske kalibrointikerroin kaavalla:
   $$\text{PWR\_CAL\_FACTOR} = \frac{\text{Todellinen teho (W)}}{(V_{\text{fwd}})^2}$$
   *Esimerkki:* Jos todellinen teho on 100 W ja $V_{\text{fwd}} = 2.68\text{ V}$:
   $$\text{PWR\_CAL\_FACTOR} = \frac{100}{2.68 \times 2.68} = \frac{100}{7.1824} \approx 13.92$$
5. Avaa [src/main.cpp](file:///src/main.cpp) ja päivitä rivi 15:
   ```cpp
   const float PWR_CAL_FACTOR = 13.85; // Aseta oma laskettu arvosi tähän
   ```

### Vaihe 2: Heijastuneen haaran tasapainotus (`REV_CAL_FACTOR`)
Diodien ja ferriittimuuntajien pienet toleranssierot korjataan tasapainottamalla kanavat:
1. **Käännä suuntakytkimen RF-kaapelit toisinpäin:**
   - Liitä Radio kytkimen **ANT**-liittimeen.
   - Liitä 50 Ω keinokuorma kytkimen **TX**-liittimeen.
2. Lähetä matalalla teholla (esim. **10 W** tai **20 W**).
3. Nyt teho kulkee suuntakytkimen läpi väärään suuntaan, jolloin teho näkyykin heijastuneena jännitteenä (`rev_v`).
4. Tarkista ohjelmasta `fwd_v` ja `rev_v`. Säädä kerrointa `REV_CAL_FACTOR` tiedostossa [src/main.cpp](file:///src/main.cpp) (rivi 18) siten, että `rev_v` vastaa tarkalleen `fwd_v`:tä (eli mitattu heijastusteho vastaa todellista lähetystehoa):
   ```cpp
   const float REV_CAL_FACTOR = 1.35; // Säädä tarvittaessa
   ```
5. **Käännä RF-kaapelit takaisin oikein päin:** (Radio ➔ TX, ANT ➔ Antenni / Keinokuorma).

### Vaihe 3: SWR-mittauksen varmistus
- Lähetä 50 Ω keinokuormaan: SWR-lukeman tulee olla **1.00 : 1** (tai enintään 1.05 : 1) ja heijastuneen tehon `0.00 W`.
- Jos kytket keinokuorman tilalle esim. 100 Ω kuorman tai 25 Ω kuorman (kaksi 50 Ω keinokuormaa rinnakkain), mittarin tulee näyttää tarkasti SWR **2.00 : 1**.

---

## 💻 Ohjelmistot ja käyttöliittymät

### 1. ESP32 Firmware ([src/main.cpp](file:///src/main.cpp))
- **200 Hz FreeRTOS-mittaussilmukka** Core 0:ssa: salamannopea analogianäytteistys ja Transient Blanking (estää SWR-piikit lähetyksen kytkentähetkellä).
- **PEP & True AVG:** Huipputehon pito (Peak Hold 1.5s) sekä ballistinen puhetehon keskiarvosuodatus (VU-mittarityyli SSB:lle).
- **Sisäänrakennettu Web Server (Portti 2222):** Responsiivinen mobiili- ja selainkäyttöliittymä liukuväreillä ja animoiduilla mittareilla (`http://swrmeter.local:2222` tai suora IP).
- **Langaton päivitys (Web OTA):** Firmwaren voi päivittää suoraan selaimella osoitteessa `http://<ip>:2222/update` ilman USB-kaapelia.

### 2. Windows Työpöytäsovellus ([swr_monitor.py](file:///swr_monitor.py))
Kehyksetön tyylikäs Windows-työpöytäikkuna, jota voi siirtää raahaamalla ja joka pysyy haluttaessa aina päällimmäisenä (`PIN 📌`).

**5 Vaihdettavaa teemaa (ikkuna skaalautuu automaattisesti ilman hukkatilaa):**
1. **MODERN:** Liukuvärikortit, PEP- ja AVG-tehopalkit, reaaliaikainen SWR-väripalkki ja tarkat telemetrianumerot.
2. **CROSS-NEEDLE:** Aito analoginen Daiwa/Nissei -tyylinen ristiviisarimittari tummalla instrumenttitaululla, pehmennetyllä mekaanisella viisari-inertialla ja alakeskuksen punaisilla SWR-viuhkoilla.
3. **MATRIX:** Cyberpunk-tyylinen vihreä ASCII-terminaali, palkit ja reaaliaikainen RF-kantoaallon oskillogrammi.
4. **RETRO RIG:** Kaksi suurta pyöreää analogista mittarikelloa metallireunuksin ja lämpimällä oranssilla valaistuksella.
5. **CLEAN OLED:** Ultraminimalistinen, virtaviivainen ja huippukontrastinen HUD-näyttö.

**Automaattinen anomaliavalvonta (visuaaliset hälytykset):**
- Äkilliset SWR-hypyt kesken lähetyksen.
- Korkea SWR (>3.0:1) -varoitus.
- Käänteisen jännitteen poikkeama (`REV > FWD`).
- ESP32 ADC-yliohjaus / leikkausvaroitus (`> 3.05 V`).
- Hälytyksen sattuessa ikkuna laajenee automaattisesti ja vilkuttaa huomioväriä ilman ärsyttäviä piippausääniä.

---

## 🚀 Asennus ja käyttöönotto

### 1. Firmwaren flashaus ESP32:lle
Voit kääntää ja asentaa firmwaren suoraan [PlatformIO](https://platformio.org/):lla:
```bash
pio run --target upload
```
*(Muokkaa tarvittaessa `WIFI_SSID` ja `WIFI_PASS` tiedostosta `src/main.cpp`).*

### 2. Työpöytäsovelluksen käynnistys
Varmista, että tietokoneessasi on Python 3 ja asenna Pillow:
```bash
pip install pillow
```
Käynnistä mittari kaksoisnapsauttamalla tiedostoa:
**`Kaynnista_SWR_Mittari.bat`**

Ohjelma etsii mittarin automaattisesti verkosta ja muistaa viimeksi käyttämäsi teeman sekä ikkunan sijainnin ruudulla!

---

## 📄 Lisenssi
MIT License – Vapaasti käytettävissä ja muokattavissa amatööriradiotoimintaan. OH-amatööriterveisin! 73!
