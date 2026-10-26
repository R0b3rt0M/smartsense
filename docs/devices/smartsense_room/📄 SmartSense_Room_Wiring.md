# **📄 Scheda Tecnica e Cablaggio: SmartSense Room**

* **Dispositivo:** SmartSense Room (MultiSensor-ZR)  
* **Microcontrollore:** Seeed Studio XIAO ESP32-C6  
* **Ruolo di Rete:** Zigbee Router (Alimentato da rete USB 5V)  
* **Sensori:** Sensirion SHT40, ROHM BH1750 (GY-302), Hi-Link HLK-LD2410C

### **1\. Tabella Assegnazione Pin (Pinout XIAO ESP32-C6)**

| Pin Fisico XIAO | GPIO Interno | Destinazione / Modulo | Pin Sensore | Funzione / Descrizione |
| :---- | :---- | :---- | :---- | :---- |
| **5V (VBUS)** | — | **HLK-LD2410C** | **VCC** (Pin 1\) | Alimentazione radar (5V da USB nativa) |
| **3V3** | — | **SHT40** \+ **BH1750** | **VCC** / **VIN** | Rail 3.3V regolato per sensori I2C |
| **GND** | — | **Tutti i moduli** | **GND** | Riferimento di massa comune a tutto il circuito |
| **D4** | **GPIO22** | **SHT40** \+ **BH1750** | **SDA** | Linea Dati I2C (condivisa in parallelo) |
| **D5** | **GPIO23** | **SHT40** \+ **BH1750** | **SCL** | Linea Clock I2C (condivisa in parallelo) |
| **D7** | **GPIO17** | **HLK-LD2410C** | **TX** | Linea Serial RX (MCU ascolta il flusso dati radar) |
| **D6** | **GPIO16** | **HLK-LD2410C** | **RX** | Linea Serial TX (MCU trasmette comandi al radar) |
| **D1** | **GPIO3** | **HLK-LD2410C** | **OUT** | Trigger digitale rapido presenza (Alto \= Rilevata) |
| **B (Tasto)** | **GPIO9** | *Integrato su scheda* | — | Pulsante BOOT (Pressione per 3s \= Factory Reset Zigbee) |

### **2\. Schema di Collegamento Dettagliato per Modulo**

#### **A. Bus di Alimentazione**

* **Linea 5V:** Il pin **5V** dello XIAO alimenta direttamente il pin **VCC** del radar HLK-LD2410C. *(Consigliato: un condensatore elettrolitico da 100–220 µF posto tra VCC e GND del radar per filtrare le oscillazioni dei picchi RF)*.  
* **Linea 3.3V:** Il pin **3V3** dello XIAO alimenta contemporaneamente i pin **VCC** del sensore **SHT40** e del sensore di luce **BH1750**. *(Non collegare mai questi sensori alla linea 5V)*.  
* **Massa (GND):** Collegare il pin **GND** dello XIAO alla linea di terra comune su cui convergono tutti i pin GND dei sensori.

#### **B. Sensori Ambientali I2C (Condivisi)**

I due sensori condividono la stessa linea a due fili, sfruttando indirizzi hardware distinti:

* **SHT40** (Indirizzo fisso 0x44):  
  * VCC $\rightarrow$ 3V3  
  * GND $\rightarrow$ GND  
  * SDA $\rightarrow$ D4 (GPIO22)  
  * SCL $\rightarrow$ D5 (GPIO23)  
* **BH1750 / GY-302** (Indirizzo configurato 0x23):  
  * VCC $\rightarrow$ 3V3  
  * GND $\rightarrow$ GND  
  * SDA $\rightarrow$ D4 (GPIO22)  
  * SCL $\rightarrow$ D5 (GPIO23)  
  * ADDR $\rightarrow$ Lasciato scollegato o a GND (se collegato a 3.3V l'indirizzo muterebbe in 0x5C mandando il bus in timeout).

#### **C. Radar mmWave Hi-Link HLK-LD2410C**

* VCC $\rightarrow$ Pin 5V (USB)  
* GND $\rightarrow$ GND  
* TX $\rightarrow$ Pin **D7** (GPIO17 / UART RX della MCU)  
* RX $\rightarrow$ Pin **D6** (GPIO16 / UART TX della MCU)  
* OUT $\rightarrow$ Pin **D1** (GPIO3 / Ingresso digitale per interrupt rapido di presenza)

### **3\. Note di Installazione e Assemblaggio Meccanico**

1. **Orientamento del Radar mmWave:** Le onde a 24 GHz attraversano plastiche sottili, legno o vetro. Il radar va orientato con la faccia piana dell'antenna verso l'area della stanza da monitorare; non deve avere viti metalliche o cavi aggrovigliati davanti all'antenna per evitare falsi rimbalzi.  
2. **Esposizione del Sensore di Luce (BH1750):** La schedina GY-302 deve essere collocata in corrispondenza di un foro o di una feritoia trasparente del contenitore, altrimenti la fotocellula rimarrà oscurata leggendo 0–1 Lux.  
3. **Isolamento Termico SHT40:** L'SHT40 deve essere distanziato dal microcontrollore o affacciato su apposite feritoie di areazione esterne per evitare che il calore della CPU alteri le letture.

