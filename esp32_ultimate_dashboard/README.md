# ESP32 Ultimate Network & System Dashboard

Ein einzelnes `.ino`-Programm, das einen **ESP32-WROOM-32** (ohne jede zusätzliche
Hardware) in ein modernes, browserbasiertes Netzwerk- und System-Dashboard verwandelt.

➡️ Vollständige Dokumentation (Architektur, Funktionsliste, Installation, Bedienung,
Fehleranalyse) siehe die Projektantwort im Chat-Verlauf dieser Session.

## Kurzfassung

- **Zielhardware:** ESP32-WROOM-32 (klassischer Xtensa-Dual-Core, BLE 4.2)
- **Benötigte Libraries:** ausschließlich im ESP32-Arduino-Core enthaltene Bibliotheken
  (WiFi, WebServer, Preferences, BLE, ESP-IDF-Header `esp_wifi.h`, `ping/ping_sock.h` usw.)
  — **keine** zusätzliche Library-Installation nötig.
- **Partitionsschema:** In der Arduino IDE unter
  `Tools → Partition Scheme → "Huge APP (3MB No OTA/1MB SPIFFS)"` auswählen
  (Sketch ist ca. 1,86 MB groß, siehe unten).
- **Geprüfter Build:** Dieses Projekt wurde mit dem echten ESP32-Arduino-Core 3.1.3
  (arduino-cli, xtensa-esp32-elf-gcc 13.2.0) gegen das Board `esp32:esp32:esp32`
  kompiliert:
  - Flash: 1.859.312 Bytes (59 % von 3.145.728 Bytes bei `huge_app`-Partition)
  - RAM: 114.676 Bytes globale Variablen (34 % von 327.680 Bytes)
  - 0 Fehler, 0 Warnungen (`-Wall`/`--warnings all`)

## Wichtige technische Hinweise

- Der ESP32-WROOM-32 kann nur **einen** SoftAP und **einen** BLE-Advertiser
  gleichzeitig senden. Der WLAN-/BLE-Beacon-Manager verwaltet deshalb mehrere
  *Profile*, von denen per Zeitrotation jeweils eines aktiv ist – es werden niemals
  mehrere echte, parallele Funkquellen vorgetäuscht.
- `temperatureRead()` basiert auf einer inoffiziellen ROM-Funktion des klassischen
  ESP32 und wird im UI klar als unkalibriert gekennzeichnet.
- Das Dashboard läuft über HTTP (kein TLS) mit HTTP-Basic-Auth
  (Standard: `admin` / `admin`, nach erstem Login in den Einstellungen ändern!).

## Dateien

- `esp32_ultimate_dashboard.ino` – vollständiger, in sich geschlossener Sketch
  (Backend + eingebettetes HTML/CSS/JS-Frontend).
