# SegmaSirra 🌱

![Platform](https://img.shields.io/badge/platform-ESP32--WROOM--32-blue?style=flat&logo=espressif&logoColor=white)
![Language](https://img.shields.io/badge/firmware-C%2B%2B%20%2F%20Arduino%20IDE-00979D?style=flat&logo=arduino&logoColor=white)
![App](https://img.shields.io/badge/app-Android%20%2F%20Kotlin-3DDC84?style=flat&logo=android&logoColor=white)
![Energy](https://img.shields.io/badge/energia-Solare-yellow?style=flat&logo=solus&logoColor=white)
![License](https://img.shields.io/badge/license-MIT-green?style=flat)

Serra completamente automatizzata, alimentata a energia solare, controllabile da app Android.

> 🌐 Sito del progetto: [SegmaSito](https://github.com/Dragonyx118/SegmaSito)

---

## Hardware

| Componente | Dettagli |
|---|---|
| Microcontrollore | ESP32-WROOM-32 |
| Alimentazione | Pannello solare + batteria AGM 12V 7.2Ah |
| Raffreddamento piante | Ventola 12V |
| Raffreddamento elettronica | Ventola 12V |
| Irrigazione | 2 pompe + 2 cisterne da 1–1.5L |
| Tetto | 2 servo motori (apertura automatica) |
| Illuminazione | Luci notturne controllabili da app |
| Sensore clima | Temperatura + umidità |
| Sensore luce | Rilevazione luminosità ambientale |

---

## Funzionalità

- **Irrigazione automatica** — le pompe si attivano in base all'umidità del suolo
- **Tetto apribile** — i servo motori aprono/chiudono il tetto in base a temperatura o comando manuale
- **Ventilazione** — due ventole indipendenti per l'ambiente delle piante e per la scatola elettronica
- **Luci notturne** — accendibili manualmente dall'app
- **Monitoraggio clima** — temperatura, umidità e luce in tempo reale
- **Alimentazione solare** — autonomia garantita dalla batteria da 12V 7.2Ah
- **Controllo remoto** — tutto gestibile via app Android tramite Wi-Fi

---

## Struttura della repository

```
SegmaSirra/
├── SegmaSirraFirmware.ino   # Firmware per ESP32 (Arduino IDE)
├── Serra/                   # Progetto app Android (Kotlin)
├── PCBnuova/                # Schema PCB
└── LICENSE
```

---

## Come iniziare

### Firmware (ESP32)

1. Apri `SegmaSirraFirmware.ino` con **Arduino IDE**
2. Seleziona la board `ESP32-WROOM-32`
3. Installa le librerie necessarie dal Library Manager
4. Carica il firmware sulla scheda

### App Android

1. Apri la cartella `Serra/` con **Android Studio**
2. Sincronizza il progetto con Gradle
3. Esegui su un dispositivo Android fisico o emulatore

---

## Licenza

MIT © 2026 Dragonyx
