# 1\. SmartSense Room \- Ottica e Calibrazione Lux (BH1750)

## Specifiche Hardware: Ottica & Misura Lux (BH1750)

* **Problema rilevato:** Il chip nudo GY-302 legge circa 7 lux in meno a causa della risposta direzionale piatta.  
* **Intervento Enclosure:** Cupola diffusore emisferica opaca/traslucida per luce incidente a 180 gradi.  
* **Intervento Firmware:** CONTINUOUS\_HIGH\_RES\_MODE\_2 (risoluzione 0.5 lx) e incremento registro MTreg per compensare l'attenuazione del case.  
* **Calibrazione Gateway:** Parametro illuminance\_calibration su Zigbee2MQTT/Matter.

# 2\. SmartSense Room \- Antenna e Segnale RF (Hospitality)

## Specifiche Hardware: Segnale RF & Antenna (Hospitality B2B)

* **Requisiti Settore Alberghiero:** Massima stabilità attraverso muri spessi, assenza di antenne esterne a stilo visibili.  
* **Tipologia Antenna:** FPC flessibile adesiva interna (3-5 dBi) su connettore U.FL (IPEX), incollata all'interno del case.  
* **Risoluzione Conflitto Pinout:** Spostare RADAR\_OUT\_PIN da GPIO3 (D1) a GPIO0 (D0) o GPIO2 (D2), poiché GPIO3 controlla l'RF switch dell'antenna interna.  
* **Potenza Trasmissione:** Impostare TX power a \+20 dBm.  
* **Topologia Mesh:** Configurazione Router per fare da ripetitore continuo in camera.

# 3\. SmartSense Room \- Flusso e Specifiche Matter (Hospitality & Produzione)

## 3.1 Provisioning di Produzione e Generazione Codici

* **Assenza di display:** Nei dispositivi commerciali finiti da parete non è presente alcuno schermo. Il QR Code e il codice manuale a 11 cifre vengono stampati su etichetta adesiva (posizionata sul retro della scocca o nel vano di fissaggio a parete) e riportati nella guida rapida della confezione.  
* **Parametri univoci in Flash NVS:** Durante il collaudo di fine linea di montaggio, il programmatore scrive in una partizione protetta permanente (*factory partition* / *matter\_nvs*) tre parametri univoci per ciascun dispositivo:  
  * **Vendor ID (VID) e Product ID (PID)**  
  * **Discriminator:** Valore a 12 bit (es. 3840\) impiegato dallo smartphone per distinguere univocamente il dispositivo qualora ve ne siano molteplici accesi nella medesima stanza d'albergo.  
  * **Setup Passcode:** Codice PIN casuale (es. 20202021\) protetto da crittografia SPAKE2+.  
* **Algoritmo CSA:** Da tali parametri l'algoritmo ufficiale della Connectivity Standards Alliance genera il payload visivo del QR Code e il codice manuale numerico di accoppiamento.

## 3.2 Flusso di Accoppiamento Out-of-the-Box (Installazione in Camera)

1. **Accensione Iniziale (Unpaired):** All'avvio, il firmware rileva l'assenza di un Fabric Matter memorizzato e avvia automaticamente la trasmissione dei beacon Bluetooth Low Energy (BLE Advertisement) con il proprio Discriminator.  
2. **Scansione e Handshake PASE:** L'installatore inquadra il QR Code con l'app (Google Home, Apple Home, Home Assistant). Lo smartphone stabilisce una connessione BLE sicura (PASE \- Password-Authenticated Session Establishment) validando il Passcode.  
3. **Iniezione Credenziali di Rete:** Il controller (es. Google Nest Hub) trasmette via BLE i parametri di connessione:  
   1. Se *Matter-over-Wi-Fi*: SSID e chiave di rete WPA2/WPA3.  
   2. Se *Matter-over-Thread*: PAN ID, Extended PAN ID e Master Key della rete Thread generata dal Thread Border Router.  
4. **Spegnimento BLE:** Una volta completata la connessione radio e conclusa l'intervista iniziale dei cluster, il modulo radio BLE viene spento definitivamente per ridurre i consumi e chiudere la superficie di attacco di sicurezza locale.

## 3.3 Gestione Decommissioning e Ripristino

* **Decommissioning Software (da Controller / App):** L'eliminazione del dispositivo dall'app invia un comando ZCL standard sul cluster *Operational Credentials*. La MCU cancella il certificato operativo del Fabric dalla memoria Flash e riattiva immediatamente i beacon BLE per consentire una nuova associazione.  
* **Factory Reset Hardware (Locale):** Pressione continuativa del pulsante fisico per 10 secondi per forzare la cancellazione totale della memoria non volatile e riavviare la macchina a stati di fabbrica.

## 3.4 Architettura Multi-Admin (Multi-Fabric)

* A differenza dello stack Zigbee (dove il cambio di coordinator richiede un reset distruttivo del nodo), in Matter il dispositivo può appartenere a più Fabric contemporaneamente.

Dal controller principale (es. Google Home) è possibile generare un codice a 11 cifre con validità temporanea di 15 minuti: tale codice consente di associare in parallelo il sensore a piattaforme di gestione alberghiera o supervisori locali (OpenHAB, Home Assistant) senza interrompere la telemetria verso Google Home.  
