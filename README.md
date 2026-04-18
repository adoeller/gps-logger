# GPS Tracker – Flipper Zero (Momentum Firmware)

---

## Deutsch

### Kurzbeschreibung

Externe App für den Flipper Zero, die einen NMEA-GPS-Empfänger über die GPIO-Pins ausliest und Koordinaten, Zeit, Höhe, Geschwindigkeit sowie Maidenhead-Locator auf dem Display anzeigt. Positionsdaten werden sekündlich als CSV auf die SD-Karte geloggt und gleichzeitig an einen angeschlossenen PC weitergeleitet.

---

### Hardware-Verdrahtung

```
Flipper Zero          GPS-Modul
─────────────         ─────────────────────────────
Pin 13  (TX)    ───►  RX
Pin 14  (RX)    ◄───  TX
Pin  9  (3,3 V) ───►  VCC  (3,3-V-Module)
Pin  1  (5 V)   ───►  VCC  (5-V-Module, z. B. NEO-6M-Breakout)
Pin 11  (GND)   ───►  GND
```

Getestete Module: u-blox NEO-6M/7M/8M, ATGM332D, GY-GPS6MV2.

---

### Kompilieren & Installieren

```bash
# Im Ordner gps_tracker/ (ufbt muss installiert sein)
ufbt build
ufbt launch        # direkt flashen via USB
# oder:
ufbt install       # .fap auf SD-Karte kopieren
```

Manuell: `.fap`-Datei nach `SD:/apps/GPS/gps_tracker.fap` kopieren.

---

### Bildschirme & Bedienung

#### Setup-Screen (erscheint beim Start)

```
┌────────────────────────────────┐
│ GPS Tracker Setup              │
├────────────────────────────────┤
│ TX  Pin 13  ->  GPS RX         │
│ RX  Pin 14  <-  GPS TX         │
│ VCC Pin  9 (3.3V) / 1 (5V)    │
│ GND Pin 11                     │
├────────────────────────────────┤
│ Baud: [9k6] 19k2 38k4 115k    │
└────────────────────────────────┘
```

| Taste   | Funktion                        |
|---------|---------------------------------|
| ← / ↑   | Baudrate verringern             |
| → / ↓   | Baudrate erhöhen                |
| OK      | Bestätigen → GPS-Screen starten |
| BACK    | App beenden                     |

Die gewählte Baudrate ist **invertiert** hervorgehoben.
Verfügbare Baudraten: **9k6 · 19k2 · 38k4 · 115k**

#### GPS-Screen

```
┌────────────────────────────────┐
│ GPS Tracker        LOG*42      │
├────────────────────────────────┤
│ Lat:  53.5503419 N             │
│ Lon:   9.9936820 E             │
│ Alt:    12m  Sat:08            │
│ Spd:  0.2km/h       JO53mn47  │
│ 10:23:45Z     [ 3D ]           │
└────────────────────────────────┘
```

| Taste | Funktion                          |
|-------|-----------------------------------|
| ↑     | Hintergrundbeleuchtung dauerhaft AN  |
| ↓     | Hintergrundbeleuchtung dauerhaft AUS |
| OK    | SD-Logging ein-/ausschalten       |
| BACK  | App beenden                       |

**Statusanzeigen:**

| Anzeige    | Bedeutung                              |
|------------|----------------------------------------|
| `LOG OFF`  | Logging inaktiv                        |
| `LOG*42`   | Logging aktiv, 42 Einträge bisher      |
| `[----]`   | Kein Fix                               |
| `[ 2D ]`   | 2D-Fix (Höhe ungenau)                  |
| `[ 3D ]`   | 3D-Fix                                 |
| `[DGPS]`   | Differenzielles GPS                    |
| `[ RTK]`   | RTK-Fix (höchste Genauigkeit)          |

Der Maidenhead-Locator (8 Zeichen, z. B. `JO53mn47`) steht immer **rechtsbündig** neben der Geschwindigkeit.

#### Beleuchtungs-Toast

Nach dem Drücken von ↑ oder ↓ erscheint kurz eine zentrierte Rückmeldung:
```
  ┌────────────────────┐
  │ Licht: dauerhaft AN │
  └────────────────────┘
```
Die Beleuchtungseinstellung wird beim Beenden der App automatisch auf **Automatik** zurückgesetzt.

---

### LED-Blinkcodes (pro Epoche / Sekunde)

