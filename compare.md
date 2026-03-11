# Matter vs Zigbee – Vergleich

Bezogen auf den ESP32-C6 Vital Gateway in diesem Projekt.

## Matter (esp32c6_matter_vital_gateway)

**Pro**
- Herstellerübergreifender Standard (Apple, Google, Amazon, Samsung)
- Native Integration in iOS Home, Google Home, Alexa ohne Bridge
- IP-basiert (WiFi / Thread) → kein proprietäres Protokoll
- OTA-Updates über Matter-Protokoll standardisiert
- TLS-verschlüsselt end-to-end
- Zukunftssicher: aktiv weiterentwickelt vom CSA-Konsortium

**Contra**
- Binary sehr groß (~1.9 MB) – wenig Flash-Reserve (1% frei)
- BLE-Commissioning komplex, erfordert App/Controller beim ersten Start
- WiFi-Betrieb erhöht Stromverbrauch (kein Sleep möglich während aktiv)
- Höhere Latenz als Zigbee (TCP/IP Stack overhead)
- Matter über WiFi verliert Verbindung bei Router-Neustart ohne Reconnect-Logik
- Kein direktes MQTT – braucht Home Assistant / Apple Home als Bridge zum Rest

---

## Zigbee (zigbee-vital-sensor)

**Pro**
- Sehr geringe Latenz (< 15 ms)
- Niedriger Stromverbrauch – geeignet für Batteriebetrieb
- Mesh-Netzwerk: Geräte routen selbst, große Reichweite
- MQTT-Integration direkt über Gateway ohne zusätzlichen Controller
- Einfaches Pairing (Permit Join), kein BLE nötig
- Binary klein (~1.5 MB), 25% Flash frei
- Bewährt in der Praxis (Millionen Geräte, Philips Hue, IKEA etc.)

**Contra**
- Proprietäre Koordinator-Hardware nötig (dieser Gateway)
- Nicht nativ in Apple Home / Google Home ohne extra Bridge (z.B. Hue Hub)
- Zigbee-Koordinator ist Single Point of Failure für das Netz
- ZBOSS-Stack benötigt spezifische Flash-Partitionen (subtype `fat`, nicht dokumentiert)
- Kein standardisierter OTA-Mechanismus über alle Hersteller hinweg
- Zigbee 3.0 und ältere Profile (ZHA, ZLL) nicht immer kompatibel

---

## Empfehlung

| Kriterium | Winner |
|-----------|--------|
| Ecosystem-Integration | Matter |
| Stromverbrauch | Zigbee |
| Latenz | Zigbee |
| Einfache Inbetriebnahme | Zigbee |
| Zukunftssicherheit | Matter |
| MQTT / Home Assistant | Zigbee |
| Sicherheit | Matter |
| Binary-Größe / Flash | Zigbee |

**Fazit:** Für dieses Projekt (MQTT-basiertes Smart Home, Home Assistant) ist Zigbee die bessere Wahl. Matter lohnt sich sobald Apple Home / Google Home direkte Integration ohne HA-Bridge benötigt wird.
