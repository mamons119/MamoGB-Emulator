# 🎮 MamoGB emulator per CardPuter

Emulatore **Game Boy (DMG)** per **M5Stack Cardputer** (ESP32-S3), con **streaming dello schermo nel browser**.

- 💾 Scegli le ROM direttamente dalla **microSD**, con un menu sul display del Cardputer
- 📺 Gioca sul Cardputer **oppure dal browser** del PC o del telefono: lo schermo viene trasmesso via Wi-Fi e i comandi tornano al Cardputer
- 🔋 Salvataggi con batteria (`.sav`) su microSD
- 🪫 Indicatore di carica della batteria nel menu

L'emulazione è basata su [Peanut-GB](https://github.com/deltabeard/Peanut-GB) di Mahyar Koshkouei (licenza MIT).

---

## 🧰 Hardware

- 🕹️ M5Stack **Cardputer** (testato su v1.1)
- 💽 Scheda **microSD** formattata **FAT32**

## 🚀 Installazione

### 1️⃣ Arduino IDE

1. In *File → Impostazioni → URL aggiuntivi per il Gestore schede* aggiungi:
   `https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`
2. In *Gestore schede* installa **M5Stack**.
3. In *Gestore librerie* installa:
   - 📦 **M5Cardputer** (con le dipendenze M5Unified e M5GFX)
   - 📦 **WebSockets** di Markus Sattler
4. *Strumenti → Scheda*: **M5Cardputer** (oppure M5StampS3 se non è presente)
5. *Strumenti → CPU Frequency*: 240 MHz
6. Apri `MamoGB/MamoGB.ino` e caricalo sul Cardputer.

> ⚠️ Il file `peanut_gb.h` deve restare nella stessa cartella di `MamoGB.ino`, e la cartella deve avere lo stesso nome del `.ino`.

> 💡 Se l'upload non parte, metti il Cardputer in modalità download: tieni premuto **G0** mentre lo colleghi, oppure premi reset.

### 2️⃣ MicroSD

```
/roms/     <- metti qui i file .gb / .gbc
/saves/    <- creata automaticamente
```

Se `/roms` non esiste, le ROM vengono cercate nella root della scheda.

## 🎯 Uso

### ⌨️ Sul Cardputer

| Azione | Tasto |
|---|---|
| Direzioni | `;` `.` `,` `/`  oppure  `W` `A` `S` `D` |
| A | `J` |
| B | `K` |
| START | `Enter` |
| SELECT | `Del` |
| Torna al menu | `` ` `` (Esc) |
| Cambia palette | `P` |
| Streaming on/off | `Spazio` |

Nel menu: su/giù scorrono la lista, sinistra/destra saltano di una pagina, **A** o **Enter** avviano la ROM.

### 🌐 Dal browser

1. Collegati al Wi-Fi creato dal Cardputer: SSID `PocketStream`, password `cardputer`
2. Apri `http://192.168.4.1` (l'indirizzo è scritto anche in fondo al menu)
3. Usa i pulsanti a schermo o la tastiera: frecce/WASD, `J` = A, `K` = B, `Enter` = START, `Backspace` = SELECT, `Esc` = menu, `P` = palette

Il menu delle ROM si vede e si usa anche dal browser.

## ⚙️ Come funziona

- 💿 **ROM sulla SD, non in RAM:** l'ESP32-S3 non ha abbastanza memoria per ROM grandi, quindi i blocchi da 4 KiB vengono letti dalla SD quando servono e tenuti in una cache.
- 📡 **Streaming:** il framebuffer 160×144 a 2 bit per pixel (5760 byte) viene inviato via WebSocket a circa 20 FPS. Il browser lo colora con la palette scelta.
- ⏩ **Frame skip automatico:** se l'emulazione è troppo lenta, salta frame per mantenere la velocità di gioco.
- 💾 **Salvataggi:** scritti su `/saves/<nome>.sav` due secondi dopo l'ultima modifica e all'uscita dal gioco, tramite file temporaneo per non rovinare il salvataggio precedente.

## 🚧 Limiti noti

- 🔇 Nessun **audio**
- 🟩 Solo giochi **DMG**: i giochi Game Boy Color solo-CGB non funzionano, quelli compatibili girano in modalità DMG
- 🧩 Supporta le cartucce gestite da Peanut-GB (MBC1, MBC2, MBC3, MBC5 e senza MBC); le altre vengono rifiutate con un messaggio
- 🧠 Il Cardputer non ha PSRAM: ROM molto grandi o con RAM di cartuccia da 128 KB possono non entrare in memoria
- ⏰ L'orologio (RTC) dei giochi MBC3 parte da zero a ogni avvio

## 🔍 Diagnostica

Il firmware scrive sulla porta seriale (115200 baud) una riga al secondo durante il gioco: FPS, memoria libera, durata del frame più lungo, letture dalla SD e invio più lento al browser. All'accensione, il display mostra il motivo dell'ultimo riavvio (crash, watchdog, brownout...).

Per vedere la seriale in Arduino IDE imposta *Strumenti → USB CDC On Boot → Enabled*.

## 📀 ROM

Questo repository **non contiene ROM**. Vanno scaricati ed inseriti nella microSD (/roms)

## 🙏 Crediti e licenze

- 🥜 [Peanut-GB](https://github.com/deltabeard/Peanut-GB) — Mahyar Koshkouei, licenza MIT (header incluso in `peanut_gb.h` con la sua nota di copyright)
- 🔧 [M5Cardputer / M5Unified / M5GFX](https://github.com/m5stack) — M5Stack
- 🔌 [arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets) — Markus Sattler