| Fix-Stufe         | LED-Muster        |
|-------------------|-------------------|
| Kein Fix          | 🔴 · 🔵 · 🔴      |
| 2D- oder 3D-Fix   | 🟢 · 🔵 · 🟢      |
| DGPS oder RTK     | 🟢 · 🟢 · 🟢      |

---

### USB-Weiterleitung (NMEA → PC)

Wenn ein PC per USB angeschlossen ist, werden alle NMEA-Rohdaten automatisch an den virtuellen COM-Port weitergeleitet – ohne Konfiguration, im laufenden Betrieb. Das An- und Abstecken des Kabels unterbricht die App nicht.

Jede Nachricht wird mit `\r\n` abgeschlossen. Am PC erscheint der Flipper als:
- Windows: `COMx`
- Linux/macOS: `/dev/ttyACM0`

Kompatibel mit: u-center (u-blox), PuTTY, minicom, screen, QGIS.

---

### Log-Datei

**Pfad:** `/ext/gps_log.csv`  
**Modus:** Append – bestehende Daten bleiben erhalten  
**Intervall:** 1 Eintrag pro Sekunde (nur bei gültigem Fix)

**Format:**
```csv
DateTime_UTC,Latitude,Longitude,Altitude_m,Speed_kmh,Satellites
2024-06-15T10:23:45Z,53.5503419,9.9936820,12.0,0.20,8
2024-06-15T10:23:46Z,53.5503420,9.9936821,12.1,0.22,8
```

| Feld           | Format                  | Einheit |
|----------------|-------------------------|---------|
| `DateTime_UTC` | ISO 8601: `YYYY-MM-DDTHH:MM:SSZ` | UTC |
| `Latitude`     | Dezimalgrad, + = Nord, 7 Stellen | ° |
| `Longitude`    | Dezimalgrad, + = Ost, 7 Stellen  | ° |
| `Altitude_m`   | Höhe über NN             | m       |
| `Speed_kmh`    | Geschwindigkeit          | km/h    |
| `Satellites`   | Verwendete Satelliten    | –       |

---

### Unterstützte NMEA-Sätze

| Satz              | Inhalt                                         |
|-------------------|------------------------------------------------|
| `$GPRMC`/`$GNRMC` | Zeit, Datum, Koordinaten, Geschwindigkeit, Fix |
| `$GPGGA`/`$GNGGA` | Satellitenzahl, Höhe, Fix-Qualität            |

---

### Troubleshooting

| Problem                      | Lösung                                            |
|------------------------------|---------------------------------------------------|
| „Searching for GPS..."       | Baudrate prüfen, RX/TX vertauscht?, VCC korrekt?  |
| Fix bleibt aus               | Modul braucht freie Himmelssicht                  |
| Logging schlägt fehl         | SD-Karte eingelegt und nicht schreibgeschützt?    |
| Koordinaten zeigen 0.0000000 | Modul sendet, aber Status `V` (kein Fix)          |
| Kein USB-COM-Port sichtbar   | Flipper-USB-Profil muss auf „Serial" stehen       |

---
---

## English

### Summary

External app for the Flipper Zero that reads a NMEA GPS receiver via the GPIO pins and displays coordinates, time, altitude, speed and Maidenhead locator on screen. Position data is logged every second as a CSV file to the SD card and simultaneously forwarded to a connected PC.

---

### Hardware Wiring

```
Flipper Zero          GPS module
─────────────         ─────────────────────────────
Pin 13  (TX)    ───►  RX
Pin 14  (RX)    ◄───  TX
Pin  9  (3.3 V) ───►  VCC  (3.3 V modules)
Pin  1  (5 V)   ───►  VCC  (5 V modules, e.g. NEO-6M breakout)
Pin 11  (GND)   ───►  GND
```

Tested modules: u-blox NEO-6M/7M/8M, ATGM332D, GY-GPS6MV2.

---

### Build & Install

```bash
# Inside the gps_tracker/ folder (ufbt must be installed)
ufbt build
ufbt launch        # flash directly via USB
# or:
ufbt install       # copy .fap to SD card
```

Manual: copy the `.fap` file to `SD:/apps/GPS/gps_tracker.fap`.

---

### Screens & Controls

#### Setup Screen (shown at launch)

```
┌────────────────────────────────┐
│ GPS Tracker Setup              │
├────────────────────────────────┤
│ TX  Pin 13  ->  GPS RX         │
│ RX  Pin 14  <-  GPS TX         │
│ VCC Pin  9 (3.3V) / 1 (5V)    │
│ GND Pin 11                     │
├────────────────────────────────┤
│ Baud: [9k6] 19k2 38k4 115k    │
└────────────────────────────────┘
```

