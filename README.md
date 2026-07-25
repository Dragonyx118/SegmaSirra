# 🌿 SegmaSirra — Smart Solar Greenhouse

<p align="center">
<b>Un'ecosistema IoT completo per la gestione, l'irrigazione e il monitoraggio remoto di una serra automatizzata e autosufficiente.</b>
</p>

<p align="center">
<img src="https://img.shields.io/badge/Microcontroller-ESP32--WROOM--32-blue?style=for-the-badge&logo=espressif" alt="ESP32" />
<img src="https://img.shields.io/badge/Language-C%2B%2B-00599C?style=for-the-badge&logo=cplusplus" alt="C++" />
<img src="https://img.shields.io/badge/Android-Kotlin-purple?style=for-the-badge&logo=android" alt="Android" />
<img src="https://img.shields.io/badge/Power-12V%20Solar%20AGM-yellow?style=for-the-badge&logo=solaredge" alt="Solar Powered" />
<img src="https://img.shields.io/badge/License-MIT-green?style=for-the-badge" alt="MIT License" />
</p>

<br />

<p align="center">
<img src="data/img/img04.jpeg" alt="SegmaSirra Main Overview" width="85%" style="border-radius: 12px; box-shadow: 0 4px 8px rgba(0,0,0,0.2);">
</p>

---

## 📌 Indice
- [Cos'è SegmaSirra?](#-cosè-segmasirra)
- [Galleria Fotografica](#-galleria-fotografica)
- [Architettura del Sistema](#-architettura-del-sistema)
- [Specifiche Hardware](#-specifiche-hardware)
- [Funzionalità nel Dettaglio](#-funzionalità-nel-dettaglio)
- [Struttura della Repository](#-struttura-della-repository)
- [Guida all'Installazione](#-guida-allinstallazione)
- [1. Firmware (ESP32)](#1-firmware-esp32)
- [2. App Android](#2-app-android)
- [Roadmap & Sviluppi Futuri](#-roadmap--sviluppi-futuri)
- [Licenza](#-licenza)

---

## 🍃 Cos'è SegmaSirra?

**SegmaSirra** è una serra domotica completamente **autosufficiente dal punto di vista energetico**, progettata per ottimizzare la crescita delle piante senza richiedere interventi umani continui.
**SegmaSirra** è una serra domotica completamente **autosufficiente**, progettata per ottimizzare la crescita delle piante senza richiedere interventi umani continui.

Grazie al microcontrollore **ESP32-WROOM-32** e all'alimentazione integrata da pannello solare a 12V, il sistema analizza costantemente i parametri ambientali (temperatura, umidità del suolo e dell'aria, luminosità) ed esegue azioni correttive automatiche. Tutto l'ecosistema è interfacciato in tempo reale con un'app Android dedicata tramite connessione Wi-Fi.

🌐 **Sito del Progetto:** [dragonyx118.github.io/SegmaSirra-Web/](https://dragonyx118.github.io/SegmaSirra-Web/)

---

## 📱 App Android (SegmaIpp)

Il controllo e il monitoraggio remoto della serra avvengono tramite un'applicazione Android sviluppata ad hoc e ospitata in una repository dedicata:

👉 **Repository dell'App:** [**Dragonyx118/SegmaIpp**](https://github.com/Dragonyx118/SegmaIpp)

### Caratteristiche dell'App:
* **Dashboard in tempo reale:** Visualizzazione dei dati di temperatura, umidità ambientale, umidità del terreno e luminosità.
* **Controllo remoto manuale:** Attivazione forzata dell'irrigazione, gestione delle luci notturne ed apertura/chiusura del tetto via Wi-Fi.
* **Notifiche ed avvisi:** Monitoraggio dello stato delle cisterne e della batteria.

---

## 📸 Galleria Fotografica
## 📸 Galleria

<div align="center">
<table>
<tr>
 <td align="center" width="50%">
   <img src="data/img/img04.jpeg" width="100%" alt=""/><br />
   <b></b>
 </td>
 <td align="center" width="50%">
   <img src="data/img/img01.jpeg" width="100%" alt="" onerror="this.src='data/img/img04.jpeg'"/><br />
   <b></b>
 </td>
</tr>
<tr>
 <td align="center" width="50%">
   <img src="data/img/img02.jpeg" width="100%" alt="" onerror="this.src='data/img/img04.jpeg'"/><br />
   <b></b>
 </td>
 <td align="center" width="50%">
   <img src="data/img/img03.jpeg" width="100%" alt="" onerror="this.src='data/img/img04.jpeg'"/><br />
   <b></b>
 </td>
</tr>
</table>
</div>

---

## ⚡ Architettura del Sistema

```text
              +----------------------------------+
              |     Pannello Solare + Reg.      |
              +-----------------+----------------+
                                |
                                v
                      +------------------+
                      | Batteria AGM 12V |
                      +--------+---------+
                                |
                                v
+--------------------------------+--------------------------------+
|                       Modulo ESP32-WROOM-32                      |
|                                                                 |
|  [Sensori Clima & Luce]  --->  Elaborazione Data  --->  [Wi-Fi] |
+-------+------------------------+------------------------+-------+
       |                        |                        |
       v                        v                        v
+---------------+        +---------------+        +---------------+
|   Attuatori   |        | Servomotori   |        |  App Android  |
| 2x Ventole    |        | Apertura      |        | Monitoraggio  |
| 2x Pompe 12V  |        | Tetto         |        |  & Comandi    |
| Luci Notturne |        +---------------+        +---------------+