\# Changelog



Tutte le modifiche rilevanti al progetto \*\*SmartSense\*\* saranno documentate in questo file.

Il formato è basato su \[Keep a Changelog](https://keepachangelog.com/it/1.0.0/) e aderisce al \[Semantic Versioning](https://semver.org/).



\## \[v1.0.0-hal] - 2026-10-29



Milestone architetturale che introduce l'astrazione hardware completa (HAL) e separa i target firmware per supportare in modo indipendente sia Zigbee 3.0 sia Matter sullo stesso hardware XIAO ESP32-C6.



\### 🚀 Aggiunto (Added)

\- \*\*Supporto Matter Nativo (Google Home):\*\* Integrato lo stack Matter di Espressif con endpoint ambientali (Temperatura, Umidità, Illuminamento) e sensore di Occupancy, validato con Google Nest Hub.

\- \*\*Struttura Firmware Dedicata:\*\* Create le cartelle `firmware/zigbee/` e `firmware/matter/` per compilare in modo autonomo i due stack radio IEEE 802.15.4 con i rispettivi schemi di partizionamento (4MB spiffs vs Huge APP 3MB).

\- \*\*Diagnostica Sensori:\*\* Implementato tracciamento della salute hardware (`SensorHealth`) con timestamp dell'ultimo dato valido, prevenendo la pubblicazione di falsi zeri in caso di errori di bus.



\### 🛠️ Modificato (Changed)

\- \*\*Hardware Abstraction Layer (`SensorsManager`):\*\* Centralizzata la gestione dei sensori I2C (SHT40, BH1750) e del radar mmWave (HLK-LD2410C) in un unico modulo riutilizzabile cross-protocollo.

\- \*\*Parser Radar a Finestra Scorrevole (Sliding Window):\*\* Riscritto il parser UART a 256.000 baud per l'LD2410C: i byte spuri o il rumore elettrico non provocano più lo scarto dell'intero buffer, garantendo sincronizzazione continua sui frame `F4 F3 F2 F1`.

\- \*\*Disaccoppiamento Misure Fisiche:\*\* L'HAL gestisce ora esclusivamente unità di misura SI grezze (float per °C, %RH e Lux); le formule di conversione specifiche (come il calcolo logaritmico ZCL per Zigbee) sono state spostate nel relativo layer applicativo.



\---



\## \[v0.4.1] - 2026-10-28



\### 🚀 Aggiunto (Added)

\- \*\*Supporto Zigbee Router (IEEE 802.15.4):\*\* Configurazione dello XIAO ESP32-C6 come router di rete mesh per Zigbee2MQTT con 4 endpoint (Occupancy, Temperature/Humidity, Illuminance, Distance cm).

\- \*\*Cluster ZCL OTA Upgrade Client (0x0019):\*\* Predisposto il supporto per aggiornamenti firmware via radio con script di conversione immagine container.



\---



\## \[v0.2.0] - 2026-10-27



\### 🚀 Aggiunto (Added)

\- \*\*Firmware Salotto Wi-Fi/MQTT:\*\* Prima versione stabile con pubblicazione periodica, gestione disconnessioni non bloccante, MQTT Last Will and Testament (LWT) e Captive Portal di emergenza su pressione del tasto BOOT.