| Button  | Action                          |
|---------|---------------------------------|
| ← / ↑   | Decrease baud rate              |
| → / ↓   | Increase baud rate              |
| OK      | Confirm → switch to GPS screen  |
| BACK    | Exit app                        |

The selected baud rate is highlighted with **inverted colours**.  
Available baud rates: **9k6 · 19k2 · 38k4 · 115k**

#### GPS Screen

```
┌────────────────────────────────┐
│ GPS Tracker        LOG*42      │
├────────────────────────────────┤
│ Lat:  53.5503419 N             │
│ Lon:   9.9936820 E             │
│ Alt:    12m  Sat:08            │
│ Spd:  0.2km/h       JO53mn47  │
│ 10:23:45Z     [ 3D ]           │
└────────────────────────────────┘
```

| Button | Action                               |
|--------|--------------------------------------|
| ↑      | Backlight permanently ON             |
| ↓      | Backlight permanently OFF            |
| OK     | Toggle SD card logging on/off        |
| BACK   | Exit app                             |

**Status indicators:**

| Display   | Meaning                              |
|-----------|--------------------------------------|
| `LOG OFF` | Logging inactive                     |
| `LOG*42`  | Logging active, 42 entries written   |
| `[----]`  | No fix                               |
| `[ 2D ]`  | 2D fix (altitude unreliable)         |
| `[ 3D ]`  | 3D fix                               |
| `[DGPS]`  | Differential GPS                     |
| `[ RTK]`  | RTK fix (highest accuracy)           |

The Maidenhead locator (8 chars, e.g. `JO53mn47`) is always **right-aligned** next to the speed value.

#### Backlight Toast

After pressing ↑ or ↓ a centred confirmation overlay appears briefly:
```
  ┌──────────────────────┐
  │ Licht: dauerhaft AN  │
  └──────────────────────┘
```
The backlight setting is automatically restored to **auto** when the app exits.

---

### LED Blink Codes (once per epoch / second)

| Fix level         | LED pattern       |
|-------------------|-------------------|
| No fix            | 🔴 · 🔵 · 🔴      |
| 2D or 3D fix      | 🟢 · 🔵 · 🟢      |
| DGPS or RTK       | 🟢 · 🟢 · 🟢      |

---

### USB Forwarding (NMEA → PC)

When a PC is connected via USB, all raw NMEA data is forwarded automatically to the virtual COM port – no configuration required, while the app is running. Plugging or unplugging the cable does not affect the app.

Each sentence is terminated with `\r\n`. The Flipper appears as:
- Windows: `COMx`
- Linux/macOS: `/dev/ttyACM0`

Compatible with: u-center (u-blox), PuTTY, minicom, screen, QGIS.

---

### Log File

**Path:** `/ext/gps_log.csv`  
**Mode:** Append – existing data is preserved  
**Interval:** 1 entry per second (only when fix is valid)

**Format:**
```csv
DateTime_UTC,Latitude,Longitude,Altitude_m,Speed_kmh,Satellites
2024-06-15T10:23:45Z,53.5503419,9.9936820,12.0,0.20,8
2024-06-15T10:23:46Z,53.5503420,9.9936821,12.1,0.22,8
```

| Field          | Format                           | Unit |
|----------------|----------------------------------|------|
| `DateTime_UTC` | ISO 8601: `YYYY-MM-DDTHH:MM:SSZ` | UTC  |
| `Latitude`     | Decimal degrees, + = North, 7 dp | °    |
| `Longitude`    | Decimal degrees, + = East, 7 dp  | °    |
| `Altitude_m`   | Altitude above sea level         | m    |
| `Speed_kmh`    | Ground speed                     | km/h |
| `Satellites`   | Satellites used                  | –    |

---

### Supported NMEA Sentences

| Sentence          | Content                                         |
|-------------------|-------------------------------------------------|
| `$GPRMC`/`$GNRMC` | Time, date, coordinates, speed, fix status      |
| `$GPGGA`/`$GNGGA` | Satellite count, altitude, fix quality          |

---

### Troubleshooting

| Problem                        | Solution                                           |
|--------------------------------|----------------------------------------------------|
| "Searching for GPS..."         | Check baud rate, RX/TX swapped?, VCC correct?      |
| No fix acquired                | Module needs a clear view of the sky               |
| Logging fails                  | SD card inserted and not write-protected?          |
| Coordinates show 0.0000000     | Module is sending but status is `V` (no fix)       |
| No USB COM port visible        | Flipper USB profile must be set to "Serial"        |
